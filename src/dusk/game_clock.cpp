#include "dusk/game_clock.h"

#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "dusk/time.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace dusk::game_clock {

using clock = std::chrono::steady_clock;

bool s_initialized = false;
clock::time_point s_previous_sample{};
clock::time_point s_current_snapshot_time{};
Limiter s_frame_limiter;

std::unordered_map<uintptr_t, clock::time_point> s_interval_last_sample;

constexpr clock::duration kSimPeriodDuration =
    std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(sim_pace()));
constexpr clock::duration kAbnormalGapResetThreshold = std::chrono::milliseconds(250);
constexpr int kMaxSimTicksPerFrame = 2;

int selected_frame_rate_limit() {
    if (dusk::getTransientSettings().forceThirtyFpsLimit) {
        return 30;
    }

    if (dusk::getTransientSettings().skipFrameRateLimit) {
        return 0;
    }

    if (!dusk::getSettings().game.enableFrameInterpolation) {
        return 30;
    }

    return dusk::getSettings().game.frameRateLimit.getValue();
}

void apply_frame_rate_limit() {
    const int limit = selected_frame_rate_limit();
    if (limit <= 0) {
        s_frame_limiter.Reset();
        return;
    }

    s_frame_limiter.Sleep(1'000'000'000ULL / static_cast<Limiter::duration_t>(limit));
}

clock::duration interpolation_buffer_duration() {
    return kSimPeriodDuration;
}

void ensure_initialized() {
    if (s_initialized) {
        return;
    }
    s_previous_sample = clock::now();
    s_current_snapshot_time = s_previous_sample;
    s_frame_limiter.Reset();
    s_initialized = true;
}

void reset_frame_timer() {
    s_previous_sample = clock::now();
    s_current_snapshot_time = s_previous_sample - kSimPeriodDuration;
    s_frame_limiter.Reset();
}

MainLoopPacer advance_main_loop() {
    ensure_initialized();

    const clock::time_point now = clock::now();
    const clock::duration frame_gap = now - s_previous_sample;
    const float presentation_dt = std::chrono::duration<float>(frame_gap).count();
    s_previous_sample = now;

    MainLoopPacer out{};
    out.presentation_dt_seconds = presentation_dt;

    const bool should_interpolate = !dusk::getTransientSettings().forceThirtyFpsLimit &&
                                    dusk::getSettings().game.enableFrameInterpolation &&
                                    !dusk::getTransientSettings().skipFrameRateLimit;
    out.is_interpolating = should_interpolate;
    out.sim_pace = sim_pace();
    out.interpolation_buffer_seconds = 0.0f;

    if (!should_interpolate) {
        s_current_snapshot_time = now;
        out.sim_ticks_to_run = 1;
        return out;
    }

    if (frame_gap > kAbnormalGapResetThreshold) {
        s_current_snapshot_time = now - kSimPeriodDuration;
        out.sim_ticks_to_run = 0;
        return out;
    }

    int sim_ticks_to_run = 0;
    clock::time_point projected_snapshot_time = s_current_snapshot_time;
    const clock::duration interpolation_buffer = interpolation_buffer_duration();
    out.interpolation_buffer_seconds = std::chrono::duration<float>(interpolation_buffer).count();

    const clock::time_point render_time = now - interpolation_buffer;
    while (sim_ticks_to_run < kMaxSimTicksPerFrame && projected_snapshot_time < render_time) {
        projected_snapshot_time += kSimPeriodDuration;
        sim_ticks_to_run++;
    }
    out.sim_ticks_to_run = sim_ticks_to_run;
    return out;
}

void finish_main_loop() {
    ensure_initialized();
    apply_frame_rate_limit();
}

void commit_sim_tick() {
    ensure_initialized();
    s_current_snapshot_time += kSimPeriodDuration;
}

float sample_interpolation_step() {
    ensure_initialized();
    const clock::duration interpolation_buffer = interpolation_buffer_duration();
    const float step =
        std::chrono::duration<float>(clock::now() - s_current_snapshot_time +
                                     kSimPeriodDuration - interpolation_buffer).count() /
        sim_pace();
    return std::clamp(step, 0.0f, 1.0f);
}

float consume_interval(const void* consumer) {
    ensure_initialized();
    const uintptr_t key = reinterpret_cast<uintptr_t>(consumer);
    const clock::time_point now = clock::now();

    float dt = ui_initial_dt();
    const auto it = s_interval_last_sample.find(key);
    if (it != s_interval_last_sample.end()) {
        dt = std::chrono::duration<float>(now - it->second).count();
        dt = std::min(dt, ui_maximum_dt());
    }
    s_interval_last_sample[key] = now;
    return dt;
}

}  // namespace dusk::game_clock

namespace dusk {

bool low_latency_presentation_enabled() {
    const bool is_thirty_fps_mode = getTransientSettings().forceThirtyFpsLimit ||
                                    !getSettings().game.enableFrameInterpolation.getValue();
    return is_thirty_fps_mode && getSettings().game.lowLatencyPresentation.getValue();
}

}  // namespace dusk
