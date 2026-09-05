#include "dusk/game_clock.h"

#include "dusk/dusk.h"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "dusk/time.h"

#include "dusk/frame_interpolation.h"

#include <aurora/time.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace dusk::game_clock {

using native_clock = aurora::time::native_clock;
using game_clock = aurora::time::game_clock;

FrameTiming g_frameTiming;

namespace {
bool s_initialized = false;
bool s_fixedStepActive = false;
bool s_simTickActive = false;
native_clock::time_point s_previousNativeSample{};
game_clock::time_point s_latestGameSample{};
game_clock::time_point s_currentSnapshotTime{};
game_clock::time_point s_pendingSimTime{};
float s_simRateHz = 30.0f;
game_clock::duration s_simPeriodDuration =
    std::chrono::duration_cast<game_clock::duration>(std::chrono::duration<float>(kSimPeriod));
Limiter s_frameLimiter;

std::unordered_map<uintptr_t, game_clock::time_point> s_intervalLastSample;

constexpr native_clock::duration kAbnormalGapResetThreshold = std::chrono::milliseconds(250);
constexpr int kMaxSimTicksPerFrame = static_cast<int>(aurora::time::kMaximumTimeScale) * 4;

bool interpolation_enabled() {
    return !getTransientSettings().forceThirtyFpsLimit &&
           !getTransientSettings().skipFrameRateLimit &&
           getSettings().game.enableFrameInterpolation.getValue() != FrameInterpMode::Off;
}

int selected_frame_rate_limit() {
    if (getTransientSettings().skipFrameRateLimit || getTransientSettings().turboMode) {
        return 0;
    }
    if (!interpolation_enabled()) {
        return static_cast<int>(std::round(s_simRateHz));
    }
    return getSettings().game.frameRateLimit.getValue();
}

void apply_frame_rate_limit() {
    const int limit = selected_frame_rate_limit();
    if (limit <= 0) {
        s_frameLimiter.Reset();
        return;
    }
    const auto target = 1'000'000'000ULL / static_cast<Limiter::duration_t>(limit);
    const auto sleepTime = s_frameLimiter.Sleep(target);
    frameUsagePct = 100.0f * (1.0f - static_cast<float>(sleepTime) / static_cast<float>(target));
}
} // namespace

void initialize() {
    if (s_initialized) {
        return;
    }
    s_previousNativeSample = native_clock::now();
    s_latestGameSample = game_clock::now();
    s_currentSnapshotTime = s_latestGameSample;
    s_pendingSimTime = s_latestGameSample;
    s_frameLimiter.Reset();
    s_initialized = true;
}

void ensure_initialized() {
    initialize();
}

void reset() {
    initialize();
    s_previousNativeSample = native_clock::now();
    s_latestGameSample = game_clock::now();
    s_currentSnapshotTime = s_latestGameSample - s_simPeriodDuration;
    s_pendingSimTime = s_currentSnapshotTime;
    s_simTickActive = false;
    s_frameLimiter.Reset();
}

void reset_frame_timer() {
    reset();
}

void set_sim_rate(float hz) {
    const float clamped = std::clamp(hz, 1.0f, 120.0f);
    if (std::abs(s_simRateHz - clamped) < 0.001f) {
        return;
    }
    s_simRateHz = clamped;
    s_simPeriodDuration =
        std::chrono::duration_cast<game_clock::duration>(std::chrono::duration<float>(1.0f / clamped));
    reset();
}

float get_sim_rate() {
    return s_simRateHz;
}

float sim_pace() {
    return 1.0f / s_simRateHz;
}

float period_for_original_frames(float frame_count) {
    return frame_count * sim_pace();
}

const FrameTiming& advance() {
    initialize();
    const auto nativeNow = native_clock::now();
    const auto gameNow = game_clock::now();
    const auto nativeFrameGap = nativeNow - s_previousNativeSample;
    s_previousNativeSample = nativeNow;
    s_latestGameSample = gameNow;

    auto& out = g_frameTiming;
    out = {.dt = std::chrono::duration<float>(nativeFrameGap).count()};

    const float timeScale = aurora::time::scale();
    const bool interpolating = interpolation_enabled();
    const bool separatePresentation = interpolating || timeScale != 1.0f;
    out.interpolating = interpolating;
    out.separatePresentation = separatePresentation;
    s_fixedStepActive = separatePresentation;

    if (!separatePresentation) {
        s_currentSnapshotTime = gameNow;
        out.numSimTicks = 1;
        return out;
    }

    const auto simulationTarget = interpolating ? gameNow - s_simPeriodDuration : gameNow;
    if (timeScale == 0.f || nativeFrameGap > kAbnormalGapResetThreshold) {
        s_currentSnapshotTime = simulationTarget;
        out.numSimTicks = 0;
        return out;
    }

    int numSimTicks = 0;
    auto projectedSnapshotTime = s_currentSnapshotTime;
    while (numSimTicks < kMaxSimTicksPerFrame) {
        const bool tickDue = interpolating ? projectedSnapshotTime < simulationTarget :
                                             projectedSnapshotTime + s_simPeriodDuration <= simulationTarget;
        if (!tickDue) {
            break;
        }
        projectedSnapshotTime += s_simPeriodDuration;
        ++numSimTicks;
    }
    out.numSimTicks = numSimTicks;
    return out;
}

void finish_main_loop() {
    initialize();
    apply_frame_rate_limit();
}

void begin_sim_tick() {
    s_pendingSimTime = s_fixedStepActive ? s_currentSnapshotTime + s_simPeriodDuration : s_latestGameSample;
    s_simTickActive = true;
}

void commit_sim_tick() {
    if (s_simTickActive) {
        s_currentSnapshotTime = s_pendingSimTime;
        s_simTickActive = false;
    } else {
        s_currentSnapshotTime += s_simPeriodDuration;
    }
}

float sample_interpolation_step() {
    const float step = std::chrono::duration<float>(game_clock::now() - s_currentSnapshotTime).count() / sim_pace();
    return std::clamp(step, 0.0f, 1.0f);
}

float consume_interval(const void* consumer) {
    const auto key = reinterpret_cast<uintptr_t>(consumer);
    const auto now = s_simTickActive ? s_pendingSimTime : game_clock::now();
    const float timeScale = aurora::time::scale();
    float dt = kUiInitialDt * timeScale;
    if (const auto it = s_intervalLastSample.find(key); it != s_intervalLastSample.end()) {
        dt = std::chrono::duration<float>(now - it->second).count();
        const float maximumDt = std::max(kUiMaximumDt * timeScale, sim_pace());
        dt = std::min(dt, maximumDt);
    }
    s_intervalLastSample[key] = now;
    return dt;
}

} // namespace dusk::game_clock

namespace dusk {

bool low_latency_presentation_enabled() {
    const bool isThirtyFpsMode = getTransientSettings().forceThirtyFpsLimit ||
                                 getSettings().game.enableFrameInterpolation.getValue() == FrameInterpMode::Off;
    return isThirtyFpsMode && getSettings().game.lowLatencyPresentation.getValue();
}

} // namespace dusk
