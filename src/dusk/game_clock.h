#pragma once

namespace dusk::game_clock {

// Default amount of time advanced by a simulation tick.
constexpr float kSimPeriod = 1.0f / 30.0f;
constexpr float kUiMaximumDt = 0.05f;
constexpr float kUiInitialDt = 1.0f / 60.0f;

struct FrameTiming {
    float dt;
    bool interpolating;
    bool separatePresentation;
    int numSimTicks;
};
extern FrameTiming g_frameTiming;

void initialize();
void ensure_initialized();
void reset();
void reset_frame_timer();
void set_sim_rate(float hz);
float get_sim_rate();
float sim_pace();
float period_for_original_frames(float frame_count);
constexpr float ui_maximum_dt() { return kUiMaximumDt; }
constexpr float ui_initial_dt() { return kUiInitialDt; }

const FrameTiming& advance();
void finish_main_loop();
void begin_sim_tick();
void commit_sim_tick();
float sample_interpolation_step();
float consume_interval(const void* consumer);

} // namespace dusk::game_clock
