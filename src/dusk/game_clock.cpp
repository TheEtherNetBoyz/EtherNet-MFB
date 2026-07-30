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

float s_sim_rate_hz = 30.0f;
clock::duration s_sim_period_duration =
    std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(sim_pace()));
constexpr clock::duration kAbnormalGapResetThreshold = std::chrono::milliseconds(250);
constexpr int kMaxSimTicksPerFrame = 2;

int selected_frame_rate_limit() {
    if (dusk::getTransientSettings().skipFrameRateLimit) {
        return 0;
    }

    const bool interpolationOff =
        dusk::getTransientSettings().forceThirtyFpsLimit ||
        dusk::getSettings().game.enableFrameInterpolation.getValue() == FrameInterpMode::Off;
    if (interpolationOff) {
        return static_cast<int>(std::round(s_sim_rate_hz));
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
    return s_sim_period_duration;
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
    ensure_initialized();
    s_previous_sample = clock::now();
    s_current_snapshot_time = s_previous_sample - s_sim_period_duration;
    s_frame_limiter.Reset();
}

void set_sim_rate(float hz) {
    const float clamped = std::clamp(hz, 1.0f, 120.0f);
    if (std::abs(s_sim_rate_hz - clamped) < 0.001f) {
        return;
    }
    s_sim_rate_hz = clamped;
    s_sim_period_duration =
        std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(1.0f / clamped));
    reset_frame_timer();
}

float get_sim_rate() {
    return s_sim_rate_hz;
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
                                    dusk::getSettings().game.enableFrameInterpolation.getValue() != FrameInterpMode::Off &&
                                    !dusk::getTransientSettings().skipFrameRateLimit;
    out.is_interpolating = should_interpolate;
    out.sim_pace = 1.0f / s_sim_rate_hz;
    out.interpolation_buffer_seconds = 0.0f;

    if (!should_interpolate) {
        s_current_snapshot_time = now;
        out.sim_ticks_to_run = 1;
        return out;
    }

    if (frame_gap > kAbnormalGapResetThreshold) {
        s_current_snapshot_time = now - s_sim_period_duration;
        out.sim_ticks_to_run = 0;
        return out;
    }

    int sim_ticks_to_run = 0;
    clock::time_point projected_snapshot_time = s_current_snapshot_time;
    const clock::duration interpolation_buffer = interpolation_buffer_duration();
    out.interpolation_buffer_seconds = std::chrono::duration<float>(interpolation_buffer).count();

    const clock::time_point render_time = now - interpolation_buffer;
    while (sim_ticks_to_run < kMaxSimTicksPerFrame && projected_snapshot_time < render_time) {
        projected_snapshot_time += s_sim_period_duration;
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
    s_current_snapshot_time += s_sim_period_duration;
}

float sample_interpolation_step() {
    ensure_initialized();
    const clock::duration interpolation_buffer = interpolation_buffer_duration();
    const float step =
        std::chrono::duration<float>(clock::now() - s_current_snapshot_time +
                                     s_sim_period_duration - interpolation_buffer).count() /
        (1.0f / s_sim_rate_hz);
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
                                    getSettings().game.enableFrameInterpolation.getValue() == FrameInterpMode::Off;
    return is_thirty_fps_mode && getSettings().game.lowLatencyPresentation.getValue();
}

}  // namespace dusk
