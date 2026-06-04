#ifndef DUSK_MUSIC_RESOLVER_H
#define DUSK_MUSIC_RESOLVER_H

#include "JSystem/JAudio2/JAISound.h"
#include "global.h"

#include <array>
#include <cstddef>

namespace dusk::music {

enum class Route {
    Scene,
    Main,
    Sub,
    Fanfare,
};

constexpr size_t MaxBgmWaves = 8;

struct ResolvedMusic {
    bool matched = false;
    JAISoundID originalId;
    JAISoundID replacementId;
    std::array<u32, MaxBgmWaves> bgmWaves {};
    size_t bgmWaveCount = 0;
};

ResolvedMusic Resolve(Route route, JAISoundID originalId);
void ApplySceneResolution(JAISoundID& bgm, u8& bgmWave1, u8& bgmWave2);
JAISoundID ResolvePlaybackAndLoad(Route route, JAISoundID originalId);
void LoadResolvedBgmWaves(const ResolvedMusic& resolved, size_t startIndex = 0);

} // namespace dusk::music

#endif /* DUSK_MUSIC_RESOLVER_H */
