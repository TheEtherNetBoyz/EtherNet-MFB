#include "dusk/multiplayer/multiplayer.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_sword.h"
#include "d/actor/d_a_tbox.h"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_item_data.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/event_sync.hpp"
#include "dusk/multiplayer/invite_code.hpp"
#include "dusk/multiplayer/remote_link_dummy.hpp"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_mtx.h"
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

#include <array>
#include <charconv>
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

namespace dusk::multiplayer {
namespace {

using json = nlohmann::json;

// Direct mode (DirectHost/DirectJoin) is a raw 1:1 TCP pipe -- the peer
// never tags its own messages with a client_id, unlike the relay
// (tools/multiplayer_relay), which assigns one and stamps every routed
// message with the sender's. This placeholder key stands in for "the one
// peer" when there's no real client_id to key peer-specific state by.
const char* const kDirectPeerId = "direct";

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
    std::string rxBuffer;
    int port = 34197;
    bool debugMarker = false;
    uint32_t reconnectTicks = 0;
    uint32_t pingTicks = 0;
    uint32_t poseSequence = 0;
    uint32_t peerPoseLogTicks = 0;
    // Keyed by peer ID (relay client_id, or kDirectPeerId in direct mode).
    // Only one peer exists in practice today (direct mode is 1:1), but this
    // is peer-keyed so multi-peer support is a drawing/networking-layer
    // change later, not a data-model one -- see remote_link_dummy.hpp.
    std::map<std::string, PeerPoseSnapshot> peerPoses;
    bool helloSent = false;
    bool welcomeSent = false;
    bool welcomed = false;
    bool snapshotPending = false;
    uint32_t repairSweepTicks = 0;
};

bool sInitialized = false;
bool sEnabled = false;
Session sSession;

bool sDummyModelEnabled = false;

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

bool is_stage_dependent_message_type(const std::string& type) {
    return type == "save_snapshot" || type == "tbox_bit" || type == "switch_bit" ||
           type == "item_bit" || type == "dungeon_item_bit";
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

void reset_connection_state() {
    close_socket(sSession.sock);
    close_socket(sSession.listenSock);
    sSession.state = ConnectionState::Disconnected;
    sSession.helloSent = false;
    sSession.welcomeSent = false;
    sSession.welcomed = false;
    sSession.snapshotPending = false;
    sPendingStageMessages.clear();
    sSession.rxBuffer.clear();
    sSession.reconnectTicks = 0;
    sSession.pingTicks = 0;
    sSession.poseSequence = 0;
    sSession.peerPoseLogTicks = 0;
    sSession.peerPoses.clear();
    destroy_all_remote_link_dummies();
}

void disconnect(const char* reason) {
    if (sSession.state != ConnectionState::Disconnected) {
        DuskLog.warn("Multiplayer disconnected: {}", reason);
    }

    reset_connection_state();
}

bool send_json(const json& message) {
    if (sSession.sock == INVALID_SOCKET) {
        return false;
    }

    std::string bytes = message.dump();
    bytes.push_back('\n');
    const char* cursor = bytes.data();
    int remaining = static_cast<int>(bytes.size());

    while (remaining > 0) {
        const int sent = send(sSession.sock, cursor, remaining, kSendFlags);
        if (sent > 0) {
            cursor += sent;
            remaining -= sent;
            continue;
        }

        if (would_block()) {
            return true;
        }

        disconnect("send failed");
        return false;
    }

    return true;
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
void send_save_snapshot() {
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
    }

    send_json({
        {"type", "save_snapshot"},
        {"event_flags", eventFlags},
        {"chests", chestStages},
        {"switches", switchStages},
        {"items", itemStages},
        {"dungeon_items", dungeonStages},
        {"max_life", dComIfGs_getMaxLife()},
    });
    DuskLog.info(
        "Multiplayer sent save snapshot event_flags={} chest_stages={} switch_stages={} item_stages={} dungeon_stages={}",
        eventFlags.size(), chestStages.size(), switchStages.size(), itemStages.size(),
        dungeonStages.size());
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

void add_link_matrices(json& state) {
    if (!sDummyModelEnabled) {
        return;
    }

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
        return;
    }

    daAlink_c* link = static_cast<daAlink_c*>(playerActor);
    if (link->mProcID == daAlink_c::PROC_METAMORPHOSE || link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY) {
        // Wolf<->human transformation. daAlink_c::changeWolf()/changeLink()
        // free the old model/arc resources (mpArcHeap->freeAll()) and then
        // reassign mpLinkModel/mSwordModel/mShieldModel to freshly-allocated
        // objects partway through this state, not atomically with the state
        // transition itself -- reading those fields during the window is a
        // use-after-free risk, not just a stale-data one.
        return;
    }

    if (link->mpLinkModel == nullptr) {
        return;
    }

    state["link_matrices"] = {
        {"body", model_matrices_to_json(link->mpLinkModel)},
        {"hat", model_matrices_to_json(link->mpLinkHatModel)},
        {"face", model_matrices_to_json(link->mpLinkFaceModel)},
        {"hand", model_matrices_to_json(link->mpLinkHandModel)},
        {"sword", model_matrices_to_json(link->mSwordModel)},
        {"shield", model_matrices_to_json(link->mShieldModel)},
    };
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
    snapshot.shield = parse_model_matrices(it->value("shield", json::object()));
    snapshot.valid = snapshot.body.valid;
    return snapshot;
}

void handle_message(const json& message) {
    const std::string type = message.value("type", "");
    if (is_stage_dependent_message_type(type) && dComIfGp_getStageStagInfo() == nullptr) {
        sPendingStageMessages.push_back(message);
        return;
    }

    if (type == "hello" && sSession.mode == NetworkMode::DirectHost) {
        DuskLog.info("Multiplayer direct peer joined name={} room={}", message.value("name", ""),
                     message.value("room_id", ""));
        send_welcome();
    } else if (type == "welcome") {
        sSession.welcomed = true;
        DuskLog.info("Multiplayer joined room={} client_id={} peers={}",
                     message.value("room_id", ""), message.value("client_id", ""),
                     message.value("peers", json::array()).size());
        sSession.snapshotPending = true;
    } else if (type == "peer_joined") {
        DuskLog.info("Multiplayer peer joined id={} name={}", message.value("client_id", ""),
                     message.value("name", ""));
    } else if (type == "peer_left") {
        const std::string leftPeerId = resolve_peer_id(message);
        DuskLog.info("Multiplayer peer left id={}", leftPeerId);
        sSession.peerPoses.erase(leftPeerId);
        destroy_remote_link_dummy(leftPeerId);
    } else if (type == "save_snapshot") {
        sApplyingRemoteSaveBit = true;
        for (const json& flag : message.value("event_flags", json::array())) {
            dComIfGs_onEventBit(static_cast<uint16_t>(flag.get<int>()));
        }
        for (const json& entry : message.value("chests", json::array())) {
            const int stage = entry.value("stage", -1);
            for (const json& flag : entry.value("flags", json::array())) {
                dComIfGs_onStageTbox(stage, flag.get<int>());
                DuskLog.info("Multiplayer snapshot chest bit stage={} flag={}", stage,
                             flag.get<int>());
            }
        }
        for (const json& entry : message.value("switches", json::array())) {
            const int stage = entry.value("stage", -1);
            for (const json& flag : entry.value("flags", json::array())) {
                dComIfGs_onStageSwitch(stage, flag.get<int>());
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
        sApplyingRemoteSaveBit = false;
        const int remoteMaxLife = message.value("max_life", 0);
        if (remoteMaxLife > dComIfGs_getMaxLife() && remoteMaxLife <= 100) {
            dComIfGs_setMaxLife(static_cast<u8>(remoteMaxLife));
            DuskLog.info("Multiplayer snapshot max life set to {}", remoteMaxLife);
        }
        DuskLog.info("Multiplayer applied save snapshot from peer");
    } else if (type == "pose") {
        const std::string peerId = resolve_peer_id(message);
        const uint32_t sequence = message.value("sequence", 0U);
        auto existing = sSession.peerPoses.find(peerId);
        if (existing != sSession.peerPoses.end() && existing->second.valid &&
            sequence <= existing->second.sequence)
        {
            return;
        }

        const json state = message.value("state", json::object());
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
        pose.isWolf = state.value("is_wolf", false);
        pose.linkMatrices = parse_link_matrices(state);
        sSession.peerPoses[peerId] = pose;

        if ((sSession.peerPoseLogTicks++ % 150) == 0) {
            DuskLog.info("Multiplayer peer pose peer={} stage={} room={} pos=({}, {}, {})", peerId,
                         pose.stage, pose.room, pose.x, pose.y, pose.z);
        }
    } else if (type == "event_bit") {
        const uint16_t flag = message.value("flag", 0U);
        sApplyingRemoteSaveBit = true;
        dComIfGs_onEventBit(flag);
        sApplyingRemoteSaveBit = false;
        DuskLog.info("Multiplayer applied remote event bit flag={}", flag);
    } else if (type == "tbox_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        if (stage >= 0 && flag >= 0) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onStageTbox(stage, flag);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote chest bit stage={} flag={}", stage, flag);

            stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
            if (stagInfo != nullptr && stage == dStage_stagInfo_GetSaveTbl(stagInfo)) {
                duskRepairTboxVisual(flag);
            }
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
    } else if (type == "switch_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        if (stage >= 0 && flag >= 0) {
            sApplyingRemoteSaveBit = true;
            dComIfGs_onStageSwitch(stage, flag);
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote switch bit stage={} flag={}", stage, flag);
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
        if (itemId == dItemNo_KAKERA_HEART_e || itemId == dItemNo_UTAWA_HEART_e) {
            sApplyingRemoteSaveBit = true;
            execItemGet(static_cast<u8>(itemId));
            sApplyingRemoteSaveBit = false;
            DuskLog.info("Multiplayer applied remote item get item_id={}", itemId);
        }
    } else if (type == "pong") {
        DuskLog.debug("Multiplayer pong");
    } else if (type == "error") {
        DuskLog.warn("Multiplayer remote error: {}", message.value("error", ""));
    } else {
        DuskLog.debug("Multiplayer message type={}", type);
    }
}

void pump_receive() {
    std::array<char, 4096> buffer{};

    while (true) {
        const int read = recv(sSession.sock, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (read > 0) {
            sSession.rxBuffer.append(buffer.data(), static_cast<size_t>(read));

            size_t newline = std::string::npos;
            while ((newline = sSession.rxBuffer.find('\n')) != std::string::npos) {
                const std::string line = sSession.rxBuffer.substr(0, newline);
                sSession.rxBuffer.erase(0, newline + 1);
                if (line.empty()) {
                    continue;
                }

                try {
                    handle_message(json::parse(line));
                } catch (const json::exception& e) {
                    DuskLog.warn("Multiplayer received invalid JSON: {}", e.what());
                }
            }
            continue;
        }

        if (read == 0) {
            disconnect("remote closed");
            return;
        }

        if (would_block()) {
            return;
        }

        disconnect("recv failed");
        return;
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
        listen(sSession.listenSock, 1) != 0) {
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

    close_socket(sSession.listenSock);
    sSession.sock = accepted;
    if (!set_nonblocking(sSession.sock)) {
        disconnect("accepted nonblocking failed");
        return;
    }

    sSession.state = ConnectionState::Connected;
    DuskLog.info("Multiplayer direct peer connected");
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
    const bool isWolf = fopAcM_GetName(player) == fpcNm_ALINK_e &&
                         static_cast<daAlink_c*>(player)->checkWolf();

    json state = {
        {"stage", dComIfGp_getStartStageName()},
        {"room", static_cast<int>(fopAcM_GetRoomNo(player))},
        {"layer", static_cast<int>(dComIfGp_getStartStageLayer())},
        {"x", player->current.pos.x},
        {"y", player->current.pos.y},
        {"z", player->current.pos.z},
        {"angle_y", static_cast<int>(player->shape_angle.y)},
        {"is_wolf", isWolf},
    };
    add_link_matrices(state);

    send_json({
        {"type", "pose"},
        {"sequence", ++sSession.poseSequence},
        {"state", state},
    });
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
    if (sSession.mode == NetworkMode::RelayHarness || sSession.mode == NetworkMode::DirectJoin) {
        send_hello();
    }

    pump_receive();

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
    sSession.port = env_int("DUSK_MP_PORT", 34197);
    sSession.debugMarker = env_enabled("DUSK_MP_DEBUG_MARKER");
    sDummyModelEnabled = env_enabled("DUSK_MP_DUMMY_MODEL");
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

#if _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        sEnabled = false;
        DuskLog.warn("Multiplayer module disabled: WSAStartup failed");
        return;
    }
#endif

    DuskLog.info("Multiplayer module enabled mode={} room={}", mode_name(sSession.mode),
                 sSession.room);
}

void update() {
    if (!sInitialized || !sEnabled) {
        return;
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
    if (sEnabled) {
        WSACleanup();
    }
#endif
    sInitialized = false;
    sEnabled = false;
    DuskLog.info("Multiplayer module shut down");
}

bool is_enabled() {
    return sEnabled;
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
    if (!sEnabled || !sDummyModelEnabled || !sSession.debugMarker || !has_recent_peer_pose(30) ||
        !is_peer_dummy_gameplay_ready())
    {
        return;
    }

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    const int localRoom = static_cast<int>(fopAcM_GetRoomNo(player));
    const char* localStage = dComIfGp_getStartStageName();

    // Only one entry exists in practice today (direct mode is 1:1), but
    // this iterates the full map so multi-peer rendering later is just a
    // matter of more peers showing up here -- no further changes needed
    // to this loop.
    for (const auto& entry : sSession.peerPoses) {
        const PeerPoseSnapshot& pose = entry.second;
        if (!pose.valid || pose.ageTicks > 30) {
            continue;
        }
        if (pose.stage != localStage || pose.room != localRoom) {
            continue;
        }
        draw_remote_link_dummy(entry.first, pose);
    }
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

    if (itemId != dItemNo_KAKERA_HEART_e && itemId != dItemNo_UTAWA_HEART_e) {
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
    send_json({
        {"type", "switch_bit"},
        {"stage", stageNo},
        {"flag", flag},
    });
    DuskLog.info("Multiplayer sent local switch bit stage={} flag={}", stageNo, flag);
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

}  // namespace dusk::multiplayer
