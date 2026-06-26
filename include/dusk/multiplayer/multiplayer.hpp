#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace dusk::multiplayer {

struct RemoteModelMatrixSnapshot {
    bool valid = false;
    uint16_t jointCount = 0;
    uint16_t weightCount = 0;
    std::array<float, 12> base{};
    std::vector<float> joints;
    std::vector<float> weights;
};

struct RemoteLinkMatrixSnapshot {
    bool valid = false;
    RemoteModelMatrixSnapshot body;
    RemoteModelMatrixSnapshot hat;
    RemoteModelMatrixSnapshot face;
    RemoteModelMatrixSnapshot hand;
    RemoteModelMatrixSnapshot sword;
    RemoteModelMatrixSnapshot sheath;
    RemoteModelMatrixSnapshot shield;
    RemoteModelMatrixSnapshot heldItem;
    RemoteModelMatrixSnapshot hookTip;
    RemoteModelMatrixSnapshot hookSubItem;
    RemoteModelMatrixSnapshot hookSubTip;
    RemoteModelMatrixSnapshot arrow;
    RemoteModelMatrixSnapshot kantera;
    RemoteModelMatrixSnapshot kanteraGlow;
    RemoteModelMatrixSnapshot itemActor;
    RemoteModelMatrixSnapshot rideActor;
    RemoteModelMatrixSnapshot midna;
    RemoteModelMatrixSnapshot midnaMask;
    RemoteModelMatrixSnapshot midnaHand;
    RemoteModelMatrixSnapshot midnaHair;
    int midnaHairShape = 0;
};

struct PeerPoseSnapshot {
    bool valid = false;
    // Identifies which peer this pose belongs to: the relay-assigned
    // client_id when connected via the relay (tools/multiplayer_relay),
    // or a fixed placeholder in direct mode (which only ever has one
    // peer today). Stored per-pose, not just as a map key, so callers
    // that copy a snapshot out of the map don't lose the association.
    std::string peerId;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string stage;
    int room = -1;
    int layer = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    int procId = 0;
    int procVar0 = 0;
    int procVar1 = 0;
    int procVar2 = 0;
    int procVar3 = 0;
    int procVar5 = 0;
    float underFrame = 0.0f;
    int underBck0 = 0;
    float underFrame0 = 0.0f;
    float underRate0 = 1.0f;
    int upperBck2 = 0;
    float upperFrame2 = 0.0f;
    float upperRate2 = 1.0f;
    bool isWolf = false;
    bool isTransforming = false;
    bool transformFromWolf = false;
    bool transformToWolf = false;
    int transformProcVar0 = 0;
    int transformProcVar5 = 0;
    int transformClothesWait = 0;
    float transformFrame = 0.0f;
    int transformProcVar2 = 0;
    int transformProcVar3 = 0;
    int transformShapeX = 0;
    uint16_t equipItem = 0xFFFF;
    int swordVariant = 0;
    int shieldVariant = 0;
    int clothesVariant = 0;
    bool swordDraw = false;
    bool shieldDraw = false;
    bool swordOut = false;
    bool itemDraw = false;
    bool kanteraDraw = false;
    int itemActorKind = 0;
    int rideActorKind = 0;
    RemoteLinkMatrixSnapshot linkMatrices;
};

struct DirectHostOptions {
    std::string name = "Host";
    std::string room = "dev";
    std::string bindHost = "0.0.0.0";
    std::string publicHost = "127.0.0.1";
    int port = 34197;
    bool debugMarker = true;
    bool dummyModel = true;
    bool nameLabels = true;
};

struct DirectJoinOptions {
    std::string name = "Joiner";
    std::string inviteCode;
    bool debugMarker = true;
    bool dummyModel = true;
    bool nameLabels = true;
};

struct RelayJoinOptions {
    std::string name = "Player";
    std::string room = "dev";
    std::string password;
    std::string host = "127.0.0.1";
    int port = 34197;
    bool debugMarker = true;
    bool dummyModel = true;
    bool nameLabels = true;
};

struct SessionStatus {
    bool enabled = false;
    std::string mode = "disabled";
    std::string state = "disabled";
    std::string name;
    std::string room;
    std::string host;
    std::string bindHost;
    std::string publicHost;
    std::string inviteCode;
    int port = 0;
    bool debugMarker = false;
    bool dummyModel = false;
    bool nameLabels = false;
    bool nameLabelsHostControlled = false;
    bool hasRecentPeerPose = false;
};

struct PlayerListEntry {
    std::string id;
    std::string name;
    std::string status;
    std::string area;
    uint32_t ageTicks = 0;
    bool local = false;
    bool recentPose = false;
};

void initialize();
void update();
void shutdown();
bool is_enabled();
bool host_direct(const DirectHostOptions& options, std::string* errorOut = nullptr);
bool join_direct(const DirectJoinOptions& options, std::string* errorOut = nullptr);
bool join_relay(const RelayJoinOptions& options, std::string* errorOut = nullptr);
void disconnect_session();
SessionStatus get_session_status();
std::vector<PlayerListEntry> get_player_list();
void set_name_labels_enabled(bool enabled);
bool has_recent_peer_pose(uint32_t maxAgeTicks);
PeerPoseSnapshot get_latest_peer_pose();
void draw_debug_peer_marker();
void draw_notifications_overlay();
bool was_switch_recently_remote_set(int stage, int flag, uint32_t* ageTicks = nullptr);

}  // namespace dusk::multiplayer
