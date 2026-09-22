#pragma once

#include <cstdint>

namespace dusk::game_clock {

// Default amount of time advanced by a simulation tick.
constexpr float kSimPeriod = 1.0f / 30.0f;
constexpr float kUiMaximumDt = 0.05f;
constexpr float kUiInitialDt = 1.0f / 60.0f;

float original_frames();
// TAS-only presentation budget; -1 uses normal delta time, reset by advance().
void set_presentation_tick_override(int ticks);

struct FrameTiming {
    float dt;
    bool interpolating;
    bool separatePresentation;
    int numSimTicks;
    // Changes whenever presentation history must be discarded and re-anchored.
    uint64_t presentationEpoch;
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
bool is_sim_frame();
bool is_presentation_frame();

double sample_time();
float consume_interval(double& lastSample);

} // namespace dusk::game_clock
