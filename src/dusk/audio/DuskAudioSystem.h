#pragma once

#include <cmath>
#include <dolphin/types.h>

namespace dusk::audio {

    // Dedicated native stream rings, in addition to vanilla's audio budget.
    inline constexpr u32 AstralStreamBlockSize = 0x2760;
    inline constexpr u32 AstralStreamRingSize = AstralStreamBlockSize * 20;
    inline constexpr u32 AstralStreamAramReserve = AstralStreamRingSize * 2;

    // Converts a 0-1 volume to a linear amplitude multiplier.
    // The curve is -4 dB per 10% step: 100% = 0 dB, 90% = -4 dB, ..., 0% = -inf dB
    inline f32 MasterVolumeToLinear(f32 v) {
        if (v <= 0.0f) {
            return 0.0f;
        }
        return std::pow(10.0f, (v - 1.0f) * 2.0f);
    }

    /**
     * Initialize the audio system and start playing audio.
     */
    void Initialize();

    void ApplySettings();

    // Optional native AST music asset. Builds without the asset
    // never silence the game's soundtrack.
    bool AstralPlaneAvailable();
    void UpdateTwilightMusic(bool replacementScene, bool eligible, int musicMode, float gain,
                            bool battleScope, bool battleActive, float battleVolume);
    float TwilightPalaceGain();
    float TwilightBattleGain();

    void SetEnableReverb(bool value);

    void SetMasterVolume(f32 value);

    void SetPaused(bool paused);

    u32 GetResetCount(int channelIdx);

    f32 VolumeFromU16(u16 value);
}

// Registers optional native replacement tracks owned by an external mod.
// The caller must keep both buffers alive for the lifetime of audio playback.
#if defined(_WIN32)
extern "C" __declspec(dllexport) void DuskRegisterTwilightAstTracks(
    void* ambientData, size_t ambientSize, void* combatData, size_t combatSize);
extern "C" __declspec(dllexport) void DuskRegisterTwilightMp3Track(
    void* data, size_t size);
extern "C" __declspec(dllexport) void DuskLoadTwilightExternalMusic(
    const char* ambientFile, const char* combatFile, const char* darkHourFile);
extern "C" __declspec(dllexport) void DuskLoadTwilightExternalMusicV2(
    const char* ambientFile, const char* combatFile, const char* darkHourFile,
    const char* darkHourCombatFile);
extern "C" __declspec(dllexport) void DuskSetTwilightMusicVolume(float volume);
extern "C" __declspec(dllexport) float DuskGetMasterVolume();
#else
extern "C" void DuskRegisterTwilightAstTracks(
    void* ambientData, size_t ambientSize, void* combatData, size_t combatSize);
extern "C" void DuskRegisterTwilightMp3Track(void* data, size_t size);
extern "C" void DuskLoadTwilightExternalMusic(
    const char* ambientFile, const char* combatFile, const char* darkHourFile);
extern "C" void DuskLoadTwilightExternalMusicV2(
    const char* ambientFile, const char* combatFile, const char* darkHourFile,
    const char* darkHourCombatFile);
extern "C" void DuskSetTwilightMusicVolume(float volume);
extern "C" float DuskGetMasterVolume();
#endif
