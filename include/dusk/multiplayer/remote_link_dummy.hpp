#pragma once

#include <map>
#include <string>

#include "SSystem/SComponent/c_xyz.h"
#include "dusk/multiplayer/multiplayer.hpp"

namespace dusk::multiplayer {

// peerId selects which peer's actor-backed visual dummy to update/destroy.
// Direct host sessions can have several remote peers, so dummy storage is
// keyed by peerId and cleanup must be per-peer.
void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose);
void sync_remote_link_actor_dummies(const std::map<std::string, PeerPoseSnapshot>& poses);
bool get_remote_link_dummy_label_position(const std::string& peerId, cXyz* outPos);
void destroy_remote_link_dummy(const std::string& peerId);
void destroy_all_remote_link_dummies();

}  // namespace dusk::multiplayer
