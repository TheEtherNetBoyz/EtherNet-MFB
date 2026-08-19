#pragma once

namespace dusk {

// Reload the current area using the entrance retained by the game's stage
// transition state.
void reload_area();

// Poll the fixed L+R+Start+A controller command once per game frame.
void update_area_reload_input();

}  // namespace dusk
