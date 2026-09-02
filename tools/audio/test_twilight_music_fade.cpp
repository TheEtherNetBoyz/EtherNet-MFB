#include "../../src/dusk/audio/TwilightMusicFade.h"
#include <cassert>
#include <cstdio>

int main() {
    using dusk::audio::TwilightMusicFade;
    TwilightMusicFade timing;
    for (int i = 0; i < 30; ++i) timing.update(true, 1.0f / 60);
    assert(timing.palace() > 0.4f && timing.palace() < 0.6f);
    assert(timing.astral() == 0);
    for (int i = 0; i < 60; ++i) timing.update(true, 1.0f / 60);
    assert(timing.palace() == 0);
    assert(timing.astral() > 0.4f && timing.astral() < 0.6f);
    for (float dt : {1.0f / 30, 1.0f / 60, 1.0f / 240, 1.0f}) {
        TwilightMusicFade fade;
        const auto check = [&] {
            assert(fade.palace() >= 0 && fade.palace() <= 1);
            assert(fade.astral() >= 0 && fade.astral() <= 1);
            assert(fade.palace() == 0 || fade.astral() == 0);
        };
        assert(fade.palace() == 1 && fade.astral() == 0);
        for (int i = 0; i < 600; ++i) { fade.update(true, dt); check(); }
        assert(fade.palace() == 0 && fade.astral() == 1);
        for (int i = 0; i < 600; ++i) { fade.update(false, dt); check(); }
        assert(fade.palace() == 1 && fade.astral() == 0);
        // Rapid selection reversal must not allow overlapping gains either.
        for (int i = 0; i < 1000; ++i) { fade.update(i % 23 < 12, dt); check(); }
        // Once Astral is selected, interruptions must not reset the selector
        // and briefly expose Palace. Readiness/audibility is gated separately.
        for (int i = 0; i < 600; ++i) fade.select(true, true, dt);
        for (int i = 0; i < 300; ++i) {
            fade.select(true, true, dt);
            assert(fade.palace() == 0 && fade.astral() == 1);
        }
        // Leaving the replacement scene and returning also preserves choice.
        fade.select(true, false, dt);
        fade.select(true, true, dt);
        assert(fade.palace() == 0 && fade.astral() == 1);
        // Missing asset or explicit Palace selection still restores Palace.
        for (int i = 0; i < 600; ++i) { fade.select(false, true, dt); check(); }
        fade.select(false, false, dt);
        check();
        assert(fade.palace() == 1 && fade.astral() == 0);
    }
    // Nested selector + encounter fades must never overlap custom tracks or
    // let Palace through while either custom track is audible.
    TwilightMusicFade selector, encounter;
    // Combat exit: smooth, monotonic, exclusive, and three seconds total.
    for (float dt : {1.0f / 30, 1.0f / 60, 1.0f / 240}) {
        TwilightMusicFade outro;
        outro.position = 1.0f;
        float previousCombat = 1.0f, previousAmbient = 0.0f;
        const int frames = static_cast<int>(3.0f / dt + 0.5f);
        for (int i = 0; i <= frames; ++i) {
            outro.update(false, dt, 3.0f);
            assert(outro.astral() <= previousCombat);
            assert(outro.palace() >= previousAmbient);
            assert(previousCombat - outro.astral() < 0.04f);
            assert(outro.palace() == 0 || outro.astral() == 0);
            previousCombat = outro.astral();
            previousAmbient = outro.palace();
            if (i == frames / 4 - 1) {
                assert(outro.astral() > 0.4f && outro.astral() < 0.6f);
                assert(outro.palace() == 0);
            }
        }
        assert(outro.palace() == 1 && outro.astral() == 0);
    }
    for (int i = 0; i < 6000; ++i) {
        selector.select(i % 1700 < 1300, true, 1.0f / 240);
        const bool combatActive = i % 1100 < 650;
        encounter.update(combatActive, 1.0f / 240, combatActive ? 2.0f : 3.0f);
        const float ambient = selector.astral() * encounter.palace();
        const float combat = selector.astral() * encounter.astral();
        assert(ambient == 0 || combat == 0);
        assert(selector.palace() == 0 || (ambient == 0 && combat == 0));
    }
    TwilightMusicFade gap;
    gap.position = 0.499f;
    assert(gap.palace() > 0 && gap.astral() == 0);
    gap.position = 0.501f;
    assert(gap.astral() > 0 && gap.palace() == 0);
    std::puts("PASS: gap-free exclusive gains, combat/exploration handoffs, interruption/re-entry, fallback, rapid reversals and multiple frame rates.");
}
