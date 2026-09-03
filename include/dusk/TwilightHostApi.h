#pragma once

#include "dolphin/types.h"
#include <mtx.h>

struct cXyz;
class dKankyo_housi_Packet;

// Stable, optional host services used by the standalone Twilight Visuals mod.
// Newer hosts may append fields, but must preserve the version-1 prefix.
struct DuskTwilightSkyboxV1;
struct DuskSequenceHooksV1 {
    void (*update)(void* sequenceManager, f32 baseVolume);
    void (*tick)();
    s32 (*query)(u32 point);
};
struct DuskPlayerHooksV1 {
    s32 (*invoke)(void* player, u32 point, f32 value);
    void (*animation)(void* player, u32 point, s32* value, f32* rate);
};
struct DuskEnvironmentHooksV1 {
    void (*commitArea)();
    void (*update)();
    u8 (*query)(u32 kind, u8 nativeValue);
    void (*actorContext)(u8 enabled);
};
// Audio callbacks execute under the host audio lock. Clearing them waits for
// any in-flight callback before the module may release its decoder state.
struct DuskAudioHooksV1 {
    void (*mix)(f32* interleavedStereo, u32 frames, u32 sampleRate);
    void (*sequence)(bool replacementScene, bool eligible, s32 mode, f32 gain,
                     bool battleScope, bool battleActive, f32 battleVolume);
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
    void (*particle)(cXyz* corners, cXyz* position, void* color, u32 index, f32 time);
    f32 (*bloomGain)();
    void (*afterBackground)(void* view, void* viewport);
    bool (*celestialVisibility)(u32 point, bool nativeValue);
    void (*celestialParameter)(u32 point, void* value);
};
using DuskTwilightVisualStateProviderV1 = void (*)(u8* enabled, u8* style,
                                                    f32* brightness,
                                                    s32* chromaticAberration,
                                                    u8* skyVariant, u8* weather,
                                                    u8* alternateRun);
using DuskTwilightEnemyProcProviderV1 = s16 (*)(s16 procName);
using DuskTwilightEnvironmentLayerProviderV1 = s32 (*)(s32 currentLayer);
using DuskTwilightBloomProviderV1 = u8 (*)(u8 defaultProfile);
using DuskTwilightSceneMusicProviderV1 = bool (*)(const char* spot, s32 room, s32 layer,
                                                   s32 sceneNo, bool inDarkness, u8 demoWave,
                                                   u32* bgmId, u8* bgmWave1, u8* bgmWave2,
                                                   bool* preserveStreams, bool* fieldBgmPlay,
                                                   s32* musicStatus);
struct DuskTwilightAudioSequenceV1 {
    bool enabled;
    u8 style;
    bool sceneMusicForced;
    bool safeMusicEvent;
    bool mainReplacementReady;
    bool ordinaryBattle;
    bool battleFlagActive;
    bool subMusicEligible;
    bool replacementScene;
    bool customMusicEligible;
    bool battleScope;
    u8 musicMode;
    f32 gain;
    f32 battleVolume;
};
using DuskTwilightAudioSequenceProviderV1 = void (*)(DuskTwilightAudioSequenceV1* state);
struct DuskTwilightRunningV1 {
    bool enabled;
    f32 speed;
    f32 heavyBootsRate;
};
using DuskTwilightRunningProviderV1 = void (*)(DuskTwilightRunningV1* state);
using DuskTwilightGrassProviderV1 = bool (*)(bool* monochrome);
struct DuskTwilightRenderPolicyV1 {
    bool astralFog;
    bool astralFragments;
};
using DuskTwilightRenderPolicyProviderV1 = void (*)(DuskTwilightRenderPolicyV1* state);
struct DuskTwilightHostApiV1 {
    u32 abiVersion;
    u32 structSize;
    u64 capabilities;

    void (*setVisualConfig)(u8 enabled, u8 style, f32 brightness,
                            s32 chromaticAberration, u8 skyVariant, u8 weather,
                            u8 alternateRun);
    void (*setMoonOverride)(bool enabled, bool forceVisible, bool hideSun,
                            bool forceFull, f32 scale, cXyz* position);
    void (*moveHousi)(f32 timeScale);
    bool (*drawHousi)(Mtx drawMtx, u8** texture, dKankyo_housi_Packet* packet,
                      f32 timeScale);
    void (*loadExternalMusic)(const char* ambientFile, const char* combatFile,
                              const char* darkHourFile, const char* darkHourCombatFile);
    void (*setMusicVolume)(float volume);
    float (*getMasterVolume)();
    bool (*readEnvironmentSky)(DuskTwilightSkyboxV1* outSky, const char* stage, u8 layer, u8 palette, u8 minimumLayer);
    void (*setVisualStateProvider)(DuskTwilightVisualStateProviderV1 provider);
    void (*setEnemyProcProvider)(DuskTwilightEnemyProcProviderV1 provider);
    void (*setEnvironmentLayerProvider)(DuskTwilightEnvironmentLayerProviderV1 provider);
    void (*setBloomProvider)(DuskTwilightBloomProviderV1 provider);
    void (*setSceneMusicProvider)(DuskTwilightSceneMusicProviderV1 provider);
    void (*setAudioSequenceProvider)(DuskTwilightAudioSequenceProviderV1 provider);
    void (*setRunningProvider)(DuskTwilightRunningProviderV1 provider);
    void (*setGrassProvider)(DuskTwilightGrassProviderV1 provider);
    void (*setRenderPolicyProvider)(DuskTwilightRenderPolicyProviderV1 provider);
    void (*setGeometryHooks)(const DuskGeometryHooksV1* hooks);
    void (*setAudioHooks)(const DuskAudioHooksV1* hooks);
    void (*setEnvironmentHooks)(const DuskEnvironmentHooksV1* hooks);
    bool (*selectEnvironmentLayer)(s32 layer, s32 minimumLayer);
    void (*setPlayerHooks)(const DuskPlayerHooksV1* hooks);
    void (*setSequenceHooks)(const DuskSequenceHooksV1* hooks);
    void (*refreshSceneMusic)(const char* stage, s8 room, s8 layer, s32 restoredStatus);
    f32 (*interpolationStep)();
    bool (*interpolationEnabled)();
    bool (*simulationFrame)();
    void (*recordMatrix)(Mtx, const void*);
    bool (*lookupMatrix)(const void*, Mtx);
    void* (*audioManager)(u32 kind);
};

struct DuskTwilightRgbV1 {
    u8 r;
    u8 g;
    u8 b;
};

struct DuskTwilightRgbaV1 {
    u8 r;
    u8 g;
    u8 b;
    u8 a;
};

struct DuskTwilightSkyboxV1 {
    DuskTwilightRgbV1 sky;
    DuskTwilightRgbV1 cloudTop;
    DuskTwilightRgbV1 cloudBottom;
    DuskTwilightRgbaV1 cloudShadow;
    DuskTwilightRgbaV1 hazeOuter;
    DuskTwilightRgbaV1 hazeInner;
};

// Revision 2 also requires the generic sequence-id channel callback contract.
inline constexpr u32 DUSK_TWILIGHT_HOST_ABI_V1 = 3;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_VISUAL_CONFIG = 1ull << 0;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_MOON_OVERRIDE = 1ull << 1;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_HOUSI = 1ull << 2;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_EXTERNAL_MUSIC = 1ull << 3;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_MASTER_VOLUME = 1ull << 4;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_AUTHORED_SKY = 1ull << 5;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_STATE_PROVIDER = 1ull << 6;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_ENEMY_PROC_PROVIDER = 1ull << 7;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_ENVIRONMENT_LAYER_PROVIDER = 1ull << 8;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_BLOOM_PROVIDER = 1ull << 9;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_SCENE_MUSIC_PROVIDER = 1ull << 10;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_AUDIO_SEQUENCE_PROVIDER = 1ull << 11;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_RUNNING_PROVIDER = 1ull << 12;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_GRASS_PROVIDER = 1ull << 13;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_RENDER_POLICY_PROVIDER = 1ull << 14;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_GEOMETRY_HOOKS = 1ull << 15;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_AUDIO_HOOKS = 1ull << 16;

#if defined(_WIN32)
extern "C" __declspec(dllexport) const DuskTwilightHostApiV1*
DuskGetTwilightHostApiV1();
#else
extern "C" const DuskTwilightHostApiV1* DuskGetTwilightHostApiV1();
#endif
