#include "logging.hpp"

#include "mods/svc/log.hpp"
#include "mods/svc/window.h"

namespace {

const char* window_event_name(WindowEventType type) {
    switch (type) {
    case WINDOW_EVENT_CLOSE_REQUESTED:
        return "close requested";
    case WINDOW_EVENT_RESIZED:
        return "resized";
    case WINDOW_EVENT_MOVED:
        return "moved";
    case WINDOW_EVENT_FOCUS_GAINED:
        return "focus gained";
    case WINDOW_EVENT_FOCUS_LOST:
        return "focus lost";
    case WINDOW_EVENT_SHOWN:
        return "shown";
    case WINDOW_EVENT_HIDDEN:
        return "hidden";
    case WINDOW_EVENT_KEY_DOWN:
        return "key down";
    case WINDOW_EVENT_KEY_UP:
        return "key up";
    case WINDOW_EVENT_MOUSE_MOTION:
        return "mouse motion";
    case WINDOW_EVENT_MOUSE_BUTTON_DOWN:
        return "mouse button down";
    case WINDOW_EVENT_MOUSE_BUTTON_UP:
        return "mouse button up";
    }
    return "unknown";
}

}  // namespace

void window_demo::log_window_event(const WindowEvent* event) {
    mods::log::info(
        "window event: {}; position=({}, {}), size={}x{}, pixels={}x{}, scale={:.2f}",
        window_event_name(event->type), event->x, event->y, event->width, event->height,
        event->pixel_width, event->pixel_height, event->display_scale);
}
