#include "dusk/audio/DuskAudioSystem.h"

#include <SDL3/SDL_init.h>
#include <array>
#include <cassert>
#include <span>
#include <atomic>
#include <algorithm>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_timer.h>
#include "TwilightMusicFade.h"
#include <string>
#include <new>
#include <borealis/log.hpp>
#define DR_MP3_IMPLEMENTATION
#include "../../../tools/audio/dr_mp3.h"
#include "JSystem/JAudio2/JASAramStream.h"
#include "Z2AudioLib/Z2Param.h"

#include "JSystem/JAudio2/JASAiCtrl.h"
#include "JSystem/JAudio2/JASChannel.h"
#include "JSystem/JAudio2/JASCriticalSection.h"
#include "JSystem/JAudio2/JASDSPChannel.h"
#include "JSystem/JAudio2/JASHeapCtrl.h"

#include "DuskDsp.hpp"
#include "JSystem/JAudio2/JASAudioThread.h"
#include "JSystem/JAudio2/JASDriverIF.h"
#include "dusk/settings.h"
#include "tracy/Tracy.hpp"

using namespace dusk::audio;

static OutputSubframe OutBuffer;
static std::array<f32, DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS> OutInterleaveBuffer;

static SDL_AudioStream* PlaybackStream;

// An ordinary native AST asset, retained while the native block loader uses it.
// Playback goes through JASAramStream and the game's DSP, not an SDL music mixer.
enum class AstralState { Idle, Preparing, Playing };
static constexpr borealis::Log AstralLog{"dusk::astral_music"};
struct NativeAstralTrack {
    const char* name = "unloaded";
    void* data = nullptr;
    size_t size = 0;
    bool available = false;
    JASAramStream stream;
    std::atomic<bool> prepared{false};
    std::atomic<bool> finished{false};
    AstralState playback = AstralState::Idle;
    JASHeap* ring = nullptr;
    float audibleGain = 0.0f;
    Uint64 preparationStart = 0;
    bool warnedWaiting = false;
    bool reportedPlaying = false;

    void attach(const char* trackName, void* trackData, size_t trackSize) {
        name = trackName;
        data = trackData;
        size = trackSize;
        available = data != nullptr && size >= 64 && size <= UINT32_MAX;
        if (available) AstralLog.info("Registered mod-owned {} ({} bytes)", name, size);
    }
    void setGain(float target, float elapsed) {
        audibleGain = std::min(target, audibleGain + std::clamp(elapsed, 0.0f, 0.05f));
        stream.setVolume(audibleGain * 0.85f * Z2Param::VOL_BGM_DEFAULT);
    }
};
static NativeAstralTrack AstralAmbient;
static NativeAstralTrack AstralCombat;

struct ExternalMp3Track {
    const char* name = "unloaded";
    drmp3 decoder{};
    SDL_AudioStream* converter = nullptr;
    std::string sourcePath;
    void* sourceData = nullptr;
    size_t sourceSize = 0;
    bool available = false;
    bool playbackStarted = false;
    float audibleGain = 0.0f;
    std::array<float, 4096> decoded{};

    void reset() {
        if (converter != nullptr) SDL_DestroyAudioStream(converter);
        if (available) drmp3_uninit(&decoder);
        decoder = {};
        converter = nullptr;
        sourcePath.clear();
        sourceData = nullptr;
        sourceSize = 0;
        available = false;
        playbackStarted = false;
        audibleGain = 0.0f;
    }

    void attach(const char* trackName, void* data, size_t size) {
        reset();
        name = trackName;
        if (data == nullptr || size == 0) return;
        sourceData = data;
        sourceSize = size;
        if (!drmp3_init_memory(&decoder, data, size, nullptr)) {
            AstralLog.warn("Cannot initialize mod-owned {}", name);
            return;
        }
        if (decoder.channels == 0 || decoder.channels > 2 || decoder.sampleRate == 0) {
            AstralLog.warn("{} has an unsupported MP3 format ({} channels, {} Hz)",
                name, decoder.channels, decoder.sampleRate);
            drmp3_uninit(&decoder);
            return;
        }
        const SDL_AudioSpec source{SDL_AUDIO_F32, static_cast<int>(decoder.channels),
                                   static_cast<int>(decoder.sampleRate)};
        const SDL_AudioSpec destination{SDL_AUDIO_F32, 2, static_cast<int>(SampleRate)};
        converter = SDL_CreateAudioStream(&source, &destination);
        available = converter != nullptr;
        if (available) AstralLog.info("Registered mod-owned {}", name);
        else {
            AstralLog.warn("Cannot create the MP3 converter for {}: {}", name, SDL_GetError());
            drmp3_uninit(&decoder);
        }
    }

    void loadFile(const char* trackName, const char* filename) {
        reset();
        name = trackName;
        if (filename == nullptr || filename[0] == '\0') return;
        const char* base = SDL_GetBasePath();
        if (base == nullptr) return;
        const std::string path = std::string(base) + filename;
        sourcePath = path;
        if (!drmp3_init_file(&decoder, path.c_str(), nullptr)) {
            AstralLog.warn("Optional {} not found at {}", name, path);
            return;
        }
        if (decoder.channels == 0 || decoder.channels > 2 || decoder.sampleRate == 0) {
            AstralLog.warn("{} has an unsupported MP3 format ({} channels, {} Hz)",
                name, decoder.channels, decoder.sampleRate);
            drmp3_uninit(&decoder);
            return;
        }
        const SDL_AudioSpec source{SDL_AUDIO_F32, static_cast<int>(decoder.channels),
                                   static_cast<int>(decoder.sampleRate)};
        const SDL_AudioSpec destination{SDL_AUDIO_F32, 2, static_cast<int>(SampleRate)};
        converter = SDL_CreateAudioStream(&source, &destination);
        available = converter != nullptr;
        if (available) AstralLog.info("Streaming optional {} from {}", name, path);
        else {
            AstralLog.warn("Cannot create the MP3 converter for {}: {}", name, SDL_GetError());
            drmp3_uninit(&decoder);
        }
    }

    bool restartDecoder() {
        if (drmp3_seek_to_pcm_frame(&decoder, 0)) return true;

        // Some MP3s reach EOF with a decoder seek callback that refuses the
        // rewind even though the file itself is valid. Reinitialize from the
        // original source as a fallback so playback cannot stop permanently.
        drmp3_uninit(&decoder);
        decoder = {};
        const bool initialized = sourcePath.empty()
            ? drmp3_init_memory(&decoder, sourceData, sourceSize, nullptr)
            : drmp3_init_file(&decoder, sourcePath.c_str(), nullptr);
        if (!initialized) {
            AstralLog.warn("{} reached EOF and could not be restarted", name);
            return false;
        }
        return true;
    }

    void setGain(float target, float elapsed, bool smoothReduction = false,
                 bool rewindWhenSilent = false) {
        if (target > 0.0f) playbackStarted = true;
        const float step = std::clamp(elapsed, 0.0f, 0.05f);
        if (smoothReduction) {
            audibleGain += std::clamp(target - audibleGain, -step, step);
        } else {
            audibleGain = std::min(target, audibleGain + step);
        }

        // Combat tracks are encounter-scoped. Once their fade-out is fully
        // silent, discard converted tail samples and rewind so the next battle
        // begins at the first frame instead of resuming in the middle.
        if (rewindWhenSilent && target <= 0.0f && audibleGain <= 0.0001f && playbackStarted) {
            SDL_ClearAudioStream(converter);
            if (restartDecoder()) playbackStarted = false;
        }
    }

    void mix(std::span<float> output, float calibration, float userVolume) {
        // Do not consume the MP3 during boot/save loading. Its first decoded
        // frame must coincide with the point where map music becomes audible.
        // Once started, it continues advancing silently through later fades and
        // interruptions so returning to it remains seamless.
        if (!available || converter == nullptr || !playbackStarted) return;
        const int bytesNeeded = static_cast<int>(output.size_bytes());
        while (SDL_GetAudioStreamAvailable(converter) < bytesNeeded) {
            const drmp3_uint64 capacity = decoded.size() / decoder.channels;
            drmp3_uint64 frames = drmp3_read_pcm_frames_f32(&decoder, capacity, decoded.data());
            if (frames == 0) {
                if (!restartDecoder()) return;
                frames = drmp3_read_pcm_frames_f32(&decoder, capacity, decoded.data());
                if (frames == 0) return;
            }
            SDL_PutAudioStreamData(converter, decoded.data(),
                static_cast<int>(frames * decoder.channels * sizeof(float)));
        }
        std::array<float, DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS> converted{};
        const int bytesRead = SDL_GetAudioStreamData(converter, converted.data(), bytesNeeded);
        if (bytesRead <= 0) return;
        const size_t samples = static_cast<size_t>(bytesRead) / sizeof(float);
        const float volume = audibleGain * calibration * userVolume * Z2Param::VOL_BGM_DEFAULT;
        for (size_t i = 0; i < samples; ++i) output[i] += converted[i] * volume;
    }
};
static ExternalMp3Track AstralMp3Ambient;
static ExternalMp3Track AstralMp3Combat;
static ExternalMp3Track DarkHourAmbient;
static ExternalMp3Track DarkHourCombat;
static std::atomic<float> TwilightMusicVolume{1.0f};
static std::string AstralAmbientFilename;
static std::string AstralCombatFilename;
static std::string DarkHourFilename;
static std::string DarkHourCombatFilename;
static std::atomic<float> PalaceGain{1.0f};
static std::atomic<float> BattleGain{1.0f};

static void LoadExternalTwilightMusic() {
    AstralMp3Ambient.loadFile("Astral ambient music",
        AstralAmbientFilename.empty() ? nullptr : AstralAmbientFilename.c_str());
    AstralMp3Combat.loadFile("Astral combat music",
        AstralCombatFilename.empty() ? nullptr : AstralCombatFilename.c_str());
    DarkHourAmbient.loadFile("Dark Hour music",
        DarkHourFilename.empty() ? nullptr : DarkHourFilename.c_str());
    DarkHourCombat.loadFile("Dark Hour combat music",
        DarkHourCombatFilename.empty() ? nullptr : DarkHourCombatFilename.c_str());
}

static void AstralCallback(u32 type, JASAramStream*, void* userdata) {
    auto& track = *static_cast<NativeAstralTrack*>(userdata);
    // Native names are historical: CB_STOP signals prepared, CB_START finished.
    if (type == JASAramStream::CB_STOP) track.prepared.store(true);
    else track.finished.store(true);
}

static void InitAstralPlane() {
    LoadExternalTwilightMusic();
    // Reserve the Astral rings before scene wave banks can fragment/fill ARAM.
    // The PC audio budget includes this space separately from vanilla waves.
    static JASHeap rings[2];
    NativeAstralTrack* tracks[] = {&AstralAmbient, &AstralCombat};
    for (size_t i = 0; i < 2; ++i) {
        auto& track = *tracks[i];
        track.ring = &rings[i];
        if (!track.available) continue;
        if (!track.ring->isAllocated() &&
            !track.ring->alloc(JASKernel::getAramHeap(), AstralStreamRingSize)) {
            AstralLog.warn("{}: startup ring reservation failed; retaining vanilla music", track.name);
            track.available = false;
        } else {
            AstralLog.info("{}: reserved {} bytes for native playback", track.name, AstralStreamRingSize);
        }
    }
}

extern "C" void DuskRegisterTwilightAstTracks(void* ambientData, size_t ambientSize,
                                               void* combatData, size_t combatSize) {
    AstralAmbient.attach("ambient replacement track", ambientData, ambientSize);
    AstralCombat.attach("combat replacement track", combatData, combatSize);
}

extern "C" void DuskRegisterTwilightMp3Track(void* data, size_t size) {
    DarkHourAmbient.attach("MP3 replacement track", data, size);
}

extern "C" void DuskLoadTwilightExternalMusicV2(const char* ambientFile,
                                                  const char* combatFile,
                                                  const char* darkHourFile,
                                                  const char* darkHourCombatFile) {
    AstralAmbientFilename = ambientFile != nullptr ? ambientFile : "";
    AstralCombatFilename = combatFile != nullptr ? combatFile : "";
    DarkHourFilename = darkHourFile != nullptr ? darkHourFile : "";
    DarkHourCombatFilename = darkHourCombatFile != nullptr ? darkHourCombatFile :
        (darkHourFile != nullptr && darkHourFile[0] != '\0' ? "Mass Destruction.mp3" : "");
    if (PlaybackStream != nullptr) LoadExternalTwilightMusic();
}

// Keep the original ABI available for older Twilight Visuals DLLs. Older
// callers only provide the three original filenames, so the combat track is
// intentionally cleared in that compatibility path.
extern "C" void DuskLoadTwilightExternalMusic(const char* ambientFile,
                                                const char* combatFile,
                                                const char* darkHourFile) {
    DuskLoadTwilightExternalMusicV2(ambientFile, combatFile, darkHourFile, nullptr);
}

extern "C" void DuskSetTwilightMusicVolume(float volume) {
    TwilightMusicVolume.store(std::clamp(volume, 0.0f, 1.0f));
}

extern "C" float DuskGetMasterVolume() {
    JASCriticalSection section;
    return std::clamp(MasterVolume, 0.0f, 1.0f);
}

bool dusk::audio::AstralPlaneAvailable() { return AstralMp3Ambient.available; }

static bool PrepareAstralPlane(NativeAstralTrack& track, bool requested) {
    JASCriticalSection section;
    if (track.finished.exchange(false)) track.playback = AstralState::Idle;
    if (!track.available) return false;
    if (track.playback == AstralState::Idle) {
        if (!requested) return false;
        if (JASAramStream::getBlockSize() == 0) return false;
        // Already reserved at startup, independent of scene load/track order.
        assert(JASAramStream::getBlockSize() == AstralStreamBlockSize);
        assert(track.ring != nullptr && track.ring->isAllocated());
        auto& ring = *track.ring;
        track.prepared.store(false);
        track.preparationStart = SDL_GetTicksNS();
        track.warnedWaiting = false;
        track.reportedPlaying = false;
        // Vanilla creates a fresh stream each time; init alone does not reset
        // every end-of-loop/channel field. Reuse storage only after finish.
        track.stream.~JASAramStream();
        new (&track.stream) JASAramStream();
        track.stream.init(static_cast<u32>(reinterpret_cast<uintptr_t>(ring.getBase())), ring.getSize(), AstralCallback, &track);
        track.stream.setVolume(0.0f);
        track.stream.setChannelPan(0, 0.0f);
        track.stream.setChannelPan(1, 1.0f);
        if (!track.stream.prepareMemory(track.data, static_cast<u32>(track.size))) {
            AstralLog.warn("{}: AST preparation failed; retaining vanilla music", track.name);
            track.available = false;
            return false;
        }
        track.playback = AstralState::Preparing;
    }
    if (track.playback == AstralState::Preparing && track.prepared.load()) {
        if (track.stream.start()) track.playback = AstralState::Playing;
    }
    // Enqueuing start() isn't proof of playback: wait for actual native DSP
    // channels, and never silence the original soundtrack for a stalled player.
    const bool ready = track.playback == AstralState::Playing &&
        track.stream.mChannels[0] != nullptr && track.stream.mChannels[1] != nullptr &&
        track.stream.mPauseFlags == 0;
    if (ready && !track.reportedPlaying) {
        AstralLog.info("{}: native stereo playback ready", track.name);
        track.reportedPlaying = true;
    }
    if (!ready && track.preparationStart != 0 && !track.warnedWaiting &&
        SDL_GetTicksNS() - track.preparationStart > 5000000000ULL) {
        AstralLog.warn("{}: playback waiting (prepared={}, pauseFlags={}); retaining vanilla music",
            track.name, track.prepared.load(), track.stream.mPauseFlags);
        track.warnedWaiting = true;
    }
    return ready;
}

float dusk::audio::TwilightPalaceGain() { return PalaceGain.load(); }
float dusk::audio::TwilightBattleGain() { return BattleGain.load(); }

void dusk::audio::UpdateTwilightMusic(bool replacementScene, bool eligible, int musicMode,
                                    float gain, bool battleScope, bool battleActive, float battleVolume) {
    JASCriticalSection section;
    static TwilightMusicFade fade;
    static TwilightMusicFade encounterFade;
    static Uint64 lastTick = 0;
    const Uint64 tick = SDL_GetTicksNS();
    const float elapsed = lastTick == 0 ? 0.0f : float(tick - lastTick) / 1.0e9f;
    lastTick = tick;
    const bool astral = musicMode == 1;
    const bool darkHour = musicMode == 2;
    const bool ready = AstralMp3Ambient.available;
    const bool combatReady = AstralMp3Combat.available;
    const bool darkHourReady = DarkHourAmbient.available;
    const bool darkHourCombatReady = DarkHourCombat.available;
    const bool selectionScope = replacementScene || battleScope;
    const bool customSelected = astral || darkHour;
    const bool selectionReady = astral ? (ready || combatReady) : (darkHour && darkHourReady);
    fade.select(customSelected && selectionReady, selectionScope, elapsed);
    // A prepared stream remains alive and advances silently after its style is
    // deselected. Track identity must therefore gate every audible path; ready
    // alone does not mean that track is currently selected.
    const bool replaceAstralBattle = astral && battleScope && combatReady;
    const bool replaceDarkHourBattle = darkHour && battleScope && darkHourCombatReady;
    const bool replaceBattle = replaceAstralBattle || replaceDarkHourBattle;
    const bool enteringCombat = battleActive && replaceBattle;
    // Give the combat outro and exploration re-entry about 1.5 seconds each.
    // Keep combat entry/Palace selection at their existing speed, and preserve
    // the exclusive handoff so the two Astral tracks never play over each other.
    encounterFade.update(enteringCombat, elapsed, enteringCombat ? 2.0f : 3.0f);
    // Keep the replacement Palace sequence silent through interruptions and
    // AST preparation. Native Palace areas are outside replacementScene.
    PalaceGain.store(replacementScene && selectionReady ? fade.palace() : 1.0f);
    // Gate all ordinary battle sequences at their native channel output,
    // including detached fade-out tails. Never tag boss/miniboss themes.
    BattleGain.store(replaceBattle ? fade.palace() : 1.0f);
    // Zero volume is deliberately NOT stop or pause. The native loop and its
    // sample position keep advancing through battles, menus and track changes.
    const float targetGain = astral && eligible && ready && (!battleActive || replaceAstralBattle) ?
        std::clamp(gain, 0.0f, 1.0f) * fade.astral() * encounterFade.palace() : 0.0f;
    // Gentle re-entry, immediate reductions: never let a fade-out trail cross
    // the exclusive handoff into Palace or protected music.
    AstralMp3Ambient.setGain(targetGain, elapsed);
    AstralMp3Combat.setGain(astral && replaceAstralBattle ? std::clamp(battleVolume, 0.0f, 1.0f) *
        fade.astral() * encounterFade.astral() : 0.0f, elapsed, true, true);
    DarkHourAmbient.setGain(darkHour && eligible && darkHourReady ? std::clamp(gain, 0.0f, 1.0f) *
        fade.astral() * encounterFade.palace() : 0.0f, elapsed);
    DarkHourCombat.setGain(replaceDarkHourBattle && battleActive ?
        std::clamp(battleVolume, 0.0f, 1.0f) * fade.astral() * encounterFade.astral() : 0.0f,
        elapsed, true, true);
}

/**
 * SDL audiostream callback to trigger rendering of new audio data.
 */
static void SDLCALL GetNewAudio(
    void*,
    SDL_AudioStream*,
    int needed,
    int);

/**
 * Render an entire new frame of audio and output it to SDL3.
 * Note: "audio frames" are unrelated to video frames.
 * @return Amount of audio samples rendered in bytes.
 */
static int RenderNewAudioFrame();

/**
 * Render an audio subframe and output it to SDL3.
 */
static void RenderAudioSubframe();

static void InitSDL3Output() {
    SDL_Init(SDL_INIT_AUDIO);

    constexpr SDL_AudioSpec spec = {
        SDL_AUDIO_F32,
        2,
        SampleRate,
    };
    PlaybackStream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        &GetNewAudio,
        nullptr);
}

void dusk::audio::Initialize() {
    InitSDL3Output();
    DspInit();
    ApplySettings();

    JASDsp::initBuffer();
    JASDSPChannel::initAll();

    JASPoolAllocObject_MultiThreaded<JASChannel>::newMemPool(0x48);

    InitAstralPlane();
    SDL_ResumeAudioStreamDevice(PlaybackStream);
}

void dusk::audio::ApplySettings() {
    const auto& settings = dusk::getSettings().audio;
    SetMasterVolume(MasterVolumeToLinear(settings.masterVolume.getValue() / 100.0f));
    SetEnableReverb(settings.enableReverb.getValue());
    EnableHrtf = settings.enableHrtf.getValue();
}

void dusk::audio::SetMasterVolume(const f32 value) {
    JASCriticalSection section;

    MasterVolume = value;
}

void dusk::audio::SetPaused(const bool paused) {
    if (paused) {
        SDL_PauseAudioStreamDevice(PlaybackStream);
    } else {
        SDL_ResumeAudioStreamDevice(PlaybackStream);
    }
}

void dusk::audio::SetEnableReverb(const bool value) {
    JASCriticalSection section;

    EnableReverb = value;
}

#ifdef TRACY_ENABLE
static auto FrameName = "GetNewAudio";
#endif

void SDLCALL GetNewAudio(
    void*,
    SDL_AudioStream*,
    int needed,
    int) {
    FrameMarkStart(FrameName);
    while (needed > 0) {
        const int rendered = RenderNewAudioFrame();
        needed -= rendered;
    }
    FrameMarkEnd(FrameName);
}

int RenderNewAudioFrame() {
    ZoneScoped;
    JASCriticalSection section;
    const u32 countSubframes = JASDriver::getSubFrames();

    JASAudioThread::setDSPSyncCount(countSubframes);

    for (u32 i = 0; i < countSubframes; i++) {
        RenderAudioSubframe();

        JASAudioThread::snIntCount -= 1;
    }

    return static_cast<u16>(countSubframes) * sizeof(OutputSubframe);
}

static void InterleaveOutputData(const OutputSubframe& data, std::span<f32> target) {
    assert(target.size() >= data.channels[0].size() * OutputSubframe::NUM_CHANNELS);

    size_t outPos = 0;
    for (size_t inPos = 0; inPos < data.channels[0].size(); inPos++) {
        for (size_t channelIdx = 0; channelIdx < OutputSubframe::NUM_CHANNELS; channelIdx++) {
            target[outPos++] = data.channels[channelIdx][inPos];
        }
    }
}

void RenderAudioSubframe() {
    ZoneScoped;
    OutBuffer = {};

    JASDriver::updateDSP();
    DspRender(OutBuffer);

    InterleaveOutputData(OutBuffer, OutInterleaveBuffer);
    const float customVolume = TwilightMusicVolume.load();
    AstralMp3Ambient.mix(OutInterleaveBuffer, 0.85f, customVolume);
    AstralMp3Combat.mix(OutInterleaveBuffer, 0.85f, customVolume);
    DarkHourAmbient.mix(OutInterleaveBuffer, 0.65f, customVolume);
    DarkHourCombat.mix(OutInterleaveBuffer, 0.65f, customVolume);

    if (JASDriver::extMixCallback != nullptr && JASDriver::sMixMode == MIX_MODE_INTERLEAVE) {
        static_assert(OutputSubframe::NUM_CHANNELS == 2); // This code only works with Stereo so far.
        // NOTE: In the real game, this gets called on the entire audio frame, rather than the subframe.
        // That's probably more efficient, but I didn't wanna change the code to calculate the
        // entire audio buffers at once.
        // This is only used for the movie player, and it seems to work fine with the smaller calls.
        const auto mixData = JASDriver::extMixCallback(DSP_SUBFRAME_SIZE);
        if (mixData) {
            for (int i = 0; i < OutInterleaveBuffer.size(); i++) {
                OutInterleaveBuffer[i] += static_cast<f32>(mixData[i]) / static_cast<f32>(0x7FFF);
            }
        }
    }

    SDL_PutAudioStreamData(PlaybackStream, &OutInterleaveBuffer, sizeof(OutInterleaveBuffer));
}

u32 dusk::audio::GetResetCount(int channelIdx) {
    return ChannelAux[channelIdx].resetCount;
}

f32 dusk::audio::VolumeFromU16(u16 value) {
    return static_cast<f32>(value) / static_cast<f32>(JASDriver::getChannelLevel_dsp());
}
