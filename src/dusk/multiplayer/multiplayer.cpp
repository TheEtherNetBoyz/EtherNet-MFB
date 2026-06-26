#include "dusk/multiplayer/multiplayer.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_e_pz.h"
#include "d/actor/d_a_npc_chin.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/actor/d_a_obj_cblock.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_lv4PoGate.h"
#include "d/actor/d_a_obj_picture.h"
#include "d/actor/d_a_obj_scannon.h"
#include "d/actor/d_a_obj_smallkey.h"
#include "d/actor/d_a_obj_sword.h"
#include "d/actor/d_a_spinner.h"
#include "d/actor/d_a_tbox.h"
#include "d/d_com_inf_game.h"
#include "d/d_bomb.h"
#include "d/d_item.h"
#include "d/d_item_data.h"
#include "d/d_msg_object.h"
#include "d/d_s_play.h"
#include "dusk/logging.h"
#include "dusk/map_loader_definitions.h"
#include "dusk/multiplayer/event_sync.hpp"
#include "dusk/multiplayer/invite_code.hpp"
#include "dusk/multiplayer/remote_link_dummy.hpp"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include "m_Do/m_Do_mtx.h"
#include "aurora/lib/window.hpp"
#include "imgui.h"
#include "nlohmann/json.hpp"

#if _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    static constexpr int kSendFlags = 0;
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    static constexpr int kSendFlags =
    #if defined(__APPLE__)
        0;
    #else
        MSG_NOSIGNAL;
    #endif
    #ifndef INVALID_SOCKET
        #define INVALID_SOCKET -1
    #endif
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dusk::multiplayer {
namespace {

using json = nlohmann::json;

// Direct joiners still talk to one endpoint, but direct hosts now behave as a
// small hub: every accepted TCP peer gets a host-assigned client_id and the
// host forwards peer messages to the rest of the room. Messages from the host
// itself still omit client_id, so direct joiners render the host under this
// placeholder key.
const char* const kDirectPeerId = "direct";
constexpr size_t kMaxDirectPeers = 7;

std::string resolve_peer_id(const json& message) {
    return message.value("client_id", kDirectPeerId);
}

enum class NetworkMode {
    Disabled,
    RelayHarness,
    DirectHost,
    DirectJoin,
};

enum class ConnectionState {
    Disconnected,
    Listening,
    Connecting,
    Connected,
};

enum RemoteSwordVariant {
    REMOTE_SWORD_UNKNOWN = 0,
    REMOTE_SWORD_ORDON = 1,
    REMOTE_SWORD_WOOD = 2,
    REMOTE_SWORD_MASTER = 3,
};

enum RemoteShieldVariant {
    REMOTE_SHIELD_UNKNOWN = 0,
    REMOTE_SHIELD_CARVING_WOOD = 1,
    REMOTE_SHIELD_ORDON = 2,
    REMOTE_SHIELD_HYLIAN = 3,
};

// Mirrors daAlink_c::changeLink()'s clothing-tier branches
// (d_a_alink_wolf.inc:312-369) -- each tier uses a different body archive
// (joint/weight-envelope counts differ even when the skeleton's joint count
// matches), so the remote dummy needs to know which one to clone instead of
// always assuming Hero's Clothes ("Kmdl"). Sumo wrestling outfit
// (FLG2_UNK_200000) is deliberately not covered -- a rare minigame-only
// state.
enum RemoteClothesVariant {
    REMOTE_CLOTHES_HERO = 0,    // "Kmdl" -- changeLink()'s default/else branch
    REMOTE_CLOTHES_CASUAL = 1,  // "Bmdl" -- checkCasualWearFlg()
    REMOTE_CLOTHES_ZORA = 2,    // "Zmdl" -- checkZoraWearFlg()
    REMOTE_CLOTHES_ARMOR = 3,   // "Mmdl" -- checkMagicArmorWearFlg()
};

enum RemoteItemActorKind {
    REMOTE_ITEM_ACTOR_NONE = 0,
    REMOTE_ITEM_ACTOR_BOOMERANG = 1,
    REMOTE_ITEM_ACTOR_BOMB_NORMAL = 2,
    REMOTE_ITEM_ACTOR_BOMB_WATER = 3,
    REMOTE_ITEM_ACTOR_BOMB_INSECT = 4,
};

enum RemoteRideActorKind {
    REMOTE_RIDE_ACTOR_NONE = 0,
    REMOTE_RIDE_ACTOR_SPINNER = 1,
};

int detect_item_actor_kind(fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return REMOTE_ITEM_ACTOR_NONE;
    }

    switch (fopAcM_GetName(actor)) {
    case fpcNm_BOOMERANG_e:
        return REMOTE_ITEM_ACTOR_BOOMERANG;
    case fpcNm_NBOMB_e:
        switch (fopAcM_GetParam(actor)) {
        case dBomb_c::PRM_WATER_BOMB_PLAYER:
            return REMOTE_ITEM_ACTOR_BOMB_WATER;
        case dBomb_c::PRM_INSECT_BOMB_PLAYER:
            return REMOTE_ITEM_ACTOR_BOMB_INSECT;
        case dBomb_c::PRM_NORMAL_BOMB_PLAYER:
        default:
            return REMOTE_ITEM_ACTOR_BOMB_NORMAL;
        }
    default:
        return REMOTE_ITEM_ACTOR_NONE;
    }
}

int detect_ride_actor_kind(fopAc_ac_c* actor) {
    if (actor != nullptr && fopAcM_GetName(actor) == fpcNm_SPINNER_e) {
        return REMOTE_RIDE_ACTOR_SPINNER;
    }

    return REMOTE_RIDE_ACTOR_NONE;
}

struct DirectPeer {
    socket_t sock = INVALID_SOCKET;
    std::string id;
    std::string name = "Peer";
    std::string rxBuffer;
    bool welcomed = false;
    bool snapshotPending = false;
};

int detect_clothes_variant() {
    if (daPy_py_c::checkCasualWearFlg()) {
        return REMOTE_CLOTHES_CASUAL;
    }
    if (daPy_py_c::checkZoraWearFlg()) {
        return REMOTE_CLOTHES_ZORA;
    }
    if (daPy_py_c::checkMagicArmorWearFlg()) {
        return REMOTE_CLOTHES_ARMOR;
    }
    return REMOTE_CLOTHES_HERO;
}

struct Session {
    NetworkMode mode = NetworkMode::Disabled;
    ConnectionState state = ConnectionState::Disconnected;
    socket_t sock = INVALID_SOCKET;
    socket_t listenSock = INVALID_SOCKET;
    std::string host = "127.0.0.1";
    std::string bindHost = "0.0.0.0";
    std::string publicHost = "127.0.0.1";
    std::string room = "dev";
    std::string name = "TP Player";
    std::string inviteCode;
    std::string sessionId;
    std::string sessionKey;
    std::string relayPassword;
    std::string rxBuffer;
    int port = 34197;
    bool debugMarker = false;
    uint32_t reconnectTicks = 0;
    uint32_t pingTicks = 0;
    uint32_t poseSequence = 0;
    uint32_t peerPoseLogTicks = 0;
    // Keyed by peer ID (relay client_id, the direct host's placeholder
    // kDirectPeerId, or host-assigned directN IDs for direct joiners).
    std::map<std::string, PeerPoseSnapshot> peerPoses;
    bool helloSent = false;
    bool welcomeSent = false;
    bool welcomed = false;
    bool snapshotPending = false;
    uint32_t repairSweepTicks = 0;
    uint32_t nextDirectPeerId = 1;
    std::map<std::string, DirectPeer> directPeers;
};

bool sInitialized = false;
bool sEnabled = false;
Session sSession;

bool sDummyModelEnabled = false;
bool sNameLabelsEnabled = true;
fpc_ProcID sLastPlayerBombActorId = fpcM_ERROR_PROCESS_ID_e;

// DarkClearLV/TransformLV (Twilight-clear / wolf-transform region levels)
// double as room-layer/actor-set selectors in at least Ordon Village and
// related rooms (confirmed by direct read of d_com_inf_game.cpp:454-607,
// interleaved with already-synced event bits in the same if/else chain).
// This is the same hazard class OoT Anchor explicitly excludes from its
// blanket switch-sync (Water Temple water level, Forest Temple elevator,
// Ganon's Tower collapse timer) -- the full extent of which TP stage/room
// functions also select layers from these values was never enumerated.
// Off by default; opt in with DUSK_MP_LAYER_SYNC=1 once that audit is done,
// or for testing with both peers in the same room at the same time.
bool sLayerRiskSyncEnabled = false;
bool sNetworkStackStarted = false;

struct Notification {
    std::string text;
    float ageSeconds = 0.0f;
    float durationSeconds = 5.0f;
};

std::vector<Notification> sNotifications;
std::map<std::string, std::string> sPeerNames;
std::map<std::string, uint8_t> sPeerColorSlots;
uint8_t sLocalPlayerColorSlot = 0;
bool sLocalPlayerColorSlotReserved = true;

// Set while applying any remote save-state bit (event bit, chest bit, ...),
// so the dComIfGs_* setter call that applies it doesn't loop back through
// its notify_local_*_set() hook and re-send it to the peer that just sent it
// to us.
bool sApplyingRemoteSaveBit = false;

// Local memory-tier item bits observed through the live setter hook. This
// intentionally records before the network "welcomed" check, so a player can
// collect a heart piece, then have a peer join later and still include that
// pickup in the catch-up snapshot even if vanilla has not committed the
// current stage memory back into the savedata table yet.
std::map<int, std::set<int>> sObservedMemoryItems;

// Incoming messages that touch per-stage save state (everything except
// event bits) need dComIfGp_getStageStagInfo() to be non-null -- every
// dComIfGs_onStageX accessor dereferences it via dStage_stagInfo_GetSaveTbl
// with no null check, because nothing had ever called them before a stage
// was loaded. The TCP handshake completes within milliseconds, well before
// the game finishes booting to a loaded stage, so a peer's save_snapshot (or
// even a live tbox_bit/switch_bit/etc.) can arrive while we're still on the
// boot logo. Messages of these types are queued here and replayed once a
// stage is actually loaded, rather than processed (and crashing) or dropped
// (and silently losing catch-up data).
std::vector<json> sPendingStageMessages;

enum class RemoteSwitchApplyReason {
    Immediate,
    Flush,
};

enum class RemoteSwitchPolicyMode {
    ApplyImmediately,
    DeferUntilRoomInit,
    SuppressRemote,
};

struct RemoteSwitchPolicy {
    int stage;
    int flag;
    RemoteSwitchPolicyMode mode;
    const char* stageName;
    int room;
    const char* reason;
};

struct DeferredRemoteSwitch {
    int stage;
    int flag;
    const RemoteSwitchPolicy* policy;
    uint32_t ageTicks = 0;
};

struct RecentRemoteSwitch {
    int stage;
    int flag;
    uint32_t ageTicks = 0;
    bool deferred = false;
};

struct LocalSwitchActorContext {
    bool active = false;
    int actorName = -1;
    int room = -128;
    int flag = -1;
    int depth = 0;
};

constexpr uint32_t kRemoteSwitchRoomInitTicks = 3;
constexpr uint32_t kRecentRemoteSwitchTicks = 600;

const RemoteSwitchPolicy kRemoteSwitchPolicies[] = {
    {0, 4, RemoteSwitchPolicyMode::DeferUntilRoomInit, "F_SP104", 1,
     "Tag_Hinit horse initializer output switch"},
    {0, 11, RemoteSwitchPolicyMode::SuppressRemote, "F_SP104", 1,
     "Tag_Hinit local trigger switch"},
    {0, 12, RemoteSwitchPolicyMode::DeferUntilRoomInit, "F_SP104", 1,
     "Tag_Hinit secondary horse initializer output switch"},
    {19, 0x26, RemoteSwitchPolicyMode::DeferUntilRoomInit, "D_MN10", -1,
     "E_PO Arbiter's Grounds Poe gate completion switch"},
    {19, 0x43, RemoteSwitchPolicyMode::DeferUntilRoomInit, "D_MN10", -1,
     "E_PO Arbiter's Grounds Poe sequence switch"},
    {23, 63, RemoteSwitchPolicyMode::SuppressRemote, "D_MN08", 0,
     "Palace light ball local carry/recovery guard switch"},
};

std::vector<DeferredRemoteSwitch> sDeferredRemoteSwitches;
std::vector<RecentRemoteSwitch> sRecentRemoteSwitches;
std::string sPolicyStageName;
int sPolicyRoom = -128;
uint32_t sPolicyRoomStableTicks = 0;
std::string sPolicyInitializedStageName;
int sPolicyInitializedRoom = -128;
uint32_t sPolicyRoomInitializedTicks = 0;
bool sPolicyRoomInitialized = false;
LocalSwitchActorContext sLocalSwitchActorContext;

int detect_sword_variant(const daAlink_c* link) {
    if (link == nullptr || link->mSwordModel == nullptr) {
        // Without this guard, a momentarily-null mSwordModel (equip/sheath
        // transition, or before the wood-sword resource is loaded/after it's
        // freed) would compare equal to an ALSO-null mWoodSwordModel below
        // (nullptr == nullptr), misreporting REMOTE_SWORD_WOOD even though no
        // sword is meaningfully equipped -- sending the wood/training-sword
        // model (al_SWB.bmd) to the dummy instead of leaving it unrecognized.
        return REMOTE_SWORD_UNKNOWN;
    }
    if (link->mSwordModel == link->mWoodSwordModel) {
        return REMOTE_SWORD_WOOD;
    }
    if (link->mSwordModel == link->mpSwMModel) {
        return REMOTE_SWORD_MASTER;
    }
    if (link->mSwordModel == link->mpSwAModel) {
        return REMOTE_SWORD_ORDON;
    }
    return REMOTE_SWORD_UNKNOWN;
}

int detect_shield_variant() {
    const u16 equipShield = dComIfGs_getSelectEquipShield();
    switch (equipShield) {
    case dItemNo_WOOD_SHIELD_e: return REMOTE_SHIELD_CARVING_WOOD;
    case dItemNo_SHIELD_e: return REMOTE_SHIELD_ORDON;
    case dItemNo_HYLIA_SHIELD_e: return REMOTE_SHIELD_HYLIAN;
    default: return REMOTE_SHIELD_UNKNOWN;
    }
}

bool is_stage_dependent_message_type(const std::string& type) {
    // key_num touches the same per-stage savedata accessor as tbox_bit/
    // switch_bit/item_bit, so it carries the same risk if applied before a
    // stage is loaded. light_drop_num does not -- it writes directly to
    // player-level save data with no per-stage lookup, same exemption as
    // event_bit.
    return type == "save_snapshot" || type == "tbox_bit" || type == "switch_bit" ||
           type == "item_bit" || type == "dungeon_item_bit" || type == "key_num";
}

// Key items / progression unlocks that are safe to apply on a remote peer by
// simply re-running execItemGet(). Deliberately excludes: rupees/bombs/
// arrows/magic-refill counts and bug catches (volatile consumables, matches
// TP's existing merge rules); small keys, key pieces, and boss keys (small
// keys are explicitly flagged as needing dungeon-scoped conflict handling in
// state-audit.md; boss keys are already synced via dungeon_item_bit kind=2,
// re-applying here would double the hook); mirror pieces (handled by the
// dedicated dSv_player_collect_c::mMirror sync instead, not this item-table
// path, to avoid two mechanisms fighting over the same progression).
bool is_synced_key_item(int itemId) {
    switch (itemId) {
    // Heart pieces / heart containers (already-shipped baseline).
    case dItemNo_KAKERA_HEART_e:
    case dItemNo_UTAWA_HEART_e:
    // Tools.
    case dItemNo_HOOKSHOT_e:
    case dItemNo_W_HOOKSHOT_e:
    case dItemNo_BOOMERANG_e:
    case dItemNo_SPINNER_e:
    case dItemNo_COPY_ROD_e:
    case dItemNo_COPY_ROD_2_e:
    case dItemNo_BOW_e:
    case dItemNo_IRONBALL_e:
    case dItemNo_HAWK_EYE_e:
    case dItemNo_HVY_BOOTS_e:
    case dItemNo_ARMOR_e:
    case dItemNo_PACHINKO_e:
    case dItemNo_KANTERA_e:
    case dItemNo_KANTERA2_e:
    case dItemNo_FISHING_ROD_1_e:
    case dItemNo_LURE_ROD_e:
    case dItemNo_WOOD_STICK_e:
    // Sword / shield / clothing tiers.
    case dItemNo_SWORD_e:
    case dItemNo_MASTER_SWORD_e:
    case dItemNo_LIGHT_SWORD_e:
    case dItemNo_WOOD_SHIELD_e:
    case dItemNo_SHIELD_e:
    case dItemNo_HYLIA_SHIELD_e:
    case dItemNo_WEAR_CASUAL_e:
    case dItemNo_WEAR_KOKIRI_e:
    case dItemNo_WEAR_ZORA_e:
    // Bottle slot unlocks.
    case dItemNo_EMPTY_BOTTLE_e:
    case dItemNo_RED_BOTTLE_e:
    case dItemNo_GREEN_BOTTLE_e:
    case dItemNo_BLUE_BOTTLE_e:
    case dItemNo_MILK_BOTTLE_e:
    case dItemNo_HALF_MILK_BOTTLE_e:
    case dItemNo_OIL_BOTTLE_e:
    case dItemNo_WATER_BOTTLE_e:
    case dItemNo_OIL_BOTTLE_2_e:
    case dItemNo_RED_BOTTLE_2_e:
    case dItemNo_OIL_BOTTLE3_e:
    // Capacity upgrades (quiver/bomb bag/wallet/magic).
    case dItemNo_WALLET_LV1_e:
    case dItemNo_WALLET_LV2_e:
    case dItemNo_WALLET_LV3_e:
    case dItemNo_BOMB_BAG_LV1_e:
    case dItemNo_BOMB_BAG_LV2_e:
    case dItemNo_MAGIC_LV1_e:
    case dItemNo_ARROW_LV1_e:
    case dItemNo_ARROW_LV2_e:
    case dItemNo_ARROW_LV3_e:
    // Quest / letter items.
    case dItemNo_TKS_LETTER_e:
    case dItemNo_RAFRELS_MEMO_e:
    case dItemNo_ASHS_SCRIBBLING_e:
    case dItemNo_LETTER_e:
    case dItemNo_BILL_e:
    case dItemNo_WOOD_STATUE_e:
    case dItemNo_IRIAS_PENDANT_e:
    case dItemNo_HORSE_FLUTE_e:
    case dItemNo_ZORAS_JEWEL_e:
    case dItemNo_ANCIENT_DOCUMENT_e:
    case dItemNo_ANCIENT_DOCUMENT2_e:
    case dItemNo_AIR_LETTER_e:
    case dItemNo_LINKS_SAVINGS_e:
    case dItemNo_TOMATO_PUREE_e:
    case dItemNo_TASTE_e:
    case dItemNo_SURFBOARD_e:
        return true;
    default:
        return false;
    }
}

void remember_memory_item_bit(int stageNo, int flag) {
    if (stageNo >= 0 && flag >= 0 && flag < dSv_info_c::DAN_ITEM) {
        sObservedMemoryItems[stageNo].insert(flag);
    }
}

void reapply_observed_memory_items_for_stage(int stageNo) {
    const auto observedItems = sObservedMemoryItems.find(stageNo);
    if (observedItems == sObservedMemoryItems.end()) {
        return;
    }

    for (const int flag : observedItems->second) {
        if (dComIfGs_isStageMemoryItem(stageNo, flag)) {
            continue;
        }

        sApplyingRemoteSaveBit = true;
        dComIfGs_onStageMemoryItem(stageNo, flag);
        sApplyingRemoteSaveBit = false;
        DuskLog.info("Multiplayer restored cached item bit stage={} flag={}", stageNo, flag);
    }
}

const char* current_stage_name() {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr ? stage : "";
}

const RemoteSwitchPolicy* find_remote_switch_policy(int stage, int flag) {
    for (const RemoteSwitchPolicy& policy : kRemoteSwitchPolicies) {
        if (policy.stage == stage && policy.flag == flag) {
            return &policy;
        }
    }

    return nullptr;
}

const char* remote_switch_policy_mode_name(RemoteSwitchPolicyMode mode) {
    switch (mode) {
    case RemoteSwitchPolicyMode::ApplyImmediately: return "apply";
    case RemoteSwitchPolicyMode::DeferUntilRoomInit: return "defer_until_room_init";
    case RemoteSwitchPolicyMode::SuppressRemote: return "suppress_remote";
    default: return "unknown";
    }
}

const char* group2_lifecycle_actor_reason(int actorName) {
    switch (actorName) {
    case fpcNm_Tag_Mhint_e: return "Tag_Mhint local hint lifecycle switch";
    case fpcNm_Tag_Mmsg_e: return "Tag_Mmsg local message lifecycle switch";
    case fpcNm_Tag_Mstop_e: return "Tag_Mstop local Midna stop lifecycle switch";
    case fpcNm_Tag_TheBHint_e: return "Tag_TheBHint local hint lifecycle switch";
    case fpcNm_Obj_Timer_e: return "Obj_Timer local timer lifecycle switch";
    case fpcNm_NPC_BLUENS_e: return "NpcBlueNS local NPC interaction lifecycle switch";
    case fpcNm_SWC00_e: return "SWC00 local area trigger output switch";
    default: return nullptr;
    }
}

bool is_group2_lifecycle_actor(int actorName) {
    return group2_lifecycle_actor_reason(actorName) != nullptr;
}

void remember_remote_switch(int stage, int flag, bool deferred) {
    for (RecentRemoteSwitch& recent : sRecentRemoteSwitches) {
        if (recent.stage == stage && recent.flag == flag) {
            recent.ageTicks = 0;
            recent.deferred = deferred;
            return;
        }
    }

    sRecentRemoteSwitches.push_back({stage, flag, 0, deferred});
}

void dispatch_remote_switch_repair_hook(int stage, int flag, const char* reason) {
    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr || stage != dStage_stagInfo_GetSaveTbl(stagInfo)) {
        return;
    }

    const bool repairedCBlock = duskRepairCBlockSwitchPosition(flag);
    const bool repairedJumpTbox = duskRepairTboxJumpSwitchPosition(flag);
    const bool monitoredPicture = duskMonitorObjPictureSwitch(flag);
    const bool monitoredNpcChin = duskMonitorNpcChinSwitch(flag);
    const bool repairedLv4PoGate = stage == 19 && flag == 0x26 && duskRepairLv4PoGateOpen();
    const bool repairedPZ = duskRepairE_PZRemoteDefeat(flag);
    const bool repairedSCannon = duskRepairSCannonRemotePortalClosed(flag);

    if (repairedCBlock || repairedJumpTbox || repairedLv4PoGate || repairedPZ || repairedSCannon) {
        DuskLog.info("Multiplayer remote switch repair stage={} flag={} reason={} "
                     "cblock={} jump_tbox={} lv4_poe_gate={} e_pz={} scannon={} "
                     "picture_monitor={} npc_chin_monitor={}",
                     stage, flag, reason, repairedCBlock, repairedJumpTbox, repairedLv4PoGate,
                     repairedPZ, repairedSCannon, monitoredPicture, monitoredNpcChin);
    } else if (monitoredPicture || monitoredNpcChin) {
        DuskLog.info("Multiplayer remote switch monitor stage={} flag={} reason={} "
                     "picture_monitor={} npc_chin_monitor={} repair=reload_or_cosmetic",
                     stage, flag, reason, monitoredPicture, monitoredNpcChin);
    } else {
        DuskLog.info("Multiplayer remote switch repair checked stage={} flag={} reason={} "
                     "cblock={} jump_tbox={} lv4_poe_gate={} e_pz={} scannon={} "
                     "picture_monitor={} npc_chin_monitor={}",
                     stage, flag, reason, repairedCBlock, repairedJumpTbox, repairedLv4PoGate,
                     repairedPZ, repairedSCannon, monitoredPicture, monitoredNpcChin);
    }
}

void update_remote_switch_policy_room_state() {
    const char* stage = current_stage_name();
    const int room = dComIfGp_roomControl_getStayNo();

    if (sPolicyStageName == stage && sPolicyRoom == room) {
        if (sPolicyRoomStableTicks < kRemoteSwitchRoomInitTicks) {
            ++sPolicyRoomStableTicks;
        }
        if (sPolicyRoomInitialized && sPolicyInitializedStageName == stage &&
            sPolicyInitializedRoom == room &&
            sPolicyRoomInitializedTicks < kRemoteSwitchRoomInitTicks)
        {
            ++sPolicyRoomInitializedTicks;
        }
        return;
    }

    sPolicyStageName = stage;
    sPolicyRoom = room;
    sPolicyRoomStableTicks = 0;
    sPolicyRoomInitialized = false;
    sPolicyInitializedStageName.clear();
    sPolicyInitializedRoom = -128;
    sPolicyRoomInitializedTicks = 0;
}

bool is_remote_switch_policy_ready(const RemoteSwitchPolicy& policy) {
    const bool roomMatches = policy.room < 0 || sPolicyRoom == policy.room;
    const bool initializedRoomMatches = policy.room < 0 || sPolicyInitializedRoom == policy.room;
    return sPolicyStageName == policy.stageName && roomMatches &&
           sPolicyRoomStableTicks >= kRemoteSwitchRoomInitTicks && sPolicyRoomInitialized &&
           sPolicyInitializedStageName == policy.stageName && initializedRoomMatches &&
           sPolicyRoomInitializedTicks >= kRemoteSwitchRoomInitTicks;
}

bool is_remote_switch_deferred(int stage, int flag) {
    for (const DeferredRemoteSwitch& deferred : sDeferredRemoteSwitches) {
        if (deferred.stage == stage && deferred.flag == flag) {
            return true;
        }
    }

    return false;
}

void apply_remote_switch_bit_now(int stage, int flag, const char* reason,
                                 RemoteSwitchApplyReason applyReason) {
    if (dComIfGs_isStageSwitch(stage, flag)) {
        DuskLog.info("Multiplayer remote switch already set stage={} flag={} reason={} mode={}",
                     stage, flag, reason,
                     applyReason == RemoteSwitchApplyReason::Flush ? "flush" : "immediate");
        dispatch_remote_switch_repair_hook(stage, flag, reason);
        return;
    }

    const bool wasApplyingRemoteSaveBit = sApplyingRemoteSaveBit;
    sApplyingRemoteSaveBit = true;
    dComIfGs_onStageSwitch(stage, flag);
    sApplyingRemoteSaveBit = wasApplyingRemoteSaveBit;
    remember_remote_switch(stage, flag, applyReason == RemoteSwitchApplyReason::Flush);

    DuskLog.info("Multiplayer applied remote switch bit stage={} flag={} reason={} mode={}",
                 stage, flag, reason,
                 applyReason == RemoteSwitchApplyReason::Flush ? "flush" : "immediate");
    dispatch_remote_switch_repair_hook(stage, flag, reason);
}

void defer_remote_switch_bit(int stage, int flag, const RemoteSwitchPolicy& policy) {
    if (is_remote_switch_deferred(stage, flag)) {
        DuskLog.info("Multiplayer remote switch defer duplicate stage={} flag={} policyMode={} "
                     "targetStage={} targetRoom={} currentStage={} currentRoom={} stableTicks={} "
                     "roomInitialized={} initializedStage={} initializedRoom={} initializedTicks={} "
                     "reason={}",
                     stage, flag, remote_switch_policy_mode_name(policy.mode), policy.stageName,
                     policy.room, current_stage_name(), dComIfGp_roomControl_getStayNo(),
                     sPolicyRoomStableTicks, sPolicyRoomInitialized, sPolicyInitializedStageName,
                     sPolicyInitializedRoom, sPolicyRoomInitializedTicks, policy.reason);
        return;
    }

    sDeferredRemoteSwitches.push_back({stage, flag, &policy, 0});
    DuskLog.info("Multiplayer deferred remote switch bit stage={} flag={} policyMode={} "
                 "targetStage={} targetRoom={} currentStage={} currentRoom={} stableTicks={} "
                 "roomInitialized={} initializedStage={} initializedRoom={} initializedTicks={} "
                 "reason={}",
                 stage, flag, remote_switch_policy_mode_name(policy.mode), policy.stageName,
                 policy.room, current_stage_name(), dComIfGp_roomControl_getStayNo(),
                 sPolicyRoomStableTicks, sPolicyRoomInitialized, sPolicyInitializedStageName,
                 sPolicyInitializedRoom, sPolicyRoomInitializedTicks, policy.reason);
}

void suppress_remote_switch_bit(int stage, int flag, const RemoteSwitchPolicy& policy) {
    DuskLog.info("Multiplayer suppressed remote switch bit stage={} flag={} policyMode={} "
                 "targetStage={} targetRoom={} currentStage={} currentRoom={} reason={}",
                 stage, flag, remote_switch_policy_mode_name(policy.mode), policy.stageName,
                 policy.room, current_stage_name(), dComIfGp_roomControl_getStayNo(),
                 policy.reason);
}

bool suppress_remote_switch_from_source_actor(int stage, int flag, int sourceActor) {
    const char* reason = group2_lifecycle_actor_reason(sourceActor);
    if (reason == nullptr) {
        return false;
    }

    DuskLog.info("Multiplayer suppressed remote switch bit stage={} flag={} sourceActor={} "
                 "reason={}",
                 stage, flag, sourceActor, reason);
    return true;
}

void apply_remote_switch_bit(int stage, int flag) {
    const RemoteSwitchPolicy* policy = find_remote_switch_policy(stage, flag);
    if (policy == nullptr) {
        apply_remote_switch_bit_now(stage, flag, "default", RemoteSwitchApplyReason::Immediate);
        return;
    }

    switch (policy->mode) {
    case RemoteSwitchPolicyMode::ApplyImmediately:
        apply_remote_switch_bit_now(stage, flag, policy->reason, RemoteSwitchApplyReason::Immediate);
        return;
    case RemoteSwitchPolicyMode::DeferUntilRoomInit:
        if (!is_remote_switch_policy_ready(*policy)) {
            defer_remote_switch_bit(stage, flag, *policy);
            return;
        }
        apply_remote_switch_bit_now(stage, flag, policy->reason, RemoteSwitchApplyReason::Immediate);
        return;
    case RemoteSwitchPolicyMode::SuppressRemote:
        suppress_remote_switch_bit(stage, flag, *policy);
        return;
    }

    DuskLog.warn("Multiplayer remote switch policy has unknown mode stage={} flag={} reason={}",
                 stage, flag, policy->reason);
}

void flush_deferred_remote_switches() {
    if (sDeferredRemoteSwitches.empty()) {
        return;
    }

    std::vector<DeferredRemoteSwitch> stillDeferred;
    stillDeferred.reserve(sDeferredRemoteSwitches.size());

    for (DeferredRemoteSwitch& deferred : sDeferredRemoteSwitches) {
        ++deferred.ageTicks;

        if (deferred.policy != nullptr && is_remote_switch_policy_ready(*deferred.policy)) {
            DuskLog.info("Multiplayer flushing deferred remote switch bit stage={} flag={} "
                         "policyMode={} currentStage={} currentRoom={} stableTicks={} "
                         "roomInitialized={} initializedStage={} initializedRoom={} "
                         "initializedTicks={} ageTicks={} reason={}",
                         deferred.stage, deferred.flag,
                         remote_switch_policy_mode_name(deferred.policy->mode),
                         current_stage_name(), dComIfGp_roomControl_getStayNo(),
                         sPolicyRoomStableTicks, sPolicyRoomInitialized,
                         sPolicyInitializedStageName, sPolicyInitializedRoom,
                         sPolicyRoomInitializedTicks, deferred.ageTicks, deferred.policy->reason);
            apply_remote_switch_bit_now(deferred.stage, deferred.flag, deferred.policy->reason,
                                        RemoteSwitchApplyReason::Flush);
        } else {
            stillDeferred.push_back(deferred);
        }
    }

    sDeferredRemoteSwitches = std::move(stillDeferred);
}

void age_recent_remote_switches() {
    std::vector<RecentRemoteSwitch> stillRecent;
    stillRecent.reserve(sRecentRemoteSwitches.size());

    for (RecentRemoteSwitch& recent : sRecentRemoteSwitches) {
        if (++recent.ageTicks <= kRecentRemoteSwitchTicks) {
            stillRecent.push_back(recent);
        }
    }

    sRecentRemoteSwitches = std::move(stillRecent);
}

#if 0
J3DModel* get_or_create_peer_dummy_model() {
    if (sPeerDummyModel != nullptr) {
        return sPeerDummyModel;
    }

    if (sPeerDummyModelLoadFailed) {
        return nullptr;
    }

    DuskLog.info("Multiplayer dummy model: requesting Always/0x20 resource");
    // NOT al.bmd (Link's human-form body model). al.bmd's joint hierarchy
    // bakes in a per-joint calc callback (daAlink_modelCallBack,
    // d_a_alink.cpp:2586) that every J3DModel instance built from that shared
    // model data runs through, regardless of which instance is calc'ing it.
    // The callback unconditionally casts j3dSys.getModel()->getUserArea() to
    // daAlink_c* and calls mutating methods on it (modelCallBack ->
    // jointControll -> resetRootMtx/setFootMatrix/setArmMatrix/
    // changeBlendRate). A bare J3DModel defaults userArea to 0, so that's a
    // null-deref crash; pointing userArea at the real local player instead
    // avoids the crash but corrupts the player's own root/foot/arm matrices
    // every frame using the dummy's transform, which manifested as a
    // whole-PC GPU freeze a frame or two later (degenerate matrices feeding
    // the GPU). daAlink_c::changeModelDataDirect
    // (d_a_alink_swindow.inc:179-193) re-applies this callback onto al.bmd's
    // shared joints continuously, so there is no way to use al.bmd here
    // safely without a real daAlink_c-shaped owner. "Always"/0x20 (the broken
    // jar model used by d_a_obj.cpp's tsubo effect) is a plain static prop
    // with no per-joint callback and no skeleton-owner assumptions, so it's
    // safe to drive from a bare J3DModel. It's a placeholder shape, not the
    // final remote-player visual — see architecture.md.
    sPeerDummyModelData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Always", 0x20));
    if (sPeerDummyModelData == nullptr) {
        if ((sPeerDummyModelRetryTicks++ % 90) == 0) {
            DuskLog.warn("Multiplayer peer dummy model unavailable: Always/0x20 not resident");
        }
        return nullptr;
    }
    sPeerDummyModelRetryTicks = 0;
    DuskLog.info("Multiplayer dummy model: resource resident, creating solid heap");

    // The model must own a dedicated solid heap, like every other long-lived
    // J3DModel in this codebase (d_simple_model.cpp, f_ap_game.cpp). Without
    // one, mDoExt_J3DModel__create allocates from whatever heap happens to be
    // "current" at this call site.
    sPeerDummyHeap = mDoExt_createSolidHeapFromGameToCurrent(0x80000, 0x20);
    if (sPeerDummyHeap == nullptr) {
        sPeerDummyModelLoadFailed = true;
        DuskLog.warn("Multiplayer peer dummy model heap allocation failed");
        return nullptr;
    }
    DuskLog.info("Multiplayer dummy model: heap={} created, creating J3DModel",
                 (void*)sPeerDummyHeap);

    sPeerDummyModel = mDoExt_J3DModel__create(sPeerDummyModelData, 0x80000, 0x11000084);
    if (sPeerDummyModel == nullptr) {
        sPeerDummyModelLoadFailed = true;
        DuskLog.warn("Multiplayer peer dummy model creation failed");
        mDoExt_destroySolidHeap(sPeerDummyHeap);
        sPeerDummyHeap = nullptr;
        mDoExt_restoreCurrentHeap();
        return nullptr;
    }
    DuskLog.info("Multiplayer dummy model: J3DModel={} created, setting base transform",
                 (void*)sPeerDummyModel);

    sPeerDummyModel->setBaseScale(cXyz(8.0f, 8.0f, 8.0f));
    sPeerDummyModel->setBaseTRMtx(mDoMtx_getIdentity());
    std::strncpy(sPeerDummyStage, dComIfGp_getStartStageName(), sizeof(sPeerDummyStage) - 1);
    sPeerDummyStage[sizeof(sPeerDummyStage) - 1] = '\0';

    DuskLog.info("Multiplayer dummy model: adjusting solid heap to system");
    mDoExt_adjustSolidHeapToSystem(sPeerDummyHeap);
    DuskLog.info("Multiplayer dummy model: creation complete");
    return sPeerDummyModel;
}

void remove_peer_dummy_simple_model_registration() {
    if (sPeerDummyModelData != nullptr && sPeerDummyRegisteredRoom != -128) {
        dComIfGp_removeSimpleModel(sPeerDummyModelData, sPeerDummyRegisteredRoom);
        sPeerDummyRegisteredRoom = -128;
    }
}

void destroy_peer_dummy_model() {
    remove_peer_dummy_simple_model_registration();

    if (sPeerDummyHeap != nullptr) {
        mDoExt_destroySolidHeap(sPeerDummyHeap);
    }

    sPeerDummyModel = nullptr;
    sPeerDummyHeap = nullptr;
    sPeerDummyModelData = nullptr;
    sPeerDummyRegisteredRoom = -128;
    sPeerDummyModelLoadFailed = false;
    sPeerDummyModelRetryTicks = 0;
    sPeerDummyStage[0] = '\0';
    sDummyModelDrawLogCount = 0;
}
#endif

bool is_peer_dummy_gameplay_ready() {
    if (fpcM_SearchByName(fpcNm_TITLE_e) != nullptr ||
        fpcM_SearchByName(fpcNm_PLAY_SCENE_e) == nullptr ||
        dComIfGp_getWindowNum() == 0)
    {
        return false;
    }

    static bool sWasEventRunning = false;
    const bool eventRunning = dComIfGp_event_runCheck();
    if (eventRunning != sWasEventRunning) {
        DuskLog.info("Multiplayer remote Link dummy: event_runCheck transitioned {} -> {}",
                     sWasEventRunning, eventRunning);
        sWasEventRunning = eventRunning;
    }
    if (eventRunning) {
        // Cutscenes/events swap and reparent Link's actor/model state in ways
        // the dummy clone path has not been validated against; stay out of
        // the way rather than risk casting/dereferencing a player actor that
        // is mid-transition.
        return false;
    }

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return false;
    }

    if (fopAcM_GetName(player) != fpcNm_ALINK_e) {
        DuskLog.warn("Multiplayer remote Link dummy: player(0) is not ALINK (name={}), skipping",
                     fopAcM_GetName(player));
        return false;
    }

    const daAlink_c* link = static_cast<const daAlink_c*>(player);
    if (link->mClothesChangeWaitTimer != 0) {
        // Confirmed crash (JKRArchive::findNameResource) while the local
        // player was mid clothes/armor change (Magic Armor toggle) -- see
        // the matching guard in remote_link_dummy.cpp's draw_remote_link_
        // dummy() for the full explanation.
        return false;
    }
    if (link->mProcID == daAlink_c::PROC_METAMORPHOSE || link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY) {
        // Wolf<->human transformation frees and reassigns mpLinkModel/
        // mSwordModel/mShieldModel partway through this state -- see
        // add_link_matrices() for the full explanation. Not gated through
        // event_runCheck() above since metamorphosis is its own mProcID
        // state, not the cutscene/event system.
        return false;
    }

    // This only covers gameplay-readiness that's the same regardless of
    // which peer's pose is being considered. The per-peer stage/room match
    // (was here previously, when there was only ever one pose to check) now
    // happens per-peer in draw_debug_peer_marker(), since sSession.peerPoses
    // can hold more than one entry.
    return true;
}

bool has_transforming_peer_pose() {
    for (const auto& entry : sSession.peerPoses) {
        const PeerPoseSnapshot& pose = entry.second;
        if (pose.valid && pose.ageTicks <= 30 && pose.isTransforming) {
            return true;
        }
    }

    return false;
}

#if 0
void destroy_peer_dummy_model_if_stage_changed() {
    if (sPeerDummyModel != nullptr && sPeerDummyStage[0] != '\0' &&
        std::strcmp(sPeerDummyStage, dComIfGp_getStartStageName()) != 0)
    {
        DuskLog.info("Multiplayer peer dummy model discarded for stage change {} -> {}",
                     sPeerDummyStage, dComIfGp_getStartStageName());
        destroy_peer_dummy_model();
    }
}
#endif

#if 0
bool ensure_peer_dummy_simple_model_registered(int roomNo) {
    if (sPeerDummyModelData == nullptr || dComIfGp_getSimpleModel() == nullptr) {
        return false;
    }

    if (sPeerDummyRegisteredRoom == roomNo) {
        return true;
    }

    remove_peer_dummy_simple_model_registration();

    if (dComIfGp_addSimpleModel(sPeerDummyModelData, roomNo, 0) != 1) {
        DuskLog.warn("Multiplayer peer dummy model simple-model registration failed room={}", roomNo);
        return false;
    }

    sPeerDummyRegisteredRoom = roomNo;
    sDummyModelDrawLogCount = 0;
    DuskLog.info("Multiplayer peer dummy model registered with simple-model manager room={}", roomNo);
    return true;
}
#endif

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
            std::strcmp(value, "ON") == 0);
}

std::string env_string(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }

    int parsed = fallback;
    const std::string_view view(value);
    const auto result = std::from_chars(view.data(), view.data() + view.size(), parsed);
    return result.ec == std::errc{} ? parsed : fallback;
}

void close_socket(socket_t& sock) {
    if (sock == INVALID_SOCKET) {
        return;
    }

#if _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    sock = INVALID_SOCKET;
}

int socket_error(socket_t sock) {
    int err = 0;
#if _WIN32
    int len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
#else
    socklen_t len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
    return err;
}

bool would_block() {
#if _WIN32
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

bool set_nonblocking(socket_t sock) {
#if _WIN32
    u_long nonblocking = 1;
    return ioctlsocket(sock, FIONBIO, &nonblocking) == 0;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void push_notification(std::string text, float durationSeconds = 5.0f) {
    if (text.empty()) {
        return;
    }

    sNotifications.push_back({std::move(text), 0.0f, durationSeconds});
    if (sNotifications.size() > 5) {
        sNotifications.erase(sNotifications.begin());
    }
}

std::string display_name_for_peer(const std::string& peerId) {
    const auto name = sPeerNames.find(peerId);
    if (name != sPeerNames.end() && !name->second.empty()) {
        return name->second;
    }
    return peerId.empty() ? "Peer" : peerId;
}

std::string display_area_for_stage(std::string_view stage, int room) {
    if (stage.empty()) {
        return "Unknown";
    }

    const MapEntry* firstStageMatch = nullptr;
    for (const auto& region : gameRegions) {
        for (const auto& map : region.maps) {
            if (map.mapFile == nullptr || stage != map.mapFile) {
                continue;
            }

            if (firstStageMatch == nullptr) {
                firstStageMatch = &map;
            }

            for (const auto& mapRoom : map.mapRooms) {
                if (room >= 0 && mapRoom.roomNo == room) {
                    return map.mapName != nullptr ? map.mapName : std::string(stage);
                }
            }
        }
    }

    if (firstStageMatch != nullptr && firstStageMatch->mapName != nullptr) {
        return firstStageMatch->mapName;
    }
    return std::string(stage);
}

bool is_player_color_slot_used(uint8_t slot) {
    if (sLocalPlayerColorSlotReserved && sLocalPlayerColorSlot == slot) {
        return true;
    }

    for (const auto& entry : sPeerColorSlots) {
        if (entry.second == slot) {
            return true;
        }
    }
    return false;
}

void reserve_local_player_color_slot(uint8_t slot) {
    sLocalPlayerColorSlot = std::min<uint8_t>(slot, static_cast<uint8_t>(7));
    sLocalPlayerColorSlotReserved = true;
}

void assign_peer_color_slot(const std::string& peerId) {
    if (peerId.empty() || sPeerColorSlots.find(peerId) != sPeerColorSlots.end()) {
        return;
    }

    for (uint8_t slot = 0; slot < 8; ++slot) {
        if (!is_player_color_slot_used(slot)) {
            sPeerColorSlots[peerId] = slot;
            return;
        }
    }

    sPeerColorSlots[peerId] = 7;
}

void assign_peer_color_slot(const std::string& peerId, uint8_t slot) {
    if (!peerId.empty()) {
        sPeerColorSlots[peerId] = std::min<uint8_t>(slot, static_cast<uint8_t>(7));
    }
}

ImU32 color_for_peer(const std::string& peerId) {
    assign_peer_color_slot(peerId);
    const auto slotIt = sPeerColorSlots.find(peerId);
    const uint8_t slot = slotIt != sPeerColorSlots.end() ? slotIt->second : 7;

    static const ImU32 kPlayerNameColors[8] = {
        IM_COL32(255, 255, 255, 255),  // Player 1
        IM_COL32(94, 211, 255, 255),
        IM_COL32(255, 214, 92, 255),
        IM_COL32(101, 232, 132, 255),
        IM_COL32(255, 133, 203, 255),
        IM_COL32(255, 169, 82, 255),
        IM_COL32(184, 160, 255, 255),
        IM_COL32(90, 232, 209, 255),
    };
    return kPlayerNameColors[slot];
}

void clear_player_color_slots() {
    sPeerColorSlots.clear();
    reserve_local_player_color_slot(0);
}

float peer_label_height(const PeerPoseSnapshot& pose) {
    if (pose.isTransforming) {
        return pose.transformFromWolf && pose.transformToWolf ? 115.0f : 205.0f;
    }

    return pose.isWolf ? 115.0f : 205.0f;
}

bool peer_pose_is_visible_for_labels(const PeerPoseSnapshot& pose, const char* localStage) {
    if (!pose.valid || pose.ageTicks > 30 || localStage == nullptr || pose.stage != localStage) {
        return false;
    }

    return dComIfGp_roomControl_checkRoomDisp(pose.room);
}

bool should_draw_peer_name_labels_over_game() {
    if (dComIfGp_isPauseFlag() || dScnPly_c::isPause() || dComIfGp_getMesgStatus() != 0) {
        return false;
    }

    dMsgObject_c* msgObject = dMsgObject_getMsgObjectClass();
    if (msgObject != nullptr && dMsgObject_isTalkNowCheck()) {
        return false;
    }

    return true;
}

bool project_peer_label_position(const PeerPoseSnapshot& pose, ImVec2* outPos) {
    if (outPos == nullptr) {
        return false;
    }

    cXyz worldPos(pose.x, pose.y + peer_label_height(pose), pose.z);
    cXyz cameraPos;
    mDoLib_pos2camera(&worldPos, &cameraPos);
    if (cameraPos.z >= -1.0f) {
        return false;
    }

    cXyz projected;
    mDoLib_project(&worldPos, &projected);

    const float gameMinX = mDoGph_gInf_c::getMinXF();
    const float gameMinY = mDoGph_gInf_c::getMinYF();
    const float gameWidth = mDoGph_gInf_c::getWidthF();
    const float gameHeight = mDoGph_gInf_c::getHeightF();
    if (gameWidth <= 0.0f || gameHeight <= 0.0f ||
        projected.x < gameMinX || projected.x > gameMinX + gameWidth ||
        projected.y < gameMinY || projected.y > gameMinY + gameHeight)
    {
        return false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return false;
    }

    const AuroraWindowSize windowSize = aurora::window::get_window_size();
    if (windowSize.native_fb_width == 0 || windowSize.native_fb_height == 0 ||
        windowSize.fb_width == 0 || windowSize.fb_height == 0)
    {
        return false;
    }

    uint32_t presentWidth = windowSize.native_fb_width;
    uint32_t presentHeight = std::min<uint32_t>(
        windowSize.native_fb_height,
        std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(
                                  static_cast<double>(presentWidth) *
                                  static_cast<double>(windowSize.fb_height) /
                                  static_cast<double>(windowSize.fb_width)))));
    if (presentHeight == windowSize.native_fb_height) {
        presentWidth = std::min<uint32_t>(
            windowSize.native_fb_width,
            std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(
                                      static_cast<double>(presentHeight) *
                                      static_cast<double>(windowSize.fb_width) /
                                      static_cast<double>(windowSize.fb_height)))));
    }

    const float presentLeft =
        static_cast<float>((windowSize.native_fb_width - presentWidth) / 2u);
    const float presentTop =
        static_cast<float>((windowSize.native_fb_height - presentHeight) / 2u);
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    const float scaleX = framebufferScale.x > 0.0f ? framebufferScale.x : 1.0f;
    const float scaleY = framebufferScale.y > 0.0f ? framebufferScale.y : 1.0f;

    const float normX = (projected.x - gameMinX) / gameWidth;
    const float normY = (projected.y - gameMinY) / gameHeight;
    outPos->x = viewport->Pos.x + (presentLeft + normX * static_cast<float>(presentWidth)) / scaleX;
    outPos->y = viewport->Pos.y + (presentTop + normY * static_cast<float>(presentHeight)) / scaleY;
    return std::isfinite(outPos->x) && std::isfinite(outPos->y);
}

void draw_peer_name_labels() {
    if (!sEnabled || !sNameLabelsEnabled || !sDummyModelEnabled || !sSession.debugMarker ||
        !has_recent_peer_pose(30) || !should_draw_peer_name_labels_over_game() ||
        !is_peer_dummy_gameplay_ready())
    {
        return;
    }

    const char* localStage = dComIfGp_getStartStageName();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (drawList == nullptr) {
        return;
    }

    for (const auto& entry : sSession.peerPoses) {
        const PeerPoseSnapshot& pose = entry.second;
        if (!peer_pose_is_visible_for_labels(pose, localStage)) {
            continue;
        }

        const std::string label = display_name_for_peer(entry.first);
        if (label.empty()) {
            continue;
        }

        ImVec2 pos;
        if (!project_peer_label_position(pose, &pos)) {
            continue;
        }

        const float fontSize = ImGui::GetFontSize() * 0.92f;
        const float fontScale = fontSize / ImGui::GetFontSize();
        const ImVec2 baseTextSize = ImGui::CalcTextSize(label.c_str());
        const ImVec2 textSize(baseTextSize.x * fontScale, baseTextSize.y * fontScale);
        pos.x -= textSize.x * 0.5f;
        pos.y -= fontSize + 8.0f;
        pos.x = std::round(pos.x);
        pos.y = std::round(pos.y);

        const ImU32 outline = IM_COL32(0, 0, 0, 235);
        const ImU32 text = color_for_peer(entry.first);
        for (const ImVec2 offset : {ImVec2(-2.0f, 0.0f), ImVec2(-1.0f, 0.0f),
                                    ImVec2(1.0f, 0.0f), ImVec2(2.0f, 0.0f),
                                    ImVec2(0.0f, -2.0f), ImVec2(0.0f, -1.0f),
                                    ImVec2(0.0f, 1.0f), ImVec2(0.0f, 2.0f),
                                    ImVec2(-1.0f, -1.0f), ImVec2(1.0f, -1.0f),
                                    ImVec2(-1.0f, 1.0f), ImVec2(1.0f, 1.0f)})
        {
            drawList->AddText(nullptr, fontSize, ImVec2(pos.x + offset.x, pos.y + offset.y),
                              outline, label.c_str());
        }
        drawList->AddText(nullptr, fontSize, pos, text, label.c_str());
    }
}

void reset_connection_state() {
    close_socket(sSession.sock);
    close_socket(sSession.listenSock);
    for (auto& entry : sSession.directPeers) {
        close_socket(entry.second.sock);
    }
    sSession.directPeers.clear();
    sSession.nextDirectPeerId = 1;
    sSession.state = ConnectionState::Disconnected;
    sSession.helloSent = false;
    sSession.welcomeSent = false;
    sSession.welcomed = false;
    sSession.snapshotPending = false;
    sPendingStageMessages.clear();
    sDeferredRemoteSwitches.clear();
    sRecentRemoteSwitches.clear();
    sPolicyStageName.clear();
    sPolicyRoom = -128;
    sPolicyRoomStableTicks = 0;
    sPolicyInitializedStageName.clear();
    sPolicyInitializedRoom = -128;
    sPolicyRoomInitializedTicks = 0;
    sPolicyRoomInitialized = false;
    sLocalSwitchActorContext = {};
    sSession.rxBuffer.clear();
    sSession.reconnectTicks = 0;
    sSession.pingTicks = 0;
    sSession.poseSequence = 0;
    sSession.peerPoseLogTicks = 0;
    sSession.peerPoses.clear();
    sPeerNames.clear();
    clear_player_color_slots();
    sNameLabelsEnabled = true;
    sNotifications.clear();
    destroy_all_remote_link_dummies();
}

void disconnect(const char* reason) {
    if (sSession.state != ConnectionState::Disconnected) {
        DuskLog.warn("Multiplayer disconnected: {}", reason);
    }

    reset_connection_state();
}

bool ensure_network_stack(std::string* errorOut = nullptr) {
#if _WIN32
    if (sNetworkStackStarted) {
        return true;
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        if (errorOut != nullptr) {
            *errorOut = "WSAStartup failed";
        }
        DuskLog.warn("Multiplayer module disabled: WSAStartup failed");
        return false;
    }
    sNetworkStackStarted = true;
#endif
    return true;
}

std::string serialize_json_line(const json& message) {
    std::string bytes = message.dump();
    bytes.push_back('\n');
    return bytes;
}

bool send_bytes(socket_t sock, const std::string& bytes) {
    if (sock == INVALID_SOCKET) {
        return false;
    }

    const char* cursor = bytes.data();
    int remaining = static_cast<int>(bytes.size());

    while (remaining > 0) {
        const int sent = send(sock, cursor, remaining, kSendFlags);
        if (sent > 0) {
            cursor += sent;
            remaining -= sent;
            continue;
        }

        if (would_block()) {
            return true;
        }

        return false;
    }

    return true;
}

bool send_json_to_socket(socket_t sock, const json& message) {
    return send_bytes(sock, serialize_json_line(message));
}

bool send_json_to_peer(DirectPeer& peer, const json& message) {
    if (send_json_to_socket(peer.sock, message)) {
        return true;
    }

    DuskLog.warn("Multiplayer direct peer send failed id={} name={}", peer.id, peer.name);
    close_socket(peer.sock);
    return false;
}

bool send_json(const json& message) {
    if (sSession.mode == NetworkMode::DirectHost) {
        bool sentAny = false;
        for (auto& entry : sSession.directPeers) {
            DirectPeer& peer = entry.second;
            if (peer.welcomed && send_json_to_peer(peer, message)) {
                sentAny = true;
            }
        }
        return sentAny || sSession.directPeers.empty();
    }

    if (send_json_to_socket(sSession.sock, message)) {
        return true;
    }

    disconnect("send failed");
    return false;
}

void send_hello() {
    if (sSession.helloSent) {
        return;
    }

    sSession.helloSent = send_json({
        {"type", "hello"},
        {"protocol_version", 1},
        {"room_id", sSession.room},
        {"session_id", sSession.sessionId},
        {"password", sSession.relayPassword},
        {"name", sSession.name},
    });
}

// Sent once, immediately after this side becomes "welcomed", so a peer that
// joined or reconnected mid-session catches up on durable state it missed
// instead of only receiving bits set after it connected. Lists only
// currently-set bits (not the full bit space) to keep the payload small;
// applying each one through the same setters the live hooks use (below)
// means a late-set local bit during the brief gap before this snapshot
// arrives is naturally preserved -- these are all monotonic OR-merges, so
// receiving a bit twice or in any order is harmless.
void send_save_snapshot(DirectPeer* peer = nullptr) {
    json eventFlags = json::array();
    for (int i = 0; i < 256 * 8; ++i) {
        if (dComIfGs_isEventBit(static_cast<u16>(i))) {
            eventFlags.push_back(i);
        }
    }

    json chestStages = json::array();
    json switchStages = json::array();
    json itemStages = json::array();
    json dungeonStages = json::array();
    json keyCounts = json::array();

    for (int s = 0; s < dSv_save_c::STAGE_MAX; ++s) {
        json chests = json::array();
        for (int i = 0; i < 64; ++i) {
            if (dComIfGs_isStageTbox(s, i)) {
                chests.push_back(i);
            }
        }
        if (!chests.empty()) {
            chestStages.push_back({{"stage", s}, {"flags", chests}});
        }

        json switches = json::array();
        for (int i = 0; i < dSv_info_c::MEMORY_SWITCH; ++i) {
            if (dComIfGs_isStageSwitch(s, i)) {
                switches.push_back(i);
            }
        }
        if (!switches.empty()) {
            switchStages.push_back({{"stage", s}, {"flags", switches}});
        }

        json items = json::array();
        for (int i = 0; i < dSv_info_c::DAN_ITEM; ++i) {
            if (dComIfGs_isStageMemoryItem(s, i)) {
                items.push_back(i);
            }
        }
        const auto observedItems = sObservedMemoryItems.find(s);
        if (observedItems != sObservedMemoryItems.end()) {
            for (const int flag : observedItems->second) {
                bool alreadyListed = false;
                for (const json& item : items) {
                    if (item.get<int>() == flag) {
                        alreadyListed = true;
                        break;
                    }
                }
                if (!alreadyListed) {
                    items.push_back(flag);
                }
            }
        }
        if (!items.empty()) {
            itemStages.push_back({{"stage", s}, {"flags", items}});
        }

        json kinds = json::array();
        if (dComIfGs_isDungeonItemMap(s)) kinds.push_back(0);
        if (dComIfGs_isDungeonItemCompass(s)) kinds.push_back(1);
        if (dComIfGs_isDungeonItemBossKey(s)) kinds.push_back(2);
        if (dComIfGs_isStageBossEnemy(s)) kinds.push_back(3);
        if (dComIfGs_isStageLife(s)) kinds.push_back(4);
        if (dComIfGs_isStageBossDemo(s)) kinds.push_back(5);
        if (dComIfGs_isDungeonItemWarp(s)) kinds.push_back(6);
        if (dComIfGs_isStageMiddleBoss(s)) kinds.push_back(7);
        if (!kinds.empty()) {
            dungeonStages.push_back({{"stage", s}, {"kinds", kinds}});
        }

        const u8 keyNum = dComIfGs_getKeyNum(s);
        if (keyNum > 0) {
            keyCounts.push_back({{"stage", s}, {"count", keyNum}});
        }
    }

    json lightDropCounts = json::array();
    for (int area = 0; area < 4; ++area) {
        const u8 num = dComIfGs_getLightDropNum(static_cast<u8>(area));
        if (num > 0) {
            lightDropCounts.push_back({{"area", area}, {"count", num}});
        }
    }

    json keyItems = json::array();
    for (int i = 0; i < 256; ++i) {
        if (is_synced_key_item(i) && dComIfGs_isItemFirstBit(static_cast<u8>(i))) {
            keyItems.push_back(i);
        }
    }

    json crystals = json::array();
    json mirrors = json::array();
    json darkClearLevels = json::array();
    json transformLevels = json::array();
    json regionBits = json::array();
    for (int i = 0; i < 8; ++i) {
        if (dComIfGs_isCollectCrystal(static_cast<u8>(i))) crystals.push_back(i);
        if (dComIfGs_isCollectMirror(static_cast<u8>(i))) mirrors.push_back(i);
        // Gated by DUSK_MP_LAYER_SYNC -- see sLayerRiskSyncEnabled.
        if (sLayerRiskSyncEnabled && dComIfGs_isDarkClearLV(i)) darkClearLevels.push_back(i);
        if (sLayerRiskSyncEnabled && dComIfGs_isTransformLV(i)) transformLevels.push_back(i);
        if (dComIfGs_isRegionBit(i)) regionBits.push_back(i);
    }

    json collectClothing = json::array();
    json collectSword = json::array();
    json collectShield = json::array();
    for (int i = 0; i < 8; ++i) {
        if (dComIfGs_isCollectClothing(static_cast<u8>(i))) collectClothing.push_back(i);
        if (dComIfGs_isCollectSword(static_cast<u8>(i))) collectSword.push_back(i);
        if (dComIfGs_isCollectShield(static_cast<u8>(i))) collectShield.push_back(i);
    }

    json letterGetFlags = json::array();
    for (int i = 0; i < 64; ++i) {
        if (dComIfGs_isLetterGetFlag(i)) {
            letterGetFlags.push_back(i);
        }
    }

    json snapshot = {
        {"type", "save_snapshot"},
        {"event_flags", eventFlags},
        {"chests", chestStages},
        {"switches", switchStages},
        {"items", itemStages},
        {"dungeon_items", dungeonStages},
        {"key_counts", keyCounts},
        {"light_drop_counts", lightDropCounts},
        {"key_items", keyItems},
        {"crystals", crystals},
        {"mirrors", mirrors},
        {"dark_clear_levels", darkClearLevels},
        {"transform_levels", transformLevels},
        {"region_bits", regionBits},
        {"collect_clothing", collectClothing},
        {"collect_sword", collectSword},
        {"collect_shield", collectShield},
        {"letter_get_flags", letterGetFlags},
        {"max_life", dComIfGs_getMaxLife()},
        {"bottle_slots", dComIfGs_getBottleSlotCount()},
    };
    if (peer != nullptr) {
        send_json_to_peer(*peer, snapshot);
    } else {
        send_json(snapshot);
    }
    DuskLog.info(
        "Multiplayer sent save snapshot event_flags={} chest_stages={} switch_stages={} "
        "item_stages={} dungeon_stages={} key_items={} collect_clothing={} "
        "collect_sword={} collect_shield={} equip_sword={} equip_shield={} equip_armor={}",
        eventFlags.size(), chestStages.size(), switchStages.size(), itemStages.size(),
        dungeonStages.size(), keyItems.size(), collectClothing.size(), collectSword.size(),
        collectShield.size(), dComIfGs_getSelectEquipSword(), dComIfGs_getSelectEquipShield(),
        dComIfGs_getSelectEquipClothes());
}

json direct_peer_list(const std::string& excludePeerId = "") {
    json peers = json::array();
    for (const auto& entry : sSession.directPeers) {
        const DirectPeer& peer = entry.second;
        if (!peer.welcomed || peer.id == excludePeerId) {
            continue;
        }
        peers.push_back({{"client_id", peer.id}, {"name", peer.name}});
    }
    return peers;
}

void send_welcome_to_peer(DirectPeer& peer) {
    if (peer.welcomed) {
        return;
    }

    peer.welcomed = send_json_to_peer(peer, {
        {"type", "welcome"},
        {"protocol_version", 1},
        {"room_id", sSession.room},
        {"client_id", peer.id},
        {"name_labels", sNameLabelsEnabled},
        {"peers", direct_peer_list(peer.id)},
    });
    peer.snapshotPending = peer.welcomed;
    sSession.welcomed = sSession.welcomed || peer.welcomed;
}

void send_welcome() {
    if (sSession.welcomeSent) {
        return;
    }

    sSession.welcomeSent = send_json({
        {"type", "welcome"},
        {"protocol_version", 1},
        {"room_id", sSession.room},
        {"client_id", "host"},
        {"name_labels", sNameLabelsEnabled},
        {"peers", json::array()},
    });
    sSession.welcomed = sSession.welcomeSent;
    if (sSession.welcomed) {
        sSession.snapshotPending = true;
    }
}

json matrix_to_json(CMtxP matrix) {
    json values = json::array();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            values.push_back(matrix[row][col]);
        }
    }
    return values;
}

json model_matrices_to_json(J3DModel* model) {
    if (model == nullptr || model->getModelData() == nullptr) {
        return nullptr;
    }

    J3DModelData* data = model->getModelData();
    const u16 jointCount = data->getJointNum();
    const u16 weightCount = data->getWEvlpMtxNum();

    json joints = json::array();
    for (u16 i = 0; i < jointCount; ++i) {
        const json matrix = matrix_to_json(model->getAnmMtx(i));
        for (const json& value : matrix) {
            joints.push_back(value);
        }
    }

    json weights = json::array();
    for (u16 i = 0; i < weightCount; ++i) {
        const json matrix = matrix_to_json(model->getWeightAnmMtx(i));
        for (const json& value : matrix) {
            weights.push_back(value);
        }
    }

    return {
        {"base", matrix_to_json(model->getBaseTRMtx())},
        {"joint_count", jointCount},
        {"weight_count", weightCount},
        {"joints", joints},
        {"weights", weights},
    };
}

json model_matrices_to_json_if(bool enabled, J3DModel* model) {
    return enabled ? model_matrices_to_json(model) : json(nullptr);
}

int visible_material_shape_index(J3DModel* model, int count, int fallback) {
    if (model == nullptr || model->getModelData() == nullptr) {
        return fallback;
    }

    J3DModelData* data = model->getModelData();
    const int materialCount = data->getMaterialNum();
    for (int i = 0; i < count && i < materialCount; ++i) {
        J3DMaterial* material = data->getMaterialNodePointer(i);
        J3DShape* shape = material != nullptr ? material->getShape() : nullptr;
        if (shape != nullptr && !shape->checkFlag(J3DShpFlag_Visible)) {
            return i;
        }
    }

    return fallback;
}

bool add_link_matrices(json& state) {
    // Note: deliberately not gated on dComIfGp_event_runCheck() here, unlike
    // the dummy draw path. This only reads already-built matrices off the
    // real local Link actor; it doesn't allocate/destroy anything. Personal
    // animations (chest-open, post-respawn get-up) run through the event
    // system without recreating Link's actor, and skipping capture during
    // them made the remote dummy go static for their duration. The actual
    // crash risk was casting through a non-Link actor below, which the name
    // check guards directly.
    fopAc_ac_c* playerActor = dComIfGp_getPlayer(0);
    if (playerActor == nullptr || fopAcM_GetName(playerActor) != fpcNm_ALINK_e) {
        return false;
    }

    daAlink_c* link = static_cast<daAlink_c*>(playerActor);
    if (link->mClothesChangeWaitTimer != 0) {
        // Confirmed crash (JKRArchive::findNameResource) on the receiving
        // side while the local player there was mid clothes/armor change.
        // Skipping capture here too means we don't send a pose mid-change
        // in the first place, same defense-in-depth as the metamorphosis
        // guard below.
        return false;
    }
    if (link->mProcID == daAlink_c::PROC_METAMORPHOSE || link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY) {
        // Wolf<->human transformation. daAlink_c::changeWolf()/changeLink()
        // free the old model/arc resources (mpArcHeap->freeAll()) and then
        // reassign mpLinkModel/mSwordModel/mShieldModel to freshly-allocated
        // objects partway through this state, not atomically with the state
        // transition itself -- reading those fields during the window is a
        // use-after-free risk, not just a stale-data one.
        return false;
    }

    if (link->mpLinkModel == nullptr) {
        return false;
    }

    const bool isWolf = static_cast<bool>(link->checkWolf());
    if (!sDummyModelEnabled && !isWolf) {
        return true;
    }

    const bool includeHumanParts = !isWolf;
    J3DModel* arrowModel = nullptr;
    J3DModel* itemActorModel = nullptr;
    J3DModel* rideActorModel = nullptr;
    int itemActorKind = REMOTE_ITEM_ACTOR_NONE;
    int rideActorKind = REMOTE_RIDE_ACTOR_NONE;
    if (includeHumanParts) {
        fopAc_ac_c* itemActor = link->mItemAcKeep.getActor();
        fopAc_ac_c* thrownBoomerangActor = link->getThrowBoomerangAcKeep()->getActor();
        fopAc_ac_c* grabActor = link->mGrabItemAcKeep.getActor();
        fopAc_ac_c* rememberedBombActor =
            sLastPlayerBombActorId != fpcM_ERROR_PROCESS_ID_e
                ? fopAcM_SearchByID(sLastPlayerBombActorId)
                : nullptr;
        if (rememberedBombActor == nullptr ||
            fopAcM_GetName(rememberedBombActor) != fpcNm_NBOMB_e)
        {
            rememberedBombActor = nullptr;
            sLastPlayerBombActorId = fpcM_ERROR_PROCESS_ID_e;
        }

        if (grabActor != nullptr && fopAcM_GetName(grabActor) == fpcNm_NBOMB_e) {
            sLastPlayerBombActorId = fopAcM_GetID(grabActor);
            rememberedBombActor = grabActor;
        }

        if (thrownBoomerangActor != nullptr &&
            fopAcM_GetName(thrownBoomerangActor) == fpcNm_BOOMERANG_e)
        {
            itemActor = thrownBoomerangActor;
        } else if (grabActor != nullptr && detect_item_actor_kind(grabActor) != REMOTE_ITEM_ACTOR_NONE) {
            itemActor = grabActor;
        } else if (rememberedBombActor != nullptr) {
            itemActor = rememberedBombActor;
        }

        if (itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_ARROW_e) {
            arrowModel = itemActor->model;
        } else if (itemActor != nullptr) {
            itemActorModel = itemActor->model;
            itemActorKind = detect_item_actor_kind(itemActor);
        }

        fopAc_ac_c* rideActor = link->mRideAcKeep.getActor();
        rideActorKind = detect_ride_actor_kind(rideActor);
        if (rideActorKind == REMOTE_RIDE_ACTOR_SPINNER) {
            rideActorModel = rideActor->model;
        }
    }

    state["link_matrices"] = {
        {"body", model_matrices_to_json(link->mpLinkModel)},
        {"hat", model_matrices_to_json_if(includeHumanParts, link->mpLinkHatModel)},
        {"face", model_matrices_to_json_if(includeHumanParts, link->mpLinkFaceModel)},
        {"hand", model_matrices_to_json_if(includeHumanParts, link->mpLinkHandModel)},
        {"sword", model_matrices_to_json_if(includeHumanParts, link->mSwordModel)},
        {"sheath", model_matrices_to_json_if(includeHumanParts, link->mSheathModel)},
        {"shield", model_matrices_to_json_if(includeHumanParts, link->mShieldModel)},
        {"held_item", model_matrices_to_json_if(includeHumanParts, link->mHeldItemModel)},
        {"hook_tip", model_matrices_to_json_if(includeHumanParts, link->mpHookTipModel)},
        {"hook_sub_item", model_matrices_to_json_if(includeHumanParts, link->field_0x0710)},
        {"hook_sub_tip", model_matrices_to_json_if(includeHumanParts, link->field_0x0714)},
        {"arrow", model_matrices_to_json_if(includeHumanParts, arrowModel)},
        {"kantera", model_matrices_to_json_if(includeHumanParts, link->mpKanteraModel)},
        {"kantera_glow", model_matrices_to_json_if(includeHumanParts, link->mpKanteraGlowModel)},
        {"item_actor", model_matrices_to_json_if(includeHumanParts, itemActorModel)},
        {"ride_actor", model_matrices_to_json_if(includeHumanParts, rideActorModel)},
        {"midna", model_matrices_to_json_if(isWolf, link->getMidnaModel())},
        {"midna_mask", model_matrices_to_json_if(isWolf, link->getMidnaMaskModel())},
        {"midna_hand", model_matrices_to_json_if(isWolf, link->getMidnaHandModel())},
        {"midna_hair", model_matrices_to_json_if(isWolf, link->getMidnaHairHandModel())},
        {"midna_hair_shape", isWolf ? visible_material_shape_index(link->getMidnaHairHandModel(), 3, 0) : 0},
    };

    state["equip_item"] = static_cast<int>(link->mEquipItem);
    state["sword_variant"] = detect_sword_variant(link);
    state["shield_variant"] = detect_shield_variant();
    state["clothes_variant"] = detect_clothes_variant();
    state["sword_draw"] = !isWolf && static_cast<bool>(link->checkSwordDraw());
    state["shield_draw"] = !isWolf && static_cast<bool>(link->checkShieldDraw());
    state["sword_out"] = !isWolf && link->mEquipItem == 0x103;
    state["item_draw"] = !isWolf && static_cast<bool>(link->checkItemDraw());
    state["kantera_draw"] =
        !isWolf && (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) ||
                    link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_20000));
    state["item_actor_kind"] = itemActorKind;
    state["ride_actor_kind"] = rideActorKind;
    state["form"] = isWolf ? "wolf" : "human";
    return true;
}

bool read_matrix_array(const json& source, std::array<float, 12>& out) {
    if (!source.is_array() || source.size() != out.size()) {
        return false;
    }

    for (size_t i = 0; i < out.size(); ++i) {
        if (!source[i].is_number()) {
            return false;
        }
        out[i] = source[i].get<float>();
    }
    return true;
}

bool read_matrix_block(const json& source, uint16_t matrixCount, std::vector<float>& out) {
    const size_t expectedSize = static_cast<size_t>(matrixCount) * 12;
    if (!source.is_array() || source.size() != expectedSize) {
        return false;
    }

    out.clear();
    out.reserve(expectedSize);
    for (const json& value : source) {
        if (!value.is_number()) {
            out.clear();
            return false;
        }
        out.push_back(value.get<float>());
    }
    return true;
}

RemoteModelMatrixSnapshot parse_model_matrices(const json& source) {
    RemoteModelMatrixSnapshot snapshot;
    if (!source.is_object()) {
        return snapshot;
    }

    const int jointCount = source.value("joint_count", -1);
    const int weightCount = source.value("weight_count", -1);
    if (jointCount < 0 || weightCount < 0 ||
        jointCount > (std::numeric_limits<uint16_t>::max)() ||
        weightCount > (std::numeric_limits<uint16_t>::max)())
    {
        return snapshot;
    }

    snapshot.jointCount = static_cast<uint16_t>(jointCount);
    snapshot.weightCount = static_cast<uint16_t>(weightCount);

    if (!read_matrix_array(source.value("base", json::array()), snapshot.base) ||
        !read_matrix_block(source.value("joints", json::array()), snapshot.jointCount,
                           snapshot.joints) ||
        !read_matrix_block(source.value("weights", json::array()), snapshot.weightCount,
                           snapshot.weights))
    {
        snapshot = {};
        return snapshot;
    }

    snapshot.valid = true;
    return snapshot;
}

RemoteLinkMatrixSnapshot parse_link_matrices(const json& state) {
    RemoteLinkMatrixSnapshot snapshot;
    const auto it = state.find("link_matrices");
    if (it == state.end() || !it->is_object()) {
        return snapshot;
    }

    snapshot.body = parse_model_matrices(it->value("body", json::object()));
    snapshot.hat = parse_model_matrices(it->value("hat", json::object()));
    snapshot.face = parse_model_matrices(it->value("face", json::object()));
    snapshot.hand = parse_model_matrices(it->value("hand", json::object()));
    snapshot.sword = parse_model_matrices(it->value("sword", json::object()));
    snapshot.sheath = parse_model_matrices(it->value("sheath", json::object()));
    snapshot.shield = parse_model_matrices(it->value("shield", json::object()));
    snapshot.heldItem = parse_model_matrices(it->value("held_item", json::object()));
    snapshot.hookTip = parse_model_matrices(it->value("hook_tip", json::object()));
    snapshot.hookSubItem = parse_model_matrices(it->value("hook_sub_item", json::object()));
    snapshot.hookSubTip = parse_model_matrices(it->value("hook_sub_tip", json::object()));
    snapshot.arrow = parse_model_matrices(it->value("arrow", json::object()));
    snapshot.kantera = parse_model_matrices(it->value("kantera", json::object()));
    snapshot.kanteraGlow = parse_model_matrices(it->value("kantera_glow", json::object()));
    snapshot.itemActor = parse_model_matrices(it->value("item_actor", json::object()));
    snapshot.rideActor = parse_model_matrices(it->value("ride_actor", json::object()));
    snapshot.midna = parse_model_matrices(it->value("midna", json::object()));
    snapshot.midnaMask = parse_model_matrices(it->value("midna_mask", json::object()));
    snapshot.midnaHand = parse_model_matrices(it->value("midna_hand", json::object()));
    snapshot.midnaHair = parse_model_matrices(it->value("midna_hair", json::object()));
    snapshot.midnaHairShape = it->value("midna_hair_shape", 0);
    if (snapshot.midnaHairShape < 0 || snapshot.midnaHairShape > 2) {
        snapshot.midnaHairShape = 0;
    }
    snapshot.valid = snapshot.body.valid;
    return snapshot;
}

void apply_remote_tbox_bit(int stage, int flag);

void remove_direct_peer(const std::string& peerId, const char* reason) {
    auto it = sSession.directPeers.find(peerId);
    if (it == sSession.directPeers.end()) {
        return;
    }

    DuskLog.warn("Multiplayer direct peer disconnected id={} reason={}", peerId, reason);
    const std::string peerName = display_name_for_peer(peerId);
    push_notification(peerName + " left");
    close_socket(it->second.sock);
    sSession.directPeers.erase(it);
    sSession.peerPoses.erase(peerId);
    sPeerNames.erase(peerId);
    sPeerColorSlots.erase(peerId);
    destroy_remote_link_dummy(peerId);

    json left = {{"type", "peer_left"}, {"client_id", peerId}};
    for (auto& entry : sSession.directPeers) {
        if (entry.second.welcomed) {
            send_json_to_peer(entry.second, left);
        }
    }
}

void broadcast_to_direct_peers(const json& message, const std::string& excludePeerId = "") {
    std::vector<std::string> failedPeers;
    for (auto& entry : sSession.directPeers) {
        DirectPeer& peer = entry.second;
        if (!peer.welcomed || peer.id == excludePeerId) {
            continue;
        }
        if (!send_json_to_peer(peer, message)) {
            failedPeers.push_back(peer.id);
        }
    }

    for (const std::string& peerId : failedPeers) {
        remove_direct_peer(peerId, "send failed");
    }
}

bool should_forward_peer_message(const std::string& type) {
    return type != "hello" && type != "ping" && type != "pong" && type != "error";
}

void handle_message(const json& message, DirectPeer* sender = nullptr) {
    const std::string type = message.value("type", "");
    json routedMessage = message;
    if (sender != nullptr && type != "hello") {
        routedMessage["client_id"] = sender->id;
    }
    if (is_stage_dependent_message_type(type) && dComIfGp_getStageStagInfo() == nullptr) {
        sPendingStageMessages.push_back(routedMessage);
        if (sender != nullptr && should_forward_peer_message(type)) {
            broadcast_to_direct_peers(routedMessage, sender->id);
        }
        return;
    }

    if (type == "hello" && sSession.mode == NetworkMode::DirectHost) {
        if (sender == nullptr) {
            return;
        }

        sender->name = message.value("name", sender->name);
        sPeerNames[sender->id] = sender->name;
        assign_peer_color_slot(sender->id);
        DuskLog.info("Multiplayer direct peer joined id={} name={} room={}", sender->id,
                     sender->name, message.value("room_id", ""));
        push_notification(sender->name + " joined");
        send_welcome_to_peer(*sender);
        broadcast_to_direct_peers({
            {"type", "peer_joined"},
            {"client_id", sender->id},
            {"name", sender->name},
        }, sender->id);
    } else if (type == "welcome") {
        sSession.welcomed = true;
        if (message.contains("name_labels")) {
            sNameLabelsEnabled = message.value("name_labels", sNameLabelsEnabled);
        }
        DuskLog.info("Multiplayer joined room={} client_id={} peers={}",
                     message.value("room_id", ""), message.value("client_id", ""),
                     message.value("peers", json::array()).size());

        sPeerColorSlots.clear();
        uint8_t nextColorSlot = 0;
        if (sSession.mode == NetworkMode::DirectJoin) {
            sPeerNames[kDirectPeerId] = "Host";
            assign_peer_color_slot(kDirectPeerId, nextColorSlot++);
        }
        for (const json& peer : message.value("peers", json::array())) {
            const std::string peerId = peer.value("client_id", "");
            const std::string peerName = peer.value("name", peerId);
            if (!peerId.empty()) {
                sPeerNames[peerId] = peerName;
                assign_peer_color_slot(peerId, nextColorSlot++);
            }
        }
        reserve_local_player_color_slot(nextColorSlot);
        push_notification("Joined lobby " + message.value("room_id", sSession.room));
        sSession.snapshotPending = true;
    } else if (type == "peer_joined") {
        const std::string peerId = message.value("client_id", "");
        const std::string peerName = message.value("name", peerId);
        if (!peerId.empty()) {
            sPeerNames[peerId] = peerName;
            assign_peer_color_slot(peerId);
        }
        DuskLog.info("Multiplayer peer joined id={} name={}", message.value("client_id", ""),
                     message.value("name", ""));
        push_notification(display_name_for_peer(peerId) + " joined");
    } else if (type == "name_labels") {
        if (sSession.mode == NetworkMode::DirectJoin) {
            sNameLabelsEnabled = message.value("enabled", sNameLabelsEnabled);
            DuskLog.info("Multiplayer name labels {}", sNameLabelsEnabled ? "enabled" : "disabled");
        }
    } else if (type == "peer_left") {
        const std::string leftPeerId = resolve_peer_id(routedMessage);
        const std::string peerName = display_name_for_peer(leftPeerId);
        DuskLog.info("Multiplayer peer left id={}", leftPeerId);
        sSession.peerPoses.erase(leftPeerId);
        sPeerNames.erase(leftPeerId);
        sPeerColorSlots.erase(leftPeerId);
        destroy_remote_link_dummy(leftPeerId);
        push_notification(peerName + " left");
    } else if (type == "save_snapshot") {
        sApplyingRemoteSaveBit = true;
        for (const json& flag : message.value("event_flags", json::array())) {
            dComIfGs_onEventBit(static_cast<uint16_t>(flag.get<int>()));
        }
        for (const json& entry : message.value("chests", json::array())) {
            const int stage = entry.value("stage", -1);
            for (const json& flag : entry.value("flags", json::array())) {
                apply_remote_tbox_bit(stage, flag.get<int>());
                DuskLog.info("Multiplayer snapshot chest bit stage={} flag={}", stage,
                             flag.get<int>());
            }
        }
        for (const json& entry : message.value("switches", json::array())) {
            const int stage = entry.value("stage", -1);
            for (const json& flag : entry.value("flags", json::array())) {
                apply_remote_switch_bit(stage, flag.get<int>());
            }
        }
        for (const json& entry : message.value("items", json::array())) {
            const int stage = entry.value("stage", -1);
            for (const json& flag : entry.value("flags", json::array())) {
                const int flagValue = flag.get<int>();
                dComIfGs_onStageMemoryItem(stage, flagValue);
                remember_memory_item_bit(stage, flagValue);
                DuskLog.info("Multiplayer snapshot item bit stage={} flag={}", stage,
                             flagValue);
            }
        }
        {
            stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
            const int currentStage = stagInfo != nullptr ? dStage_stagInfo_GetSaveTbl(stagInfo) : -1;
            DuskLog.info("Multiplayer snapshot item repair pass current_stage={}", currentStage);
            for (const json& entry : message.value("items", json::array())) {
                if (entry.value("stage", -1) != currentStage) {
                    continue;
                }
                for (const json& flag : entry.value("flags", json::array())) {
                    const int globalBit = flag.get<int>() + dSv_info_c::MEMORY_ITEM;
                    const bool repairedLife = duskRepairObjLifeVisual(globalBit);
                    const bool repairedSword = !repairedLife && duskRepairObjSwordVisual(globalBit);
                    DuskLog.info(
                        "Multiplayer snapshot item bit repair stage={} flag={} found_life={} found_sword={}",
                        currentStage, flag.get<int>(), repairedLife, repairedSword);
                }
            }
        }
        for (const json& entry : message.value("dungeon_items", json::array())) {
            const int stage = entry.value("stage", -1);
            for (const json& kind : entry.value("kinds", json::array())) {
                switch (kind.get<int>()) {
                case 0: dComIfGs_onDungeonItemMap(stage); break;
                case 1: dComIfGs_onDungeonItemCompass(stage); break;
                case 2: dComIfGs_onDungeonItemBossKey(stage); break;
                case 3: dComIfGs_onStageBossEnemy(stage); break;
                case 4: dComIfGs_onStageLife(stage); break;
                case 5: dComIfGs_onStageBossDemo(stage); break;
                case 6: dComIfGs_onDungeonItemWarp(stage); break;
                case 7: dComIfGs_onStageMiddleBoss(stage); break;
                default: break;
                }
            }
        }
        for (const json& entry : message.value("key_counts", json::array())) {
            const int stage = entry.value("stage", -1);
            const int count = entry.value("count", -1);
            if (stage >= 0 && stage < dSv_save_c::STAGE_MAX && count >= 0 && count <= 99) {
                dComIfGs_setKeyNum(stage, static_cast<u8>(count));
            }
        }
        for (const json& entry : message.value("light_drop_counts", json::array())) {
            const int area = entry.value("area", -1);
            const int count = entry.value("count", -1);
            if (area >= 0 && area <= 0xFF && count >= 0 && count <= 0xFF) {
                dComIfGs_setLightDropNum(static_cast<u8>(area), static_cast<u8>(count));
                if (area == 2 && count == 15) {
                    /* dSv_event_flag_c::F_0005 - Misc. - Gathered 14 Tears of Light in area 4 */
                    dComIfGs_onEventBit(dSv_event_flag_c::saveBitLabels[9]);
                }
            }
        }
        for (const json& idEntry : message.value("key_items", json::array())) {
            const int itemId = idEntry.get<int>();
            if (is_synced_key_item(itemId) && !dComIfGs_isItemFirstBit(static_cast<u8>(itemId))) {
                execItemGet(static_cast<u8>(itemId));
                DuskLog.info("Multiplayer snapshot key item item_id={}", itemId);
            }
        }
        for (const json& entry : message.value("crystals", json::array())) {
            dComIfGs_onCollectCrystal(static_cast<u8>(entry.get<int>()));
        }
        for (const json& entry : message.value("mirrors", json::array())) {
            dComIfGs_onCollectMirror(static_cast<u8>(entry.get<int>()));
        }
        // Gated by DUSK_MP_LAYER_SYNC -- see sLayerRiskSyncEnabled. A peer
        // with the flag off must not apply these even if a flag-on sender
        // included them in its snapshot.
        if (sLayerRiskSyncEnabled) {
            for (const json& entry : message.value("dark_clear_levels", json::array())) {
                const int no = entry.get<int>();
                if (no >= 0 && no < 8) {
                    dComIfGs_onDarkClearLV(no);
                }
            }
            for (const json& entry : message.value("transform_levels", json::array())) {
                const int no = entry.get<int>();
                if (no >= 0 && no < 8) {
                    dComIfGs_onTransformLV(no);
                }
            }
        }
        for (const json& entry : message.value("region_bits", json::array())) {
            dComIfGs_onRegionBit(entry.get<int>());
        }
        for (const json& entry : message.value("collect_clothing", json::array())) {
            g_dComIfG_gameInfo.info.getPlayer().getCollect().setCollect(
                COLLECT_CLOTHING, static_cast<u8>(entry.get<int>()));
        }
        for (const json& entry : message.value("collect_sword", json::array())) {
            g_dComIfG_gameInfo.info.getPlayer().getCollect().setCollect(
                COLLECT_SWORD, static_cast<u8>(entry.get<int>()));
        }
        for (const json& entry : message.value("collect_shield", json::array())) {
            g_dComIfG_gameInfo.info.getPlayer().getCollect().setCollect(
                COLLECT_SHIELD, static_cast<u8>(entry.get<int>()));
        }
        for (const json& entry : message.value("letter_get_flags", json::array())) {
            dComIfGs_onLetterGetFlag(entry.get<int>());
        }
        sApplyingRemoteSaveBit = false;
        const int remoteMaxLife = message.value("max_life", 0);
        if (remoteMaxLife > dComIfGs_getMaxLife() && remoteMaxLife <= 100) {
            dComIfGs_setMaxLife(static_cast<u8>(remoteMaxLife));
            DuskLog.info("Multiplayer snapshot max life set to {}", remoteMaxLife);
        }
        const int remoteBottleSlots = message.value("bottle_slots", 0);
        const int localBottleSlots = dComIfGs_getBottleSlotCount();
        if (remoteBottleSlots > localBottleSlots && remoteBottleSlots <= 4) {
            for (int i = localBottleSlots; i < remoteBottleSlots; ++i) {
                dComIfGs_setEmptyBottle();
            }
            DuskLog.info("Multiplayer snapshot bottle slots set to {}", remoteBottleSlots);
        }
        DuskLog.info("Multiplayer applied save snapshot from peer");
    } else if (type == "pose") {
        const std::string peerId = resolve_peer_id(routedMessage);
        const uint32_t sequence = routedMessage.value("sequence", 0U);
        auto existing = sSession.peerPoses.find(peerId);
        if (existing != sSession.peerPoses.end() && existing->second.valid &&
            sequence <= existing->second.sequence)
        {
            return;
        }

        const json state = routedMessage.value("state", json::object());
        PeerPoseSnapshot pose;
        pose.valid = true;
        pose.peerId = peerId;
        pose.sequence = sequence;
        pose.ageTicks = 0;
        pose.stage = state.value("stage", "");
        pose.room = state.value("room", -1);
        pose.layer = state.value("layer", -1);
        pose.x = state.value("x", 0.0f);
        pose.y = state.value("y", 0.0f);
        pose.z = state.value("z", 0.0f);
        pose.angleY = state.value("angle_y", 0);
        pose.procId = state.value("proc_id", 0);
        pose.procVar0 = state.value("proc_v0", 0);
        pose.procVar1 = state.value("proc_v1", 0);
        pose.procVar2 = state.value("proc_v2", 0);
        pose.procVar3 = state.value("proc_v3", 0);
        pose.procVar5 = state.value("proc_v5", 0);
        pose.underFrame = state.value("under_frame", 0.0f);
        pose.underBck0 = state.value("under_bck0", 0);
        pose.underFrame0 = state.value("under_frame0", pose.underFrame);
        pose.underRate0 = state.value("under_rate0", 1.0f);
        pose.upperBck2 = state.value("upper_bck2", 0);
        pose.upperFrame2 = state.value("upper_frame2", 0.0f);
        pose.upperRate2 = state.value("upper_rate2", 1.0f);
        pose.isWolf = state.value("is_wolf", false);
        pose.isTransforming = state.value("is_transforming", false);
        pose.transformFromWolf = state.value("transform_from_wolf", pose.isWolf);
        pose.transformToWolf = state.value("transform_to_wolf", !pose.transformFromWolf);
        pose.transformProcVar0 = state.value("transform_proc_v0", 0);
        pose.transformProcVar5 = state.value("transform_proc_v5", 0);
        pose.transformClothesWait = state.value("transform_clothes_wait", 0);
        pose.transformFrame = state.value("transform_frame", 0.0f);
        pose.transformProcVar2 = state.value("transform_proc_v2", 0);
        pose.transformProcVar3 = state.value("transform_proc_v3", 0);
        pose.transformShapeX = state.value("transform_shape_x", 0);
        pose.equipItem = static_cast<uint16_t>(state.value("equip_item", 0xFFFF));
        pose.swordVariant = state.value("sword_variant", REMOTE_SWORD_UNKNOWN);
        pose.shieldVariant = state.value("shield_variant", REMOTE_SHIELD_UNKNOWN);
        pose.clothesVariant = state.value("clothes_variant", REMOTE_CLOTHES_HERO);
        pose.swordDraw = state.value("sword_draw", false);
        pose.shieldDraw = state.value("shield_draw", false);
        pose.swordOut = state.value("sword_out", false);
        pose.itemDraw = state.value("item_draw", false);
        pose.kanteraDraw = state.value("kantera_draw", false);
        pose.itemActorKind = state.value("item_actor_kind", REMOTE_ITEM_ACTOR_NONE);
        pose.rideActorKind = state.value("ride_actor_kind", REMOTE_RIDE_ACTOR_NONE);
        pose.linkMatrices = parse_link_matrices(state);
        const bool wasTransforming =
            existing != sSession.peerPoses.end() && existing->second.valid &&
            existing->second.isTransforming;
        sSession.peerPoses[peerId] = pose;

        static uint32_t sTransformPoseRxLogCount = 0;
        if ((pose.isTransforming || wasTransforming) && sTransformPoseRxLogCount < 80)
        {
            ++sTransformPoseRxLogCount;
            DuskLog.info(
                "Multiplayer transform debug rx_pose peer={} seq={} transforming={} from_wolf={} "
                "to_wolf={} is_wolf={} proc_v0={} proc_v2={} proc_v3={} proc_v5={} "
                "shape_x={} clothes_wait={} frame={} "
                "matrices_valid={} age={} stage={} room={}",
                peerId, pose.sequence, pose.isTransforming, pose.transformFromWolf,
                pose.transformToWolf, pose.isWolf, pose.transformProcVar0,
                pose.transformProcVar2, pose.transformProcVar3, pose.transformProcVar5,
                pose.transformShapeX, pose.transformClothesWait, pose.transformFrame,
                pose.linkMatrices.valid, pose.ageTicks, pose.stage, pose.room);
        }

        if ((sSession.peerPoseLogTicks++ % 150) == 0) {
            DuskLog.info("Multiplayer peer pose peer={} form={} stage={} room={} pos=({}, {}, {})",
                         peerId, pose.isWolf ? "wolf" : "human", pose.stage, pose.room, pose.x,
                         pose.y, pose.z);
        }
    } else if (type == "event_bit") {
        const uint16_t flag = routedMessage.value("flag", 0U);
        sApplyingRemoteSaveBit = true;
        dComIfGs_onEventBit(flag);
        sApplyingRemoteSaveBit = false;
        DuskLog.info("Multiplayer applied remote event bit flag={}", flag);
    } else if (type == "tbox_bit") {
        const int stage = routedMessage.value("stage", -1);
        const int flag = routedMessage.value("flag", -1);
        if (stage >= 0 && flag >= 0) {
            sApplyingRemoteSaveBit = true;
            apply_remote_tbox_bit(stage, flag);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote chest bit stage={} flag={}", stage, flag);
        }
    } else if (type == "dungeon_item_bit") {
        const int stage = message.value("stage", -1);
        const int kind = message.value("kind", -1);
        if (stage >= 0 && kind >= 0) {
            sApplyingRemoteSaveBit = true;
            switch (kind) {
            case 0: dComIfGs_onDungeonItemMap(stage); break;
            case 1: dComIfGs_onDungeonItemCompass(stage); break;
            case 2: dComIfGs_onDungeonItemBossKey(stage); break;
            case 3: dComIfGs_onStageBossEnemy(stage); break;
            case 4: dComIfGs_onStageLife(stage); break;
            case 5: dComIfGs_onStageBossDemo(stage); break;
            case 6: dComIfGs_onDungeonItemWarp(stage); break;
            case 7: dComIfGs_onStageMiddleBoss(stage); break;
            default: break;
            }
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote dungeon item bit stage={} kind={}", stage, kind);
        }
    } else if (type == "key_num") {
        // OoT Anchor model: absolute overwrite (last-write-wins) of the
        // whole-dungeon key count, not a per-key bit/identity. No actor
        // lookup needed -- works even if the player is nowhere near the
        // dungeon this count belongs to.
        const int stage = message.value("stage", -1);
        const int count = message.value("count", -1);
        if (stage >= 0 && stage < dSv_save_c::STAGE_MAX && count >= 0 && count <= 99) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_setKeyNum(stage, static_cast<u8>(count));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote key num stage={} count={}", stage, count);
        }
    } else if (type == "light_drop_num") {
        // Same absolute-overwrite model as key_num. Light Drop counts are
        // stored per dark-twilight area, not per-stage, so no stage lookup
        // is needed at all.
        const int area = message.value("area", -1);
        const int count = message.value("count", -1);
        if (area >= 0 && area <= 0xFF && count >= 0 && count <= 0xFF) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_setLightDropNum(static_cast<u8>(area), static_cast<u8>(count));
            if (area == 2 && count == 15) {
                /* dSv_event_flag_c::F_0005 - Misc. - Gathered 14 Tears of Light in area 4 */
                dComIfGs_onEventBit(dSv_event_flag_c::saveBitLabels[9]);
            }
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote light drop num area={} count={}", area, count);
        }
    } else if (type == "max_life_update") {
        // Monotonic max-merge, not last-write-wins: max life should never
        // decrease from a remote update, so only raise it, never lower it.
        const int value = message.value("value", -1);
        if (value >= 0 && value <= 100 && value > dComIfGs_getMaxLife()) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_setMaxLife(static_cast<u8>(value));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote max life value={}", value);
        }
    } else if (type == "bottle_slots") {
        // Same monotonic max-merge as max_life. Fills additional empty
        // slots locally (always as a generic empty bottle) to match the
        // remote count -- never touches existing slot contents.
        const int count = message.value("count", -1);
        if (count >= 0 && count <= 4) {
            const int localCount = dComIfGs_getBottleSlotCount();
            if (count > localCount) {
                sApplyingRemoteSaveBit = true;
                for (int i = localCount; i < count; ++i) {
                    dComIfGs_setEmptyBottle();
                }
                sApplyingRemoteSaveBit = false;
                DuskLog.info("Multiplayer applied remote bottle slots count={}", count);
            }
        }
    } else if (type == "switch_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        const int sourceActor = message.value("source_actor", -1);
        if (stage >= 0 && flag >= 0) {
            if (!suppress_remote_switch_from_source_actor(stage, flag, sourceActor)) {
                apply_remote_switch_bit(stage, flag);
            }
        }
    } else if (type == "item_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        if (stage >= 0 && flag >= 0) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onStageMemoryItem(stage, flag);
            remember_memory_item_bit(stage, flag);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote item bit stage={} flag={}", stage, flag);

            stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
            if (stagInfo != nullptr && stage == dStage_stagInfo_GetSaveTbl(stagInfo)) {
                const int globalBit = flag + dSv_info_c::MEMORY_ITEM;
                const bool repairedLife = duskRepairObjLifeVisual(globalBit);
                const bool repairedSword = !repairedLife && duskRepairObjSwordVisual(globalBit);
                DuskLog.info(
                    "Multiplayer item bit repair stage={} flag={} found_life={} found_sword={}",
                    stage, flag, repairedLife, repairedSword);
            }
        }
    } else if (type == "item_get") {
        const int itemId = message.value("item_id", -1);
        // Guard against double-apply. Verified directly in d_item.cpp: every
        // capacity item_func (WALLET_LV1-3, BOMB_BAG_LV1, MAGIC_LV1,
        // ARROW_LV1-3) calls an absolute setter (setWalletSize/setArrowMax/
        // etc.), not a cumulative addX(), so replaying one does not double
        // the capacity value. The guard matters for a different reason: many
        // item_funcs (BOW, HOOKSHOT, BOOMERANG, etc.) also call
        // dComIfGs_setItem(SLOT_n, itemId) to occupy a C-button slot.
        // Without this guard, replaying an already-owned item's item_get
        // (e.g. from a resent snapshot) would silently reassign that slot
        // back to the original item, reverting any slot swap the local
        // player made since first picking it up.
        if (is_synced_key_item(itemId) && !dComIfGs_isItemFirstBit(static_cast<u8>(itemId))) {
            sApplyingRemoteSaveBit = true;
            execItemGet(static_cast<u8>(itemId));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote item get item_id={}", itemId);
        }
    } else if (type == "collect_crystal") {
        const int item = message.value("item", -1);
        if (item >= 0 && item < 8) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onCollectCrystal(static_cast<u8>(item));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote crystal collect item={}", item);
        }
    } else if (type == "collect_mirror") {
        const int item = message.value("item", -1);
        if (item >= 0 && item < 8) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onCollectMirror(static_cast<u8>(item));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote mirror collect item={}", item);
        }
    } else if (type == "dark_clear_lv") {
        const int no = message.value("no", -1);
        // Gated by DUSK_MP_LAYER_SYNC: this value also selects room layers
        // in some stages (see sLayerRiskSyncEnabled's declaration comment).
        // A peer with the flag off must not apply it even if a flag-on peer
        // sends it.
        if (sLayerRiskSyncEnabled && no >= 0 && no < 8) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onDarkClearLV(no);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote dark clear lv no={}", no);
        }
    } else if (type == "transform_lv") {
        const int no = message.value("no", -1);
        if (sLayerRiskSyncEnabled && no >= 0 && no < 8) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onTransformLV(no);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote transform lv no={}", no);
        }
    } else if (type == "region_bit") {
        const int region = message.value("region", -1);
        if (region >= 0 && region < 8) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onRegionBit(region);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote region bit region={}", region);
        }
    } else if (type == "collect") {
        const int collectType = message.value("collect_type", -1);
        const int item = message.value("item", -1);
        if (collectType >= 0 && collectType <= B_BUTTON_ITEM && item >= 0 && item < 8) {
            sApplyingRemoteSaveBit = true;
            g_dComIfG_gameInfo.info.getPlayer().getCollect().setCollect(collectType,
                                                                         static_cast<u8>(item));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote collect type={} item={}", collectType, item);
        }
    } else if (type == "visited_room") {
        const int stage = message.value("stage", -1);
        const int room = message.value("room", -1);
        // Bounds must match dSv_save_c::STAGE2_MAX and dSv_memory2_c's 64-bit
        // mVisitedRoom field exactly: getSave2()/onVisitedRoom() only guard
        // with JUT_ASSERT, which compiles to a no-op in release builds, so an
        // out-of-range value from the wire would otherwise index mSave2[]/
        // mVisitedRoom[] out of bounds unchecked.
        if (stage >= 0 && stage < dSv_save_c::STAGE2_MAX && room >= 0 && room < 64) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onSaveVisitedRoom(stage, room);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote visited room stage={} room={}", stage, room);
        }
    } else if (type == "letter_get") {
        const int no = message.value("no", -1);
        // Bounds must match dSv_letter_info_c::LETTER_INFO_BIT exactly --
        // onLetterGetFlag only guards with JUT_ASSERT (no-op in release), so
        // an out-of-range value from the wire would otherwise index
        // mLetterGetFlags[] out of bounds unchecked.
        if (no >= 0 && no < LETTER_INFO_BIT) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onLetterGetFlag(no);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote letter get no={}", no);
        }
    } else if (type == "ping") {
        if (sender != nullptr) {
            send_json_to_peer(*sender, {{"type", "pong"}});
        } else {
            send_json({{"type", "pong"}});
        }
    } else if (type == "pong") {
        DuskLog.debug("Multiplayer pong");
    } else if (type == "error") {
        DuskLog.warn("Multiplayer remote error: {}", message.value("error", ""));
    } else {
        DuskLog.debug("Multiplayer message type={}", type);
    }

    if (sender != nullptr && should_forward_peer_message(type)) {
        broadcast_to_direct_peers(routedMessage, sender->id);
    }
}

bool pump_receive_from_socket(socket_t sock, std::string& rxBuffer, DirectPeer* sender = nullptr) {
    std::array<char, 4096> buffer{};

    while (true) {
        const int read = recv(sock, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (read > 0) {
            rxBuffer.append(buffer.data(), static_cast<size_t>(read));

            size_t newline = std::string::npos;
            while ((newline = rxBuffer.find('\n')) != std::string::npos) {
                const std::string line = rxBuffer.substr(0, newline);
                rxBuffer.erase(0, newline + 1);
                if (line.empty()) {
                    continue;
                }

                try {
                    handle_message(json::parse(line), sender);
                } catch (const json::exception& e) {
                    DuskLog.warn("Multiplayer received invalid JSON: {}", e.what());
                }
            }
            continue;
        }

        if (read == 0) {
            return false;
        }

        if (would_block()) {
            return true;
        }

        return false;
    }
}

void pump_receive() {
    if (!pump_receive_from_socket(sSession.sock, sSession.rxBuffer)) {
        disconnect("remote closed");
    }
}

void pump_direct_peer_receives() {
    std::vector<std::string> disconnectedPeers;
    for (auto& entry : sSession.directPeers) {
        DirectPeer& peer = entry.second;
        if (!pump_receive_from_socket(peer.sock, peer.rxBuffer, &peer)) {
            disconnectedPeers.push_back(peer.id);
        }
    }

    for (const std::string& peerId : disconnectedPeers) {
        remove_direct_peer(peerId, "remote closed");
    }
}

bool fill_ipv4(sockaddr_in& addr, const std::string& host, int port) {
    addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1;
}

void begin_connect() {
    close_socket(sSession.sock);
    sSession.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sSession.sock == INVALID_SOCKET) {
        return;
    }

    if (!set_nonblocking(sSession.sock)) {
        disconnect("nonblocking failed");
        return;
    }

    sockaddr_in addr{};
    if (!fill_ipv4(addr, sSession.host, sSession.port)) {
        disconnect("invalid host");
        return;
    }

    const int result = connect(sSession.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == 0) {
        sSession.state = ConnectionState::Connected;
        DuskLog.info("Multiplayer connected to {}:{}", sSession.host, sSession.port);
        send_hello();
        return;
    }

    if (!would_block()) {
        disconnect("connect failed");
        return;
    }

    sSession.state = ConnectionState::Connecting;
}

void begin_host() {
    close_socket(sSession.listenSock);
    sSession.listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sSession.listenSock == INVALID_SOCKET) {
        return;
    }

    int reuse = 1;
    setsockopt(sSession.listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    if (!set_nonblocking(sSession.listenSock)) {
        disconnect("listen nonblocking failed");
        return;
    }

    sockaddr_in addr{};
    if (!fill_ipv4(addr, sSession.bindHost, sSession.port)) {
        disconnect("invalid bind host");
        return;
    }

    if (bind(sSession.listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(sSession.listenSock, SOMAXCONN) != 0) {
        disconnect("listen failed");
        return;
    }

    sSession.state = ConnectionState::Listening;
    DuskLog.info("Multiplayer direct host listening on {}:{}", sSession.bindHost, sSession.port);
    DuskLog.info("Multiplayer invite code: {}", sSession.inviteCode);
}

void update_connecting() {
    fd_set writefds;
    fd_set errorfds;
    FD_ZERO(&writefds);
    FD_ZERO(&errorfds);
    FD_SET(sSession.sock, &writefds);
    FD_SET(sSession.sock, &errorfds);
    timeval timeout{0, 0};
#if _WIN32
    const int result = select(0, nullptr, &writefds, &errorfds, &timeout);
#else
    const int result = select(sSession.sock + 1, nullptr, &writefds, &errorfds, &timeout);
#endif
    if (result < 0 || FD_ISSET(sSession.sock, &errorfds) || socket_error(sSession.sock) != 0) {
        disconnect("connect poll failed");
        return;
    }

    if (!FD_ISSET(sSession.sock, &writefds)) {
        return;
    }

    sSession.state = ConnectionState::Connected;
    DuskLog.info("Multiplayer connected to {}:{}", sSession.host, sSession.port);
    send_hello();
}

void update_listening() {
    if (sSession.listenSock == INVALID_SOCKET) {
        return;
    }

    sockaddr_in peerAddr{};
#if _WIN32
    int peerLen = sizeof(peerAddr);
#else
    socklen_t peerLen = sizeof(peerAddr);
#endif
    socket_t accepted = accept(sSession.listenSock, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen);
    if (accepted == INVALID_SOCKET) {
        if (!would_block()) {
            disconnect("accept failed");
        }
        return;
    }

    if (!set_nonblocking(accepted)) {
        close_socket(accepted);
        disconnect("accepted nonblocking failed");
        return;
    }
    if (sSession.directPeers.size() >= kMaxDirectPeers) {
        close_socket(accepted);
        DuskLog.warn("Multiplayer direct peer refused: room is full current_peers={} max_peers={}",
                     sSession.directPeers.size(), kMaxDirectPeers);
        return;
    }

    DirectPeer peer;
    peer.sock = accepted;
    peer.id = "direct" + std::to_string(sSession.nextDirectPeerId++);
    const std::string peerId = peer.id;
    sSession.directPeers.emplace(peerId, std::move(peer));
    sSession.state = ConnectionState::Connected;
    DuskLog.info("Multiplayer direct peer connected id={}", peerId);
}

void send_pose() {
    if (!sSession.welcomed) {
        return;
    }

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    // checkWolf() lives on daPy_py_c (daAlink_c's base), but reads a plain
    // flag rather than touching model pointers, so unlike add_link_matrices()
    // this doesn't need the metamorphosis-state guard -- just the same
    // actor-type check before the cast.
    const bool isLink = fopAcM_GetName(player) == fpcNm_ALINK_e;
    daAlink_c* link = isLink ? static_cast<daAlink_c*>(player) : nullptr;
    const bool isWolf = link != nullptr && link->checkWolf();
    const bool isTransforming =
        link != nullptr && (link->mProcID == daAlink_c::PROC_METAMORPHOSE ||
                            link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY);
    static bool sLocalTransformObserved = false;
    static bool sLocalTransformFromWolf = false;
    static bool sLocalTransformToWolf = false;
    if (isTransforming && !sLocalTransformObserved) {
        sLocalTransformObserved = true;
        sLocalTransformFromWolf = isWolf;
        sLocalTransformToWolf = !isWolf;
    } else if (!isTransforming) {
        sLocalTransformObserved = false;
    }
    const bool transformFromWolf = isTransforming && sLocalTransformFromWolf;
    const bool transformToWolf = isTransforming && sLocalTransformToWolf;
    static bool sWasSendingTransform = false;
    static uint32_t sTransformPoseTxLogCount = 0;
    if ((isTransforming || sWasSendingTransform) && sTransformPoseTxLogCount < 80) {
        ++sTransformPoseTxLogCount;
        DuskLog.info(
            "Multiplayer transform debug tx_pose seq_next={} transforming={} from_wolf={} "
            "to_wolf={} is_wolf={} proc={} proc_v0={} proc_v1={} proc_v2={} proc_v3={} "
            "proc_v5={} shape_x={} pos=({}, {}, {})",
            sSession.poseSequence + 1, isTransforming, transformFromWolf, transformToWolf,
            isWolf, link != nullptr ? link->mProcID : 0,
            link != nullptr ? link->mProcVar0.field_0x3008 : 0,
            link != nullptr ? link->mProcVar1.field_0x300a : 0,
            link != nullptr ? link->mProcVar2.field_0x300c : 0,
            link != nullptr ? link->mProcVar3.field_0x300e : 0,
            link != nullptr ? link->mProcVar5.field_0x3012 : 0,
            link != nullptr ? static_cast<int>(link->shape_angle.x) : 0,
            player->current.pos.x, player->current.pos.y, player->current.pos.z);
    }
    sWasSendingTransform = isTransforming;

    json state = {
        {"stage", dComIfGp_getStartStageName()},
        {"room", static_cast<int>(fopAcM_GetRoomNo(player))},
        {"layer", static_cast<int>(dComIfGp_getStartStageLayer())},
        {"x", player->current.pos.x},
        {"y", player->current.pos.y},
        {"z", player->current.pos.z},
        {"angle_y", static_cast<int>(player->shape_angle.y)},
        {"proc_id", link != nullptr ? static_cast<int>(link->mProcID) : 0},
        {"proc_v0", link != nullptr ? link->mProcVar0.field_0x3008 : 0},
        {"proc_v1", link != nullptr ? link->mProcVar1.field_0x300a : 0},
        {"proc_v2", link != nullptr ? link->mProcVar2.field_0x300c : 0},
        {"proc_v3", link != nullptr ? link->mProcVar3.field_0x300e : 0},
        {"proc_v5", link != nullptr ? link->mProcVar5.field_0x3012 : 0},
        {"under_frame", link != nullptr ? link->mUnderFrameCtrl[0].getFrame() : 0.0f},
        {"under_bck0", link != nullptr ? static_cast<int>(link->mUnderAnmHeap[0].getIdx()) : 0},
        {"under_frame0", link != nullptr ? link->mUnderFrameCtrl[0].getFrame() : 0.0f},
        {"under_rate0", link != nullptr ? link->mUnderFrameCtrl[0].getRate() : 1.0f},
        {"upper_bck2", link != nullptr ? static_cast<int>(link->mUpperAnmHeap[2].getIdx()) : 0},
        {"upper_frame2", link != nullptr ? link->mUpperFrameCtrl[2].getFrame() : 0.0f},
        {"upper_rate2", link != nullptr ? link->mUpperFrameCtrl[2].getRate() : 1.0f},
        {"is_wolf", isWolf},
        {"is_transforming", isTransforming},
        {"transform_from_wolf", transformFromWolf},
        {"transform_to_wolf", transformToWolf},
        {"transform_proc_v0", link != nullptr ? link->mProcVar0.field_0x3008 : 0},
        {"transform_proc_v5", link != nullptr ? link->mProcVar5.field_0x3012 : 0},
        {"transform_clothes_wait", link != nullptr ? static_cast<int>(link->mClothesChangeWaitTimer) : 0},
        {"transform_frame", link != nullptr ? link->mUnderFrameCtrl[0].getFrame() : 0.0f},
        {"transform_proc_v2", link != nullptr ? link->mProcVar2.field_0x300c : 0},
        {"transform_proc_v3", link != nullptr ? link->mProcVar3.field_0x300e : 0},
        {"transform_shape_x", link != nullptr ? static_cast<int>(link->shape_angle.x) : 0},
        {"equip_item", link != nullptr ? static_cast<int>(link->mEquipItem) : 0xFFFF},
        {"sword_variant", detect_sword_variant(link)},
        {"shield_variant", detect_shield_variant()},
        {"clothes_variant", detect_clothes_variant()},
        {"sword_draw", link != nullptr && !isWolf && static_cast<bool>(link->checkSwordDraw())},
        {"shield_draw", link != nullptr && !isWolf && static_cast<bool>(link->checkShieldDraw())},
        {"sword_out", link != nullptr && !isWolf && link->mEquipItem == 0x103},
        {"item_draw", link != nullptr && !isWolf && static_cast<bool>(link->checkItemDraw())},
        {"kantera_draw",
         link != nullptr && !isWolf &&
             (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) ||
              link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_20000))},
        {"item_actor_kind", REMOTE_ITEM_ACTOR_NONE},
        {"ride_actor_kind", REMOTE_RIDE_ACTOR_NONE},
    };
    if (!isTransforming && isLink && (sDummyModelEnabled || isWolf) && !add_link_matrices(state)) {
        static uint32_t sMatrixPoseDropLogCount = 0;
        if (sMatrixPoseDropLogCount < 20) {
            ++sMatrixPoseDropLogCount;
            DuskLog.info(
                "Multiplayer transform debug tx_pose_dropped matrix_capture_failed seq_next={} "
                "is_wolf={} proc={}",
                sSession.poseSequence + 1, isWolf, link != nullptr ? link->mProcID : 0);
        }
        return;
    }

    send_json({
        {"type", "pose"},
        {"sequence", ++sSession.poseSequence},
        {"state", state},
    });
}

// Applies a remote chest-bit-space transition (stage/flag, the same slot
// space chests/keys/Light Drop tears all share via dComIfGs_isTbox/onTbox)
// and, if this is a genuine unset->set transition for the CURRENT stage,
// also repairs whichever local state is stale: a visible chest's open
// visual, or -- for small keys/Light Drop tears -- the derived counter
// (mKeyNum / light-drop count) that a remote pickup's bit alone does not
// update. The bit itself was already correctly OR-merging before this
// function existed; only the derived side effects were missing. Checking
// "was it already set" first makes this naturally safe to call repeatedly
// (live message, snapshot catch-up, or a resend of either) without double-
// counting: a bit that's already true locally (including one this same
// client set moments ago via its own pickup) is treated as a no-op.
void apply_remote_tbox_bit(int stage, int flag) {
    const bool wasUnset = !dComIfGs_isStageTbox(stage, flag);
    dComIfGs_onStageTbox(stage, flag);

    if (!wasUnset) {
        return;
    }

    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr || stage != dStage_stagInfo_GetSaveTbl(stagInfo)) {
        return;
    }

    duskRepairTboxVisual(flag);
    // Visual-only: deletes a currently-spawned key/tear so it doesn't sit
    // there a moment longer than necessary. Does NOT touch mKeyNum/
    // LightDropNum -- that's handled independently by the key_num/
    // light_drop_num absolute-broadcast messages (see notify_local_key_num_
    // set/notify_local_light_drop_num_set), which work without needing a
    // live actor at all. Calling both would double-count.
    if (!duskRepairKeyVisual(flag)) {
        duskRepairLightDropVisual(flag);
    }
}

// Safety net independent of exactly how/when a chest/item bit got applied
// (live single-bit message, late-join snapshot, or replayed from
// sPendingStageMessages): periodically re-checks every already-collected
// memory-tier chest/item bit in the CURRENT stage against whatever's
// actually spawned right now, and repairs any mismatch. This does not
// depend on catching the exact moment a bit arrives, so it should mask any
// ordering/timing bug in the targeted repair calls elsewhere -- if a
// collected pickup is still visible, it will self-correct within one sweep
// interval of the player being in that room, even if we can't identify why
// the targeted repair didn't fire.
void repair_current_stage_collectibles() {
    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr) {
        return;
    }

    const int stage = dStage_stagInfo_GetSaveTbl(stagInfo);
    reapply_observed_memory_items_for_stage(stage);

    for (int i = 0; i < 64; ++i) {
        if (dComIfGs_isStageTbox(stage, i)) {
            duskRepairTboxVisual(i);
        }
    }

    for (int i = 0; i < dSv_info_c::DAN_ITEM; ++i) {
        if (dComIfGs_isStageMemoryItem(stage, i)) {
            const int globalBit = i + dSv_info_c::MEMORY_ITEM;
            if (!duskRepairObjLifeVisual(globalBit)) {
                duskRepairObjSwordVisual(globalBit);
            }
        }
    }
}

void update_connected() {
    update_remote_switch_policy_room_state();
    age_recent_remote_switches();

    if (sSession.mode == NetworkMode::DirectHost) {
        update_listening();
    }

    if (sSession.mode == NetworkMode::RelayHarness || sSession.mode == NetworkMode::DirectJoin) {
        send_hello();
    }

    if (sSession.mode == NetworkMode::DirectHost) {
        pump_direct_peer_receives();
    } else {
        pump_receive();
    }

    if (sSession.state != ConnectionState::Connected) {
        return;
    }

    if (dComIfGp_getStageStagInfo() != nullptr) {
        if (sSession.snapshotPending) {
            // The network handshake can complete within milliseconds, well
            // before the game has finished booting to a loaded stage (still
            // on the boot logo/title). Every per-stage save accessor used by
            // send_save_snapshot() dereferences dComIfGp_getStageStagInfo()
            // without a null check, so wait for a real stage before sending.
            send_save_snapshot();
            sSession.snapshotPending = false;
        }
        for (auto& entry : sSession.directPeers) {
            DirectPeer& peer = entry.second;
            if (peer.snapshotPending) {
                send_save_snapshot(&peer);
                peer.snapshotPending = false;
            }
        }

        if (!sPendingStageMessages.empty()) {
            // Same problem on the receive side: a peer's save_snapshot (or
            // even a live tbox_bit/etc.) can arrive before we have a stage
            // loaded ourselves. handle_message() queued those; replay them
            // now that dComIfGp_getStageStagInfo() is safe to use.
            std::vector<json> pending = std::move(sPendingStageMessages);
            sPendingStageMessages.clear();
            for (const json& queued : pending) {
                handle_message(queued);
            }
        }

        update_remote_switch_policy_room_state();
        flush_deferred_remote_switches();

        if (++sSession.repairSweepTicks >= 60) {
            sSession.repairSweepTicks = 0;
            repair_current_stage_collectibles();
        }
    }

    send_pose();

    if (++sSession.pingTicks >= 30) {
        sSession.pingTicks = 0;
        send_json({{"type", "ping"}});
    }
}

NetworkMode parse_mode() {
    const std::string mode = env_string("DUSK_MP_MODE", env_string("DUSK_MP_CODE", "").empty() ? "relay" : "join");
    if (mode == "host") return NetworkMode::DirectHost;
    if (mode == "join") return NetworkMode::DirectJoin;
    if (mode == "relay") return NetworkMode::RelayHarness;
    return NetworkMode::Disabled;
}

bool configure_session() {
    sSession.mode = parse_mode();
    sSession.name = env_string("DUSK_MP_NAME", "TP Player");
    sSession.room = env_string("DUSK_MP_ROOM", "dev");
    sSession.relayPassword = env_string("DUSK_MP_PASSWORD", "");
    sSession.port = env_int("DUSK_MP_PORT", 34197);
    sSession.debugMarker = env_enabled("DUSK_MP_DEBUG_MARKER");
    sDummyModelEnabled = env_enabled("DUSK_MP_DUMMY_MODEL");
    sNameLabelsEnabled = !env_enabled("DUSK_MP_HIDE_NAME_LABELS");
    sLayerRiskSyncEnabled = env_enabled("DUSK_MP_LAYER_SYNC");
    sSession.sessionId = make_session_token(9);
    sSession.sessionKey = make_session_token(16);

    if (sSession.mode == NetworkMode::DirectJoin) {
        const std::string code = env_string("DUSK_MP_CODE", "");
        std::string error;
        const std::optional<InviteCodePayload> payload = decode_invite_code(code, &error);
        if (!payload) {
            DuskLog.warn("Multiplayer join disabled: invalid invite code ({})", error);
            return false;
        }

        sSession.host = payload->host;
        sSession.port = payload->port;
        sSession.room = payload->room;
        sSession.sessionId = payload->sessionId;
        sSession.sessionKey = payload->sessionKey;
        return true;
    }

    if (sSession.mode == NetworkMode::DirectHost) {
        sSession.bindHost = env_string("DUSK_MP_BIND", "0.0.0.0");
        sSession.publicHost = env_string("DUSK_MP_HOST_PUBLIC", "127.0.0.1");
        InviteCodePayload payload;
        payload.transport = "direct";
        payload.host = sSession.publicHost;
        payload.port = sSession.port;
        payload.room = sSession.room;
        payload.sessionId = sSession.sessionId;
        payload.sessionKey = sSession.sessionKey;
        sSession.inviteCode = create_invite_code(payload);
        return true;
    }

    if (sSession.mode == NetworkMode::RelayHarness) {
        sSession.host = env_string("DUSK_MP_HOST", "127.0.0.1");
        sSession.room = env_string("DUSK_MP_ROOM", "dev");
        sSession.relayPassword = env_string("DUSK_MP_PASSWORD", "");
        return true;
    }

    DuskLog.warn("Multiplayer disabled: unknown DUSK_MP_MODE");
    return false;
}

const char* mode_name(NetworkMode mode) {
    switch (mode) {
    case NetworkMode::RelayHarness: return "relay";
    case NetworkMode::DirectHost: return "host";
    case NetworkMode::DirectJoin: return "join";
    default: return "disabled";
    }
}

const char* state_name(ConnectionState state) {
    switch (state) {
    case ConnectionState::Listening: return "listening";
    case ConnectionState::Connecting: return "connecting";
    case ConnectionState::Connected: return "connected";
    default: return "disconnected";
    }
}

}  // namespace

void initialize() {
    if (sInitialized) {
        return;
    }

    sInitialized = true;
    sEnabled = env_enabled("DUSK_MP") || env_enabled("DUSK_MULTIPLAYER");
    if (!sEnabled) {
        DuskLog.info("Multiplayer module initialized disabled");
        return;
    }

    if (!configure_session()) {
        sEnabled = false;
        return;
    }

    if (!ensure_network_stack()) {
        sEnabled = false;
        return;
    }

    DuskLog.info("Multiplayer module enabled mode={} room={}", mode_name(sSession.mode),
                 sSession.room);
}

void update() {
    if (!sInitialized || !sEnabled) {
        return;
    }

    const bool dummyGameplayReady = is_peer_dummy_gameplay_ready();
    if (sDummyModelEnabled && dummyGameplayReady) {
        // Must happen here (the simulation tick), not from the draw phase --
        // see the comment on preload_remote_link_dummy_resources()'s
        // declaration for why issuing these archive loads from the draw
        // phase was a suspected cause of a real crash during a peer's
        // transform.
        //
        // is_peer_dummy_gameplay_ready() is required here too: without it,
        // this fired from the very first update() tick after enabling the
        // module -- including at the boot logo, before any stage/player
        // exists -- and crashed inside the engine's own resource/texture
        // loader (dRes_info_c::loadResource -> addWarpMaterial ->
        // J3DTexture::addResTIMG, null deref) because the object-archive
        // loading path isn't safe to use that early. The old lazy-load (from
        // inside draw_remote_link_dummy(), only reachable once this same
        // readiness check already passed) never hit this because it could
        // only ever run once real gameplay had started.
        preload_remote_link_dummy_resources();
    }

    for (auto& entry : sSession.peerPoses) {
        if (entry.second.valid) {
            ++entry.second.ageTicks;
        }
    }

    if (sDummyModelEnabled && dummyGameplayReady) {
        sync_remote_link_actor_dummies(sSession.peerPoses);
    } else if (!sDummyModelEnabled) {
        destroy_all_remote_link_dummies();
    }

    if (sSession.state == ConnectionState::Disconnected) {
        if ((sSession.reconnectTicks++ % 30) == 0) {
            if (sSession.mode == NetworkMode::DirectHost) {
                begin_host();
            } else {
                begin_connect();
            }
        }
    } else if (sSession.state == ConnectionState::Listening) {
        update_listening();
    } else if (sSession.state == ConnectionState::Connecting) {
        update_connecting();
    } else {
        update_connected();
    }

    // Remote visuals are drawn from the frame hook after Link has calculated
    // his matrices for the current frame.
}

void shutdown() {
    if (!sInitialized) {
        return;
    }

    disconnect("shutdown");
    destroy_all_remote_link_dummies();
#if _WIN32
    if (sNetworkStackStarted) {
        WSACleanup();
        sNetworkStackStarted = false;
    }
#endif
    sInitialized = false;
    sEnabled = false;
    DuskLog.info("Multiplayer module shut down");
}

bool is_enabled() {
    return sEnabled;
}

bool was_switch_recently_remote_set(int stage, int flag, uint32_t* ageTicks) {
    for (const RecentRemoteSwitch& recent : sRecentRemoteSwitches) {
        if (recent.stage == stage && recent.flag == flag) {
            if (ageTicks != nullptr) {
                *ageTicks = recent.ageTicks;
            }
            return true;
        }
    }

    return false;
}

bool host_direct(const DirectHostOptions& options, std::string* errorOut) {
    if (options.port <= 0 || options.port > 65535) {
        if (errorOut != nullptr) {
            *errorOut = "Port must be between 1 and 65535";
        }
        return false;
    }
    if (options.bindHost.empty() || options.publicHost.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "Bind and public host cannot be empty";
        }
        return false;
    }

    sInitialized = true;
    if (!ensure_network_stack(errorOut)) {
        sEnabled = false;
        return false;
    }

    reset_connection_state();
    sEnabled = true;
    sSession.mode = NetworkMode::DirectHost;
    sSession.name = options.name.empty() ? "Host" : options.name;
    sSession.room = options.room.empty() ? "dev" : options.room;
    sSession.bindHost = options.bindHost;
    sSession.publicHost = options.publicHost;
    sSession.port = options.port;
    sSession.debugMarker = options.debugMarker;
    sDummyModelEnabled = options.dummyModel;
    sNameLabelsEnabled = options.nameLabels;
    sSession.sessionId = make_session_token(9);
    sSession.sessionKey = make_session_token(16);

    InviteCodePayload payload;
    payload.transport = "direct";
    payload.host = sSession.publicHost;
    payload.port = sSession.port;
    payload.room = sSession.room;
    payload.sessionId = sSession.sessionId;
    payload.sessionKey = sSession.sessionKey;
    sSession.inviteCode = create_invite_code(payload);

    DuskLog.info("Multiplayer module enabled mode={} room={}", mode_name(sSession.mode),
                 sSession.room);
    begin_host();
    if (sSession.state != ConnectionState::Listening) {
        sEnabled = false;
        sSession.mode = NetworkMode::Disabled;
        if (errorOut != nullptr) {
            *errorOut = "Failed to start listening";
        }
        return false;
    }
    return true;
}

bool join_direct(const DirectJoinOptions& options, std::string* errorOut) {
    if (options.inviteCode.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "Invite code cannot be empty";
        }
        return false;
    }

    std::string decodeError;
    const std::optional<InviteCodePayload> payload = decode_invite_code(options.inviteCode, &decodeError);
    if (!payload) {
        if (errorOut != nullptr) {
            *errorOut = "Invalid invite code: " + decodeError;
        }
        return false;
    }

    sInitialized = true;
    if (!ensure_network_stack(errorOut)) {
        sEnabled = false;
        return false;
    }

    reset_connection_state();
    sEnabled = true;
    sSession.mode = NetworkMode::DirectJoin;
    sSession.name = options.name.empty() ? "Joiner" : options.name;
    sSession.host = payload->host;
    sSession.port = payload->port;
    sSession.room = payload->room;
    sSession.sessionId = payload->sessionId;
    sSession.sessionKey = payload->sessionKey;
    sSession.inviteCode = options.inviteCode;
    sSession.debugMarker = options.debugMarker;
    sDummyModelEnabled = options.dummyModel;
    sNameLabelsEnabled = options.nameLabels;

    DuskLog.info("Multiplayer module enabled mode={} room={}", mode_name(sSession.mode),
                 sSession.room);
    begin_connect();
    if (sSession.state == ConnectionState::Disconnected) {
        sEnabled = false;
        sSession.mode = NetworkMode::Disabled;
        if (errorOut != nullptr) {
            *errorOut = "Failed to begin connection";
        }
        return false;
    }
    return true;
}

bool join_relay(const RelayJoinOptions& options, std::string* errorOut) {
    if (options.port <= 0 || options.port > 65535) {
        if (errorOut != nullptr) {
            *errorOut = "Port must be between 1 and 65535";
        }
        return false;
    }
    if (options.host.empty() || options.room.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "Relay host and lobby name cannot be empty";
        }
        return false;
    }

    sInitialized = true;
    if (!ensure_network_stack(errorOut)) {
        sEnabled = false;
        return false;
    }

    reset_connection_state();
    sEnabled = true;
    sSession.mode = NetworkMode::RelayHarness;
    sSession.name = options.name.empty() ? "Player" : options.name;
    sSession.host = options.host;
    sSession.port = options.port;
    sSession.room = options.room;
    sSession.relayPassword = options.password;
    sSession.debugMarker = options.debugMarker;
    sDummyModelEnabled = options.dummyModel;
    sNameLabelsEnabled = options.nameLabels;

    DuskLog.info("Multiplayer module enabled mode={} room={}", mode_name(sSession.mode),
                 sSession.room);
    begin_connect();
    if (sSession.state == ConnectionState::Disconnected) {
        sEnabled = false;
        sSession.mode = NetworkMode::Disabled;
        if (errorOut != nullptr) {
            *errorOut = "Failed to begin relay connection";
        }
        return false;
    }
    return true;
}

void disconnect_session() {
    disconnect("user requested");
    sEnabled = false;
    sSession.mode = NetworkMode::Disabled;
}

SessionStatus get_session_status() {
    SessionStatus status;
    status.enabled = sEnabled;
    status.mode = sEnabled ? mode_name(sSession.mode) : "disabled";
    status.state = sEnabled ? state_name(sSession.state) : "disabled";
    status.name = sSession.name;
    status.room = sSession.room;
    status.host = sSession.host;
    status.bindHost = sSession.bindHost;
    status.publicHost = sSession.publicHost;
    status.inviteCode = sSession.inviteCode;
    status.port = sSession.port;
    status.debugMarker = sSession.debugMarker;
    status.dummyModel = sDummyModelEnabled;
    status.nameLabels = sNameLabelsEnabled;
    status.nameLabelsHostControlled = sSession.mode == NetworkMode::DirectJoin;
    status.hasRecentPeerPose = has_recent_peer_pose(90);
    return status;
}

std::vector<PlayerListEntry> get_player_list() {
    std::vector<PlayerListEntry> players;
    if (!sEnabled) {
        return players;
    }

    PlayerListEntry local;
    local.id = "local";
    local.name = sSession.name.empty() ? "You" : sSession.name;
    local.area = display_area_for_stage(current_stage_name(), dComIfGp_roomControl_getStayNo());
    local.local = true;
    local.recentPose = sSession.state == ConnectionState::Connected ||
                       sSession.state == ConnectionState::Listening;
    local.status = sSession.mode == NetworkMode::DirectHost &&
                       sSession.state == ConnectionState::Listening ?
                       "hosting" : state_name(sSession.state);
    players.push_back(std::move(local));

    auto add_or_update_peer = [&](const std::string& peerId, std::string name, std::string status,
                                  std::string area, uint32_t ageTicks, bool recentPose) {
        if (peerId.empty()) {
            return;
        }
        auto existing = std::find_if(players.begin(), players.end(), [&](const PlayerListEntry& entry) {
            return entry.id == peerId;
        });
        if (existing == players.end()) {
            players.push_back(PlayerListEntry{
                .id = peerId,
                .name = name.empty() ? display_name_for_peer(peerId) : std::move(name),
                .status = std::move(status),
                .area = area.empty() ? "Unknown" : std::move(area),
                .ageTicks = ageTicks,
                .local = false,
                .recentPose = recentPose,
            });
            return;
        }

        if (!name.empty()) {
            existing->name = std::move(name);
        }
        existing->status = std::move(status);
        if (!area.empty()) {
            existing->area = std::move(area);
        }
        existing->ageTicks = ageTicks;
        existing->recentPose = recentPose;
    };

    for (const auto& entry : sPeerNames) {
        add_or_update_peer(entry.first, entry.second, "joined", "Unknown", 0, false);
    }

    if (sSession.mode == NetworkMode::DirectHost) {
        for (const auto& entry : sSession.directPeers) {
            const DirectPeer& peer = entry.second;
            add_or_update_peer(peer.id, display_name_for_peer(peer.id),
                               peer.welcomed ? "connected" : "connecting", "Unknown", 0,
                               peer.welcomed);
        }
    }

    for (const auto& entry : sSession.peerPoses) {
        const PeerPoseSnapshot& pose = entry.second;
        if (!pose.valid) {
            add_or_update_peer(entry.first, display_name_for_peer(entry.first), "waiting", "Unknown",
                               0, false);
            continue;
        }

        const bool recent = pose.ageTicks <= 90;
        add_or_update_peer(entry.first, display_name_for_peer(entry.first),
                           recent ? "online" : "stale",
                           display_area_for_stage(pose.stage, pose.room), pose.ageTicks, recent);
    }

    return players;
}

void set_name_labels_enabled(bool enabled) {
    sNameLabelsEnabled = enabled;
    if (sEnabled && sSession.mode == NetworkMode::DirectHost) {
        send_json({
            {"type", "name_labels"},
            {"enabled", sNameLabelsEnabled},
        });
    }
}

bool has_recent_peer_pose(uint32_t maxAgeTicks) {
    for (const auto& entry : sSession.peerPoses) {
        if (entry.second.valid && entry.second.ageTicks <= maxAgeTicks) {
            return true;
        }
    }
    return false;
}

PeerPoseSnapshot get_latest_peer_pose() {
    // No current callers (kept for API compatibility / future debug
    // overlays) -- with more than one peer, "latest" is ambiguous; this
    // just returns an arbitrary one. Use sSession.peerPoses directly (via
    // draw_debug_peer_marker()'s loop pattern) for anything that needs to
    // see every peer.
    if (sSession.peerPoses.empty()) {
        return PeerPoseSnapshot{};
    }
    return sSession.peerPoses.begin()->second;
}

void draw_debug_peer_marker() {
    static uint32_t sTransformDrawGateLogCount = 0;
    const bool hasTransformPose = has_transforming_peer_pose();
    auto log_transform_draw_gate = [&](const char* reason) {
        if (hasTransformPose && sTransformDrawGateLogCount < 40) {
            ++sTransformDrawGateLogCount;
            DuskLog.info("Multiplayer transform debug draw_gate_skip reason={}", reason);
        }
    };

    if (!sEnabled) {
        log_transform_draw_gate("disabled");
        return;
    }
    if (!sDummyModelEnabled) {
        log_transform_draw_gate("dummy_model_disabled");
        return;
    }
    if (!sSession.debugMarker) {
        log_transform_draw_gate("debug_marker_disabled");
        return;
    }
    if (!has_recent_peer_pose(30)) {
        log_transform_draw_gate("no_recent_peer_pose");
        return;
    }
    if (!is_peer_dummy_gameplay_ready()) {
        log_transform_draw_gate("gameplay_not_ready");
        return;
    }

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    const char* localStage = dComIfGp_getStartStageName();

    // Draw every fresh peer pose for the local stage. Room display filtering
    // happens inside draw_remote_link_dummy(), where it can use
    // dComIfGp_roomControl_checkRoomDisp() instead of requiring exact room
    // equality; open fields often display adjacent rooms at the same time.
    for (const auto& entry : sSession.peerPoses) {
        const PeerPoseSnapshot& pose = entry.second;
        if (!pose.valid || pose.ageTicks > 30) {
            if (pose.isTransforming && sTransformDrawGateLogCount < 40) {
                ++sTransformDrawGateLogCount;
                DuskLog.info(
                    "Multiplayer transform debug draw_pose_skip peer={} reason=stale_or_invalid "
                    "valid={} age={} seq={}",
                    entry.first, pose.valid, pose.ageTicks, pose.sequence);
            }
            continue;
        }
        if (pose.stage != localStage) {
            if (pose.isTransforming && sTransformDrawGateLogCount < 40) {
                ++sTransformDrawGateLogCount;
                DuskLog.info(
                    "Multiplayer transform debug draw_pose_skip peer={} reason=stage_mismatch "
                    "peer_stage={} local_stage={} seq={}",
                    entry.first, pose.stage, localStage, pose.sequence);
            }
            continue;
        }
        if (pose.isTransforming && sTransformDrawGateLogCount < 40) {
            ++sTransformDrawGateLogCount;
            DuskLog.info(
                "Multiplayer transform debug draw_call peer={} seq={} from_wolf={} to_wolf={} "
                "is_wolf={} matrices_valid={} age={}",
                entry.first, pose.sequence, pose.transformFromWolf, pose.transformToWolf,
                pose.isWolf, pose.linkMatrices.valid, pose.ageTicks);
        }
        draw_remote_link_dummy(entry.first, pose);
    }
}

void draw_notifications_overlay() {
    draw_peer_name_labels();

    if (!sEnabled || sNotifications.empty()) {
        return;
    }

    const float dt = ImGui::GetIO().DeltaTime;
    for (Notification& notification : sNotifications) {
        notification.ageSeconds += dt;
    }
    sNotifications.erase(
        std::remove_if(sNotifications.begin(), sNotifications.end(), [](const Notification& item) {
            return item.ageSeconds >= item.durationSeconds;
        }),
        sNotifications.end());

    if (sNotifications.empty()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workPos = viewport != nullptr ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
    ImGui::SetNextWindowPos(ImVec2(workPos.x + 16.0f, workPos.y + 44.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.58f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("Multiplayer Notices", nullptr, flags)) {
        for (const Notification& notification : sNotifications) {
            const float remaining = notification.durationSeconds - notification.ageSeconds;
            const float alpha = remaining < 1.0f ? remaining : 1.0f;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.97f, 1.0f, alpha));
            ImGui::TextUnformatted(notification.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

void notify_local_event_bit_set(uint16_t flag) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "event_bit"},
        {"flag", flag},
    });
    DuskLog.info("Multiplayer sent local event bit flag={}", flag);
}

void notify_local_tbox_set(int flag) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr) {
        return;
    }

    const int stageNo = dStage_stagInfo_GetSaveTbl(stagInfo);
    send_json({
        {"type", "tbox_bit"},
        {"stage", stageNo},
        {"flag", flag},
    });
    DuskLog.info("Multiplayer sent local chest bit stage={} flag={}", stageNo, flag);
}

void notify_local_dungeon_item_set(int kind) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr) {
        return;
    }

    const int stageNo = dStage_stagInfo_GetSaveTbl(stagInfo);
    send_json({
        {"type", "dungeon_item_bit"},
        {"stage", stageNo},
        {"kind", kind},
    });
    DuskLog.info("Multiplayer sent local dungeon item bit stage={} kind={}", stageNo, kind);
}

void notify_local_item_get(int itemId) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    if (!is_synced_key_item(itemId)) {
        return;
    }

    send_json({
        {"type", "item_get"},
        {"item_id", itemId},
    });
    DuskLog.info("Multiplayer sent local item get item_id={}", itemId);
}

void notify_local_memory_switch_set(int flag) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr) {
        return;
    }

    const int stageNo = dStage_stagInfo_GetSaveTbl(stagInfo);
    const bool hasActorContext = sLocalSwitchActorContext.active &&
        sLocalSwitchActorContext.flag == flag;

    if (hasActorContext && is_group2_lifecycle_actor(sLocalSwitchActorContext.actorName)) {
        DuskLog.info("Multiplayer suppressed local switch bit stage={} flag={} sourceActor={} "
                     "sourceRoom={} reason={}",
                     stageNo, flag, sLocalSwitchActorContext.actorName,
                     sLocalSwitchActorContext.room,
                     group2_lifecycle_actor_reason(sLocalSwitchActorContext.actorName));
        return;
    }

    json message = {
        {"type", "switch_bit"},
        {"stage", stageNo},
        {"flag", flag},
    };

    if (hasActorContext) {
        message["source_actor"] = sLocalSwitchActorContext.actorName;
        message["source_room"] = sLocalSwitchActorContext.room;
    }

    send_json(message);
    if (hasActorContext) {
        DuskLog.info("Multiplayer sent local switch bit stage={} flag={} sourceActor={} sourceRoom={}",
                     stageNo, flag, sLocalSwitchActorContext.actorName,
                     sLocalSwitchActorContext.room);
    } else {
        DuskLog.info("Multiplayer sent local switch bit stage={} flag={}", stageNo, flag);
    }
}

void begin_local_switch_actor_context(int actorName, int room, int flag) {
    if (sLocalSwitchActorContext.depth++ == 0) {
        sLocalSwitchActorContext.active = true;
        sLocalSwitchActorContext.actorName = actorName;
        sLocalSwitchActorContext.room = room;
        sLocalSwitchActorContext.flag = flag;
    }
}

void end_local_switch_actor_context() {
    if (sLocalSwitchActorContext.depth <= 0) {
        sLocalSwitchActorContext.depth = 0;
        sLocalSwitchActorContext.active = false;
        return;
    }

    if (--sLocalSwitchActorContext.depth == 0) {
        sLocalSwitchActorContext.active = false;
        sLocalSwitchActorContext.actorName = -1;
        sLocalSwitchActorContext.room = -128;
        sLocalSwitchActorContext.flag = -1;
    }
}

void notify_local_room_scene_initialized(int room) {
    const char* stage = current_stage_name();
    if (stage == nullptr || stage[0] == '\0') {
        return;
    }

    if (sPolicyRoomInitialized && sPolicyInitializedStageName == stage &&
        sPolicyInitializedRoom == room)
    {
        return;
    }

    sPolicyRoomInitialized = true;
    sPolicyInitializedStageName = stage;
    sPolicyInitializedRoom = room;
    sPolicyRoomInitializedTicks = 0;

    DuskLog.info("Multiplayer room scene initialized currentStage={} currentRoom={} "
                 "initializedRoom={}",
                 stage, dComIfGp_roomControl_getStayNo(), room);
}

void notify_local_memory_item_set(int flag) {
    if (!sEnabled || sApplyingRemoteSaveBit) {
        return;
    }

    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr) {
        return;
    }

    const int stageNo = dStage_stagInfo_GetSaveTbl(stagInfo);
    remember_memory_item_bit(stageNo, flag);

    if (!sSession.welcomed) {
        DuskLog.info("Multiplayer remembered local item bit stage={} flag={} before peer welcome",
                     stageNo, flag);
        return;
    }

    send_json({
        {"type", "item_bit"},
        {"stage", stageNo},
        {"flag", flag},
    });
    DuskLog.info("Multiplayer sent local item bit stage={} flag={}", stageNo, flag);
}

void notify_local_collect_crystal_set(int item) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "collect_crystal"},
        {"item", item},
    });
    DuskLog.info("Multiplayer sent local crystal collect item={}", item);
}

void notify_local_collect_mirror_set(int item) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "collect_mirror"},
        {"item", item},
    });
    DuskLog.info("Multiplayer sent local mirror collect item={}", item);
}

void notify_local_dark_clear_lv_set(int no) {
    if (!sEnabled || !sLayerRiskSyncEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "dark_clear_lv"},
        {"no", no},
    });
    DuskLog.info("Multiplayer sent local dark clear lv no={}", no);
}

void notify_local_transform_lv_set(int no) {
    if (!sEnabled || !sLayerRiskSyncEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "transform_lv"},
        {"no", no},
    });
    DuskLog.info("Multiplayer sent local transform lv no={}", no);
}

void notify_local_region_bit_set(int region) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "region_bit"},
        {"region", region},
    });
    DuskLog.info("Multiplayer sent local region bit region={}", region);
}

void notify_local_collect_set(int type, int item) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "collect"},
        {"collect_type", type},
        {"item", item},
    });
    DuskLog.info("Multiplayer sent local collect type={} item={}", type, item);
}

void notify_local_visited_room_set(int stage, int roomNo) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "visited_room"},
        {"stage", stage},
        {"room", roomNo},
    });
    DuskLog.info("Multiplayer sent local visited room stage={} room={}", stage, roomNo);
}

void notify_local_letter_get_set(int no) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "letter_get"},
        {"no", no},
    });
    DuskLog.info("Multiplayer sent local letter get no={}", no);
}

// OoT Anchor model (verified against oot_pc/soh/soh/Network/Anchor/Packets/
// UpdateDungeonItems.cpp): broadcast the absolute current count read
// straight from save state, not a per-instance bit. Works regardless of
// where the receiving player physically is, unlike a fix that depends on
// finding a live actor in the world.
void notify_local_key_num_set(uint8_t keyNum) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
    if (stagInfo == nullptr) {
        return;
    }

    const int stageNo = dStage_stagInfo_GetSaveTbl(stagInfo);
    send_json({
        {"type", "key_num"},
        {"stage", stageNo},
        {"count", keyNum},
    });
    DuskLog.info("Multiplayer sent local key num stage={} count={}", stageNo, keyNum);
}

void notify_local_light_drop_num_set(uint8_t area, uint8_t num) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "light_drop_num"},
        {"area", area},
        {"count", num},
    });
    DuskLog.info("Multiplayer sent local light drop num area={} count={}", area, num);
}

// Heart pieces/containers share one item ID each across many pickups, so
// the item_get lane's first-bit guard only lets the first one of each type
// replay remotely. Broadcast the absolute value instead, same as key_num/
// light_drop_num, but merged as monotonic-max on receive since max life
// should never decrease.
void notify_local_max_life_set(uint8_t maxLife) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "max_life_update"},
        {"value", maxLife},
    });
    DuskLog.info("Multiplayer sent local max life value={}", maxLife);
}

// Same shape as max_life: empty-bottle grants share one item ID across up
// to 4 NPCs/quests, so the item_get lane swallows every one after the
// first. Broadcasts the slot count only, not contents (see
// notify_local_bottle_slot_count_set's declaration for why).
void notify_local_bottle_slot_count_set(uint8_t count) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "bottle_slots"},
        {"count", count},
    });
    DuskLog.info("Multiplayer sent local bottle slot count={}", count);
}

}  // namespace dusk::multiplayer
