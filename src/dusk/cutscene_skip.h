#pragma once

#include "dusk/settings.h"

namespace dusk {
namespace cutscene_skip {
inline bool enabled() {
    return getSettings().game.skipAllCutscenes.getValue();
}
}  // namespace cutscene_skip
}  // namespace dusk
