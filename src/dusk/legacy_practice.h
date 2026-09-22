#pragma once

// The built-in GZ prototype is superseded by the standalone mod. Keep this a
// compile-time switch: saved CVars, console commands and mod calls must not
// reactivate its loaders, input capture or gameplay callbacks.
#define DUSK_LEGACY_PRACTICE_TOOLS 0
