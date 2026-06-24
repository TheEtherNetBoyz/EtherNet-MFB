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
    uint16_t equipItem = 0xFFFF;
    int swordVariant = 0;
    int clothesVariant = 0;
    bool swordDraw = false;
    bool shieldDraw = false;
    bool swordOut = false;
    RemoteLinkMatrixSnapshot linkMatrices;
};

void initialize();
void update();
void shutdown();
bool is_enabled();
bool has_recent_peer_pose(uint32_t maxAgeTicks);
PeerPoseSnapshot get_latest_peer_pose();
void draw_debug_peer_marker();

}  // namespace dusk::multiplayer
