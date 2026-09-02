#pragma once
#include <algorithm>

namespace dusk::audio {
// Fade out and immediately fade in: never sum two audible soundtracks.
struct TwilightMusicFade {
    float position = 0.0f;
    void select(bool astral, bool replacementScene, float seconds) {
        // Audibility (battle/cutscene/loading) is deliberately not an input.
        // Never fade back through Palace just because Astral was interrupted.
        if (replacementScene) update(astral, seconds);
        else position = astral ? 1.0f : 0.0f;
    }
    void update(bool astral, float seconds, float duration = 2.0f) {
        const float target = astral ? 1.0f : 0.0f;
        const float step = std::clamp(seconds, 0.0f, 0.05f) / std::max(duration, 0.05f);
        position += std::clamp(target - position, -step, step);
    }
    static float smooth(float value) {
        const float t = std::clamp(value, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    // The curves meet at the midpoint without a silent interval.
    float palace() const { return smooth(1.0f - position / 0.5f); }
    float astral() const { return position >= 1.0f ? 1.0f : smooth((position - 0.5f) / 0.5f); }
};
}
