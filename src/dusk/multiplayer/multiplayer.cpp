#include "dusk/multiplayer/multiplayer.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_e_pz.h"
#include "d/actor/d_a_midna.h"
#include "d/actor/d_a_nbomb.h"
#include "d/actor/d_a_npc_chin.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/actor/d_a_obj_cblock.h"
#include "d/actor/d_a_obj_bbox.h"
#include "d/actor/d_a_obj_carry.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_lv4PoGate.h"
#include "d/actor/d_a_obj_picture.h"
#include "d/actor/d_a_obj_scannon.h"
#include "d/actor/d_a_obj_smallkey.h"
#include "d/actor/d_a_obj_sword.h"
#include "d/actor/d_a_spinner.h"
#include "d/actor/d_a_tbox.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
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
#include "dusk/autosave.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include "m_Do/m_Do_mtx.h"
#include "m_Do/m_Do_controller_pad.h"
#include "aurora/lib/window.hpp"
#include "absl/strings/escaping.h"
#include "imgui.h"
#include "nlohmann/json.hpp"
#include <zstd.h>

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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dusk::multiplayer {

void flush_pending_progression_sync();
void consume_progression_prompt_start_button();

namespace {

using json = nlohmann::json;

// Direct joiners still talk to one endpoint, but direct hosts now behave as a
// small hub: every accepted TCP peer gets a host-assigned client_id and the
// host forwards peer messages to the rest of the room. Messages from the host
// itself still omit client_id, so direct joiners render the host under this
// placeholder key.
const char* const kDirectPeerId = "direct";
constexpr size_t kMaxDirectPeers = 7;
size_t sLastPoseMatrixPackedBytes = 0;
size_t sLastPoseMatrixBase64Bytes = 0;
size_t sLastPoseMatrixPresentSlots = 0;

struct MatrixPackSlotInput {
    const char* name;
    J3DModel* model;
};

struct MatrixPackSlotMetrics {
    const char* name = "";
    bool present = false;
    uint16_t jointCount = 0;
    uint16_t weightCount = 0;
    size_t bytes = 0;
    uint32_t basisQuantizedMatrices = 0;
    uint32_t basisFloatMatrices = 0;
    float basisMaxAbs = 0.0f;
    uint64_t weightHash = 0;
    bool weightChanged = false;
    uint32_t weightStableFrames = 0;
};

struct MatrixWeightTraceState {
    uint16_t jointCount = 0;
    uint16_t weightCount = 0;
    uint64_t weightHash = 0;
    uint32_t stableFrames = 0;
    bool initialized = false;
};

std::vector<MatrixPackSlotMetrics> sLastPoseMatrixSlotMetrics;
std::map<std::string, MatrixWeightTraceState> sMatrixWeightTraceBySlot;

std::string resolve_peer_id(const json& message) {
    return message.value("client_id", kDirectPeerId);
}

#pragma pack(push, 1)
struct ManualSyncStatePacket {
    char stageName[8];
    int8_t roomNo;
    int8_t layer;
    int16_t startPoint;
};
#pragma pack(pop)

constexpr size_t kManualSyncStatePacketSize =
    sizeof(ManualSyncStatePacket) + sizeof(dSv_info_c);

constexpr size_t kUdpPoseChunkPayloadBytes = 1100;
constexpr size_t kUdpPoseSenderIdBytes = 32;
constexpr size_t kUdpPoseMaxCompressedBytes = 256 * 1024;
constexpr size_t kUdpPoseMaxUncompressedBytes = 512 * 1024;
constexpr size_t kUdpPoseMaxInflightSequences = 8;
constexpr int kUdpPoseSocketBufferBytes = 1024 * 1024;

bool pose_float_is_finite(const char* name, f32 value) {
    if (std::isfinite(value)) {
        return true;
    }

    static uint32_t sBadPoseFloatLogCount = 0;
    if (sBadPoseFloatLogCount < 20) {
        ++sBadPoseFloatLogCount;
        DuskLog.warn("Multiplayer pose tx dropped: non-finite {}={}", name, value);
    }
    return false;
}

#pragma pack(push, 1)
struct UdpPoseChunkHeader {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint16_t headerSize;
    uint32_t sequence;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint32_t uncompressedSize;
    uint32_t compressedSize;
    uint16_t payloadSize;
    char senderId[kUdpPoseSenderIdBytes];
};
#pragma pack(pop)

struct UdpPoseReassembly {
    uint32_t sequence = 0;
    uint16_t chunkCount = 0;
    uint32_t uncompressedSize = 0;
    uint32_t compressedSize = 0;
    uint16_t receivedCount = 0;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> received;
};

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

int calc_remote_bomb_flash(daNbomb_c* bomb, daAlink_c* link) {
    if (bomb == nullptr || link == nullptr) {
        return -1;
    }

    const int exTime = bomb->getExTime();
    const int explodeTime = link->getBombExplodeTime();
    if (explodeTime <= 8 || exTime < 0) {
        return 0;
    }

    float brightness = 0.0f;
    if (exTime > explodeTime) {
        brightness = 1.0f - std::fabs(std::cos(((static_cast<float>(exTime - explodeTime)) * M_PI) /
                                               static_cast<float>(std::max(1, explodeTime / 2))));
    } else if (exTime > explodeTime / 2) {
        brightness = 1.0f - std::fabs(std::cos(((static_cast<float>(exTime - explodeTime / 2)) * M_PI) /
                                               static_cast<float>(std::max(1, explodeTime / 4))));
    } else if (exTime > explodeTime / 4) {
        brightness = std::fabs(std::sin(((static_cast<float>(exTime - explodeTime / 4)) * M_PI) /
                                        static_cast<float>(std::max(1, explodeTime / 7))));
    } else {
        brightness = std::fabs(std::sin(((static_cast<float>(exTime - explodeTime / 7)) * M_PI) /
                                        static_cast<float>(std::max(1, explodeTime / 8))));
    }

    return std::clamp(static_cast<int>(brightness * 15.0f), 0, 15);
}

json audio_events_to_json(const std::vector<RemoteAudioEvent>& events) {
    json out = json::array();
    for (const RemoteAudioEvent& event : events) {
        out.push_back({
            {"seq", event.sequence},
            {"sound_id", event.soundId},
            {"mapinfo", event.mapInfo},
            {"reverb", static_cast<int>(event.reverb)},
            {"source", static_cast<int>(event.sourceKind)},
            {"level", event.level},
        });
    }
    return out;
}

std::vector<RemoteAudioEvent> parse_audio_events(const json& state) {
    std::vector<RemoteAudioEvent> events;
    const json entries = state.value("audio_events", json::array());
    for (const json& entry : entries) {
        if (!entry.is_object() || events.size() >= 8) {
            continue;
        }

        RemoteAudioEvent event;
        event.sequence = entry.value("seq", 0U);
        event.soundId = entry.value("sound_id", 0U);
        event.mapInfo = entry.value("mapinfo", 0U);
        event.reverb = static_cast<int8_t>(entry.value("reverb", -1));
        event.sourceKind = static_cast<uint8_t>(entry.value("source", 0));
        event.level = entry.value("level", false);
        if (event.sequence != 0 && event.soundId != 0) {
            events.push_back(event);
        }
    }
    return events;
}

struct DirectPeer {
    socket_t sock = INVALID_SOCKET;
    sockaddr_in udpAddr{};
    std::string id;
    std::string name = "Peer";
    std::string rxBuffer;
    bool welcomed = false;
    bool snapshotPending = false;
    bool udpAddrKnown = false;
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
    socket_t udpSock = INVALID_SOCKET;
    sockaddr_in udpRemoteAddr{};
    std::string host = "127.0.0.1";
    std::string bindHost = "0.0.0.0";
    std::string publicHost = "127.0.0.1";
    std::string room = "dev";
    std::string name = "TP Player";
    std::string inviteCode;
    std::string clientId;
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
    std::map<std::string, std::map<uint32_t, UdpPoseReassembly>> udpPoseReassembly;
    std::map<std::string, uint32_t> udpPoseLastProcessedSequence;
    bool udpRemoteAddrKnown = false;
};

bool sInitialized = false;
bool sEnabled = false;
Session sSession;

bool sDummyModelEnabled = false;
bool sDummyTraceEnabled = false;
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

struct ProgressionSyncPrompt {
    bool active = false;
    std::string peerId;
    std::string peerName;
    std::string cueKey;
    std::string title;
    std::string body;
    float ageSeconds = 0.0f;
    float holdSeconds = 0.0f;
    bool waiting = false;
};

struct PendingProgressionSync {
    bool active = false;
    std::string peerId;
    std::string peerName;
    std::string cueKey;
    uint32_t stableReadyTicks = 0;
};

// A progression cue (event bit) can fire while the peer is still mid-walkout
// in the stage they're leaving, well before the stage actually swaps to the
// destination. Showing the sync prompt at that instant lets the user sync
// into the peer's still-stale stage. So these cues wait here, invisible to
// the player, until the peer's broadcast pose actually reports the
// destination stage before becoming a real ProgressionSyncPrompt.
struct PendingProgressionCueArrival {
    bool active = false;
    std::string peerId;
    std::string cueKey;
    std::string title;
    std::string body;
    std::string expectedStage;
};

// Held on the *replying* side of a sync_request when this client's own live
// state doesn't yet satisfy the requested cue's readiness condition (see
// is_local_state_ready_for_cue()). Retried each tick so the eventual
// snapshot+reply reflects this client's state at the moment it's actually
// taken, instead of racing whatever was true when the request first arrived.
struct PendingSyncReply {
    bool active = false;
    bool isDirectPeer = false;
    std::string peerKey;        // DirectPeer id, re-resolved at flush time.
    std::string targetClientId; // Relay mode requester id.
    std::string cueKey;
    uint32_t waitTicks = 0;
};

std::vector<Notification> sNotifications;
ProgressionSyncPrompt sProgressionSyncPrompt;
PendingProgressionSync sPendingProgressionSync;
std::vector<PendingProgressionCueArrival> sPendingProgressionCueArrivals;
std::vector<PendingSyncReply> sPendingSyncReplies;
bool sProgressionPromptExactStartHeld = false;
std::set<std::string> sShownPoseProgressionCues;
std::map<std::string, std::string> sPeerNames;
std::map<std::string, uint8_t> sPeerColorSlots;
uint8_t sLocalPlayerColorSlot = 0;
bool sLocalPlayerColorSlotReserved = true;
std::vector<RemoteAudioEvent> sPendingLocalAudioEvents;
uint32_t sLocalAudioEventSequence = 0;
bool sManualSyncReloadPending = false;
std::optional<dSv_info_c> sPendingManualSyncInfo;
std::optional<u8> sPendingManualSyncVibration;
// Set right before a manual-sync request goes out for a progression cue, so
// that when the peer's full-state reply lands, apply_manual_sync_full_state
// knows to override the warp target instead of trusting the peer's raw
// position/layer snapshot (see kProgressionCueDescriptors).
std::string sAwaitingManualSyncCueKey;

// Set while applying any remote save-state bit (event bit, chest bit, ...),
// so the dComIfGs_* setter call that applies it doesn't loop back through
// its notify_local_*_set() hook and re-send it to the peer that just sent it
// to us.
bool sApplyingRemoteSaveBit = false;

constexpr int kProgressionCueSewersStage = dStage_SaveTbl_PRISON;
constexpr int kProgressionCueWakeUpInJailSwitch = 27; // ImGui flag 08:08 / "wake up in jail cs".
constexpr uint16_t kProgressionCueSewersCompleteEventBit = 0x6140; // offset 0x61 bit 64 - "remove midna from z (temporary flag after sewers)".
constexpr uint16_t kProgressionCueFaronTwilightEventBit = 0x0640; // offset 0x06 bit 64 - "watched faron twilight intro cutscene".
constexpr int kUnsyncedSwitchOrdonStage = dStage_SaveTbl_ORDON;
constexpr int kUnsyncedSwitchOrdonKingBulblinCs = 0x68; // Ordon area flag 16:01 - "King Bulblin cs".
constexpr int kUnsyncedSwitchSewersStage = dStage_SaveTbl_PRISON;
constexpr int kUnsyncedSwitchSewersTwilightFinalCs = 0x1F; // Sewers area flag 08:80 - "twilight final cs".
constexpr int kUnsyncedSwitchLanayruStage = dStage_SaveTbl_LANAYRU;
constexpr int kUnsyncedSwitchLanayruLakeHyliaIntroTwilightCs = 0x1E; // Lanayru area flag 08:40 - "Lake hylia intro cs twilight".
constexpr uint16_t kUnsyncedEventBitOrdonSpringMonsterAttack = 0x0580;
constexpr uint16_t kUnsyncedEventBitFaronCaptureWolfCutscene = 0x4D08;
constexpr uint16_t kUnsyncedEventBitSewersWarpedFromCastleByMidna = 0x0502;
constexpr int kProgressionCueHyruleFieldStage = dStage_SaveTbl_FIELD;
constexpr int kProgressionCueEldinTwilightSwitch = 0x0C; // Hyrule Field area flag 0A:10 - "entered Eldin twilight cs".
constexpr int kProgressionCueLanayruTwilightSwitch = 0x0D; // Hyrule Field area flag 0A:20 - "entered Lanayru twilight cs".
constexpr int kProgressionCueForestTempleStage = dStage_SaveTbl_FARON;
constexpr int kProgressionCueForestTempleSavePromptSwitch = 0x01; // Faron area flag 0B:02 - "FT save prompt".
constexpr int kProgressionCueGoronMinesStage = dStage_SaveTbl_ELDIN;
constexpr int kProgressionCueGoronMinesSavePromptSwitch = 0x7C; // Eldin area flag 14:10 - "GM save prompt".
constexpr int kProgressionCueLakebedTempleStage = dStage_SaveTbl_LANAYRU;
constexpr int kProgressionCueLakebedTempleSavePromptSwitch = 0x0E; // Lanayru area flag 0A:40 - "Save Prompt after Lakebed".
constexpr int kProgressionCueArbitersGroundsStage = dStage_SaveTbl_DESERT;
constexpr int kProgressionCueArbitersGroundsSavePromptSwitch = 0x0A; // Desert area flag 0A:04 - "save prompt after beating Arbiter's Grounds".
constexpr int kProgressionCueSnowpeakRuinsStage = dStage_SaveTbl_SNOWPEAK;
constexpr int kProgressionCueSnowpeakRuinsSavePromptSwitch = 0x19; // Snowpeak area flag 08:02 - "Post SPR Save Prompt".
constexpr int kProgressionCueCityInTheSkyStage = dStage_SaveTbl_LV7;
constexpr int kProgressionCueCityInTheSkySavePromptSwitch = 0x25; // CitS area flag 0F:20 - "save promt after boss".
constexpr int kProgressionCuePalaceOfTwilightStage = dStage_SaveTbl_LV8;
constexpr int kProgressionCuePalaceOfTwilightSavePromptSwitch = 0x16; // PoT area flag 09:40 - "save prompt after boss".
constexpr uint16_t kProgressionCueTempleOfTimeClearEventBit = 0x2004; // LV6 dungeon clear; prompt waits for peer to reach F_SP117.
constexpr const char* kProgressionCueTempleOfTimeExitStage = "F_SP117";
constexpr const char* kProgressionCueFinalGanondorfStage = "D_MN09C"; // Dark Lord Ganondorf arena, after horseback phase.
constexpr const char* kProgressionCueSewersCompleteDestStage = "F_SP104"; // Ordon Spring.
constexpr const char* kProgressionCueFaronTwilightDestStage = "F_SP108"; // Faron Woods (twilight).
constexpr float kProgressionSyncPromptDuration = 12.0f;
constexpr float kProgressionSyncHoldDuration = 1.0f;
constexpr uint32_t kProgressionSyncStableReadyTicks = 1;
constexpr uint32_t kFlagTraceActorWindowTicks = 180;
// Generous cap on how long a deferred sync_request reply waits for this
// client's own state to satisfy the cue's readiness condition before giving
// up and replying anyway (better than leaving the requester hanging forever
// if something about the expected stage/form never actually materializes).
constexpr uint32_t kPendingSyncReplyTimeoutTicks = 1800;

// Local memory-tier item bits observed through the live setter hook. This
// intentionally records before the network "welcomed" check, so a player can
// collect a heart piece, then have a peer join later and still include that
// pickup in the catch-up snapshot even if vanilla has not committed the
// current stage memory back into the savedata table yet.
std::map<int, std::set<int>> sObservedMemoryItems;

bool is_unsynced_event_bit(uint16_t flag) {
    switch (flag) {
    case kUnsyncedEventBitOrdonSpringMonsterAttack:
    case kUnsyncedEventBitFaronCaptureWolfCutscene:
    case kUnsyncedEventBitSewersWarpedFromCastleByMidna:
        return true;
    default:
        return false;
    }
}

bool is_unsynced_switch_bit(int stage, int flag) {
    return (stage == kUnsyncedSwitchOrdonStage &&
            flag == kUnsyncedSwitchOrdonKingBulblinCs) ||
           (stage == kUnsyncedSwitchSewersStage &&
            flag == kUnsyncedSwitchSewersTwilightFinalCs) ||
           (stage == kUnsyncedSwitchLanayruStage &&
            flag == kUnsyncedSwitchLanayruLakeHyliaIntroTwilightCs);
}

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
    uint32_t actorParams = 0xFFFFFFFF;
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
uint32_t sFlagTraceActorWindowTicks = 0;

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
           type == "item_bit" || type == "dungeon_item_bit" || type == "key_num" ||
           type == "visited_room" || type == "rupee_count";
}

bool is_stage_load_unsafe_for_multiplayer() {
    return dComIfGp_getStageStagInfo() == nullptr || dComIfGp_event_runCheck() ||
           dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() || fopOvlpM_IsDoingReq();
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

void begin_flag_trace_window(const char* source, const char* lane, int stage, int flag, bool set,
                             int sourceActor = -1, int sourceRoom = -128,
                             uint32_t sourceParams = 0xFFFFFFFF) {
    sFlagTraceActorWindowTicks = kFlagTraceActorWindowTicks;
    DuskLog.info(
        "MP_FLAG_TRACE flag source={} lane={} stage={} flag={} set={} sourceActor={} "
        "sourceRoom={} sourceParams=0x{:08X} currentStage={} currentRoom={} actorWindowTicks={}",
        source, lane, stage, flag, set, sourceActor, sourceRoom, sourceParams,
        current_stage_name(), dComIfGp_roomControl_getStayNo(), sFlagTraceActorWindowTicks);
}

bool is_flag_trace_actor_window_active() {
    return sEnabled && sFlagTraceActorWindowTicks > 0;
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

bool repair_remote_breakable_carry_box_switch(int stage, int flag);
bool is_sewers_progression_switch(int stage, int flag);

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
    const bool repairedBreakableCarryBox = repair_remote_breakable_carry_box_switch(stage, flag);

    if (repairedCBlock || repairedJumpTbox || repairedLv4PoGate || repairedPZ ||
        repairedSCannon || repairedBreakableCarryBox)
    {
        DuskLog.info("Multiplayer remote switch repair stage={} flag={} reason={} "
                     "cblock={} jump_tbox={} lv4_poe_gate={} e_pz={} scannon={} breakable_box={} "
                     "picture_monitor={} npc_chin_monitor={}",
                     stage, flag, reason, repairedCBlock, repairedJumpTbox, repairedLv4PoGate,
                     repairedPZ, repairedSCannon, repairedBreakableCarryBox, monitoredPicture,
                     monitoredNpcChin);
    } else if (monitoredPicture || monitoredNpcChin) {
        DuskLog.info("Multiplayer remote switch monitor stage={} flag={} reason={} "
                     "picture_monitor={} npc_chin_monitor={} repair=reload_or_cosmetic",
                     stage, flag, reason, monitoredPicture, monitoredNpcChin);
    } else {
        DuskLog.info("Multiplayer remote switch repair checked stage={} flag={} reason={} "
                     "cblock={} jump_tbox={} lv4_poe_gate={} e_pz={} scannon={} breakable_box={} "
                     "picture_monitor={} npc_chin_monitor={}",
                     stage, flag, reason, repairedCBlock, repairedJumpTbox, repairedLv4PoGate,
                     repairedPZ, repairedSCannon, repairedBreakableCarryBox, monitoredPicture,
                     monitoredNpcChin);
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

struct BreakableCarryBoxRepairSearch {
    int flag;
    bool repaired;
    bool sawCarryBox;
    bool sawBBox;
};

void* repair_breakable_carry_box_by_switch(void* actor, void* data) {
    if (actor == nullptr || data == nullptr || !fopAcM_IsActor(actor) ||
        fopAcM_GetName(actor) != fpcNm_Obj_Carry_e)
    {
        return nullptr;
    }

    auto* search = static_cast<BreakableCarryBoxRepairSearch*>(data);
    auto* carry = static_cast<daObjCarry_c*>(actor);
    if (carry->getType() != daObjCarry_c::TYPE_KIBAKO) {
        return nullptr;
    }

    search->sawCarryBox = true;
    DuskLog.info("Multiplayer sewers 09:02 saw Obj_Carry KIBAKO room={} homeRoom={} "
                 "swbit={} params=0x{:08X} pos=({}, {}, {})",
                 fopAcM_GetRoomNo(carry), fopAcM_GetHomeRoomNo(carry), carry->getSwbit(),
                 fopAcM_GetParam(carry), carry->current.pos.x, carry->current.pos.y,
                 carry->current.pos.z);

    carry->obj_break(false, true, true);
    fopAcM_delete(carry);
    search->repaired = true;
    return actor;
}

void* repair_breakable_bbox_by_switch(void* actor, void* data) {
    if (actor == nullptr || data == nullptr || !fopAcM_IsActor(actor) ||
        fopAcM_GetName(actor) != fpcNm_Obj_BBox_e)
    {
        return nullptr;
    }

    auto* search = static_cast<BreakableCarryBoxRepairSearch*>(data);
    auto* bbox = static_cast<daObjBBox_c*>(actor);
    if (bbox->getSwNo() != search->flag) {
        return nullptr;
    }

    search->sawBBox = true;
    DuskLog.info("Multiplayer sewers box saw Obj_BBox room={} homeRoom={} sw={} "
                 "params=0x{:08X} pos=({}, {}, {})",
                 fopAcM_GetRoomNo(bbox), fopAcM_GetHomeRoomNo(bbox), bbox->getSwNo(),
                 fopAcM_GetParam(bbox), bbox->current.pos.x, bbox->current.pos.y,
                 bbox->current.pos.z);

    static const u16 particleId[5] = {0x83B0, 0x83B1, 0x83B2, 0x83B3, 0x83B4};
    for (u16 id : particleId) {
        dComIfGp_particle_set(id, &bbox->current.pos, nullptr, &bbox->scale, 0xff, nullptr, -1,
                              nullptr, nullptr, nullptr);
    }
    fopAcM_seStart(bbox, Z2SE_OBJ_WOODBOX_BREAK, 0);
    fopAcM_delete(bbox);
    search->repaired = true;
    return actor;
}

bool repair_remote_breakable_carry_box_switch(int stage, int flag) {
    if (stage != kProgressionCueSewersStage || (flag != 10 && flag != 17)) {
        return false;
    }

    BreakableCarryBoxRepairSearch search = {flag, false, false, false};
    if (flag == 10) {
        fopAcM_Search(repair_breakable_bbox_by_switch, &search);
        if (!search.sawBBox) {
            DuskLog.info("Multiplayer sewers 0A:04 found no live Obj_BBox to repair");
        }
    } else {
        fopAcM_Search(repair_breakable_carry_box_by_switch, &search);
        if (!search.sawCarryBox) {
            DuskLog.info("Multiplayer sewers 09:02 found no live Obj_Carry KIBAKO to repair");
        }
    }
    return search.repaired;
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

// Clearing a switch doesn't reposition anything the way setting one can
// (cblocks, jump-trigger Tboxes, etc.), so unlike apply_remote_switch_bit
// this doesn't need the deferred/room-init policy machinery -- a direct,
// idempotent apply is enough.
void apply_remote_switch_bit_off(int stage, int flag) {
    if (!dComIfGs_isStageSwitch(stage, flag)) {
        DuskLog.info("Multiplayer remote switch already cleared stage={} flag={}", stage, flag);
        return;
    }

    const bool wasApplyingRemoteSaveBit = sApplyingRemoteSaveBit;
    sApplyingRemoteSaveBit = true;
    dComIfGs_offStageSwitch(stage, flag);
    sApplyingRemoteSaveBit = wasApplyingRemoteSaveBit;

    DuskLog.info("Multiplayer applied remote switch bit cleared stage={} flag={}", stage, flag);
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

    if (is_sewers_progression_switch(stage, flag)) {
        DuskLog.info("Multiplayer allowed remote sewers progression switch stage={} flag={} "
                     "sourceActor={} reason={}",
                     stage, flag, sourceActor, reason);
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


bool is_peer_dummy_gameplay_ready() {
    if (fpcM_SearchByName(fpcNm_TITLE_e) != nullptr ||
        fpcM_SearchByName(fpcNm_PLAY_SCENE_e) == nullptr ||
        dComIfGp_getWindowNum() == 0 ||
        is_stage_load_unsafe_for_multiplayer())
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



bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
            std::strcmp(value, "ON") == 0);
}

bool env_disabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
            std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
            std::strcmp(value, "OFF") == 0);
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

void configure_udp_socket_buffers(socket_t sock) {
    int bufferBytes = kUdpPoseSocketBufferBytes;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferBytes),
               sizeof(bufferBytes));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferBytes),
               sizeof(bufferBytes));
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

void show_progression_sync_prompt(const std::string& peerId, std::string cueKey,
                                  std::string title, std::string body) {
    if (peerId.empty()) {
        return;
    }

    if (sProgressionSyncPrompt.active && sProgressionSyncPrompt.peerId == peerId &&
        sProgressionSyncPrompt.cueKey == cueKey)
    {
        return;
    }
    if (sPendingProgressionSync.active && sPendingProgressionSync.peerId == peerId &&
        sPendingProgressionSync.cueKey == cueKey)
    {
        return;
    }

    sProgressionSyncPrompt = {
        true,
        peerId,
        display_name_for_peer(peerId),
        std::move(cueKey),
        std::move(title),
        std::move(body),
        0.0f,
        0.0f,
        false,
    };

    DuskLog.info("Multiplayer progression sync prompt peer={} cue={} title={}", peerId,
                 sProgressionSyncPrompt.cueKey, sProgressionSyncPrompt.title);
}

void queue_progression_event_prompt(const std::string& peerId, const char* cueKey,
                                    const char* action, const char* areaName);

void maybe_show_progression_sync_prompt_for_switch(const std::string& peerId, int stage, int flag) {
    if (stage == kProgressionCueSewersStage && flag == kProgressionCueWakeUpInJailSwitch) {
        show_progression_sync_prompt(peerId, "sewers_wake_up_in_jail",
                                     display_name_for_peer(peerId) + " entered Hyrule Sewers",
                                     "Hold START to sync and reload");
    } else if (stage == kProgressionCueHyruleFieldStage &&
               flag == kProgressionCueEldinTwilightSwitch)
    {
        queue_progression_event_prompt(peerId, "eldin_twilight_entered", "entered",
                                       "Eldin Twilight");
    } else if (stage == kProgressionCueHyruleFieldStage &&
               flag == kProgressionCueLanayruTwilightSwitch)
    {
        queue_progression_event_prompt(peerId, "lanayru_twilight_entered", "entered",
                                       "Lanayru Twilight");
    } else if (stage == kProgressionCueForestTempleStage &&
               flag == kProgressionCueForestTempleSavePromptSwitch)
    {
        queue_progression_event_prompt(peerId, "forest_temple_complete", "completed",
                                       "Forest Temple");
    } else if (stage == kProgressionCueGoronMinesStage &&
               flag == kProgressionCueGoronMinesSavePromptSwitch)
    {
        queue_progression_event_prompt(peerId, "goron_mines_complete", "completed",
                                       "Goron Mines");
    } else if (stage == kProgressionCueLakebedTempleStage &&
               flag == kProgressionCueLakebedTempleSavePromptSwitch)
    {
        queue_progression_event_prompt(peerId, "lakebed_temple_complete", "completed",
                                       "Lakebed Temple");
    } else if (stage == kProgressionCueArbitersGroundsStage &&
               flag == kProgressionCueArbitersGroundsSavePromptSwitch)
    {
        queue_progression_event_prompt(peerId, "arbiters_grounds_complete", "completed",
                                       "Arbiter's Grounds");
    } else if (stage == kProgressionCueSnowpeakRuinsStage &&
               flag == kProgressionCueSnowpeakRuinsSavePromptSwitch)
    {
        queue_progression_event_prompt(peerId, "snowpeak_ruins_complete", "completed",
                                       "Snowpeak Ruins");
    } else if (stage == kProgressionCueCityInTheSkyStage &&
               flag == kProgressionCueCityInTheSkySavePromptSwitch)
    {
        queue_progression_event_prompt(peerId, "city_in_the_sky_complete", "completed",
                                       "City in the Sky");
    } else if (stage == kProgressionCuePalaceOfTwilightStage &&
               flag == kProgressionCuePalaceOfTwilightSavePromptSwitch)
    {
        queue_progression_event_prompt(peerId, "palace_of_twilight_complete", "completed",
                                       "Palace of Twilight");
    }
}

// Queues a cue to appear only once the peer's broadcast pose reports they've
// actually reached expectedStage, instead of the instant the triggering
// event bit arrives (which can be while the peer is still mid-walkout in the
// stage they're leaving -- see update_pending_progression_cue_arrivals()).
void queue_progression_cue_arrival(const std::string& peerId, std::string cueKey, std::string title,
                                   std::string body, std::string expectedStage) {
    if (peerId.empty()) {
        return;
    }

    if (sProgressionSyncPrompt.active && sProgressionSyncPrompt.peerId == peerId &&
        sProgressionSyncPrompt.cueKey == cueKey)
    {
        return;
    }
    if (sPendingProgressionSync.active && sPendingProgressionSync.peerId == peerId &&
        sPendingProgressionSync.cueKey == cueKey)
    {
        return;
    }
    for (const auto& pending : sPendingProgressionCueArrivals) {
        if (pending.active && pending.peerId == peerId && pending.cueKey == cueKey) {
            return;
        }
    }

    DuskLog.info("Multiplayer progression cue awaiting peer readiness peer={} cue={} stage={}",
                 peerId, cueKey, expectedStage.empty() ? "<any>" : expectedStage);
    sPendingProgressionCueArrivals.push_back({
        true,
        peerId,
        std::move(cueKey),
        std::move(title),
        std::move(body),
        std::move(expectedStage),
    });
}

void queue_progression_cue_ready(const std::string& peerId, std::string cueKey, std::string title,
                                 std::string body) {
    queue_progression_cue_arrival(peerId, std::move(cueKey), std::move(title), std::move(body), "");
}

void update_pending_progression_cue_arrivals() {
    if (sPendingProgressionCueArrivals.empty()) {
        return;
    }

    for (auto it = sPendingProgressionCueArrivals.begin(); it != sPendingProgressionCueArrivals.end();) {
        const auto poseIt = sSession.peerPoses.find(it->peerId);
        if (poseIt == sSession.peerPoses.end() || !poseIt->second.valid ||
            poseIt->second.ageTicks > 30 || !poseIt->second.manualSyncReady ||
            (!it->expectedStage.empty() && poseIt->second.stage != it->expectedStage))
        {
            ++it;
            continue;
        }

        DuskLog.info("Multiplayer progression cue peer arrived peer={} cue={} stage={}", it->peerId,
                     it->cueKey, poseIt->second.stage);
        show_progression_sync_prompt(it->peerId, it->cueKey, it->title, it->body);
        it = sPendingProgressionCueArrivals.erase(it);
    }
}

void queue_progression_event_prompt(const std::string& peerId, const char* cueKey,
                                    const char* action, const char* areaName) {
    queue_progression_cue_ready(peerId, cueKey,
                                display_name_for_peer(peerId) + " " + action + " " + areaName,
                                "Hold START to sync and reload");
}

void maybe_show_progression_sync_prompt_for_pose_stage(const std::string& peerId,
                                                       const std::string& stage,
                                                       bool finalGanondorfReady) {
    if (peerId.empty() ||
        (stage != kProgressionCueFinalGanondorfStage && !finalGanondorfReady))
    {
        return;
    }

    const std::string cueKey = "final_ganondorf_entered";
    const std::string guardKey = peerId + ":" + cueKey;
    if (sShownPoseProgressionCues.find(guardKey) != sShownPoseProgressionCues.end()) {
        return;
    }

    sShownPoseProgressionCues.insert(guardKey);
    queue_progression_cue_ready(peerId, cueKey,
                                display_name_for_peer(peerId) +
                                    " reached the final Ganondorf fight",
                                "Hold START to sync and reload");
}

void maybe_show_progression_sync_prompt_for_event_bit(const std::string& peerId, uint16_t flag) {
    if (flag == kProgressionCueSewersCompleteEventBit) {
        queue_progression_cue_arrival(peerId, "sewers_complete",
                                      display_name_for_peer(peerId) + " completed Sewers",
                                      "Hold START to sync and reload",
                                      kProgressionCueSewersCompleteDestStage);
    } else if (flag == kProgressionCueFaronTwilightEventBit) {
        queue_progression_cue_arrival(peerId, "faron_twilight_entered",
                                      display_name_for_peer(peerId) + " entered Faron Twilight",
                                      "Hold START to sync and reload",
                                      kProgressionCueFaronTwilightDestStage);
    } else {
        switch (flag) {
        case 0x0610:
            queue_progression_event_prompt(peerId, "faron_twilight_complete", "completed",
                                           "Faron Twilight");
            break;
        case 0x0708:
            queue_progression_event_prompt(peerId, "eldin_twilight_complete", "completed",
                                           "Eldin Twilight");
            break;
        case 0x0C02:
            queue_progression_event_prompt(peerId, "lanayru_twilight_complete", "completed",
                                           "Lanayru Twilight");
            break;
        case kProgressionCueTempleOfTimeClearEventBit:
            queue_progression_cue_arrival(peerId, "temple_of_time_complete",
                                          display_name_for_peer(peerId) +
                                              " completed Temple of Time",
                                          "Hold START to sync and reload",
                                          kProgressionCueTempleOfTimeExitStage);
            break;
        default:
            break;
        }
    }
}

bool is_sewers_progression_switch(int stage, int flag) {
    if (stage != dStage_SaveTbl_PRISON) {
        return false;
    }

    switch (flag) {
    case 1: // Obj_Wchain stairs sequence.
    case 2: // Stairs sequence follow-up.
    case 3: // Sewers progression sequence observed during stair/water test.
    case 6: // Tag_Mwait sewers progression sequence.
    case 13: // Sewers progression sequence observed during stair/water test.
    case 17: // 09:02 broke fragile floor first jump of stairway.
    case 18: // 09:04 waited long enough in jail.
    case 19: // 09:08 midna intro cs.
    case 20: // 09:10 midna cs after digging out of jail.
    case 21: // 09:20 first water gate in sewers cs.
    case 22: // 09:40 second water gate in sewers cs.
    case 24: // 08:01 pulled lever of first water gate in sewers.
    case 27: // 08:08 wake up in jail cs.
        return true;
    default:
        return false;
    }
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
    close_socket(sSession.udpSock);
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
    sSession.clientId.clear();
    sManualSyncReloadPending = false;
    sPendingManualSyncInfo.reset();
    sPendingManualSyncVibration.reset();
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
    sFlagTraceActorWindowTicks = 0;
    sSession.rxBuffer.clear();
    sSession.reconnectTicks = 0;
    sSession.pingTicks = 0;
    sSession.poseSequence = 0;
    sSession.peerPoseLogTicks = 0;
    sSession.peerPoses.clear();
    sSession.udpPoseReassembly.clear();
    sSession.udpPoseLastProcessedSequence.clear();
    sSession.udpRemoteAddrKnown = false;
    sPendingLocalAudioEvents.clear();
    sPeerNames.clear();
    clear_player_color_slots();
    sNameLabelsEnabled = true;
    sNotifications.clear();
    sProgressionSyncPrompt = {};
    sPendingProgressionSync = {};
    sPendingProgressionCueArrivals.clear();
    sPendingSyncReplies.clear();
    sShownPoseProgressionCues.clear();
    sAwaitingManualSyncCueKey.clear();
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

const char* packet_category(const std::string& type) {
    if (type == "pose") {
        return "pose";
    }
    if (type == "hello" || type == "welcome" || type == "peer_joined" ||
        type == "peer_left" || type == "name_labels")
    {
        return "session";
    }
    if (type == "ping" || type == "pong" || type == "error" || type == "ack") {
        return "control";
    }
    if (type == "sync_request") {
        return "manual_sync_request";
    }
    if (type == "save_snapshot") {
        return "save_snapshot";
    }
    if (type == "event_bit" || type == "tbox_bit" || type == "switch_bit" ||
        type == "item_bit" || type == "dungeon_item_bit")
    {
        return "world_state";
    }
    if (type == "item_get" || type == "collect_crystal" || type == "collect_mirror" ||
        type == "dark_clear_lv" || type == "transform_lv" || type == "region_bit" ||
        type == "collect" || type == "visited_room" || type == "letter_get")
    {
        return "inventory_progress";
    }
    if (type == "key_num" || type == "light_drop_num" || type == "light_drop_get_flag" ||
        type == "max_life_update" || type == "bottle_slots" || type == "rupee_count")
    {
        return "counters";
    }
    if (type == "reliable") {
        return "reliable_envelope";
    }
    return "other";
}

bool packet_trace_enabled() {
    static const bool enabled = !env_disabled("DUSK_MP_PACKET_TRACE");
    return enabled;
}

size_t json_field_bytes(const json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? 0 : it->dump().size();
}

size_t json_field_msgpack_bytes(const json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? 0 : json::to_msgpack(*it).size();
}

size_t pose_base_state_bytes(const json& state) {
    if (!state.is_object()) {
        return 0;
    }

    json base = state;
    base.erase("link_matrices");
    base.erase("audio_events");
    return base.dump().size();
}

void trace_packet_tx(const json& message, size_t bytes) {
    if (!packet_trace_enabled()) {
        return;
    }

    const std::string type = message.value("type", "");
    const char* category = packet_category(type);
    if (type == "pose") {
        const json state = message.value("state", json::object());
        DuskLog.info(
            "MP_PACKET_TX category={} type={} bytes={} sequence={} base_state={} "
            "link_matrices={} audio_events={}",
            category, type, bytes, message.value("sequence", 0U), pose_base_state_bytes(state),
            json_field_bytes(state, "link_matrices"), json_field_bytes(state, "audio_events"));
        return;
    }

    if (type == "save_snapshot") {
        DuskLog.info(
            "MP_PACKET_TX category={} type={} bytes={} manual_sync={} full_state={} "
            "event_flags={} chests={} switches={} items={} dungeon_items={}",
            category, type, bytes, message.value("manual_sync", false),
            json_field_bytes(message, "full_state"),
            json_field_bytes(message, "event_flags"), json_field_bytes(message, "chests"),
            json_field_bytes(message, "switches"), json_field_bytes(message, "items"),
            json_field_bytes(message, "dungeon_items"));
        return;
    }

    DuskLog.info("MP_PACKET_TX category={} type={} bytes={}", category, type, bytes);
}

void trace_udp_pose_packet_tx(const json& message, size_t rawBytes, size_t compressedBytes,
                              uint16_t chunkCount) {
    if (!packet_trace_enabled()) {
        return;
    }
    if (chunkCount == 0) {
        return;
    }

    const uint32_t sequence = message.value("sequence", 0U);
    const json state = message.value("state", json::object());
    const size_t totalDatagramBytes = compressedBytes + sizeof(UdpPoseChunkHeader) * chunkCount;
    const size_t lastPayload =
        compressedBytes - static_cast<size_t>(chunkCount - 1) * kUdpPoseChunkPayloadBytes;
    const size_t minDatagramBytes = sizeof(UdpPoseChunkHeader) + lastPayload;
    const size_t maxDatagramBytes =
        sizeof(UdpPoseChunkHeader) + std::min(compressedBytes, kUdpPoseChunkPayloadBytes);
    const double avgDatagramBytes =
        static_cast<double>(totalDatagramBytes) / static_cast<double>(chunkCount);

    struct UdpPoseTraceSummary {
        uint32_t count = 0;
        uint32_t firstSequence = 0;
        uint32_t lastSequence = 0;
        size_t bytesTotal = 0;
        size_t rawTotal = 0;
        size_t compressedTotal = 0;
        uint32_t chunksTotal = 0;
        uint16_t minChunks = std::numeric_limits<uint16_t>::max();
        uint16_t maxChunks = 0;
        size_t minBytes = std::numeric_limits<size_t>::max();
        size_t maxBytes = 0;
        size_t minDatagram = std::numeric_limits<size_t>::max();
        size_t maxDatagram = 0;
        size_t lastBaseState = 0;
        size_t lastLinkMatrices = 0;
        size_t lastAudioEvents = 0;
        size_t lastMatrixPacked = 0;
        size_t lastMatrixB64 = 0;
        size_t lastMatrixSlots = 0;
    };

    static UdpPoseTraceSummary sUdpPoseTraceSummary;
    if (sUdpPoseTraceSummary.count == 0) {
        sUdpPoseTraceSummary.firstSequence = sequence;
    }
    sUdpPoseTraceSummary.lastSequence = sequence;
    ++sUdpPoseTraceSummary.count;
    sUdpPoseTraceSummary.bytesTotal += totalDatagramBytes;
    sUdpPoseTraceSummary.rawTotal += rawBytes;
    sUdpPoseTraceSummary.compressedTotal += compressedBytes;
    sUdpPoseTraceSummary.chunksTotal += chunkCount;
    sUdpPoseTraceSummary.minChunks = std::min(sUdpPoseTraceSummary.minChunks, chunkCount);
    sUdpPoseTraceSummary.maxChunks = std::max(sUdpPoseTraceSummary.maxChunks, chunkCount);
    sUdpPoseTraceSummary.minBytes = std::min(sUdpPoseTraceSummary.minBytes, totalDatagramBytes);
    sUdpPoseTraceSummary.maxBytes = std::max(sUdpPoseTraceSummary.maxBytes, totalDatagramBytes);
    sUdpPoseTraceSummary.minDatagram =
        std::min(sUdpPoseTraceSummary.minDatagram, minDatagramBytes);
    sUdpPoseTraceSummary.maxDatagram =
        std::max(sUdpPoseTraceSummary.maxDatagram, maxDatagramBytes);
    sUdpPoseTraceSummary.lastBaseState = pose_base_state_bytes(state);
    sUdpPoseTraceSummary.lastLinkMatrices = json_field_msgpack_bytes(state, "link_matrices");
    sUdpPoseTraceSummary.lastAudioEvents = json_field_msgpack_bytes(state, "audio_events");
    sUdpPoseTraceSummary.lastMatrixPacked = sLastPoseMatrixPackedBytes;
    sUdpPoseTraceSummary.lastMatrixB64 = sLastPoseMatrixBase64Bytes;
    sUdpPoseTraceSummary.lastMatrixSlots = sLastPoseMatrixPresentSlots;

    constexpr uint32_t kUdpPoseTraceSummaryInterval = 150;
    if ((sUdpPoseTraceSummary.count % kUdpPoseTraceSummaryInterval) != 0) {
        return;
    }

    const double count = static_cast<double>(sUdpPoseTraceSummary.count);
    DuskLog.info(
        "MP_PACKET_TX_SUMMARY category=pose_udp type=pose samples={} seq_first={} "
        "seq_last={} avg_bytes={} min_bytes={} max_bytes={} avg_raw_msgpack={} "
        "avg_compressed={} avg_chunks={} min_chunks={} max_chunks={} datagram_min={} "
        "datagram_max={} last_base_state={} last_link_matrices={} last_audio_events={} "
        "last_matrix_packed={} last_matrix_b64={} last_matrix_slots={}",
        sUdpPoseTraceSummary.count, sUdpPoseTraceSummary.firstSequence,
        sUdpPoseTraceSummary.lastSequence,
        static_cast<double>(sUdpPoseTraceSummary.bytesTotal) / count,
        sUdpPoseTraceSummary.minBytes, sUdpPoseTraceSummary.maxBytes,
        static_cast<double>(sUdpPoseTraceSummary.rawTotal) / count,
        static_cast<double>(sUdpPoseTraceSummary.compressedTotal) / count,
        static_cast<double>(sUdpPoseTraceSummary.chunksTotal) / count,
        sUdpPoseTraceSummary.minChunks, sUdpPoseTraceSummary.maxChunks,
        sUdpPoseTraceSummary.minDatagram, sUdpPoseTraceSummary.maxDatagram,
        sUdpPoseTraceSummary.lastBaseState, sUdpPoseTraceSummary.lastLinkMatrices,
        sUdpPoseTraceSummary.lastAudioEvents, sUdpPoseTraceSummary.lastMatrixPacked,
        sUdpPoseTraceSummary.lastMatrixB64, sUdpPoseTraceSummary.lastMatrixSlots);
    sUdpPoseTraceSummary = UdpPoseTraceSummary{};
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
    const std::string bytes = serialize_json_line(message);
    trace_packet_tx(message, bytes.size());
    return send_bytes(sock, bytes);
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

std::string encode_manual_sync_full_state() {
    ManualSyncStatePacket packet = {};
    std::strncpy(packet.stageName, dComIfGp_getStartStageName(), sizeof(packet.stageName) - 1);
    packet.roomNo = static_cast<int8_t>(dComIfGp_roomControl_getStayNo());
    if (packet.roomNo < 0) {
        packet.roomNo = dComIfGp_getStartStageRoomNo();
    }
    packet.layer = dComIfGp_getStartStageLayer();
    // Manual sync is meant to land in the sender's current room, not replay
    // the sender's original entrance index from an older scene load.
    packet.startPoint = -1;

    std::string raw(kManualSyncStatePacketSize, '\0');
    std::memcpy(raw.data(), &packet, sizeof(packet));
    std::memcpy(raw.data() + sizeof(packet), &g_dComIfG_gameInfo.info, sizeof(dSv_info_c));

    const size_t bound = ZSTD_compressBound(raw.size());
    std::string compressed(bound, '\0');
    const size_t compressedSize =
        ZSTD_compress(compressed.data(), bound, raw.data(), raw.size(), 1);
    if (ZSTD_isError(compressedSize)) {
        DuskLog.warn("Multiplayer manual sync full-state compression failed: {}",
                     ZSTD_getErrorName(compressedSize));
        return "";
    }
    compressed.resize(compressedSize);
    DuskLog.info("Multiplayer encoded manual sync full state stage={} room={} layer={} bytes={}",
                 packet.stageName, static_cast<int>(packet.roomNo), static_cast<int>(packet.layer),
                 compressed.size());
    return absl::Base64Escape(compressed);
}

struct ProgressionCueDescriptor {
    const char* cueKey;
    // Stage the *replying* peer must actually be in -- checked against their
    // own live state right before they snapshot+reply to a sync_request, not
    // just the broadcast pose used to gate showing the prompt -- so the
    // snapshot reflects reality at the moment it's taken, not whatever was
    // true a network round-trip earlier.
    const char* expectedStage;
    // -1 = don't care, 0 = require human, 1 = require wolf. Lets a future cue
    // (e.g. entering Eldin/Lanayru twilight, which force-transforms the
    // player mid-cutscene) hold the reply until the transform has actually
    // landed, instead of racing it.
    int8_t requireWolf;
    const char* warpStage;
    int8_t warpRoom;
    int8_t warpLayer;
    int16_t warpStartPoint;
};

// Known-good canonical entrances for progression cues that warp a joining
// player into a new area, used instead of the peer's raw position/layer
// snapshot. Even after the stage-arrival gate in
// update_pending_progression_cue_arrivals() passes, the peer's live pose can
// still carry a layer that hasn't settled yet, which reloads into the wrong
// layer and replays the wrong cutscene. These exact stage/room/layer/point
// values are taken from known-working reference save states (gzsaves.json
// "hugo" -> Ordon Spring, "faron_twilight" -> Faron Woods), which load
// cleanly with no glitch.
constexpr ProgressionCueDescriptor kProgressionCueDescriptors[] = {
    {"sewers_complete", "F_SP104", -1, "F_SP104", 1, -1, 30},
    {"faron_twilight_entered", "F_SP108", -1, "F_SP108", 0, -1, 0},
};

const ProgressionCueDescriptor* find_progression_cue_descriptor(const std::string& cueKey) {
    if (cueKey.empty()) {
        return nullptr;
    }
    for (const auto& descriptor : kProgressionCueDescriptors) {
        if (cueKey == descriptor.cueKey) {
            return &descriptor;
        }
    }
    return nullptr;
}

// -1 = unknown (no player actor / not ALINK), 0 = human, 1 = wolf.
int8_t local_player_wolf_form() {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_ALINK_e) {
        return -1;
    }
    return static_cast<daAlink_c*>(player)->checkWolf() ? 1 : 0;
}

// Checked on the *replying* side right before it snapshots+sends its save
// state, so the snapshot reflects this client's own live state at the
// instant it's captured -- not the broadcast pose the requester gated the
// prompt on, which can be a few ticks stale by the time the request arrives.
bool is_local_state_ready_for_cue(const std::string& cueKey) {
    const ProgressionCueDescriptor* descriptor = find_progression_cue_descriptor(cueKey);
    if (descriptor == nullptr) {
        return true;
    }
    if (current_stage_name() != std::string_view(descriptor->expectedStage)) {
        return false;
    }
    if (descriptor->requireWolf >= 0 && local_player_wolf_form() != descriptor->requireWolf) {
        return false;
    }
    return true;
}

bool apply_manual_sync_full_state(const std::string& encoded) {
    std::string decoded;
    if (!absl::Base64Unescape(encoded, &decoded)) {
        DuskLog.warn("Multiplayer manual sync full state rejected: invalid base64");
        return false;
    }

    const unsigned long long decodedSize =
        ZSTD_getFrameContentSize(decoded.data(), decoded.size());
    if (decodedSize != kManualSyncStatePacketSize) {
        DuskLog.warn("Multiplayer manual sync full state rejected: size={}", decodedSize);
        return false;
    }

    std::string raw(static_cast<size_t>(decodedSize), '\0');
    const size_t result = ZSTD_decompress(raw.data(), raw.size(), decoded.data(), decoded.size());
    if (ZSTD_isError(result)) {
        DuskLog.warn("Multiplayer manual sync full-state decompression failed: {}",
                     ZSTD_getErrorName(result));
        return false;
    }

    ManualSyncStatePacket packet = {};
    std::memcpy(&packet, raw.data(), sizeof(packet));
    packet.stageName[sizeof(packet.stageName) - 1] = '\0';
    if (packet.stageName[0] == '\0') {
        DuskLog.warn("Multiplayer manual sync full state rejected: empty stage");
        return false;
    }

    const ProgressionCueDescriptor* descriptor =
        find_progression_cue_descriptor(sAwaitingManualSyncCueKey);
    if (descriptor != nullptr) {
        DuskLog.info("Multiplayer manual sync warp override cue={} stage={} room={} layer={} point={}",
                     sAwaitingManualSyncCueKey, descriptor->warpStage,
                     static_cast<int>(descriptor->warpRoom), static_cast<int>(descriptor->warpLayer),
                     static_cast<int>(descriptor->warpStartPoint));
        std::strncpy(packet.stageName, descriptor->warpStage, sizeof(packet.stageName) - 1);
        packet.stageName[sizeof(packet.stageName) - 1] = '\0';
        packet.roomNo = descriptor->warpRoom;
        packet.layer = descriptor->warpLayer;
        packet.startPoint = descriptor->warpStartPoint;
    }
    sAwaitingManualSyncCueKey.clear();

    toggleAutoSave(false);
    const u8 vibration = dComIfGs_getOptVibration();
    std::memcpy(&g_dComIfG_gameInfo.info, raw.data() + sizeof(packet), sizeof(dSv_info_c));
    g_dComIfG_gameInfo.info.getPlayer().getConfig().setVibration(vibration);
    sPendingManualSyncInfo = g_dComIfG_gameInfo.info;
    sPendingManualSyncVibration = vibration;

    const s16 spawnPoint = packet.startPoint == -4 ? -1 : packet.startPoint;
    if (spawnPoint == -1) {
        dComIfGs_setRestartRoomParam(packet.roomNo & 0x3F);
    }

    DuskLog.info("Multiplayer applying manual sync full state stage={} room={} layer={} point={}",
                 packet.stageName, static_cast<int>(packet.roomNo), static_cast<int>(packet.layer),
                 static_cast<int>(spawnPoint));
    dComIfGp_setNextStage(packet.stageName, spawnPoint, packet.roomNo, packet.layer, 0.0f, 0, 1,
                          0, 0, 1, 3);
    return true;
}

// Sent once, immediately after this side becomes "welcomed", so a peer that
// joined or reconnected mid-session catches up on durable state it missed
// instead of only receiving bits set after it connected. Lists only
// currently-set bits (not the full bit space) to keep the payload small;
// applying each one through the same setters the live hooks use (below)
// means a late-set local bit during the brief gap before this snapshot
// arrives is naturally preserved -- these are all monotonic OR-merges, so
// receiving a bit twice or in any order is harmless.
void send_save_snapshot(DirectPeer* peer = nullptr, const std::string& targetClientId = "",
                        bool manualSync = false) {
    json eventFlags = json::array();
    for (int i = 0; i < 256 * 8; ++i) {
        const uint16_t flag = static_cast<uint16_t>(i);
        if (!is_unsynced_event_bit(flag) && dComIfGs_isEventBit(flag)) {
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
            if (!is_unsynced_switch_bit(s, i) && dComIfGs_isStageSwitch(s, i)) {
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
    json lightDropGetFlags = json::array();
    for (int area = 0; area < 4; ++area) {
        const u8 num = dComIfGs_getLightDropNum(static_cast<u8>(area));
        if (num > 0) {
            lightDropCounts.push_back({{"area", area}, {"count", num}});
        }
        if (area < 3 && dComIfGs_isLightDropGetFlag(static_cast<u8>(area))) {
            lightDropGetFlags.push_back(area);
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
        {"light_drop_get_flags", lightDropGetFlags},
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
        {"rupees", dComIfGs_getRupee()},
    };
    if (!targetClientId.empty()) {
        snapshot["target_client_id"] = targetClientId;
    }
    if (manualSync) {
        snapshot["manual_sync"] = true;
        const std::string fullState = encode_manual_sync_full_state();
        if (!fullState.empty()) {
            snapshot["full_state"] = fullState;
        }
    }
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

// Retries deferred sync_request replies (see is_local_state_ready_for_cue())
// until this client's own live state actually satisfies the cue, or the
// timeout is hit. Ticked alongside the requester-side gates so both ends of
// a progression-cue sync agree on "ready" using fresh state, not a stale
// broadcast pose.
void update_pending_sync_replies() {
    if (sPendingSyncReplies.empty()) {
        return;
    }
    if (dComIfGp_getStageStagInfo() == nullptr || dComIfGp_event_runCheck()) {
        return;
    }

    for (auto it = sPendingSyncReplies.begin(); it != sPendingSyncReplies.end();) {
        ++it->waitTicks;
        const bool ready = is_local_state_ready_for_cue(it->cueKey);
        const bool timedOut = it->waitTicks >= kPendingSyncReplyTimeoutTicks;
        if (!ready && !timedOut) {
            ++it;
            continue;
        }
        if (!ready && timedOut) {
            DuskLog.warn(
                "Multiplayer deferred sync reply timed out waiting for local state cue={}, "
                "replying anyway",
                it->cueKey);
        }

        if (it->isDirectPeer) {
            const auto peerIt = sSession.directPeers.find(it->peerKey);
            if (peerIt != sSession.directPeers.end()) {
                send_save_snapshot(&peerIt->second, "", true);
                DuskLog.info(
                    "Multiplayer replied to deferred direct manual sync request peer={} cue={}",
                    it->peerKey, it->cueKey);
            }
        } else if (sSession.mode == NetworkMode::RelayHarness && !it->targetClientId.empty()) {
            send_save_snapshot(nullptr, it->targetClientId, true);
            DuskLog.info("Multiplayer replied to deferred relay manual sync request peer={} cue={}",
                         it->targetClientId, it->cueKey);
        } else {
            send_save_snapshot(nullptr, "", true);
            DuskLog.info("Multiplayer replied to deferred manual sync request cue={}", it->cueKey);
        }
        it = sPendingSyncReplies.erase(it);
    }
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

void append_pack_bytes(std::string& out, const void* data, size_t size) {
    out.append(reinterpret_cast<const char*>(data), size);
}

void append_pack_u8(std::string& out, uint8_t value) {
    append_pack_bytes(out, &value, sizeof(value));
}

void append_pack_u16(std::string& out, uint16_t value) {
    append_pack_bytes(out, &value, sizeof(value));
}

void append_pack_i16(std::string& out, int16_t value) {
    append_pack_bytes(out, &value, sizeof(value));
}

constexpr float kPackedMatrixBasisRange = 1.0f;
constexpr bool kUseQuantizedMatrixWire = true;
constexpr bool kUseBinaryMatrixUdpWire = true;
constexpr uint8_t kPackedMatrixModeFloat32 = 0;
constexpr uint8_t kPackedMatrixModeQuantizedBasis = 1;
constexpr uint64_t kFnv1a64Offset = 14695981039346656037ull;
constexpr uint64_t kFnv1a64Prime = 1099511628211ull;

uint64_t hash_pack_bytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnv1a64Prime;
    }
    return hash;
}

int16_t quantize_matrix_basis(float value) {
    const float clamped =
        std::clamp(value, -kPackedMatrixBasisRange, kPackedMatrixBasisRange);
    const float normalized = clamped / kPackedMatrixBasisRange;
    return static_cast<int16_t>(std::lround(normalized * 32767.0f));
}

float dequantize_matrix_basis(int16_t value) {
    return (static_cast<float>(value) / 32767.0f) * kPackedMatrixBasisRange;
}

bool matrix_basis_can_quantize(CMtxP matrix, float* maxAbsOut = nullptr) {
    float maxAbs = 0.0f;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float value = matrix[row][col];
            if (!std::isfinite(value)) {
                if (maxAbsOut != nullptr) {
                    *maxAbsOut = maxAbs;
                }
                return false;
            }
            maxAbs = std::max(maxAbs, std::abs(value));
            if (maxAbs > kPackedMatrixBasisRange) {
                if (maxAbsOut != nullptr) {
                    *maxAbsOut = maxAbs;
                }
                return false;
            }
        }
    }
    if (maxAbsOut != nullptr) {
        *maxAbsOut = maxAbs;
    }
    return true;
}

uint64_t hash_pack_matrix(uint64_t hash, CMtxP matrix) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (col == 3) {
                const float value = matrix[row][col];
                hash = hash_pack_bytes(hash, &value, sizeof(value));
            } else {
                const int16_t value = quantize_matrix_basis(matrix[row][col]);
                hash = hash_pack_bytes(hash, &value, sizeof(value));
            }
        }
    }
    return hash;
}

uint64_t hash_weight_matrices(J3DModel* model, uint16_t weightCount) {
    uint64_t hash = kFnv1a64Offset;
    hash = hash_pack_bytes(hash, &weightCount, sizeof(weightCount));
    for (u16 i = 0; i < weightCount; ++i) {
        hash = hash_pack_matrix(hash, model->getWeightAnmMtx(i));
    }
    return hash;
}

void append_pack_matrix(std::string& out, CMtxP matrix, MatrixPackSlotMetrics* metrics = nullptr) {
    if (!kUseQuantizedMatrixWire) {
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                const float value = matrix[row][col];
                append_pack_bytes(out, &value, sizeof(value));
            }
        }
        return;
    }

    if (metrics != nullptr) {
        float basisMaxAbs = 0.0f;
        matrix_basis_can_quantize(matrix, &basisMaxAbs);
        metrics->basisMaxAbs = std::max(metrics->basisMaxAbs, basisMaxAbs);
        ++metrics->basisQuantizedMatrices;
    }

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            append_pack_i16(out, quantize_matrix_basis(matrix[row][col]));
        }
    }
    for (int row = 0; row < 3; ++row) {
        const float value = matrix[row][3];
        append_pack_bytes(out, &value, sizeof(value));
    }
}

MatrixPackSlotMetrics append_pack_model(std::string& out, const MatrixPackSlotInput& slot) {
    MatrixPackSlotMetrics metrics{};
    metrics.name = slot.name;
    const size_t startBytes = out.size();
    J3DModel* model = slot.model;
    if (model == nullptr || model->getModelData() == nullptr) {
        append_pack_u8(out, 0);
        metrics.bytes = out.size() - startBytes;
        return metrics;
    }

    J3DModelData* data = model->getModelData();
    const u16 jointCount = data->getJointNum();
    const u16 weightCount = data->getWEvlpMtxNum();
    metrics.present = true;
    metrics.jointCount = jointCount;
    metrics.weightCount = weightCount;
    if (weightCount > 0) {
        metrics.weightHash = hash_weight_matrices(model, weightCount);
        MatrixWeightTraceState& trace = sMatrixWeightTraceBySlot[metrics.name];
        metrics.weightChanged =
            !trace.initialized || trace.jointCount != jointCount ||
            trace.weightCount != weightCount || trace.weightHash != metrics.weightHash;
        if (metrics.weightChanged) {
            trace.stableFrames = 0;
        } else {
            ++trace.stableFrames;
        }
        trace.initialized = true;
        trace.jointCount = jointCount;
        trace.weightCount = weightCount;
        trace.weightHash = metrics.weightHash;
        metrics.weightStableFrames = trace.stableFrames;
    }

    append_pack_u8(out, 1);
    append_pack_u16(out, jointCount);
    append_pack_u16(out, weightCount);
    append_pack_matrix(out, model->getBaseTRMtx(), &metrics);
    for (u16 i = 0; i < jointCount; ++i) {
        append_pack_matrix(out, model->getAnmMtx(i), &metrics);
    }
    for (u16 i = 0; i < weightCount; ++i) {
        append_pack_matrix(out, model->getWeightAnmMtx(i), &metrics);
    }
    metrics.bytes = out.size() - startBytes;
    return metrics;
}

json link_matrix_pack_to_json(std::initializer_list<MatrixPackSlotInput> models,
                              int midnaHairShape) {
    std::string packed;
    packed.reserve(32 * 1024);
    packed.append("DMPM", 4);
    append_pack_u8(packed, 1);
    append_pack_u8(packed, static_cast<uint8_t>(models.size()));
    size_t presentSlots = 0;
    sLastPoseMatrixSlotMetrics.clear();
    sLastPoseMatrixSlotMetrics.reserve(models.size());
    for (const MatrixPackSlotInput& slot : models) {
        MatrixPackSlotMetrics metrics = append_pack_model(packed, slot);
        if (metrics.present) {
            ++presentSlots;
        }
        sLastPoseMatrixSlotMetrics.push_back(metrics);
    }
    const std::string encoded = absl::Base64Escape(packed);
    sLastPoseMatrixPackedBytes = packed.size();
    sLastPoseMatrixBase64Bytes = encoded.size();
    sLastPoseMatrixPresentSlots = presentSlots;

    static uint32_t sMatrixPackTraceTicks = 0;
    if (packet_trace_enabled() && (sMatrixPackTraceTicks++ % 150) == 0) {
        DuskLog.info(
            "MP_MATRIX_PACK total_packed={} total_b64={} slots={} present={} format={}",
            sLastPoseMatrixPackedBytes, sLastPoseMatrixBase64Bytes, models.size(),
            presentSlots, kUseQuantizedMatrixWire ? "qrot16_trans32_v1" : "f32_pack_v1");
        for (const MatrixPackSlotMetrics& metrics : sLastPoseMatrixSlotMetrics) {
            if (!metrics.present) {
                continue;
            }
            DuskLog.info(
                "MP_MATRIX_SLOT name={} bytes={} joints={} weights={} matrices={} "
                "basis_q={} basis_f32={} basis_max_abs={} weight_hash={} "
                "weight_changed={} weight_stable={}",
                metrics.name, metrics.bytes, metrics.jointCount, metrics.weightCount,
                1 + static_cast<uint32_t>(metrics.jointCount) +
                    static_cast<uint32_t>(metrics.weightCount),
                metrics.basisQuantizedMatrices, metrics.basisFloatMatrices, metrics.basisMaxAbs,
                metrics.weightHash, metrics.weightChanged, metrics.weightStableFrames);
        }
    }

    return {
        {"format", kUseQuantizedMatrixWire ? "qrot16_trans32_v1" : "f32_pack_v1"},
        {"data", encoded},
        {"midna_hair_shape", midnaHairShape},
    };
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

struct LocalMidnaVisualState {
    bool body = false;
    bool mask = false;
    bool hand = false;
    bool hair = false;
    bool shadowForm = false;
    bool glow = false;
};

LocalMidnaVisualState detect_midna_visual_state(daAlink_c* link, bool isWolf) {
    LocalMidnaVisualState state;
    if (link == nullptr) {
        return state;
    }

    daMidna_c* midna = daPy_py_c::getMidnaActor();
    if (midna == nullptr) {
        return state;
    }

    const bool playerNoDrawSuppressed =
        !midna->checkStateFlg1(static_cast<daMidna_c::daMidna_FLG1>(
            daMidna_c::FLG1_SHADOW_MODEL_DRAW_DEMO_FORCE | daMidna_c::FLG1_UNK_1)) &&
        link->checkPlayerNoDraw() &&
        !midna->checkStateFlg0(static_cast<daMidna_c::daMidna_FLG0>(
            daMidna_c::FLG0_TAG_WAIT | daMidna_c::FLG0_UNK_100));

    const bool globallySuppressed = midna->checkStateFlg1(daMidna_c::FLG1_UNK_20) ||
                                    playerNoDrawSuppressed;
    if (globallySuppressed) {
        return state;
    }

    state.shadowForm = midna->checkShadowModelDraw() && !midna->checkNoDrawState() &&
                       !midna->checkShadowNoDraw() && midna->getShadowModel() != nullptr;
    if (state.shadowForm) {
        state.body = true;
        state.mask = !midna->checkNoMaskDraw() && midna->getShadowMaskModel() != nullptr;
        state.hand = midna->getShadowHandModel() != nullptr;
        state.hair = !midna->checkStateFlg1(daMidna_c::FLG1_UNK_40) &&
                     midna->getShadowHairHandModel() != nullptr;
        state.glow = midna->getGokouModel() != nullptr;
        return state;
    }

    if (!isWolf || link->getMidnaModel() == nullptr) {
        return state;
    }

    state.body = !midna->checkNoDrawState() && !playerNoDrawSuppressed &&
                 !midna->checkNoDraw() && !midna->checkShadowModelDrawDemoForce();
    state.mask = state.body && !midna->checkNoMaskDraw() && link->getMidnaMaskModel() != nullptr;
    state.hand = state.body && link->getMidnaHandModel() != nullptr;
    state.hair = state.body && link->getMidnaHairHandModel() != nullptr &&
                 !midna->checkStateFlg1(static_cast<daMidna_c::daMidna_FLG1>(
                     daMidna_c::FLG1_UNK_40 | daMidna_c::FLG1_UNK_10));
    return state;
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
    const bool isTransforming =
        link->mProcID == daAlink_c::PROC_METAMORPHOSE ||
        link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY;

    if (link->mpLinkModel == nullptr) {
        return false;
    }

    const bool isWolf = static_cast<bool>(link->checkWolf());
    if (!sDummyModelEnabled && !isWolf) {
        return true;
    }

    const bool includeHumanCoreParts = !isWolf;
    const bool includeHumanParts = includeHumanCoreParts && !isTransforming;
    const LocalMidnaVisualState midnaVisual =
        isTransforming ? LocalMidnaVisualState() : detect_midna_visual_state(link, isWolf);
    J3DModel* arrowModel = nullptr;
    J3DModel* itemActorModel = nullptr;
    J3DModel* rideActorModel = nullptr;
    int itemActorKind = REMOTE_ITEM_ACTOR_NONE;
    int itemActorBombExTime = -1;
    int itemActorBombFlash = -1;
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
            if (fopAcM_GetName(itemActor) == fpcNm_NBOMB_e) {
                daNbomb_c* bomb = static_cast<daNbomb_c*>(itemActor);
                itemActorBombExTime = bomb->getExTime();
                itemActorBombFlash = calc_remote_bomb_flash(bomb, link);
                static uint32_t sBombFuseTxLogCount = 0;
                if (sBombFuseTxLogCount < 24 || itemActorBombExTime <= 20) {
                    ++sBombFuseTxLogCount;
                    DuskLog.info("Multiplayer bomb fuse tx kind={} ex_time={} flash={} matrix={}",
                                 itemActorKind, itemActorBombExTime, itemActorBombFlash,
                                 itemActorModel != nullptr);
                }
            }
        }

        fopAc_ac_c* rideActor = link->mRideAcKeep.getActor();
        rideActorKind = detect_ride_actor_kind(rideActor);
        if (rideActorKind == REMOTE_RIDE_ACTOR_SPINNER) {
            rideActorModel = rideActor->model;
        }
    }

    daMidna_c* midnaActor = daPy_py_c::getMidnaActor();
    J3DModel* midnaModel = nullptr;
    J3DModel* midnaMaskModel = nullptr;
    J3DModel* midnaHandModel = nullptr;
    J3DModel* midnaHairModel = nullptr;
    J3DModel* midnaGlowModel = nullptr;
    if (midnaVisual.shadowForm && midnaActor != nullptr) {
        midnaModel = midnaVisual.body ? midnaActor->getShadowModel() : nullptr;
        midnaMaskModel = midnaVisual.mask ? midnaActor->getShadowMaskModel() : nullptr;
        midnaHandModel = midnaVisual.hand ? midnaActor->getShadowHandModel() : nullptr;
        midnaHairModel = midnaVisual.hair ? midnaActor->getShadowHairHandModel() : nullptr;
        midnaGlowModel = midnaVisual.glow ? midnaActor->getGokouModel() : nullptr;
    } else {
        midnaModel = midnaVisual.body ? link->getMidnaModel() : nullptr;
        midnaMaskModel = midnaVisual.mask ? link->getMidnaMaskModel() : nullptr;
        midnaHandModel = midnaVisual.hand ? link->getMidnaHandModel() : nullptr;
        midnaHairModel = midnaVisual.hair ? link->getMidnaHairHandModel() : nullptr;
    }

    const int midnaHairShape =
        (isWolf || midnaVisual.shadowForm) ? visible_material_shape_index(midnaHairModel, 3, 0) : 0;

    state["link_matrices"] = link_matrix_pack_to_json(
        {
            {"body", link->mpLinkModel},
            {"hat", nullptr},
            {"face", includeHumanCoreParts ? link->mpLinkFaceModel : nullptr},
            {"hand", includeHumanCoreParts ? link->mpLinkHandModel : nullptr},
            {"sword", includeHumanParts ? link->mSwordModel : nullptr},
            {"sheath", includeHumanParts ? link->mSheathModel : nullptr},
            {"shield", includeHumanParts ? link->mShieldModel : nullptr},
            {"held_item", includeHumanParts ? link->mHeldItemModel : nullptr},
            {"hook_tip", includeHumanParts ? link->mpHookTipModel : nullptr},
            {"hook_sub_item", includeHumanParts ? link->field_0x0710 : nullptr},
            {"hook_sub_tip", includeHumanParts ? link->field_0x0714 : nullptr},
            {"arrow", includeHumanParts ? arrowModel : nullptr},
            {"kantera", includeHumanParts ? link->mpKanteraModel : nullptr},
            {"kantera_glow", includeHumanParts ? link->mpKanteraGlowModel : nullptr},
            {"item_actor", includeHumanParts ? itemActorModel : nullptr},
            {"ride_actor", includeHumanParts ? rideActorModel : nullptr},
            {"midna", midnaModel},
            {"midna_mask", midnaMaskModel},
            {"midna_hand", midnaHandModel},
            {"midna_hair", midnaHairModel},
            {"midna_glow", midnaGlowModel},
        },
        midnaHairShape);

    state["equip_item"] = static_cast<int>(link->mEquipItem);
    state["sword_variant"] = detect_sword_variant(link);
    state["shield_variant"] = detect_shield_variant();
    state["clothes_variant"] = detect_clothes_variant();
    state["sword_draw"] = static_cast<bool>(link->checkSwordDraw());
    state["shield_draw"] = static_cast<bool>(link->checkShieldDraw());
    state["sword_out"] = !isWolf && link->mEquipItem == 0x103;
    state["midna_draw"] = midnaVisual.body;
    state["midna_mask_draw"] = midnaVisual.mask;
    state["midna_hand_draw"] = midnaVisual.hand;
    state["midna_hair_draw"] = midnaVisual.hair;
    state["midna_shadow_form"] = midnaVisual.shadowForm;
    state["heavy_boots"] = !isWolf && static_cast<bool>(link->checkEquipHeavyBoots());
    state["item_draw"] = !isWolf && static_cast<bool>(link->checkItemDraw());
    state["kantera_draw"] =
        !isWolf && (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) ||
                    link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_20000));
    state["item_actor_kind"] = itemActorKind;
    if (itemActorBombExTime >= 0) {
        state["item_actor_bomb_ex_time"] = itemActorBombExTime;
        state["item_actor_bomb_flash"] = itemActorBombFlash;
    }
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

bool read_pack_bytes(const std::string& source, size_t& cursor, void* out, size_t size) {
    if (cursor > source.size() || size > source.size() - cursor) {
        return false;
    }

    std::memcpy(out, source.data() + cursor, size);
    cursor += size;
    return true;
}

bool read_pack_u8(const std::string& source, size_t& cursor, uint8_t& out) {
    return read_pack_bytes(source, cursor, &out, sizeof(out));
}

bool read_pack_u16(const std::string& source, size_t& cursor, uint16_t& out) {
    return read_pack_bytes(source, cursor, &out, sizeof(out));
}

bool read_pack_i16(const std::string& source, size_t& cursor, int16_t& out) {
    return read_pack_bytes(source, cursor, &out, sizeof(out));
}

bool read_pack_float_array(const std::string& source, size_t& cursor,
                           std::array<float, 12>& out) {
    return read_pack_bytes(source, cursor, out.data(), out.size() * sizeof(float));
}

bool read_pack_quantized_matrix(const std::string& source, size_t& cursor, float* out) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const size_t index = static_cast<size_t>(row * 4 + col);
            int16_t value = 0;
            if (!read_pack_i16(source, cursor, value)) {
                return false;
            }
            out[index] = dequantize_matrix_basis(value);
        }
    }
    for (int row = 0; row < 3; ++row) {
        const size_t index = static_cast<size_t>(row * 4 + 3);
        if (!read_pack_bytes(source, cursor, &out[index], sizeof(float))) {
            return false;
        }
    }
    return true;
}

bool read_pack_safe_quantized_matrix(const std::string& source, size_t& cursor, float* out) {
    uint8_t mode = 0;
    if (!read_pack_u8(source, cursor, mode)) {
        return false;
    }
    if (mode == kPackedMatrixModeFloat32) {
        return read_pack_bytes(source, cursor, out, sizeof(float) * 12);
    }
    if (mode != kPackedMatrixModeQuantizedBasis) {
        return false;
    }
    return read_pack_quantized_matrix(source, cursor, out);
}

bool read_pack_quantized_array(const std::string& source, size_t& cursor,
                               std::array<float, 12>& out) {
    return read_pack_quantized_matrix(source, cursor, out.data());
}

bool read_pack_safe_quantized_array(const std::string& source, size_t& cursor,
                                    std::array<float, 12>& out) {
    return read_pack_safe_quantized_matrix(source, cursor, out.data());
}

bool read_pack_quantized_vector(const std::string& source, size_t& cursor, size_t matrixCount,
                                std::vector<float>& out) {
    if (matrixCount > 64 * 1024) {
        return false;
    }

    out.resize(matrixCount * 12);
    for (size_t i = 0; i < matrixCount; ++i) {
        if (!read_pack_quantized_matrix(source, cursor, out.data() + i * 12)) {
            out.clear();
            return false;
        }
    }
    return true;
}

bool read_pack_safe_quantized_vector(const std::string& source, size_t& cursor,
                                     size_t matrixCount, std::vector<float>& out) {
    if (matrixCount > 64 * 1024) {
        return false;
    }

    out.resize(matrixCount * 12);
    for (size_t i = 0; i < matrixCount; ++i) {
        if (!read_pack_safe_quantized_matrix(source, cursor, out.data() + i * 12)) {
            out.clear();
            return false;
        }
    }
    return true;
}

bool read_pack_float_vector(const std::string& source, size_t& cursor, size_t count,
                            std::vector<float>& out) {
    if (count > 64 * 1024) {
        return false;
    }

    out.resize(count);
    if (count == 0) {
        return true;
    }

    if (!read_pack_bytes(source, cursor, out.data(), count * sizeof(float))) {
        out.clear();
        return false;
    }
    return true;
}

bool parse_packed_model_matrices(const std::string& packed, size_t& cursor,
                                 RemoteModelMatrixSnapshot& snapshot, bool quantized,
                                 bool safeQuantized) {
    snapshot = {};

    uint8_t present = 0;
    if (!read_pack_u8(packed, cursor, present)) {
        return false;
    }
    if (present == 0) {
        return true;
    }
    if (present != 1) {
        return false;
    }

    uint16_t jointCount = 0;
    uint16_t weightCount = 0;
    if (!read_pack_u16(packed, cursor, jointCount) ||
        !read_pack_u16(packed, cursor, weightCount))
    {
        return false;
    }

    snapshot.jointCount = jointCount;
    snapshot.weightCount = weightCount;
    if (safeQuantized) {
        if (!read_pack_safe_quantized_array(packed, cursor, snapshot.base) ||
            !read_pack_safe_quantized_vector(packed, cursor, jointCount, snapshot.joints) ||
            !read_pack_safe_quantized_vector(packed, cursor, weightCount, snapshot.weights))
        {
            snapshot = {};
            return false;
        }
    } else if (quantized) {
        if (!read_pack_quantized_array(packed, cursor, snapshot.base) ||
            !read_pack_quantized_vector(packed, cursor, jointCount, snapshot.joints) ||
            !read_pack_quantized_vector(packed, cursor, weightCount, snapshot.weights))
        {
            snapshot = {};
            return false;
        }
    } else {
        if (!read_pack_float_array(packed, cursor, snapshot.base) ||
            !read_pack_float_vector(packed, cursor, static_cast<size_t>(jointCount) * 12,
                                    snapshot.joints) ||
            !read_pack_float_vector(packed, cursor, static_cast<size_t>(weightCount) * 12,
                                    snapshot.weights))
        {
            snapshot = {};
            return false;
        }
    }

    snapshot.valid = true;
    return true;
}

RemoteLinkMatrixSnapshot parse_packed_link_matrices(const json& source) {
    RemoteLinkMatrixSnapshot snapshot;
    if (!source.is_object()) {
        return snapshot;
    }

    const std::string format = source.value("format", "");
    const bool quantized =
        format == "qrot16_trans32_v1" || format == "qrot16_trans32_bin_v1";
    const bool safeQuantized =
        format == "qbasis16_trans32_safe_v1" || format == "qbasis16_trans32_safe_bin_v1";
    const bool floatPacked = format == "f32_pack_v1" || format == "f32_pack_bin_v1";
    if (!safeQuantized && !quantized && !floatPacked) {
        return snapshot;
    }

    std::string packed;
    const auto dataIt = source.find("data");
    if (dataIt == source.end()) {
        return snapshot;
    }
    if (dataIt->is_binary()) {
        const auto& bytes = dataIt->get_binary();
        packed.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    } else if (dataIt->is_string()) {
        const std::string encoded = dataIt->get<std::string>();
        if (encoded.empty() || !absl::Base64Unescape(encoded, &packed)) {
            return snapshot;
        }
    } else {
        return snapshot;
    }

    size_t cursor = 0;
    char magic[4]{};
    uint8_t version = 0;
    uint8_t slotCount = 0;
    if (!read_pack_bytes(packed, cursor, magic, sizeof(magic)) ||
        std::memcmp(magic, "DMPM", 4) != 0 ||
        !read_pack_u8(packed, cursor, version) ||
        !read_pack_u8(packed, cursor, slotCount) ||
        version != 1 || slotCount != 21)
    {
        return {};
    }

    RemoteModelMatrixSnapshot* slots[] = {
        &snapshot.body,
        &snapshot.hat,
        &snapshot.face,
        &snapshot.hand,
        &snapshot.sword,
        &snapshot.sheath,
        &snapshot.shield,
        &snapshot.heldItem,
        &snapshot.hookTip,
        &snapshot.hookSubItem,
        &snapshot.hookSubTip,
        &snapshot.arrow,
        &snapshot.kantera,
        &snapshot.kanteraGlow,
        &snapshot.itemActor,
        &snapshot.rideActor,
        &snapshot.midna,
        &snapshot.midnaMask,
        &snapshot.midnaHand,
        &snapshot.midnaHair,
        &snapshot.midnaGlow,
    };

    for (RemoteModelMatrixSnapshot* slot : slots) {
        if (!parse_packed_model_matrices(packed, cursor, *slot, quantized, safeQuantized)) {
            return {};
        }
    }
    if (cursor != packed.size()) {
        return {};
    }

    snapshot.midnaHairShape = source.value("midna_hair_shape", 0);
    if (snapshot.midnaHairShape < 0 || snapshot.midnaHairShape > 2) {
        snapshot.midnaHairShape = 0;
    }
    snapshot.valid = snapshot.body.valid;
    return snapshot;
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

    snapshot = parse_packed_link_matrices(*it);
    if (snapshot.valid) {
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
    snapshot.midnaGlow = parse_model_matrices(it->value("midna_glow", json::object()));
    snapshot.midnaHairShape = it->value("midna_hair_shape", 0);
    if (snapshot.midnaHairShape < 0 || snapshot.midnaHairShape > 2) {
        snapshot.midnaHairShape = 0;
    }
    snapshot.valid = snapshot.body.valid;
    return snapshot;
}

template <size_t N>
void parse_int16_array_field(const json& state, const char* key, std::array<int16_t, N>& out) {
    const auto it = state.find(key);
    if (it == state.end() || !it->is_array()) {
        return;
    }

    const size_t count = std::min(N, it->size());
    for (size_t i = 0; i < count; ++i) {
        const int value = (*it)[i].is_number_integer() ? (*it)[i].get<int>() : 0;
        out[i] = static_cast<int16_t>(std::clamp(value, -32768, 32767));
    }
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
    if (is_stage_dependent_message_type(type) && is_stage_load_unsafe_for_multiplayer()) {
        sPendingStageMessages.push_back(routedMessage);
        if (sender != nullptr && should_forward_peer_message(type) &&
            !(type == "save_snapshot" && routedMessage.value("manual_sync", false)))
        {
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
        sSession.clientId = message.value("client_id", "");

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
        if (message.value("manual_sync", false) && message.contains("full_state")) {
            if (apply_manual_sync_full_state(message.value("full_state", ""))) {
                sManualSyncReloadPending = false;
                DuskLog.info("Multiplayer applied manual sync full-state snapshot from peer");
            }
            return;
        }

        sApplyingRemoteSaveBit = true;
        for (const json& flag : message.value("event_flags", json::array())) {
            const uint16_t flagValue = static_cast<uint16_t>(flag.get<int>());
            if (is_unsynced_event_bit(flagValue)) {
                DuskLog.info("Multiplayer snapshot skipped unsynced event bit flag={}",
                             flagValue);
                continue;
            }
            dComIfGs_onEventBit(flagValue);
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
                const int flagValue = flag.get<int>();
                if (is_unsynced_switch_bit(stage, flagValue)) {
                    DuskLog.info("Multiplayer snapshot skipped unsynced switch bit stage={} flag={}",
                                 stage, flagValue);
                    continue;
                }
                apply_remote_switch_bit(stage, flagValue);
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
        for (const json& entry : message.value("light_drop_get_flags", json::array())) {
            const int area = entry.get<int>();
            if (area >= 0 && area < 3) {
                dComIfGs_onLightDropGetFlag(static_cast<u8>(area));
                dMeter2Info_setLightDropGetFlag(area, 0xFF);
                DuskLog.info("Multiplayer snapshot light drop get flag area={}", area);
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
        const int remoteRupees = message.value("rupees", -1);
        if (remoteRupees >= 0 && remoteRupees <= dComIfGs_getRupeeMax()) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_setRupee(static_cast<u16>(remoteRupees));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer snapshot rupees set to {}", remoteRupees);
        }
        if (message.value("manual_sync", false)) {
            sManualSyncReloadPending = true;
            DuskLog.info("Multiplayer manual sync snapshot applied; queued room restart");
        }
        DuskLog.info("Multiplayer applied save snapshot from peer");
    } else if (type == "sync_request") {
        if (dComIfGp_getStageStagInfo() == nullptr || dComIfGp_event_runCheck()) {
            DuskLog.warn("Multiplayer manual sync request ignored while stage/event is not ready");
        } else {
            const std::string cueKey = routedMessage.value("cue_key", "");
            if (!is_local_state_ready_for_cue(cueKey)) {
                DuskLog.info(
                    "Multiplayer manual sync request deferred cue={} (local state not ready yet)",
                    cueKey);
                PendingSyncReply pending;
                pending.active = true;
                pending.cueKey = cueKey;
                if (sender != nullptr) {
                    pending.isDirectPeer = true;
                    pending.peerKey = sender->id;
                } else {
                    pending.targetClientId = routedMessage.value("client_id", "");
                }
                sPendingSyncReplies.push_back(std::move(pending));
            } else if (sender != nullptr) {
                send_save_snapshot(sender, "", true);
                DuskLog.info("Multiplayer replied to direct manual sync request peer={}", sender->id);
            } else {
                const std::string requesterId = routedMessage.value("client_id", "");
                if (sSession.mode == NetworkMode::RelayHarness && !requesterId.empty()) {
                    send_save_snapshot(nullptr, requesterId, true);
                    DuskLog.info("Multiplayer replied to relay manual sync request peer={}",
                                 requesterId);
                } else {
                    send_save_snapshot(nullptr, "", true);
                    DuskLog.info("Multiplayer replied to manual sync request");
                }
            }
        }
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
        pose.manualSyncReady = state.value("manual_sync_ready", false);
        pose.underFrame = state.value("under_frame", 0.0f);
        pose.underBck0 = state.value("under_bck0", 0);
        pose.underFrame0 = state.value("under_frame0", pose.underFrame);
        pose.underRate0 = state.value("under_rate0", 1.0f);
        pose.upperBck2 = state.value("upper_bck2", 0);
        pose.upperFrame2 = state.value("upper_frame2", 0.0f);
        pose.upperRate2 = state.value("upper_rate2", 1.0f);
        parse_int16_array_field(state, "hat_rot_a", pose.hatRotA);
        parse_int16_array_field(state, "hat_rot_b", pose.hatRotB);
        parse_int16_array_field(state, "hat_swing", pose.hatSwing);
        pose.hatShapeY = state.value("hat_shape_y", 0);
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
        pose.midnaDraw = state.value("midna_draw", false);
        pose.midnaMaskDraw = state.value("midna_mask_draw", false);
        pose.midnaHandDraw = state.value("midna_hand_draw", false);
        pose.midnaHairDraw = state.value("midna_hair_draw", false);
        pose.midnaShadowForm = state.value("midna_shadow_form", false);
        pose.heavyBoots = state.value("heavy_boots", false);
        pose.itemDraw = state.value("item_draw", false);
        pose.kanteraDraw = state.value("kantera_draw", false);
        pose.itemActorKind = state.value("item_actor_kind", REMOTE_ITEM_ACTOR_NONE);
        pose.itemActorBombExTime = state.value("item_actor_bomb_ex_time", -1);
        pose.itemActorBombFlash = state.value("item_actor_bomb_flash", -1);
        pose.rideActorKind = state.value("ride_actor_kind", REMOTE_RIDE_ACTOR_NONE);
        pose.linkMatrices = parse_link_matrices(state);
        pose.audioEvents = parse_audio_events(state);
        if (sDummyTraceEnabled) {
            const bool hadExisting = existing != sSession.peerPoses.end() && existing->second.valid;
            const uint32_t previousSequence = hadExisting ? existing->second.sequence : 0;
            const uint32_t sequenceDelta = hadExisting ? sequence - previousSequence : 0;
            const f32 previousUnderFrame0 = hadExisting ? existing->second.underFrame0 : pose.underFrame0;
            const f32 previousUpperFrame2 = hadExisting ? existing->second.upperFrame2 : pose.upperFrame2;
            static uint32_t sDummyTraceRxCount = 0;
            const bool logGap = hadExisting && sequenceDelta > 1;
            if (logGap || (sDummyTraceRxCount++ % 30) == 0) {
                DuskLog.info(
                    "Multiplayer dummy trace rx peer={} seq={} seq_delta={} prev_age={} "
                    "stage={} room={} matrix={} proc={} under_bck={} under_frame={} "
                    "under_delta={} upper_bck={} upper_frame={} upper_delta={}",
                    peerId, pose.sequence, sequenceDelta,
                    hadExisting ? existing->second.ageTicks : 0, pose.stage, pose.room,
                    pose.linkMatrices.valid, pose.procId, pose.underBck0, pose.underFrame0,
                    pose.underFrame0 - previousUnderFrame0, pose.upperBck2, pose.upperFrame2,
                    pose.upperFrame2 - previousUpperFrame2);
            }
        }
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
        maybe_show_progression_sync_prompt_for_pose_stage(
            peerId, pose.stage, state.value("final_ganondorf_ready", false));
    } else if (type == "event_bit") {
        const uint16_t flag = routedMessage.value("flag", 0U);
        const bool set = routedMessage.value("set", true);
        if (is_unsynced_event_bit(flag)) {
            DuskLog.info("Multiplayer ignored unsynced remote event bit flag={} set={}", flag,
                         set);
            return;
        }
        if (set) {
            maybe_show_progression_sync_prompt_for_event_bit(resolve_peer_id(routedMessage), flag);
        }
        sApplyingRemoteSaveBit = true;
        if (set) {
            dComIfGs_onEventBit(flag);
        } else {
            dComIfGs_offEventBit(flag);
        }
        sApplyingRemoteSaveBit = false;
        DuskLog.info("Multiplayer applied remote event bit flag={} set={}", flag, set);
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
    } else if (type == "light_drop_get_flag") {
        const int area = message.value("area", -1);
        if (area >= 0 && area < 3) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onLightDropGetFlag(static_cast<u8>(area));
            dMeter2Info_setLightDropGetFlag(area, 0xFF);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote light drop get flag area={}", area);
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
    } else if (type == "rupee_count") {
        const int value = message.value("value", -1);
        if (value >= 0 && value <= dComIfGs_getRupeeMax()) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_setRupee(static_cast<u16>(value));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote rupee count value={}", value);
        }
    } else if (type == "switch_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        const int sourceActor = message.value("source_actor", -1);
        const int sourceRoom = message.value("source_room", -128);
        const uint32_t sourceParams = message.value("source_params", 0xFFFFFFFFU);
        const bool set = message.value("set", true);
        if (stage >= 0 && flag >= 0) {
            if (is_unsynced_switch_bit(stage, flag)) {
                DuskLog.info("Multiplayer ignored unsynced remote switch bit stage={} flag={} set={}",
                             stage, flag, set);
                return;
            }
            begin_flag_trace_window("remote_rx", "switch", stage, flag, set, sourceActor,
                                    sourceRoom, sourceParams);
            if (stage == kProgressionCueSewersStage && (flag == 10 || flag == 17)) {
                DuskLog.info("Multiplayer received sewers box/progression switch flag={} set={} "
                             "sourceActor={} sourceRoom={} "
                             "sourceParams=0x{:08X}",
                             flag, set, sourceActor, sourceRoom, sourceParams);
            }
            if (set) {
                maybe_show_progression_sync_prompt_for_switch(resolve_peer_id(routedMessage), stage,
                                                              flag);
            }
            if (!suppress_remote_switch_from_source_actor(stage, flag, sourceActor)) {
                if (set) {
                    apply_remote_switch_bit(stage, flag);
                } else {
                    apply_remote_switch_bit_off(stage, flag);
                }
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

    if (sender != nullptr && should_forward_peer_message(type) && type != "sync_request" &&
        !(type == "save_snapshot" && routedMessage.value("manual_sync", false)))
    {
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

std::string sender_id_from_udp_header(const UdpPoseChunkHeader& header) {
    size_t len = 0;
    while (len < sizeof(header.senderId) && header.senderId[len] != '\0') {
        ++len;
    }
    return std::string(header.senderId, len);
}

void set_udp_header_sender(UdpPoseChunkHeader& header, const std::string& senderId) {
    std::memset(header.senderId, 0, sizeof(header.senderId));
    std::memcpy(header.senderId, senderId.data(),
                std::min(senderId.size(), sizeof(header.senderId) - 1));
}

std::string local_udp_pose_sender_id() {
    if (sSession.mode == NetworkMode::DirectJoin && !sSession.clientId.empty()) {
        return sSession.clientId;
    }
    return kDirectPeerId;
}

size_t compressed_size_for_diagnostics(const void* data, size_t size) {
    const size_t bound = ZSTD_compressBound(size);
    std::vector<uint8_t> compressed(bound);
    const size_t compressedSize = ZSTD_compress(compressed.data(), compressed.size(), data, size, 1);
    return ZSTD_isError(compressedSize) ? 0 : compressedSize;
}

size_t msgpack_size_for_diagnostics(const json& value) {
    return json::to_msgpack(value).size();
}

void erase_state_keys_for_diagnostics(json& message, const char* const* keys, size_t keyCount) {
    auto stateIt = message.find("state");
    if (stateIt == message.end() || !stateIt->is_object()) {
        return;
    }

    for (size_t i = 0; i < keyCount; ++i) {
        stateIt->erase(keys[i]);
    }
}

size_t compressed_without_state_keys_for_diagnostics(const json& message,
                                                     const char* const* keys, size_t keyCount) {
    json copy = message;
    erase_state_keys_for_diagnostics(copy, keys, keyCount);
    const std::vector<uint8_t> packed = json::to_msgpack(copy);
    return compressed_size_for_diagnostics(packed.data(), packed.size());
}

size_t raw_state_group_size_for_diagnostics(const json* state, const char* const* keys,
                                            size_t keyCount) {
    if (state == nullptr || !state->is_object()) {
        return 0;
    }

    json group = json::object();
    for (size_t i = 0; i < keyCount; ++i) {
        const auto it = state->find(keys[i]);
        if (it != state->end()) {
            group[keys[i]] = *it;
        }
    }
    return msgpack_size_for_diagnostics(group);
}

json udp_pose_message_for_wire(const json& message) {
    if (!kUseBinaryMatrixUdpWire) {
        return message;
    }

    json wire = message;
    auto stateIt = wire.find("state");
    if (stateIt == wire.end() || !stateIt->is_object()) {
        return wire;
    }

    auto matricesIt = stateIt->find("link_matrices");
    if (matricesIt == stateIt->end() || !matricesIt->is_object()) {
        return wire;
    }

    const std::string format = matricesIt->value("format", "");
    if (format == "f32_pack_bin_v1" || format == "qrot16_trans32_bin_v1" ||
        format == "qbasis16_trans32_safe_bin_v1")
    {
        return wire;
    }
    if (format != "f32_pack_v1" && format != "qrot16_trans32_v1" &&
        format != "qbasis16_trans32_safe_v1")
    {
        return wire;
    }
    if (format != "f32_pack_v1" && !kUseQuantizedMatrixWire) {
        return wire;
    }

    auto dataIt = matricesIt->find("data");
    if (dataIt == matricesIt->end() || !dataIt->is_string()) {
        return wire;
    }

    std::string packed;
    if (!absl::Base64Unescape(dataIt->get<std::string>(), &packed)) {
        return wire;
    }

    std::vector<uint8_t> bytes(packed.begin(), packed.end());
    if (format == "f32_pack_v1") {
        (*matricesIt)["format"] = "f32_pack_bin_v1";
    } else if (format == "qbasis16_trans32_safe_v1") {
        (*matricesIt)["format"] = "qbasis16_trans32_safe_bin_v1";
    } else {
        (*matricesIt)["format"] = "qrot16_trans32_bin_v1";
    }
    (*matricesIt)["data"] = json::binary(std::move(bytes));
    return wire;
}

bool open_udp_socket(const std::string& bindHost, int bindPort) {
    close_socket(sSession.udpSock);
    sSession.udpRemoteAddrKnown = false;
    sSession.udpPoseReassembly.clear();
    sSession.udpPoseLastProcessedSequence.clear();

    sSession.udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSession.udpSock == INVALID_SOCKET) {
        DuskLog.warn("Multiplayer direct UDP disabled: socket failed");
        return false;
    }

    if (!set_nonblocking(sSession.udpSock)) {
        DuskLog.warn("Multiplayer direct UDP disabled: nonblocking failed");
        close_socket(sSession.udpSock);
        return false;
    }
    configure_udp_socket_buffers(sSession.udpSock);

    sockaddr_in addr{};
    if (!fill_ipv4(addr, bindHost, bindPort)) {
        DuskLog.warn("Multiplayer direct UDP disabled: invalid bind host {}", bindHost);
        close_socket(sSession.udpSock);
        return false;
    }

    if (bind(sSession.udpSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        DuskLog.warn("Multiplayer direct UDP disabled: bind failed {}:{}", bindHost, bindPort);
        close_socket(sSession.udpSock);
        return false;
    }

    DuskLog.info("Multiplayer direct UDP pose socket bound on {}:{}", bindHost, bindPort);
    return true;
}

void setup_direct_host_udp() {
    open_udp_socket(sSession.bindHost, sSession.port);
}

void setup_direct_join_udp() {
    if (!open_udp_socket("0.0.0.0", 0)) {
        return;
    }

    if (!fill_ipv4(sSession.udpRemoteAddr, sSession.host, sSession.port)) {
        DuskLog.warn("Multiplayer direct UDP disabled: invalid remote host {}", sSession.host);
        close_socket(sSession.udpSock);
        return;
    }
    sSession.udpRemoteAddrKnown = true;
    DuskLog.info("Multiplayer direct UDP pose remote set to {}:{}", sSession.host, sSession.port);
}

bool send_udp_pose_to_addr(const sockaddr_in& addr, const json& message,
                           const std::string& senderId) {
    if (sSession.udpSock == INVALID_SOCKET) {
        return false;
    }

    const json wireMessage = udp_pose_message_for_wire(message);
    const std::vector<uint8_t> raw = json::to_msgpack(wireMessage);
    const size_t bound = ZSTD_compressBound(raw.size());
    std::vector<uint8_t> compressed(bound);
    const size_t compressedSize =
        ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
    if (ZSTD_isError(compressedSize)) {
        DuskLog.warn("Multiplayer direct UDP pose compression failed: {}",
                     ZSTD_getErrorName(compressedSize));
        return false;
    }
    if (compressedSize == 0 || compressedSize > kUdpPoseMaxCompressedBytes ||
        raw.size() > kUdpPoseMaxUncompressedBytes)
    {
        DuskLog.warn("Multiplayer direct UDP pose too large raw={} compressed={}", raw.size(),
                     compressedSize);
        return false;
    }
    compressed.resize(compressedSize);

    const uint16_t chunkCount =
        static_cast<uint16_t>((compressed.size() + kUdpPoseChunkPayloadBytes - 1) /
                              kUdpPoseChunkPayloadBytes);
    if (chunkCount == 0) {
        return false;
    }

    const uint32_t sequence = wireMessage.value("sequence", 0U);
    trace_udp_pose_packet_tx(wireMessage, raw.size(), compressed.size(), chunkCount);
    std::array<uint8_t, sizeof(UdpPoseChunkHeader) + kUdpPoseChunkPayloadBytes> packet{};
    for (uint16_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        const size_t offset = static_cast<size_t>(chunkIndex) * kUdpPoseChunkPayloadBytes;
        const size_t payloadSize =
            std::min(kUdpPoseChunkPayloadBytes, compressed.size() - offset);

        UdpPoseChunkHeader header{};
        header.magic[0] = 'D';
        header.magic[1] = 'M';
        header.magic[2] = 'P';
        header.magic[3] = 'U';
        header.version = 1;
        header.type = 2;
        header.headerSize = sizeof(UdpPoseChunkHeader);
        header.sequence = sequence;
        header.chunkIndex = chunkIndex;
        header.chunkCount = chunkCount;
        header.uncompressedSize = static_cast<uint32_t>(raw.size());
        header.compressedSize = static_cast<uint32_t>(compressed.size());
        header.payloadSize = static_cast<uint16_t>(payloadSize);
        set_udp_header_sender(header, senderId);

        std::memcpy(packet.data(), &header, sizeof(header));
        std::memcpy(packet.data() + sizeof(header), compressed.data() + offset, payloadSize);
        const int packetSize = static_cast<int>(sizeof(header) + payloadSize);
        const int sent =
            sendto(sSession.udpSock, reinterpret_cast<const char*>(packet.data()),
                   packetSize, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (sent < 0) {
            static uint32_t sUdpPoseSendWouldBlockLogCount = 0;
            if (would_block()) {
                if (sUdpPoseSendWouldBlockLogCount < 20 ||
                    (sUdpPoseSendWouldBlockLogCount % 120) == 0)
                {
                    DuskLog.warn("Multiplayer direct UDP pose send would block seq={} chunk={}/{}",
                                 sequence, chunkIndex + 1, chunkCount);
                }
                ++sUdpPoseSendWouldBlockLogCount;
            }
            return false;
        }
        if (sent != packetSize) {
            DuskLog.warn("Multiplayer direct UDP pose partial datagram send seq={} chunk={}/{} "
                         "sent={} expected={}",
                         sequence, chunkIndex + 1, chunkCount, sent, packetSize);
            return false;
        }
    }

    static uint32_t sUdpPoseTxLogTicks = 0;
    if ((sUdpPoseTxLogTicks++ % 150) == 0) {
        const json* state =
            wireMessage.find("state") != wireMessage.end() ? &wireMessage["state"] : nullptr;
        static const char* const matrixKeys[] = {"link_matrices"};
        static const char* const audioKeys[] = {"audio_events"};
        static const char* const worldKeys[] = {
            "stage", "room", "layer", "final_ganondorf_ready", "x", "y", "z", "angle_y",
            "manual_sync_ready",
        };
        static const char* const animKeys[] = {
            "proc_id", "proc_v0", "proc_v1", "proc_v2", "proc_v3", "proc_v5",
            "under_frame", "under_bck0", "under_frame0", "under_rate0", "upper_bck2",
            "upper_frame2", "upper_rate2",
        };
        static const char* const transformKeys[] = {
            "is_wolf", "is_transforming", "transform_from_wolf", "transform_to_wolf",
            "transform_proc_v0", "transform_proc_v5", "transform_clothes_wait",
            "transform_frame", "transform_proc_v2", "transform_proc_v3", "transform_shape_x",
        };
        static const char* const visualKeys[] = {
            "equip_item", "sword_variant", "shield_variant", "clothes_variant", "sword_draw",
            "shield_draw", "sword_out", "midna_draw", "midna_mask_draw", "midna_hand_draw",
            "midna_hair_draw", "midna_shadow_form", "heavy_boots", "item_draw",
            "kantera_draw", "item_actor_kind", "item_actor_bomb_ex_time",
            "item_actor_bomb_flash", "ride_actor_kind",
        };

        auto groupRaw = [state](const char* const* keys, size_t keyCount) {
            return raw_state_group_size_for_diagnostics(state, keys, keyCount);
        };
        auto groupDelta = [&message, compressedSize](const char* const* keys, size_t keyCount) {
            const size_t without =
                compressed_without_state_keys_for_diagnostics(message, keys, keyCount);
            return static_cast<int64_t>(compressedSize) - static_cast<int64_t>(without);
        };

        size_t matrixB64Compressed = 0;
        if (state != nullptr && state->is_object()) {
            const auto matrixIt = state->find("link_matrices");
            const json* matrices = matrixIt != state->end() ? &(*matrixIt) : nullptr;
            if (matrices != nullptr && matrices->is_object()) {
                const auto dataIt = matrices->find("data");
                if (dataIt != matrices->end() && dataIt->is_binary()) {
                    const auto& matrixData = dataIt->get_binary();
                    matrixB64Compressed =
                        compressed_size_for_diagnostics(matrixData.data(), matrixData.size());
                } else {
                    const std::string matrixData = matrices->value("data", "");
                    if (!matrixData.empty()) {
                        matrixB64Compressed =
                            compressed_size_for_diagnostics(matrixData.data(), matrixData.size());
                    }
                }
            }
        }

        DuskLog.info(
            "Multiplayer direct UDP pose tx seq={} sender={} raw={} compressed={} chunks={} "
            "matrix_packed={} matrix_b64={} matrix_b64_zstd={} matrix_slots={}",
            sequence, senderId, raw.size(), compressed.size(), chunkCount,
            sLastPoseMatrixPackedBytes, sLastPoseMatrixBase64Bytes, matrixB64Compressed,
            sLastPoseMatrixPresentSlots);
        DuskLog.info(
            "Multiplayer direct UDP pose tx breakdown seq={} raw_matrix={} raw_audio={} "
            "raw_world={} raw_anim={} raw_transform={} raw_visual={} zdelta_matrix={} "
            "zdelta_audio={} zdelta_world={} zdelta_anim={} zdelta_transform={} zdelta_visual={}",
            sequence,
            groupRaw(matrixKeys, std::size(matrixKeys)),
            groupRaw(audioKeys, std::size(audioKeys)),
            groupRaw(worldKeys, std::size(worldKeys)),
            groupRaw(animKeys, std::size(animKeys)),
            groupRaw(transformKeys, std::size(transformKeys)),
            groupRaw(visualKeys, std::size(visualKeys)),
            groupDelta(matrixKeys, std::size(matrixKeys)),
            groupDelta(audioKeys, std::size(audioKeys)),
            groupDelta(worldKeys, std::size(worldKeys)),
            groupDelta(animKeys, std::size(animKeys)),
            groupDelta(transformKeys, std::size(transformKeys)),
            groupDelta(visualKeys, std::size(visualKeys)));
    }
    return true;
}

bool send_udp_pose_to_peer(DirectPeer& peer, const json& message, const std::string& senderId) {
    if (!peer.welcomed || !peer.udpAddrKnown) {
        return false;
    }
    return send_udp_pose_to_addr(peer.udpAddr, message, senderId);
}

bool broadcast_udp_pose_to_direct_peers(const json& message, const std::string& senderId,
                                        const std::string& excludePeerId = "") {
    bool sentAny = false;
    for (auto& entry : sSession.directPeers) {
        DirectPeer& peer = entry.second;
        if (peer.id == excludePeerId) {
            continue;
        }
        sentAny = send_udp_pose_to_peer(peer, message, senderId) || sentAny;
    }
    return sentAny || sSession.directPeers.empty();
}

bool send_direct_pose_udp(const json& message) {
    if (sSession.mode == NetworkMode::DirectHost) {
        return broadcast_udp_pose_to_direct_peers(message, local_udp_pose_sender_id());
    }

    if (sSession.mode == NetworkMode::DirectJoin && sSession.udpRemoteAddrKnown &&
        !sSession.clientId.empty())
    {
        return send_udp_pose_to_addr(sSession.udpRemoteAddr, message, local_udp_pose_sender_id());
    }

    return false;
}

bool send_pose_message(const json& message) {
    if ((sSession.mode == NetworkMode::DirectHost || sSession.mode == NetworkMode::DirectJoin) &&
        send_direct_pose_udp(message))
    {
        return true;
    }
    return send_json(message);
}

void process_udp_pose_message(json message, const std::string& senderId) {
    std::string routedSender = senderId.empty() ? kDirectPeerId : senderId;
    if (routedSender != kDirectPeerId) {
        message["client_id"] = routedSender;
    }

    if (sSession.mode == NetworkMode::DirectHost) {
        if (routedSender == kDirectPeerId || sSession.directPeers.find(routedSender) == sSession.directPeers.end()) {
            return;
        }
        handle_message(message);
        broadcast_udp_pose_to_direct_peers(message, routedSender, routedSender);
        return;
    }

    handle_message(message);
}

void accept_udp_pose_chunk(const UdpPoseChunkHeader& header, const uint8_t* payload,
                           const sockaddr_in& fromAddr) {
    if (std::memcmp(header.magic, "DMPU", 4) != 0 || header.version != 1 ||
        (header.type != 1 && header.type != 2) ||
        header.headerSize != sizeof(UdpPoseChunkHeader) || header.payloadSize == 0 ||
        header.chunkCount == 0 || header.chunkIndex >= header.chunkCount ||
        header.compressedSize == 0 || header.compressedSize > kUdpPoseMaxCompressedBytes ||
        header.uncompressedSize == 0 || header.uncompressedSize > kUdpPoseMaxUncompressedBytes)
    {
        return;
    }

    std::string senderId = sender_id_from_udp_header(header);
    if (senderId.empty()) {
        senderId = kDirectPeerId;
    }

    if (sSession.mode == NetworkMode::DirectHost) {
        auto peerIt = sSession.directPeers.find(senderId);
        if (peerIt == sSession.directPeers.end()) {
            return;
        }
        peerIt->second.udpAddr = fromAddr;
        peerIt->second.udpAddrKnown = true;
    } else if (senderId == local_udp_pose_sender_id()) {
        return;
    }

    auto lastProcessedIt = sSession.udpPoseLastProcessedSequence.find(senderId);
    if (lastProcessedIt != sSession.udpPoseLastProcessedSequence.end() &&
        header.sequence <= lastProcessedIt->second)
    {
        return;
    }

    auto& reassemblies = sSession.udpPoseReassembly[senderId];
    auto reassemblyIt = reassemblies.find(header.sequence);
    if (reassemblyIt == reassemblies.end()) {
        while (reassemblies.size() >= kUdpPoseMaxInflightSequences) {
            reassemblies.erase(reassemblies.begin());
        }
        reassemblyIt = reassemblies.emplace(header.sequence, UdpPoseReassembly{}).first;
        UdpPoseReassembly& reassembly = reassemblyIt->second;
        reassembly.sequence = header.sequence;
        reassembly.chunkCount = header.chunkCount;
        reassembly.uncompressedSize = header.uncompressedSize;
        reassembly.compressedSize = header.compressedSize;
        reassembly.compressed.assign(header.compressedSize, 0);
        reassembly.received.assign(header.chunkCount, 0);
    }

    UdpPoseReassembly& reassembly = reassemblyIt->second;
    if (header.chunkCount != reassembly.chunkCount ||
        header.compressedSize != reassembly.compressedSize ||
        header.uncompressedSize != reassembly.uncompressedSize)
    {
        reassemblies.erase(reassemblyIt);
        return;
    }

    const size_t offset = static_cast<size_t>(header.chunkIndex) * kUdpPoseChunkPayloadBytes;
    if (offset + header.payloadSize > reassembly.compressed.size()) {
        return;
    }

    if (!reassembly.received[header.chunkIndex]) {
        reassembly.received[header.chunkIndex] = 1;
        ++reassembly.receivedCount;
    }
    std::memcpy(reassembly.compressed.data() + offset, payload, header.payloadSize);

    if (reassembly.receivedCount != reassembly.chunkCount) {
        return;
    }

    std::vector<uint8_t> raw(header.uncompressedSize);
    const size_t decompressedSize =
        ZSTD_decompress(raw.data(), raw.size(), reassembly.compressed.data(),
                        reassembly.compressed.size());
    if (ZSTD_isError(decompressedSize) || decompressedSize != raw.size()) {
        DuskLog.warn("Multiplayer direct UDP pose decompression failed sender={} seq={} err={}",
                     senderId, header.sequence,
                     ZSTD_isError(decompressedSize) ? ZSTD_getErrorName(decompressedSize)
                                                    : "size mismatch");
        return;
    }

    try {
        json message = header.type == 2
                           ? json::from_msgpack(raw)
                           : json::parse(std::string(reinterpret_cast<const char*>(raw.data()),
                                                     raw.size()));
        process_udp_pose_message(std::move(message), senderId);
        sSession.udpPoseLastProcessedSequence[senderId] = header.sequence;
        reassemblies.erase(header.sequence);
        while (!reassemblies.empty() && reassemblies.begin()->first <= header.sequence) {
            reassemblies.erase(reassemblies.begin());
        }
    } catch (const json::exception& e) {
        DuskLog.warn("Multiplayer direct UDP pose decode failed sender={} seq={} type={} err={}",
                     senderId, header.sequence, header.type, e.what());
    }
}

void pump_udp_pose_receives() {
    if (sSession.udpSock == INVALID_SOCKET) {
        return;
    }

    std::array<uint8_t, sizeof(UdpPoseChunkHeader) + kUdpPoseChunkPayloadBytes> packet{};
    while (true) {
        sockaddr_in fromAddr{};
#if _WIN32
        int fromLen = sizeof(fromAddr);
#else
        socklen_t fromLen = sizeof(fromAddr);
#endif
        const int read =
            recvfrom(sSession.udpSock, reinterpret_cast<char*>(packet.data()),
                     static_cast<int>(packet.size()), 0, reinterpret_cast<sockaddr*>(&fromAddr),
                     &fromLen);
        if (read < 0) {
            if (!would_block()) {
                DuskLog.warn("Multiplayer direct UDP recv failed");
            }
            return;
        }
        if (static_cast<size_t>(read) < sizeof(UdpPoseChunkHeader)) {
            continue;
        }

        UdpPoseChunkHeader header{};
        std::memcpy(&header, packet.data(), sizeof(header));
        if (sizeof(header) + header.payloadSize > static_cast<size_t>(read)) {
            continue;
        }
        accept_udp_pose_chunk(header, packet.data() + sizeof(header), fromAddr);
    }
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

    if (sSession.mode == NetworkMode::DirectJoin) {
        setup_direct_join_udp();
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
    setup_direct_host_udp();
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

std::vector<RemoteAudioEvent> drain_local_link_audio_events() {
    std::vector<RemoteAudioEvent> events;
    const size_t count = std::min<size_t>(sPendingLocalAudioEvents.size(), 8);
    events.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        events.push_back(sPendingLocalAudioEvents[i]);
    }

    if (count == sPendingLocalAudioEvents.size()) {
        sPendingLocalAudioEvents.clear();
    } else {
        sPendingLocalAudioEvents.erase(sPendingLocalAudioEvents.begin(),
                                       sPendingLocalAudioEvents.begin() + count);
    }

    return events;
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

    std::vector<RemoteAudioEvent> audioEvents = drain_local_link_audio_events();
    static uint32_t sAudioTxLogCount = 0;
    if (!audioEvents.empty() && sAudioTxLogCount < 20) {
        ++sAudioTxLogCount;
        DuskLog.info("Multiplayer audio tx count={} first_seq={} first_sound={:#x}",
                     audioEvents.size(), audioEvents.front().sequence,
                     audioEvents.front().soundId);
    }

    const LocalMidnaVisualState midnaVisual = detect_midna_visual_state(link, isWolf);
    const f32 poseX = player->current.pos.x;
    const f32 poseY = player->current.pos.y;
    const f32 poseZ = player->current.pos.z;
    const f32 underFrame0 = link != nullptr ? link->mUnderFrameCtrl[0].getFrame() : 0.0f;
    const f32 underRate0 = link != nullptr ? link->mUnderFrameCtrl[0].getRate() : 1.0f;
    const f32 upperFrame2 = link != nullptr ? link->mUpperFrameCtrl[2].getFrame() : 0.0f;
    const f32 upperRate2 = link != nullptr ? link->mUpperFrameCtrl[2].getRate() : 1.0f;
    if (!pose_float_is_finite("x", poseX) || !pose_float_is_finite("y", poseY) ||
        !pose_float_is_finite("z", poseZ) ||
        !pose_float_is_finite("under_frame0", underFrame0) ||
        !pose_float_is_finite("under_rate0", underRate0) ||
        !pose_float_is_finite("upper_frame2", upperFrame2) ||
        !pose_float_is_finite("upper_rate2", upperRate2))
    {
        return;
    }

    auto hatArray10ToJson = [&](const s16* values) {
        json out = json::array();
        for (size_t i = 0; i < 10; ++i) {
            out.push_back(values != nullptr ? static_cast<int>(values[i]) : 0);
        }
        return out;
    };
    auto hatArray3ToJson = [&](const s16* values) {
        json out = json::array();
        for (size_t i = 0; i < 3; ++i) {
            out.push_back(values != nullptr ? static_cast<int>(values[i]) : 0);
        }
        return out;
    };

    json state = {
        {"stage", dComIfGp_getStartStageName()},
        {"room", static_cast<int>(fopAcM_GetRoomNo(player))},
        {"layer", static_cast<int>(dComIfGp_getStartStageLayer())},
        {"final_ganondorf_ready",
         std::strcmp(dComIfGp_getStartStageName(), "D_MN09B") == 0 &&
             dComIfGs_isSaveDunSwitch(1)},
        {"x", poseX},
        {"y", poseY},
        {"z", poseZ},
        {"angle_y", static_cast<int>(player->shape_angle.y)},
        {"proc_id", link != nullptr ? static_cast<int>(link->mProcID) : 0},
        {"proc_v0", link != nullptr ? link->mProcVar0.field_0x3008 : 0},
        {"proc_v1", link != nullptr ? link->mProcVar1.field_0x300a : 0},
        {"proc_v2", link != nullptr ? link->mProcVar2.field_0x300c : 0},
        {"proc_v3", link != nullptr ? link->mProcVar3.field_0x300e : 0},
        {"proc_v5", link != nullptr ? link->mProcVar5.field_0x3012 : 0},
        {"manual_sync_ready", !is_stage_load_unsafe_for_multiplayer()},
        {"under_frame", underFrame0},
        {"under_bck0", link != nullptr ? static_cast<int>(link->mUnderAnmHeap[0].getIdx()) : 0},
        {"under_frame0", underFrame0},
        {"under_rate0", underRate0},
        {"upper_bck2", link != nullptr ? static_cast<int>(link->mUpperAnmHeap[2].getIdx()) : 0},
        {"upper_frame2", upperFrame2},
        {"upper_rate2", upperRate2},
        {"hat_rot_a", hatArray10ToJson(link != nullptr ? link->field_0x302c : nullptr)},
        {"hat_rot_b", hatArray10ToJson(link != nullptr ? link->field_0x3040 : nullptr)},
        {"hat_swing", hatArray3ToJson(link != nullptr ? link->field_0x3066 : nullptr)},
        {"hat_shape_y", link != nullptr ? static_cast<int>(link->field_0x3062) : 0},
        {"is_wolf", isWolf},
        {"is_transforming", isTransforming},
        {"transform_from_wolf", transformFromWolf},
        {"transform_to_wolf", transformToWolf},
        {"transform_proc_v0", link != nullptr ? link->mProcVar0.field_0x3008 : 0},
        {"transform_proc_v5", link != nullptr ? link->mProcVar5.field_0x3012 : 0},
        {"transform_clothes_wait", link != nullptr ? static_cast<int>(link->mClothesChangeWaitTimer) : 0},
        {"transform_frame", underFrame0},
        {"transform_proc_v2", link != nullptr ? link->mProcVar2.field_0x300c : 0},
        {"transform_proc_v3", link != nullptr ? link->mProcVar3.field_0x300e : 0},
        {"transform_shape_x", link != nullptr ? static_cast<int>(link->shape_angle.x) : 0},
        {"equip_item", link != nullptr ? static_cast<int>(link->mEquipItem) : 0xFFFF},
        {"sword_variant", detect_sword_variant(link)},
        {"shield_variant", detect_shield_variant()},
        {"clothes_variant", detect_clothes_variant()},
        {"sword_draw", link != nullptr && static_cast<bool>(link->checkSwordDraw())},
        {"shield_draw", link != nullptr && static_cast<bool>(link->checkShieldDraw())},
        {"sword_out", link != nullptr && !isWolf && link->mEquipItem == 0x103},
        {"midna_draw", midnaVisual.body},
        {"midna_mask_draw", midnaVisual.mask},
        {"midna_hand_draw", midnaVisual.hand},
        {"midna_hair_draw", midnaVisual.hair},
        {"midna_shadow_form", midnaVisual.shadowForm},
        {"heavy_boots",
         link != nullptr && !isWolf && static_cast<bool>(link->checkEquipHeavyBoots())},
        {"item_draw", link != nullptr && !isWolf && static_cast<bool>(link->checkItemDraw())},
        {"kantera_draw",
         link != nullptr && !isWolf &&
             (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) ||
              link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_20000))},
        {"item_actor_kind", REMOTE_ITEM_ACTOR_NONE},
        {"ride_actor_kind", REMOTE_RIDE_ACTOR_NONE},
        {"audio_events", audio_events_to_json(audioEvents)},
    };
    if (isLink && (sDummyModelEnabled || isWolf || isTransforming) && !add_link_matrices(state)) {
        static uint32_t sMatrixPoseDropLogCount = 0;
        ++sMatrixPoseDropLogCount;
        if (sMatrixPoseDropLogCount < 20 ||
            (sDummyTraceEnabled && (sMatrixPoseDropLogCount % 15) == 0))
        {
            if (sDummyTraceEnabled) {
                DuskLog.info(
                    "Multiplayer dummy trace tx_drop reason=matrix_capture_failed seq_next={} "
                    "drop_count={} is_wolf={} proc={} clothes_wait={} stage={} room={}",
                    sSession.poseSequence + 1, sMatrixPoseDropLogCount, isWolf,
                    link != nullptr ? link->mProcID : 0,
                    link != nullptr ? static_cast<int>(link->mClothesChangeWaitTimer) : 0,
                    dComIfGp_getStartStageName(), static_cast<int>(fopAcM_GetRoomNo(player)));
            } else {
                DuskLog.info(
                    "Multiplayer transform debug tx_pose_dropped matrix_capture_failed seq_next={} "
                    "is_wolf={} proc={}",
                    sSession.poseSequence + 1, isWolf, link != nullptr ? link->mProcID : 0);
            }
        }
        return;
    }

    if (sDummyTraceEnabled && (sSession.poseSequence % 30) == 0) {
        DuskLog.info(
            "Multiplayer dummy trace tx seq_next={} stage={} room={} matrix={} proc={} "
            "under_bck={} under_frame={} under_rate={} upper_bck={} upper_frame={} upper_rate={}",
            sSession.poseSequence + 1, dComIfGp_getStartStageName(),
            static_cast<int>(fopAcM_GetRoomNo(player)), state.contains("link_matrices"),
            link != nullptr ? link->mProcID : 0, state.value("under_bck0", 0),
            state.value("under_frame0", 0.0f), state.value("under_rate0", 1.0f),
            state.value("upper_bck2", 0), state.value("upper_frame2", 0.0f),
            state.value("upper_rate2", 1.0f));
    }

    send_pose_message({
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

void tick_pending_manual_sync_apply() {
    if (!sPendingManualSyncInfo.has_value()) {
        return;
    }
    if (is_stage_load_unsafe_for_multiplayer()) {
        return;
    }

    g_dComIfG_gameInfo.info = *sPendingManualSyncInfo;
    sPendingManualSyncInfo.reset();
    if (sPendingManualSyncVibration.has_value()) {
        dComIfGs_setOptVibration(*sPendingManualSyncVibration);
        dComIfGp_setNowVibration(*sPendingManualSyncVibration);
        sPendingManualSyncVibration.reset();
    }
    dComIfGp_offOxygenShowFlag();
    dComIfGp_setMaxOxygen(600);
    dComIfGp_setOxygen(600);
    DuskLog.info("Multiplayer manual sync full-state save info reapplied after stage load");
}

void update_connected() {
    update_remote_switch_policy_room_state();
    age_recent_remote_switches();
    tick_pending_manual_sync_apply();

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
    if (sSession.mode == NetworkMode::DirectHost || sSession.mode == NetworkMode::DirectJoin) {
        pump_udp_pose_receives();
    }

    if (sSession.state != ConnectionState::Connected) {
        return;
    }

    update_pending_progression_cue_arrivals();
    flush_pending_progression_sync();
    update_pending_sync_replies();

    if (!is_stage_load_unsafe_for_multiplayer()) {
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

        if (!sPendingStageMessages.empty() && !is_stage_load_unsafe_for_multiplayer()) {
            // Same problem on the receive side: a peer's save_snapshot (or
            // even a live tbox_bit/etc.) can arrive before we have a stable
            // stage, or while vanilla is inside a connected cutscene/state
            // transition. handle_message() queued those; replay them only
            // once stage access is safe and the event window is over.
            std::vector<json> pending = std::move(sPendingStageMessages);
            sPendingStageMessages.clear();
            for (const json& queued : pending) {
                handle_message(queued);
            }
        }

        if (sManualSyncReloadPending && !is_stage_load_unsafe_for_multiplayer()) {
            sManualSyncReloadPending = false;
            DuskLog.info("Multiplayer manual sync restarting current room");
            daPy_py_c::forceRestartRoom(0, 5, 0xC9);
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
    sDummyTraceEnabled = !env_disabled("DUSK_MP_DUMMY_TRACE");
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

void record_local_link_audio_event(uint32_t soundId, bool level, uint32_t mapInfo, int reverb,
                                   uint8_t sourceKind) {
    if (!sEnabled || !sSession.welcomed || soundId == 0) {
        return;
    }

    // Level sounds need an update/stop protocol before they are safe to mirror.
    // Sending them as one-shots can leave remote loops hanging, so this pass
    // mirrors vanilla one-shot Link sounds only.
    if (level) {
        return;
    }

    if (sPendingLocalAudioEvents.size() >= 32) {
        sPendingLocalAudioEvents.erase(sPendingLocalAudioEvents.begin());
    }

    RemoteAudioEvent event;
    event.sequence = ++sLocalAudioEventSequence;
    event.soundId = soundId;
    event.mapInfo = mapInfo;
    event.reverb = static_cast<int8_t>(std::clamp(reverb, -1, 127));
    event.sourceKind = sourceKind;
    event.level = false;
    sPendingLocalAudioEvents.push_back(event);
    DuskLog.info("Multiplayer audio enqueue seq={} sound={:#x} mapinfo={} reverb={} source={}",
                 event.sequence, event.soundId, event.mapInfo, static_cast<int>(event.reverb),
                 static_cast<int>(event.sourceKind));
}

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

    if (sFlagTraceActorWindowTicks > 0) {
        --sFlagTraceActorWindowTicks;
    }

    for (auto& entry : sSession.peerPoses) {
        if (entry.second.valid) {
            ++entry.second.ageTicks;
        }
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

    const bool dummyGameplayReady = is_peer_dummy_gameplay_ready();
    if (sDummyModelEnabled && dummyGameplayReady) {
        // Pump receives before applying remote visuals so poses that arrived
        // this tick do not wait an extra frame and create repeat/gap cadence.
        sync_remote_link_actor_dummies(sSession.peerPoses);
    } else {
        destroy_all_remote_link_dummies();
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

void notify_actor_create_request(int actorName, uint32_t actorParams, int room, float x, float y,
                                 float z) {
    if (!is_flag_trace_actor_window_active()) {
        return;
    }

    DuskLog.info(
        "MP_FLAG_TRACE actor create_request actor={} params=0x{:08X} room={} pos=({}, {}, {}) "
        "currentStage={} currentRoom={} ticksLeft={}",
        actorName, actorParams, room, x, y, z, current_stage_name(), dComIfGp_roomControl_getStayNo(),
        sFlagTraceActorWindowTicks);
}

void notify_actor_delete(int actorName, uint32_t actorParams, int room, float x, float y, float z) {
    if (!is_flag_trace_actor_window_active()) {
        return;
    }

    DuskLog.info(
        "MP_FLAG_TRACE actor delete actor={} params=0x{:08X} room={} pos=({}, {}, {}) "
        "currentStage={} currentRoom={} ticksLeft={}",
        actorName, actorParams, room, x, y, z, current_stage_name(), dComIfGp_roomControl_getStayNo(),
        sFlagTraceActorWindowTicks);
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

bool request_manual_sync(const std::string& peerId, std::string* errorOut) {
    if (!sEnabled || !sSession.welcomed) {
        if (errorOut != nullptr) {
            *errorOut = "Not connected.";
        }
        return false;
    }

    if (peerId.empty() || peerId == "local") {
        if (errorOut != nullptr) {
            *errorOut = "Choose a peer to sync from.";
        }
        return false;
    }

    json request = {
        {"type", "sync_request"},
        {"target_client_id", peerId},
    };
    if (!sAwaitingManualSyncCueKey.empty()) {
        request["cue_key"] = sAwaitingManualSyncCueKey;
    }

    if (sSession.mode == NetworkMode::DirectHost) {
        auto peerIt = sSession.directPeers.find(peerId);
        if (peerIt == sSession.directPeers.end() || !peerIt->second.welcomed) {
            if (errorOut != nullptr) {
                *errorOut = "Selected peer is not connected.";
            }
            return false;
        }
        request["client_id"] = "host";
        send_json_to_peer(peerIt->second, request);
    } else if (sSession.mode == NetworkMode::DirectJoin) {
        if (peerId != kDirectPeerId) {
            if (errorOut != nullptr) {
                *errorOut = "Direct join can only sync from the host.";
            }
            return false;
        }
        send_json(request);
    } else {
        if (sSession.clientId.empty()) {
            if (errorOut != nullptr) {
                *errorOut = "Relay has not assigned a client id yet.";
            }
            return false;
        }
        send_json(request);
    }

    return true;
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

bool request_progression_sync_now(const std::string& peerId, const std::string& peerName) {
    std::string error;
    if (request_manual_sync(peerId, &error)) {
        push_notification("Sync requested from " + peerName);
        DuskLog.info("Multiplayer progression sync requested peer={}", peerId);
        return true;
    }

    push_notification(error.empty() ? "Sync request failed" : error, 6.0f);
    DuskLog.warn("Multiplayer progression sync request failed peer={} error={}", peerId, error);
    return false;
}

void queue_progression_sync_after_event(const ProgressionSyncPrompt& prompt) {
    sPendingProgressionSync = {
        true,
        prompt.peerId,
        prompt.peerName,
        prompt.cueKey,
        0,
    };
    push_notification("Sync queued until cutscene ends", 4.0f);
    DuskLog.info("Multiplayer progression sync queued during event peer={} cue={}", prompt.peerId,
                 prompt.cueKey);
}

void queue_progression_sync_for_peer(const ProgressionSyncPrompt& prompt) {
    sPendingProgressionSync = {
        true,
        prompt.peerId,
        prompt.peerName,
        prompt.cueKey,
        0,
    };
    DuskLog.info("Multiplayer progression sync queued for peer readiness peer={} cue={}",
                 prompt.peerId, prompt.cueKey);
}

void flush_pending_progression_sync() {
    if (!sPendingProgressionSync.active || is_stage_load_unsafe_for_multiplayer()) {
        if (sPendingProgressionSync.active) {
            sPendingProgressionSync.stableReadyTicks = 0;
        }
        return;
    }

    const auto poseIt = sSession.peerPoses.find(sPendingProgressionSync.peerId);
    if (poseIt == sSession.peerPoses.end() || !poseIt->second.valid ||
        poseIt->second.ageTicks > 30 || !poseIt->second.manualSyncReady)
    {
        sPendingProgressionSync.stableReadyTicks = 0;
        static uint32_t sProgressionWaitLogTicks = 0;
        if ((sProgressionWaitLogTicks++ % 60) == 0) {
            DuskLog.info(
                "Multiplayer progression sync waiting for peer readiness peer={} cue={} "
                "has_pose={} valid={} age={} ready={}",
                sPendingProgressionSync.peerId, sPendingProgressionSync.cueKey,
                poseIt != sSession.peerPoses.end(),
                poseIt != sSession.peerPoses.end() ? poseIt->second.valid : false,
                poseIt != sSession.peerPoses.end() ? poseIt->second.ageTicks : 0xFFFFFFFF,
                poseIt != sSession.peerPoses.end() ? poseIt->second.manualSyncReady : false);
        }
        return;
    }

    if (++sPendingProgressionSync.stableReadyTicks < kProgressionSyncStableReadyTicks) {
        static uint32_t sProgressionStableLogTicks = 0;
        if ((sProgressionStableLogTicks++ % 60) == 0) {
            DuskLog.info("Multiplayer progression sync waiting for stable ready peer={} cue={} "
                         "ticks={}/{}",
                         sPendingProgressionSync.peerId, sPendingProgressionSync.cueKey,
                         sPendingProgressionSync.stableReadyTicks,
                         kProgressionSyncStableReadyTicks);
        }
        return;
    }

    const PendingProgressionSync pending = sPendingProgressionSync;
    sPendingProgressionSync = {};
    DuskLog.info("Multiplayer progression sync pending flush peer={} cue={}", pending.peerId,
                 pending.cueKey);
    sAwaitingManualSyncCueKey = pending.cueKey;
    request_progression_sync_now(pending.peerId, pending.peerName);
    if (sProgressionSyncPrompt.active && sProgressionSyncPrompt.waiting &&
        sProgressionSyncPrompt.peerId == pending.peerId &&
        sProgressionSyncPrompt.cueKey == pending.cueKey)
    {
        sProgressionSyncPrompt = {};
    }
}

void consume_progression_prompt_start_button() {
    sProgressionPromptExactStartHeld = false;
    if (!sProgressionSyncPrompt.active) {
        return;
    }

    interface_of_controller_pad& pad = mDoCPd_c::getCpadInfo(PAD_1);
    sProgressionPromptExactStartHeld = (pad.mButtonFlags & PAD_BUTTON_START) != 0;
    pad.mButtonFlags &= ~PAD_BUTTON_START;
    pad.mPressedButtonFlags &= ~PAD_BUTTON_START;
}

void draw_progression_sync_prompt() {
    if (!sProgressionSyncPrompt.active) {
        return;
    }

    const float dt = ImGui::GetIO().DeltaTime;
    if (sProgressionSyncPrompt.waiting) {
        sProgressionSyncPrompt.ageSeconds += dt;
    }
    const bool eventRunning = dComIfGp_event_runCheck();
    if (!eventRunning && !sProgressionSyncPrompt.waiting) {
        sProgressionSyncPrompt.ageSeconds += dt;
    }

    if (!sProgressionSyncPrompt.waiting && sProgressionPromptExactStartHeld) {
        sProgressionSyncPrompt.holdSeconds =
            std::min(kProgressionSyncHoldDuration, sProgressionSyncPrompt.holdSeconds + dt);
    } else if (!sProgressionSyncPrompt.waiting) {
        sProgressionSyncPrompt.holdSeconds = 0.0f;
    }

    if (!sProgressionSyncPrompt.waiting &&
        sProgressionSyncPrompt.holdSeconds >= kProgressionSyncHoldDuration)
    {
        if (eventRunning) {
            queue_progression_sync_after_event(sProgressionSyncPrompt);
        } else {
            queue_progression_sync_for_peer(sProgressionSyncPrompt);
        }
        sProgressionSyncPrompt.title = "Waiting for sync...";
        sProgressionSyncPrompt.body = "Waiting for peer to be ready";
        sProgressionSyncPrompt.ageSeconds = 0.0f;
        sProgressionSyncPrompt.holdSeconds = 0.0f;
        sProgressionSyncPrompt.waiting = true;
        return;
    }

    if (!sProgressionSyncPrompt.waiting && !eventRunning &&
        sProgressionSyncPrompt.ageSeconds >= kProgressionSyncPromptDuration)
    {
        DuskLog.info("Multiplayer progression sync prompt expired peer={} cue={}",
                     sProgressionSyncPrompt.peerId, sProgressionSyncPrompt.cueKey);
        sProgressionSyncPrompt = {};
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workPos = viewport != nullptr ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
    const ImVec2 workSize = viewport != nullptr ? viewport->WorkSize : ImVec2(1280.0f, 720.0f);
    const ImVec2 windowSize(360.0f, 112.0f);
    ImGui::SetNextWindowPos(ImVec2(workPos.x + workSize.x - windowSize.x - 24.0f,
                                   workPos.y + 72.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.86f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.06f, 0.86f));
    if (ImGui::Begin("Multiplayer Progression Sync", nullptr, flags)) {
        const float remaining =
            std::max(0.0f, kProgressionSyncPromptDuration - sProgressionSyncPrompt.ageSeconds);
        const float holdRatio = sProgressionSyncPrompt.holdSeconds / kProgressionSyncHoldDuration;
        const float countdownRatio = remaining / kProgressionSyncPromptDuration;
        const bool waiting = sProgressionSyncPrompt.waiting;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(245, 193, 51, 230),
                          6.0f, 0, 2.0f);

        ImGui::PushTextWrapPos(pos.x + 276.0f);
        ImGui::TextUnformatted(sProgressionSyncPrompt.title.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.85f, 0.92f, 1.0f));
        ImGui::TextUnformatted(sProgressionSyncPrompt.body.c_str());
        if (waiting) {
            ImGui::TextUnformatted("Please wait");
        } else {
            ImGui::Text("%.0fs", std::ceil(remaining));
        }
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        const ImVec2 ringCenter(pos.x + size.x - 46.0f, pos.y + size.y * 0.5f);
        constexpr float kPi = 3.14159265358979323846f;
        const float ringRadius = 20.0f;
        drawList->AddCircle(ringCenter, ringRadius, IM_COL32(255, 255, 255, 70), 48, 3.0f);
        drawList->AddCircle(ringCenter, ringRadius - 7.0f, IM_COL32(255, 255, 255, 45), 48, 2.0f);
        if (waiting) {
            const float t = std::fmod(sProgressionSyncPrompt.ageSeconds * 1.1f, 1.0f);
            drawList->PathArcTo(ringCenter, ringRadius, -0.5f * kPi + 2.0f * kPi * t,
                                -0.5f * kPi + 2.0f * kPi * (t + 0.72f), 48);
            drawList->PathStroke(IM_COL32(255, 176, 38, 255), 0, 5.0f);
        } else if (holdRatio > 0.0f) {
            drawList->PathArcTo(ringCenter, ringRadius, -0.5f * kPi,
                                -0.5f * kPi + 2.0f * kPi * holdRatio, 48);
            drawList->PathStroke(IM_COL32(255, 176, 38, 255), 0, 5.0f);
        }
        drawList->AddText(ImVec2(ringCenter.x - (waiting ? 5.0f : 4.0f), ringCenter.y - 8.0f),
                          IM_COL32(255, 255, 255, 245), waiting ? "..." : "S");

        if (!waiting) {
            const ImVec2 barMin(pos.x, pos.y + size.y - 3.0f);
            drawList->AddRectFilled(barMin,
                                    ImVec2(pos.x + size.x * countdownRatio, pos.y + size.y),
                                    IM_COL32(255, 176, 38, 210), 0.0f);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void draw_notifications_overlay() {
    draw_peer_name_labels();

    if (!sEnabled) {
        return;
    }

    draw_progression_sync_prompt();

    if (sNotifications.empty()) {
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
    if (is_unsynced_event_bit(flag)) {
        DuskLog.info("Multiplayer skipped unsynced local event bit flag={}", flag);
        return;
    }

    send_json({
        {"type", "event_bit"},
        {"flag", flag},
        {"set", true},
    });
    DuskLog.info("Multiplayer sent local event bit flag={}", flag);
}

void notify_local_event_bit_cleared(uint16_t flag) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }
    if (is_unsynced_event_bit(flag)) {
        DuskLog.info("Multiplayer skipped unsynced local event bit cleared flag={}", flag);
        return;
    }

    send_json({
        {"type", "event_bit"},
        {"flag", flag},
        {"set", false},
    });
    DuskLog.info("Multiplayer sent local event bit cleared flag={}", flag);
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

    if (is_unsynced_switch_bit(stageNo, flag)) {
        DuskLog.info("Multiplayer skipped unsynced local switch bit stage={} flag={}", stageNo,
                     flag);
        return;
    }

    if (hasActorContext && is_group2_lifecycle_actor(sLocalSwitchActorContext.actorName)) {
        if (is_sewers_progression_switch(stageNo, flag)) {
            DuskLog.info("Multiplayer allowed sewers progression switch stage={} flag={} "
                         "sourceActor={} sourceRoom={} reason={}",
                         stageNo, flag, sLocalSwitchActorContext.actorName,
                         sLocalSwitchActorContext.room,
                         group2_lifecycle_actor_reason(sLocalSwitchActorContext.actorName));
        } else {
            DuskLog.info("Multiplayer suppressed local switch bit stage={} flag={} sourceActor={} "
                         "sourceRoom={} reason={}",
                         stageNo, flag, sLocalSwitchActorContext.actorName,
                         sLocalSwitchActorContext.room,
                         group2_lifecycle_actor_reason(sLocalSwitchActorContext.actorName));
            return;
        }
    }

    begin_flag_trace_window("local_send", "switch", stageNo, flag, true,
                            hasActorContext ? sLocalSwitchActorContext.actorName : -1,
                            hasActorContext ? sLocalSwitchActorContext.room : -128,
                            hasActorContext ? sLocalSwitchActorContext.actorParams : 0xFFFFFFFF);

    json message = {
        {"type", "switch_bit"},
        {"stage", stageNo},
        {"flag", flag},
        {"set", true},
    };

    if (hasActorContext) {
        message["source_actor"] = sLocalSwitchActorContext.actorName;
        message["source_room"] = sLocalSwitchActorContext.room;
        message["source_params"] = sLocalSwitchActorContext.actorParams;
    }

    send_json(message);
    if (hasActorContext) {
        DuskLog.info("Multiplayer sent local switch bit stage={} flag={} sourceActor={} "
                     "sourceRoom={} sourceParams=0x{:08X}",
                     stageNo, flag, sLocalSwitchActorContext.actorName,
                     sLocalSwitchActorContext.room, sLocalSwitchActorContext.actorParams);
    } else {
        DuskLog.info("Multiplayer sent local switch bit stage={} flag={}", stageNo, flag);
    }
}

void notify_local_memory_switch_cleared(int flag) {
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

    if (is_unsynced_switch_bit(stageNo, flag)) {
        DuskLog.info("Multiplayer skipped unsynced local switch bit clear stage={} flag={}",
                     stageNo, flag);
        return;
    }

    if (hasActorContext && is_group2_lifecycle_actor(sLocalSwitchActorContext.actorName)) {
        if (is_sewers_progression_switch(stageNo, flag)) {
            DuskLog.info("Multiplayer allowed sewers progression switch clear stage={} flag={} "
                         "sourceActor={} sourceRoom={} reason={}",
                         stageNo, flag, sLocalSwitchActorContext.actorName,
                         sLocalSwitchActorContext.room,
                         group2_lifecycle_actor_reason(sLocalSwitchActorContext.actorName));
        } else {
            DuskLog.info("Multiplayer suppressed local switch bit clear stage={} flag={} "
                         "sourceActor={} sourceRoom={} reason={}",
                         stageNo, flag, sLocalSwitchActorContext.actorName,
                         sLocalSwitchActorContext.room,
                         group2_lifecycle_actor_reason(sLocalSwitchActorContext.actorName));
            return;
        }
    }

    begin_flag_trace_window("local_send", "switch", stageNo, flag, false,
                            hasActorContext ? sLocalSwitchActorContext.actorName : -1,
                            hasActorContext ? sLocalSwitchActorContext.room : -128,
                            hasActorContext ? sLocalSwitchActorContext.actorParams : 0xFFFFFFFF);

    json message = {
        {"type", "switch_bit"},
        {"stage", stageNo},
        {"flag", flag},
        {"set", false},
    };

    if (hasActorContext) {
        message["source_actor"] = sLocalSwitchActorContext.actorName;
        message["source_room"] = sLocalSwitchActorContext.room;
        message["source_params"] = sLocalSwitchActorContext.actorParams;
    }

    send_json(message);
    if (hasActorContext) {
        DuskLog.info(
            "Multiplayer sent local switch bit cleared stage={} flag={} sourceActor={} "
            "sourceRoom={} sourceParams=0x{:08X}",
            stageNo, flag, sLocalSwitchActorContext.actorName, sLocalSwitchActorContext.room,
            sLocalSwitchActorContext.actorParams);
    } else {
        DuskLog.info("Multiplayer sent local switch bit cleared stage={} flag={}", stageNo, flag);
    }
}

void begin_local_switch_actor_context(int actorName, int room, int flag, uint32_t actorParams) {
    if (sLocalSwitchActorContext.depth++ == 0) {
        sLocalSwitchActorContext.active = true;
        sLocalSwitchActorContext.actorName = actorName;
        sLocalSwitchActorContext.room = room;
        sLocalSwitchActorContext.flag = flag;
        sLocalSwitchActorContext.actorParams = actorParams;
    }
}

void end_local_switch_actor_context() {
    if (sLocalSwitchActorContext.depth <= 0) {
        sLocalSwitchActorContext.depth = 0;
        sLocalSwitchActorContext.active = false;
        sLocalSwitchActorContext.actorName = -1;
        sLocalSwitchActorContext.room = -128;
        sLocalSwitchActorContext.flag = -1;
        sLocalSwitchActorContext.actorParams = 0xFFFFFFFF;
        return;
    }

    if (--sLocalSwitchActorContext.depth == 0) {
        sLocalSwitchActorContext.active = false;
        sLocalSwitchActorContext.actorName = -1;
        sLocalSwitchActorContext.room = -128;
        sLocalSwitchActorContext.flag = -1;
        sLocalSwitchActorContext.actorParams = 0xFFFFFFFF;
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

void notify_local_light_drop_get_flag_set(uint8_t area) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit || area >= 3) {
        return;
    }

    send_json({
        {"type", "light_drop_get_flag"},
        {"area", area},
    });
    DuskLog.info("Multiplayer sent local light drop get flag area={}", area);
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

void notify_local_rupee_count_set(uint16_t rupees) {
    if (!sEnabled || !sSession.welcomed || sApplyingRemoteSaveBit) {
        return;
    }

    send_json({
        {"type", "rupee_count"},
        {"value", rupees},
    });
    DuskLog.info("Multiplayer sent local rupee count value={}", rupees);
}

}  // namespace dusk::multiplayer
