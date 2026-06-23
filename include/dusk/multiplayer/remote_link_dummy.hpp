#pragma once

#include <string>

#include "dusk/multiplayer/multiplayer.hpp"

namespace dusk::multiplayer {

// peerId selects which peer's dummy clone to draw/destroy -- see
// PeerPoseSnapshot::peerId. Today there is only ever one peer in practice
// (direct mode is 1:1; nothing drives multiple concurrent peers through
// here yet), but the dummy storage itself is keyed by peerId so adding
// real multi-peer rendering later doesn't require touching this storage
// again.
void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose);
void destroy_remote_link_dummy(const std::string& peerId);
void destroy_all_remote_link_dummies();

}  // namespace dusk::multiplayer
