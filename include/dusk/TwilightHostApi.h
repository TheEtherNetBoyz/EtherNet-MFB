#pragma once

#include "dolphin/types.h"
#include <mtx.h>

struct cXyz;

// Stable, optional host services used by the standalone Twilight Visuals mod.
// ABI revision 4 removes unused services. Check the revision before reading fields.
enum DuskSequenceEvent : u32 {
    DuskSequence_IsReplacementScene,
    DuskSequence_IsRefreshPending,
    DuskSequence_MusicStatusOverride,
    DuskSequence_UseTwilightBattleMusic,
    DuskSequence_IsBgmOnlyRefresh,
    DuskSequence_OnWaveRefreshFinished,
    DuskSequence_OnSceneBgmStarted,
};
struct DuskSequenceHooksV1 {
    void (*update)(void* sequenceManager, f32 baseVolume);
    void (*tick)();
    // The last two events are notifications; their return values are ignored.
    s32 (*query)(DuskSequenceEvent event);
};
// Explicit values preserve the revision-4 wire contract. Query events return
// booleans except JumpMode (native jump mode); action events return handled/result.
enum DuskPlayerEvent : u32 {
    DuskPlayer_RunEnabled = 0,
    DuskPlayer_IsRunning = 1,
    DuskPlayer_HeavyBootsRunning = 2,
    DuskPlayer_SnowRunning = 3,
    DuskPlayer_WaterRunning = 4,
    DuskPlayer_CanStepUp = 5,
    DuskPlayer_JumpMode = 6,
    DuskPlayer_UpdateRunSpeed = 10,
    DuskPlayer_UpdateSnowSpeed = 11, // value: unscaled stick input
    DuskPlayer_HandleRunAction = 12,
    DuskPlayer_UpdateWaterHeight = 13,
    DuskPlayer_ApplyLedgeBoost = 14, // value: jump mode
    DuskPlayer_TryStepUp = 15,
    DuskPlayer_SuppressWaterFall = 16,
};
enum DuskPlayerAnimationEvent : u32 {
    DuskPlayerAnimation_HeavyMovement = 0, // value: heavy movement flag
    DuskPlayerAnimation_Run = 1, // value: animation ID
    DuskPlayerAnimation_HeavyRun = 2, // value: animation ID, rate: playback rate
};
enum DuskEnvironmentQuery : u32 {
    DuskEnvironment_VisualTwilight = 0,
    DuskEnvironment_TwilightEffects = 1,
    DuskEnvironment_SnowStorm = 2,
    DuskEnvironment_ActorTwilight = 3,
};
enum DuskCelestialVisibility : u32 {
    DuskCelestial_ForceMoon = 0,
    DuskCelestial_HideSky = 1,
    DuskCelestial_DrawSun = 2,
    DuskCelestial_DrawMoon = 3,
    DuskCelestial_HideMoonForTime = 4,
    DuskCelestial_DrawSunLens = 5,
};
enum DuskCelestialParameter : u32 {
    DuskCelestial_MoonAlpha = 0, // f32*
    DuskCelestial_MoonPosition = 1, // DuskCelestialPositionV1*
    DuskCelestial_MoonPhase = 2, // int*
    DuskCelestial_MoonSize = 3, // f32*
};
enum DuskAudioManagerKind : u32 {
    DuskAudioManager_Scene = 0,
    DuskAudioManager_Sequence = 1,
    DuskAudioManager_Status = 2,
};
struct DuskPlayerHooksV1 {
    s32 (*invoke)(void* player, DuskPlayerEvent point, f32 value);
    void (*animation)(void* player, DuskPlayerAnimationEvent point, s32* value, f32* rate);
};
struct DuskEnvironmentHooksV1 {
    void (*commitArea)();
    void (*update)();
    u8 (*query)(DuskEnvironmentQuery kind, u8 nativeValue);
    void (*actorContext)(u8 enabled);
};
// Audio callbacks execute under the host audio lock. Clearing them waits for
// any in-flight callback before the module may release its decoder state.
struct DuskAudioHooksV1 {
    void (*mix)(f32* interleavedStereo, u32 frames, u32 sampleRate);
    f32 (*channelGain)(u32 channel);
};
// Synchronous render callbacks. Pointers are borrowed for this call only.
struct DuskCelestialPositionV1 {
    cXyz* relative;
    cXyz* world;
    const cXyz* eye;
};
struct DuskGeometryHooksV1 {
    void (*grassColor)(void* color, bool signedChannels);
    void* (*grassTexture)(void* original, const u8* pixels, bool firstTexture);
    bool (*grassLighting)();
    void* (*beforeModel)(void* modelData, void* lighting);
    void (*afterModel)(void* token);
    f32 (*bloomGain)();
    void (*afterBackground)(void* view, void* viewport);
    bool (*celestialVisibility)(DuskCelestialVisibility point, bool nativeValue);
    void (*celestialParameter)(DuskCelestialParameter point, void* value);
};
using DuskTwilightEnemyProcProviderV1 = s16 (*)(s16 procName);
using DuskTwilightBloomProviderV1 = u8 (*)(u8 defaultProfile);
using DuskTwilightSceneMusicProviderV1 = bool (*)(const char* spot, s32 room, s32 layer,
                                                   s32 sceneNo, bool inDarkness, u8 demoWave,
                                                   u32* bgmId, u8* bgmWave1, u8* bgmWave2,
                                                   bool* preserveStreams, bool* fieldBgmPlay,
                                                   s32* musicStatus);
struct DuskTwilightHostApiV1 {
    u32 abiVersion;
    u32 structSize;
    u64 capabilities;

    float (*getMasterVolume)();
    void (*setEnemyProcProvider)(DuskTwilightEnemyProcProviderV1 provider);
    void (*setBloomProvider)(DuskTwilightBloomProviderV1 provider);
    void (*setSceneMusicProvider)(DuskTwilightSceneMusicProviderV1 provider);
    void (*setGeometryHooks)(const DuskGeometryHooksV1* hooks);
    void (*setAudioHooks)(const DuskAudioHooksV1* hooks);
    void (*setEnvironmentHooks)(const DuskEnvironmentHooksV1* hooks);
    void (*setPlayerHooks)(const DuskPlayerHooksV1* hooks);
    void (*setSequenceHooks)(const DuskSequenceHooksV1* hooks);
    f32 (*interpolationStep)();
    bool (*interpolationEnabled)();
    bool (*simulationFrame)();
    void (*recordMatrix)(Mtx, const void*);
    bool (*lookupMatrix)(const void*, Mtx);
    void* (*audioManager)(DuskAudioManagerKind kind);
};


inline constexpr u32 DUSK_TWILIGHT_HOST_ABI_V1 = 4;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_MASTER_VOLUME = 1ull << 0;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_ENEMY_PROC_PROVIDER = 1ull << 1;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_BLOOM_PROVIDER = 1ull << 2;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_SCENE_MUSIC_PROVIDER = 1ull << 3;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_GEOMETRY_HOOKS = 1ull << 4;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_AUDIO_HOOKS = 1ull << 5;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_ENVIRONMENT_HOOKS = 1ull << 6;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_PLAYER_HOOKS = 1ull << 7;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_SEQUENCE_HOOKS = 1ull << 8;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_INTERPOLATION = 1ull << 9;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_AUDIO_MANAGER = 1ull << 10;

#if defined(_WIN32)
extern "C" __declspec(dllexport) const DuskTwilightHostApiV1*
DuskGetTwilightHostApiV1();
#else
extern "C" const DuskTwilightHostApiV1* DuskGetTwilightHostApiV1();
#endif
