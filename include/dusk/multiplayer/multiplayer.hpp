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
};

struct DirectJoinOptions {
    std::string name = "Joiner";
    std::string inviteCode;
    bool debugMarker = true;
    bool dummyModel = true;
};

struct RelayJoinOptions {
    std::string name = "Player";
    std::string room = "dev";
    std::string password;
    std::string host = "127.0.0.1";
    int port = 34197;
    bool debugMarker = true;
    bool dummyModel = true;
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
    bool hasRecentPeerPose = false;
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
bool has_recent_peer_pose(uint32_t maxAgeTicks);
PeerPoseSnapshot get_latest_peer_pose();
void draw_debug_peer_marker();
void draw_notifications_overlay();
bool was_switch_recently_remote_set(int stage, int flag, uint32_t* ageTicks = nullptr);

}  // namespace dusk::multiplayer
