#include "dusk/latency_trace.h"

#include "dusk/io.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/settings.h"

#include <SDL3/SDL_timer.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace {

struct TraceState {
    std::mutex mutex;
    FILE* file = nullptr;
    std::u8string path;
    std::uint64_t nextId = 1;
    std::uint64_t activeId = 0;
    std::uint64_t inputNs = 0;
    std::uint64_t frame = 0;
    int framesSinceInput = 0;
    bool announced = false;
};

TraceState g_state;

bool enabled() {
    return dusk::getSettings().game.enableLatencyTrace.getValue();
}

const char* event_kind(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        return event.key.repeat ? nullptr : "key_down";
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return "gamepad_button_down";
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return "mouse_button_down";
    default:
        return nullptr;
    }
}

std::uint64_t now_ns() {
    return SDL_GetTicksNS();
}

void close_file_locked() {
    if (g_state.file != nullptr) {
        std::fflush(g_state.file);
        std::fclose(g_state.file);
        g_state.file = nullptr;
    }
}

bool ensure_file_locked() {
    if (!enabled()) {
        close_file_locked();
        g_state.announced = false;
        return false;
    }

    if (g_state.file != nullptr) {
        return true;
    }

    std::error_code ec;
    const std::filesystem::path traceDir = dusk::CachePath.empty() ?
        std::filesystem::current_path(ec) :
        dusk::CachePath / "logs";
    if (!traceDir.empty()) {
        std::filesystem::create_directories(traceDir, ec);
    }

    const std::filesystem::path tracePath = traceDir / "latency-trace.csv";
    g_state.file = dusk::io::FileStream::Create(tracePath).ToInner();
    if (g_state.file == nullptr) {
        DuskLog.warn("Latency trace failed to open {}", dusk::io::fs_path_to_string(tracePath));
        return false;
    }

    g_state.path = tracePath.u8string();
    std::fprintf(g_state.file,
        "trace_id,frame,ns,ms_since_input,stage,detail,hold,trig,stick_x,stick_y\n");
    std::fflush(g_state.file);
    if (!g_state.announced) {
        DuskLog.info("Latency trace writing to {}", dusk::io::fs_path_to_string(tracePath));
        g_state.announced = true;
    }
    return true;
}

void write_locked(const char* stage, const char* detail, std::uint32_t hold, std::uint32_t trig,
    float stickX, float stickY) {
    if (!ensure_file_locked() || g_state.activeId == 0) {
        return;
    }

    const std::uint64_t now = now_ns();
    const double msSinceInput =
        g_state.inputNs == 0 ? 0.0 : static_cast<double>(now - g_state.inputNs) / 1'000'000.0;
    std::fprintf(g_state.file, "%llu,%llu,%llu,%.3f,%s,%s,%u,%u,%.4f,%.4f\n",
        static_cast<unsigned long long>(g_state.activeId),
        static_cast<unsigned long long>(g_state.frame),
        static_cast<unsigned long long>(now),
        msSinceInput,
        stage == nullptr ? "" : stage,
        detail == nullptr ? "" : detail,
        hold,
        trig,
        stickX,
        stickY);
    std::fflush(g_state.file);
}

} // namespace

namespace dusk::latency_trace {

void on_sdl_event(const SDL_Event& event) {
    const char* kind = event_kind(event);
    if (kind == nullptr) {
        return;
    }

    std::lock_guard lock(g_state.mutex);
    if (!ensure_file_locked()) {
        return;
    }

    g_state.activeId = g_state.nextId++;
    const std::uint64_t eventNs = event.common.timestamp;
    const std::uint64_t receivedNs = now_ns();
    g_state.inputNs = eventNs == 0 ? receivedNs : eventNs;
    g_state.framesSinceInput = 0;

    char detail[96];
    const double eventAgeMs =
        eventNs == 0 || eventNs > receivedNs ? 0.0 :
        static_cast<double>(receivedNs - eventNs) / 1'000'000.0;
    std::snprintf(detail, sizeof(detail), "%s event_age_ms=%.3f", kind, eventAgeMs);
    write_locked("sdl_event", detail, 0, 0, 0.0f, 0.0f);
}

void mark(const char* stage) {
    std::lock_guard lock(g_state.mutex);
    write_locked(stage, "", 0, 0, 0.0f, 0.0f);
}

void mark_detail(const char* stage, const char* detail) {
    std::lock_guard lock(g_state.mutex);
    write_locked(stage, detail, 0, 0, 0.0f, 0.0f);
}

void pad_snapshot(const char* stage, std::uint32_t hold, std::uint32_t trig, float stickX, float stickY) {
    std::lock_guard lock(g_state.mutex);
    write_locked(stage, "", hold, trig, stickX, stickY);
}

void presented() {
    std::lock_guard lock(g_state.mutex);
    write_locked("aurora_end_frame_after", "", 0, 0, 0.0f, 0.0f);
    g_state.frame++;
    if (g_state.activeId != 0 && ++g_state.framesSinceInput > 8) {
        g_state.activeId = 0;
        g_state.inputNs = 0;
    }
}

const char* path() {
    std::lock_guard lock(g_state.mutex);
    return reinterpret_cast<const char*>(g_state.path.empty() ? nullptr : g_state.path.c_str());
}

} // namespace dusk::latency_trace
