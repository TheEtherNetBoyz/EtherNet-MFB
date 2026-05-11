#ifndef DUSK_DUSK_H
#define DUSK_DUSK_H

#include <aurora/aurora.h>

#include "aurora/gfx.h"

extern AuroraInfo auroraInfo;

namespace dusk {
    struct PainterDebugInfo {
        int windowNum = 0;
        bool pauseFlag = false;
        bool uiTickPending = false;
        bool hasCameraWindow = false;
        bool hasCamera = false;
        bool ranWorldPostEffects = false;
        bool skippedWorldPostEffects = false;
        bool drew2D = false;
        bool drewNoCamera2D = false;
        AuroraStats phaseStats[7]{};
    };

    extern AuroraStats lastFrameAuroraStats;
    extern PainterDebugInfo lastPainterDebugInfo;
    extern float frameUsagePct;
}

constexpr u32 defaultWindowWidth = 608;
constexpr u32 defaultWindowHeight = 448;

constexpr u32 defaultAspectRatioW = 19;
constexpr u32 defaultAspectRatioH = 14;

static_assert(defaultWindowWidth / defaultAspectRatioW == defaultWindowHeight / defaultAspectRatioH);

#endif  // DUSK_DUSK_H
