#ifndef DUSK_MUSIC_RESOLVER_H
#define DUSK_MUSIC_RESOLVER_H

#include "JSystem/JAudio2/JAISound.h"
#include "global.h"

namespace dusk::music {

enum class Route {
    Scene,
    Main,
    Sub,
    Fanfare,
};

enum class LockSlot {
    Main,
    Sub,
    Fanfare,
};

constexpr size_t MaxBgmWaves = 8;

struct ResolvedMusic {
    bool matched = false;
    JAISoundID originalId;
    JAISoundID replacementId;
    u32 bgmWaves[MaxBgmWaves] {};
    size_t bgmWaveCount = 0;
    u32 targetBgmWaves[MaxBgmWaves] {};
    size_t targetBgmWaveCount = 0;
    u32 targetSeWaves[MaxBgmWaves] {};
    size_t targetSeWaveCount = 0;
};

ResolvedMusic Resolve(Route route, JAISoundID originalId);
JAISoundID LogicalOriginalForPlayback(Route route, JAISoundID playbackId);
bool IsResolvedPlayback(Route route, JAISoundID playbackId);
bool IsCurrentSceneReplacementPlayback(JAISoundID playbackId);
bool HasPlaybackMatch(Route route, JAISoundID requestedId);
void ApplySceneResolution(JAISoundID& bgm, u8& bgmWave1, u8& bgmWave2);
JAISoundID ResolvePlaybackAndLoad(Route route, JAISoundID originalId);
JAISoundID ResolvePlaybackAndLock(Route route, JAISoundID originalId, LockSlot slot);
void ClearBgmWaveLock(LockSlot slot);
bool IsBgmWaveLocked(u32 wave);
void ClearRuntimeSupportWavesForScene(u8 bgmWave1, u8 bgmWave2);
void RestoreEvictedMainBgmWaves();
bool ReleaseBgmWavesForSeRetry(u32 seWave);
void LoadResolvedBgmWaves(const ResolvedMusic& resolved, size_t startIndex = 0);
void ReleaseUnneededSceneBgmWaves();
void LoadSceneRequiredBgmWaves(size_t startIndex = 0);
bool IsEnemyMusicDisabledFor(JAISoundID bgmId);
bool IsAllEnemyMusicDisabled();

// Master gate for the music randomizer audio system. False on a non-music-rando
// ROM (no matching manifest) -> TP audio hooks must run the original path.
bool CustomAudioActive();

// TP-rando-style music swaps do not gate sceneBgmStart. This remains as a
// compatibility hook for callers added during earlier experiments.
bool SceneResolvedWavesStillLoading();

} // namespace dusk::music

#endif /* DUSK_MUSIC_RESOLVER_H */
