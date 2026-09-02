#pragma once

#include "dolphin/types.h"
#include "d/d_kankyo_rain.h"

// Stable, optional host services used by the standalone Twilight Visuals mod.
// Newer hosts may append fields, but must preserve the version-1 prefix.
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
};

inline constexpr u32 DUSK_TWILIGHT_HOST_ABI_V1 = 1;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_VISUAL_CONFIG = 1ull << 0;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_MOON_OVERRIDE = 1ull << 1;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_HOUSI = 1ull << 2;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_EXTERNAL_MUSIC = 1ull << 3;
inline constexpr u64 DUSK_TWILIGHT_HOST_CAP_MASTER_VOLUME = 1ull << 4;

#if defined(_WIN32)
extern "C" __declspec(dllexport) const DuskTwilightHostApiV1*
DuskGetTwilightHostApiV1();
#else
extern "C" const DuskTwilightHostApiV1* DuskGetTwilightHostApiV1();
#endif
