#pragma once

#include <cstdint>

#include "SDL3/SDL_events.h"

namespace dusk::latency_trace {

void on_sdl_event(const SDL_Event& event);
void mark(const char* stage);
void mark_detail(const char* stage, const char* detail);
void pad_snapshot(const char* stage, std::uint32_t hold, std::uint32_t trig, float stickX, float stickY);
void presented();
const char* path();

} // namespace dusk::latency_trace
