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
};

struct PeerPoseSnapshot {
    bool valid = false;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string stage;
    int room = -1;
    int layer = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
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
