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

// Issues the Wmdl/Kmdl/Alink archive load requests this module needs, if not
// already loaded. Must be called from the simulation-tick update phase, not
// the draw phase: draw_remote_link_dummy() used to issue these requests
// itself (via resolve_remote_sources()) on first use, but that runs from the
// draw-list-build phase (see f_ap_game.cpp/f_pc_manager.cpp call sites),
// which races the engine's own dRes_control_c/JKRArchive resource-loading
// calls made from daAlink_c::changeWolf()/changeLink() during a real local
// transform (those run from the simulation tick via fapGm_Execute). That
// race is the suspected cause of a JKRArchive::findNameResource crash seen
// during a remote-peer transform. Calling this from update() instead ensures
// all the archive load requests this module ever issues happen on the same
// phase the engine's own resource manager expects, and draw_remote_link_dummy
// only ever consumes already-resolved resources.
void preload_remote_link_dummy_resources();

}  // namespace dusk::multiplayer
