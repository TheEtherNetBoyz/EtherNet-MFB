#pragma once

#include <map>
#include <string>

#include "dusk/multiplayer/multiplayer.hpp"

namespace dusk::multiplayer {

// peerId selects which peer's actor-backed visual dummy to update/destroy.
// Direct host sessions can have several remote peers, so dummy storage is
// keyed by peerId and cleanup must be per-peer.
void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose);
void sync_remote_link_actor_dummies(const std::map<std::string, PeerPoseSnapshot>& poses);
void destroy_remote_link_dummy(const std::string& peerId);
void destroy_all_remote_link_dummies();

}  // namespace dusk::multiplayer
