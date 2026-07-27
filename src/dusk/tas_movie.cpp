#include "dusk/tas_movie.h"

#include "dusk/game_clock.h"
#include "dusk/frame_interpolation.h"
#include "d/d_camera.h"
#include "f_op/f_op_view.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include "m_Do/m_Do_mtx.h"

#include <SDL3/SDL_keyboard.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace dusk::tas_movie {
namespace {

constexpr char kMagic[8] = {'D', 'S', 'K', 'T', 'A', 'S', '0', '1'};
// Version 1 captured RNG before outgoing-scene teardown and cannot be replayed
// deterministically under the corrected scene-boundary semantics.
constexpr uint32_t kVersion = 2;
constexpr int kTurboTicks = 8;
constexpr float kCameraMoveSpeed = 900.0f;
constexpr float kCameraFastMultiplier = 4.0f;
constexpr float kCameraLookSensitivity = 0.004f;
constexpr float kTargetDistance = 100.0f;
constexpr float kCameraTrackFramesPerSecond = 30.0f;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t anchorSize;
    uint32_t frameCount;
    uint32_t cameraKeyframeCount;
    cM_RndState rng;
};

struct FileFrame {
    interface_of_controller_pad pads[4];
    cM_RndCallCounts rngCalls;
    uint8_t ctrlRResetRequested;
    uint8_t reserved[3];
};

struct FileCameraKeyframe {
    uint32_t frame;
    float eye[3];
    float center[3];
    float fovy;
    int16_t bank;
    uint16_t reserved;
};
#pragma pack(pop)

struct Frame {
    std::array<interface_of_controller_pad, 4> pads{};
    cM_RndCallCounts rngCalls{};
    bool ctrlRResetRequested = false;
};

struct PresentationCamera {
    bool enabled = false;
    bool controlsEnabled = false;
    bool initialized = false;
    bool mouseLookEnabled = true;
    bool pWasDown = false;
    bool captureWasDown = false;
    cXyz eye;
    cXyz center;
    float fovy = 60.0f;
    s16 bank = 0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float moveSpeed = kCameraMoveSpeed;
};

struct ViewBackup {
    bool valid = false;
    view_class* view = nullptr;
    view_class saved{};
    dCamera_c* camera = nullptr;
    cXyz cameraCenter;
    cXyz cameraEye;
    float cameraFovy = 60.0f;
    s16 cameraBank = 0;
};

State sState = State::Idle;
std::string sAnchor;
cM_RndState sInitialRng{};
std::vector<Frame> sFrames;
std::vector<CameraKeyframe> sCameraKeyframes;
size_t sPlaybackFrame = 0;
bool sPlaybackResetComboHeld = false;
bool sPendingResetRequest = false;
bool sRngCallDiverged = false;
size_t sRngDivergenceFrame = 0;
cM_RndCallCounts sExpectedRngCalls{};
cM_RndCallCounts sActualRngCalls{};
bool sPaused = false;
bool sFrameAdvancePending = false;
bool sTurbo = false;
float sSimulationRate = 30.0f;
bool sAnchorJustReady = false;
bool sPlaybackRumbleSuppressed = false;
u32 sSavedRumbleMask = 0;
bool sPendingRngRestore = false;
size_t sPauseAtFrame = std::numeric_limits<size_t>::max();
PresentationCamera sPresentationCamera;
ViewBackup sViewBackup;
bool sCameraTrackPlaying = false;
bool sCameraTrackPaused = false;
bool sCameraTrackLoop = false;
bool sCameraTrackEase = true;
bool sDualCameraCulling = true;
float sCameraTrackPreviewFrame = 0.0f;

void updateCameraAngles() {
    const cXyz direction = sPresentationCamera.center - sPresentationCamera.eye;
    sPresentationCamera.yaw = std::atan2(direction.z, direction.x);
    const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    sPresentationCamera.pitch = std::atan2(direction.y, horizontal);
}

void updateCameraCenter() {
    sPresentationCamera.center.x =
        sPresentationCamera.eye.x + std::cos(sPresentationCamera.yaw) *
                                            std::cos(sPresentationCamera.pitch) * kTargetDistance;
    sPresentationCamera.center.y =
        sPresentationCamera.eye.y + std::sin(sPresentationCamera.pitch) * kTargetDistance;
    sPresentationCamera.center.z =
        sPresentationCamera.eye.z + std::sin(sPresentationCamera.yaw) *
                                            std::cos(sPresentationCamera.pitch) * kTargetDistance;
}

void suppressPlaybackRumble() {
    if (sPlaybackRumbleSuppressed) {
        return;
    }
    sSavedRumbleMask = JUTGamePad::CRumble::mEnabled;
    for (int port = 0; port < 4; ++port) {
        JUTGamePad::CRumble::stopMotorHard(port);
    }
    JUTGamePad::CRumble::setEnabled(0);
    sPlaybackRumbleSuppressed = true;
}

void restorePlaybackRumble() {
    if (!sPlaybackRumbleSuppressed) {
        return;
    }
    JUTGamePad::CRumble::setEnabled(sSavedRumbleMask);
    sPlaybackRumbleSuppressed = false;
}

float timelineFrame() {
    if (sState == State::Playing) {
        if (frame_interp::is_enabled() && !frame_interp::is_sim_frame()) {
            return std::max(
                0.0f, static_cast<float>(sPlaybackFrame) - 1.0f +
                          frame_interp::get_interpolation_step());
        }
        return static_cast<float>(sPlaybackFrame);
    }
    return static_cast<float>(sFrames.size());
}

CameraKeyframe sampleCameraTrack(float frame) {
    if (sCameraKeyframes.size() == 1 || frame <= sCameraKeyframes.front().frame) {
        return sCameraKeyframes.front();
    }
    if (frame >= sCameraKeyframes.back().frame) {
        return sCameraKeyframes.back();
    }

    const auto next = std::upper_bound(
        sCameraKeyframes.begin(), sCameraKeyframes.end(), frame,
        [](float value, const CameraKeyframe& keyframe) { return value < keyframe.frame; });
    const auto previous = next - 1;
    const float duration = static_cast<float>(next->frame - previous->frame);
    float t = duration > 0.0f ? (frame - previous->frame) / duration : 1.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    if (sCameraTrackEase) {
        t = t * t * (3.0f - 2.0f * t);
    }

    CameraKeyframe result = *previous;
    result.eye = previous->eye + (next->eye - previous->eye) * t;
    result.center = previous->center + (next->center - previous->center) * t;
    result.fovy = previous->fovy + (next->fovy - previous->fovy) * t;
    const s16 bankDifference = static_cast<s16>(next->bank - previous->bank);
    result.bank = static_cast<s16>(previous->bank + static_cast<float>(bankDifference) * t);
    return result;
}

bool decode(std::string_view data, std::string& outAnchor, cM_RndState& outRng,
            std::vector<Frame>& outFrames, std::vector<CameraKeyframe>& outCameraKeyframes) {
    if (data.size() < sizeof(FileHeader)) {
        return false;
    }

    FileHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (std::memcmp(header.magic, kMagic, sizeof(header.magic)) != 0 ||
        header.version != kVersion || header.anchorSize == 0 || header.frameCount == 0)
    {
        return false;
    }

    const size_t expectedSize =
        sizeof(FileHeader) + static_cast<size_t>(header.anchorSize) +
        static_cast<size_t>(header.frameCount) * sizeof(FileFrame) +
        static_cast<size_t>(header.cameraKeyframeCount) * sizeof(FileCameraKeyframe);
    if (data.size() != expectedSize) {
        return false;
    }

    const char* cursor = data.data() + sizeof(FileHeader);
    std::string anchor(cursor, header.anchorSize);
    cursor += header.anchorSize;

    std::vector<Frame> frames(header.frameCount);
    for (Frame& frame : frames) {
        FileFrame fileFrame{};
        std::memcpy(&fileFrame, cursor, sizeof(fileFrame));
        cursor += sizeof(fileFrame);
        for (size_t i = 0; i < frame.pads.size(); ++i) {
            frame.pads[i] = fileFrame.pads[i];
        }
        frame.ctrlRResetRequested = fileFrame.ctrlRResetRequested != 0;
        frame.rngCalls = fileFrame.rngCalls;
    }

    std::vector<CameraKeyframe> cameraKeyframes(header.cameraKeyframeCount);
    for (CameraKeyframe& keyframe : cameraKeyframes) {
        FileCameraKeyframe fileKeyframe{};
        std::memcpy(&fileKeyframe, cursor, sizeof(fileKeyframe));
        cursor += sizeof(fileKeyframe);
        keyframe.frame = fileKeyframe.frame;
        keyframe.eye.set(fileKeyframe.eye[0], fileKeyframe.eye[1], fileKeyframe.eye[2]);
        keyframe.center.set(
            fileKeyframe.center[0], fileKeyframe.center[1], fileKeyframe.center[2]);
        keyframe.fovy = fileKeyframe.fovy;
        keyframe.bank = fileKeyframe.bank;
    }
    std::sort(cameraKeyframes.begin(), cameraKeyframes.end(),
              [](const CameraKeyframe& a, const CameraKeyframe& b) {
                  return a.frame < b.frame;
              });

    outAnchor = std::move(anchor);
    outRng = header.rng;
    outFrames = std::move(frames);
    outCameraKeyframes = std::move(cameraKeyframes);
    return true;
}

}  // namespace

State state() {
    return sState;
}

const char* stateName() {
    switch (sState) {
    case State::WaitingToRecord:
        return "Loading anchor for recording";
    case State::Recording:
        return sPaused ? "Recording (paused)" : "Recording";
    case State::WaitingToPlay:
        return "Loading anchor for playback";
    case State::Playing:
        return sPaused ? "Playback (paused)" : "Playing";
    case State::Idle:
    default:
        return "Idle";
    }
}

bool active() {
    return sState != State::Idle;
}

bool waitingForAnchor() {
    return sState == State::WaitingToRecord || sState == State::WaitingToPlay;
}

bool hasMovie() {
    return !sAnchor.empty() && !sFrames.empty();
}

bool hasAnchor() {
    return !sAnchor.empty();
}

size_t recordedFrames() {
    return sFrames.size();
}

size_t playbackFrame() {
    return sPlaybackFrame;
}

const std::string& anchor() {
    return sAnchor;
}

bool armRecording(std::string encodedState) {
    if (active() || encodedState.empty()) {
        return false;
    }
    sAnchor = std::move(encodedState);
    cM_getRndState(&sInitialRng);
    sPendingRngRestore = true;
    sFrames.clear();
    sCameraKeyframes.clear();
    sPlaybackFrame = 0;
    sPaused = false;
    sFrameAdvancePending = false;
    sAnchorJustReady = false;
    sRngCallDiverged = false;
    sState = State::WaitingToRecord;
    game_clock::set_sim_rate(sSimulationRate);
    return true;
}

bool armPlayback() {
    if (active() || !hasMovie()) {
        return false;
    }
    sPendingRngRestore = true;
    sPlaybackFrame = 0;
    sPlaybackResetComboHeld = false;
    sPendingResetRequest = false;
    sPaused = false;
    sFrameAdvancePending = false;
    sAnchorJustReady = false;
    sRngCallDiverged = false;
    sPauseAtFrame = std::numeric_limits<size_t>::max();
    sState = State::WaitingToPlay;
    game_clock::set_sim_rate(sSimulationRate);
    return true;
}

bool armPlaybackToFrame(size_t frame) {
    if (!armPlayback()) {
        return false;
    }
    sPauseAtFrame = std::min(frame, sFrames.size() - 1);
    return true;
}

bool branchRecordingFromPlayback() {
    if (sState != State::Playing || !sPaused) {
        return false;
    }
    sFrames.resize(std::min(sPlaybackFrame, sFrames.size()));
    sCameraKeyframes.erase(
        std::remove_if(
            sCameraKeyframes.begin(), sCameraKeyframes.end(),
            [](const CameraKeyframe& keyframe) { return keyframe.frame > sFrames.size(); }),
        sCameraKeyframes.end());
    sState = State::Recording;
    sPauseAtFrame = std::numeric_limits<size_t>::max();
    sRngCallDiverged = false;
    return true;
}

void onPlaySceneCreateBegin() {
    if (sPendingRngRestore && waitingForAnchor()) {
        cM_setRndState(&sInitialRng);
        sPendingRngRestore = false;
    }
}

void notifyAnchorReady() {
    if (!waitingForAnchor()) {
        return;
    }
    // Same-scene state restoration may not construct a new dScnPly_c. In that
    // case this is the first deterministic boundary after applying the anchor.
    if (sPendingRngRestore) {
        cM_setRndState(&sInitialRng);
        sPendingRngRestore = false;
    }
    if (sState == State::WaitingToRecord) {
        sFrames.clear();
        sState = State::Recording;
    } else if (sState == State::WaitingToPlay) {
        sPlaybackFrame = 0;
        sPlaybackResetComboHeld = false;
        sState = State::Playing;
        suppressPlaybackRumble();
        if (sPauseAtFrame == 0) {
            sPaused = true;
            sPauseAtFrame = std::numeric_limits<size_t>::max();
        }
    }
    // Anchor completion is detected on a presentation update. Discard any
    // fractional/deferred scheduler time accumulated across the load so a low
    // viewing rate cannot batch multiple movie inputs into the first display.
    sAnchorJustReady = true;
    game_clock::reset_frame_timer();
}

void cancelAnchorLoad() {
    if (waitingForAnchor()) {
        sPendingRngRestore = false;
        sState = State::Idle;
        game_clock::set_sim_rate(30.0f);
    }
}

void stop() {
    sState = State::Idle;
    sPlaybackFrame = 0;
    sPlaybackResetComboHeld = false;
    sPendingResetRequest = false;
    sPendingRngRestore = false;
    sPaused = false;
    sFrameAdvancePending = false;
    sAnchorJustReady = false;
    sTurbo = false;
    sPauseAtFrame = std::numeric_limits<size_t>::max();
    game_clock::set_sim_rate(30.0f);
    game_clock::reset_frame_timer();
}

void clear() {
    stop();
    sAnchor.clear();
    sFrames.clear();
    sCameraKeyframes.clear();
}

bool tick(interface_of_controller_pad* pads, bool ctrlRResetRequested) {
    if (sState == State::Recording) {
        Frame frame{};
        for (size_t i = 0; i < frame.pads.size(); ++i) {
            frame.pads[i] = pads[i];
        }
        frame.ctrlRResetRequested = ctrlRResetRequested;
        cM_getRndCallCounts(&frame.rngCalls);
        frame.ctrlRResetRequested = frame.ctrlRResetRequested || sPendingResetRequest;
        sPendingResetRequest = false;
        sFrames.push_back(frame);
        return false;
    }

    if (sState != State::Playing || sFrames.empty()) {
        return false;
    }

    const Frame& frame = sFrames[sPlaybackFrame];
    cM_RndCallCounts currentCalls{};
    cM_getRndCallCounts(&currentCalls);
    if (!sRngCallDiverged &&
        (currentCalls.primary != frame.rngCalls.primary ||
         currentCalls.secondary != frame.rngCalls.secondary))
    {
        sRngCallDiverged = true;
        sRngDivergenceFrame = sPlaybackFrame;
        sExpectedRngCalls = frame.rngCalls;
        sActualRngCalls = currentCalls;
    }
    for (size_t i = 0; i < frame.pads.size(); ++i) {
        pads[i] = frame.pads[i];
    }

    constexpr u32 resetCombo = PAD_BUTTON_START | PAD_BUTTON_X | PAD_BUTTON_B;
    const bool resetComboHeld = (frame.pads[0].mButtonFlags & resetCombo) == resetCombo;
    const bool resetRequested =
        frame.ctrlRResetRequested || (resetComboHeld && !sPlaybackResetComboHeld);
    sPlaybackResetComboHeld = resetComboHeld;

    ++sPlaybackFrame;
    if (sPauseAtFrame != std::numeric_limits<size_t>::max() &&
        sPlaybackFrame >= sPauseAtFrame && sPlaybackFrame < sFrames.size())
    {
        sPaused = true;
        sPauseAtFrame = std::numeric_limits<size_t>::max();
    }
    if (sPlaybackFrame >= sFrames.size()) {
        stop();
    }
    return resetRequested;
}

bool rngCallDiverged() {
    return sRngCallDiverged;
}

size_t rngDivergenceFrame() {
    return sRngDivergenceFrame;
}

const cM_RndCallCounts& expectedRngCalls() {
    return sExpectedRngCalls;
}

const cM_RndCallCounts& actualRngCalls() {
    return sActualRngCalls;
}

void recordResetRequest() {
    if (sState != State::Recording) {
        return;
    }
    if (sFrames.empty()) {
        sPendingResetRequest = true;
    } else {
        sFrames.back().ctrlRResetRequested = true;
    }
}

bool paused() {
    return sPaused;
}

void setPaused(bool paused) {
    if (sState == State::Recording || sState == State::Playing) {
        if (sPaused == paused) {
            return;
        }
        sPaused = paused;
        sFrameAdvancePending = false;
        // Paused wall time is presentation time, not simulation debt.
        game_clock::reset_frame_timer();
    }
}

void requestFrameAdvance() {
    if ((sState == State::Recording || sState == State::Playing) && sPaused) {
        sFrameAdvancePending = true;
    }
}

bool turbo() {
    return sTurbo;
}

void setTurbo(bool turbo) {
    if (sTurbo == turbo) {
        return;
    }
    sTurbo = turbo;
    game_clock::reset_frame_timer();
}

float simulationRate() {
    return sSimulationRate;
}

void setSimulationRate(float hz) {
    sSimulationRate = std::clamp(hz, 1.0f, 120.0f);
    if (active()) {
        game_clock::set_sim_rate(sSimulationRate);
    }
}

int simulationTicksForHostFrame(int normalTicks) {
    if (sState != State::Playing) {
        restorePlaybackRumble();
    }
    if (sState != State::Recording && sState != State::Playing) {
        return normalTicks;
    }
    if (sAnchorJustReady) {
        if (normalTicks <= 0) {
            return 0;
        }
        sAnchorJustReady = false;
        normalTicks = std::min(normalTicks, 1);
    }
    if (sPaused) {
        if (sFrameAdvancePending) {
            sFrameAdvancePending = false;
            game_clock::reset_frame_timer();
            return 1;
        }
        // advance_main_loop() has already sampled the host clock. Rebase every
        // paused presentation so elapsed wall time can never accumulate into
        // catch-up ticks when playback resumes.
        game_clock::reset_frame_timer();
        return 0;
    }
    if (sTurbo) {
        return std::max(normalTicks, kTurboTicks);
    }
    return normalTicks;
}

std::string serialize() {
    if (!hasMovie()) {
        return {};
    }

    FileHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(header.magic));
    header.version = kVersion;
    header.anchorSize = static_cast<uint32_t>(sAnchor.size());
    header.frameCount = static_cast<uint32_t>(sFrames.size());
    header.cameraKeyframeCount = static_cast<uint32_t>(sCameraKeyframes.size());
    header.rng = sInitialRng;

    std::string data(
        sizeof(header) + sAnchor.size() + sFrames.size() * sizeof(FileFrame) +
            sCameraKeyframes.size() * sizeof(FileCameraKeyframe),
        '\0');
    char* cursor = data.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    std::memcpy(cursor, sAnchor.data(), sAnchor.size());
    cursor += sAnchor.size();

    for (const Frame& frame : sFrames) {
        FileFrame fileFrame{};
        for (size_t i = 0; i < frame.pads.size(); ++i) {
            fileFrame.pads[i] = frame.pads[i];
        }
        fileFrame.ctrlRResetRequested = frame.ctrlRResetRequested ? 1 : 0;
        fileFrame.rngCalls = frame.rngCalls;
        std::memcpy(cursor, &fileFrame, sizeof(fileFrame));
        cursor += sizeof(fileFrame);
    }
    for (const CameraKeyframe& keyframe : sCameraKeyframes) {
        FileCameraKeyframe fileKeyframe{};
        fileKeyframe.frame = keyframe.frame;
        fileKeyframe.eye[0] = keyframe.eye.x;
        fileKeyframe.eye[1] = keyframe.eye.y;
        fileKeyframe.eye[2] = keyframe.eye.z;
        fileKeyframe.center[0] = keyframe.center.x;
        fileKeyframe.center[1] = keyframe.center.y;
        fileKeyframe.center[2] = keyframe.center.z;
        fileKeyframe.fovy = keyframe.fovy;
        fileKeyframe.bank = keyframe.bank;
        std::memcpy(cursor, &fileKeyframe, sizeof(fileKeyframe));
        cursor += sizeof(fileKeyframe);
    }
    return data;
}

bool validateSerialized(std::string_view data) {
    std::string anchor;
    cM_RndState rng{};
    std::vector<Frame> frames;
    std::vector<CameraKeyframe> cameraKeyframes;
    return decode(data, anchor, rng, frames, cameraKeyframes);
}

bool loadSerialized(std::string_view data) {
    std::string anchor;
    cM_RndState rng{};
    std::vector<Frame> frames;
    std::vector<CameraKeyframe> cameraKeyframes;
    if (!decode(data, anchor, rng, frames, cameraKeyframes)) {
        return false;
    }
    stop();
    sRngCallDiverged = false;
    sAnchor = std::move(anchor);
    sInitialRng = rng;
    sFrames = std::move(frames);
    sCameraKeyframes = std::move(cameraKeyframes);
    return true;
}

void setPresentationCameraEnabled(bool enabled) {
    sPresentationCamera.enabled = enabled;
    if (!enabled) {
        sPresentationCamera.controlsEnabled = false;
        stopCameraTrack();
        restorePresentationCamera();
    }
}

bool presentationCameraEnabled() {
    return sPresentationCamera.enabled;
}

void setPresentationCameraControlEnabled(bool enabled) {
    sPresentationCamera.controlsEnabled = enabled && sPresentationCamera.enabled;
    sPresentationCamera.pWasDown = false;
    sPresentationCamera.captureWasDown = false;
    if (sPresentationCamera.controlsEnabled) {
        stopCameraTrack();
    }
}

bool presentationCameraControlEnabled() {
    return sPresentationCamera.controlsEnabled;
}

void setPresentationCameraMouseLookEnabled(bool enabled) {
    sPresentationCamera.mouseLookEnabled = enabled;
}

bool presentationCameraMouseLookEnabled() {
    return sPresentationCamera.mouseLookEnabled;
}

bool presentationCameraDualCullingEnabled() {
    return sDualCameraCulling;
}

void setPresentationCameraDualCullingEnabled(bool enabled) {
    sDualCameraCulling = enabled;
}

bool getGameplayCullView(Mtx outView, float* fovy, float* aspect, float* nearPlane) {
    if (!sDualCameraCulling || !sPresentationCamera.enabled || !sViewBackup.valid) {
        return false;
    }
    MTXCopy(sViewBackup.saved.viewMtx, outView);
    if (fovy != nullptr) {
        *fovy = sViewBackup.saved.fovy;
    }
    if (aspect != nullptr) {
        *aspect = sViewBackup.saved.aspect;
    }
    if (nearPlane != nullptr) {
        *nearPlane = sViewBackup.saved.near_;
    }
    return true;
}

float presentationCameraMoveSpeed() {
    return sPresentationCamera.moveSpeed;
}

void setPresentationCameraMoveSpeed(float speed) {
    sPresentationCamera.moveSpeed = std::clamp(speed, 1.0f, 10000.0f);
}

float presentationCameraFov() {
    return sPresentationCamera.fovy;
}

void setPresentationCameraFov(float fov) {
    sPresentationCamera.fovy = std::clamp(fov, 1.0f, 179.0f);
}

float presentationCameraBankDegrees() {
    return static_cast<float>(sPresentationCamera.bank) * (180.0f / 32768.0f);
}

void setPresentationCameraBankDegrees(float degrees) {
    const float wrapped = std::remainder(degrees, 360.0f);
    sPresentationCamera.bank =
        static_cast<s16>(wrapped * (32768.0f / 180.0f));
}

void copyPresentationCameraFromView(const view_class* view) {
    if (view == nullptr) {
        return;
    }
    sPresentationCamera.eye = view->lookat.eye;
    sPresentationCamera.center = view->lookat.center;
    sPresentationCamera.fovy = view->fovy;
    sPresentationCamera.bank = view->bank;
    sPresentationCamera.initialized = true;
    updateCameraAngles();
}

void updatePresentationCameraControls(float deltaSeconds) {
    if (!sPresentationCamera.enabled) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (sCameraTrackPlaying && !sCameraTrackPaused && sCameraKeyframes.size() >= 2) {
        sCameraTrackPreviewFrame += std::max(deltaSeconds, 0.0f) * kCameraTrackFramesPerSecond;
        const float first = static_cast<float>(sCameraKeyframes.front().frame);
        const float last = static_cast<float>(sCameraKeyframes.back().frame);
        if (sCameraTrackPreviewFrame > last) {
            if (sCameraTrackLoop && last > first) {
                sCameraTrackPreviewFrame =
                    first + std::fmod(sCameraTrackPreviewFrame - first, last - first);
            } else {
                sCameraTrackPreviewFrame = last;
                const CameraKeyframe& camera = sCameraKeyframes.back();
                sPresentationCamera.eye = camera.eye;
                sPresentationCamera.center = camera.center;
                sPresentationCamera.fovy = camera.fovy;
                sPresentationCamera.bank = camera.bank;
                sPresentationCamera.initialized = true;
                updateCameraAngles();
                sCameraTrackPlaying = false;
                sCameraTrackPaused = false;
            }
        }
    }
    if (sCameraTrackPlaying && !sCameraKeyframes.empty()) {
        const CameraKeyframe camera = sampleCameraTrack(sCameraTrackPreviewFrame);
        sPresentationCamera.eye = camera.eye;
        sPresentationCamera.center = camera.center;
        sPresentationCamera.fovy = camera.fovy;
        sPresentationCamera.bank = camera.bank;
        sPresentationCamera.initialized = true;
        updateCameraAngles();
    }

    if (!sPresentationCamera.controlsEnabled || !sPresentationCamera.initialized ||
        io.WantTextInput)
    {
        return;
    }

    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    if (keys == nullptr) {
        updateCameraCenter();
        return;
    }
    const auto down = [&](SDL_Scancode key) {
        return static_cast<int>(key) < keyCount && keys[key];
    };

    const bool pDown = down(SDL_SCANCODE_P);
    if (pDown && !sPresentationCamera.pWasDown) {
        sPresentationCamera.mouseLookEnabled = !sPresentationCamera.mouseLookEnabled;
    }
    sPresentationCamera.pWasDown = pDown;

    const bool captureDown = down(SDL_SCANCODE_K);
    if (captureDown && !sPresentationCamera.captureWasDown) {
        captureCameraKeyframe();
    }
    sPresentationCamera.captureWasDown = captureDown;

    if (sPresentationCamera.mouseLookEnabled) {
        sPresentationCamera.yaw += io.MouseDelta.x * kCameraLookSensitivity;
        sPresentationCamera.pitch -= io.MouseDelta.y * kCameraLookSensitivity;
        sPresentationCamera.pitch =
            std::clamp(sPresentationCamera.pitch, -1.553343f, 1.553343f);
    }

    float forward = (down(SDL_SCANCODE_W) ? 1.0f : 0.0f) -
                    (down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
    float right = (down(SDL_SCANCODE_D) ? 1.0f : 0.0f) -
                  (down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
    float up = (down(SDL_SCANCODE_SPACE) ? 1.0f : 0.0f) -
               (down(SDL_SCANCODE_LCTRL) ? 1.0f : 0.0f);
    const float length = std::sqrt(forward * forward + right * right + up * up);
    if (length > 1.0f) {
        forward /= length;
        right /= length;
        up /= length;
    }

    const float speed = sPresentationCamera.moveSpeed * std::max(deltaSeconds, 0.0f) *
                        (down(SDL_SCANCODE_LSHIFT) ? kCameraFastMultiplier : 1.0f);
    sPresentationCamera.eye.x +=
        (forward * std::cos(sPresentationCamera.yaw) -
         right * std::sin(sPresentationCamera.yaw)) * speed;
    sPresentationCamera.eye.y += up * speed;
    sPresentationCamera.eye.z +=
        (forward * std::sin(sPresentationCamera.yaw) +
         right * std::cos(sPresentationCamera.yaw)) * speed;
    const float bankStep = 90.0f * std::max(deltaSeconds, 0.0f);
    setPresentationCameraBankDegrees(
        presentationCameraBankDegrees() +
        ((down(SDL_SCANCODE_E) ? 1.0f : 0.0f) -
         (down(SDL_SCANCODE_Q) ? 1.0f : 0.0f)) * bankStep);
    updateCameraCenter();
}

void applyPresentationCamera(view_class* view) {
    if (!sPresentationCamera.enabled || view == nullptr || sViewBackup.valid) {
        return;
    }
    if (!sPresentationCamera.initialized) {
        copyPresentationCameraFromView(view);
    }

    CameraKeyframe camera{};
    if (sCameraTrackPlaying && !sCameraKeyframes.empty()) {
        camera = sampleCameraTrack(sCameraTrackPreviewFrame);
    } else if (sState == State::Playing && !sPresentationCamera.controlsEnabled &&
        !sCameraKeyframes.empty())
    {
        camera = sampleCameraTrack(timelineFrame());
    } else {
        camera.eye = sPresentationCamera.eye;
        camera.center = sPresentationCamera.center;
        camera.fovy = sPresentationCamera.fovy;
        camera.bank = sPresentationCamera.bank;
    }

    sViewBackup.valid = true;
    sViewBackup.view = view;
    std::memcpy(&sViewBackup.saved, view, sizeof(view_class));
    if (dCam_getCamera() != nullptr) {
        sViewBackup.camera = dCam_getBody();
        sViewBackup.camera->getRawRenderTransform(
            sViewBackup.cameraCenter, sViewBackup.cameraEye,
            sViewBackup.cameraFovy, sViewBackup.cameraBank);
        sViewBackup.camera->setRawRenderTransform(
            camera.center, camera.eye, camera.fovy, camera.bank);
    }

    view->lookat.eye = camera.eye;
    view->lookat.center = camera.center;
    view->lookat.up.set(0.0f, 1.0f, 0.0f);
    view->fovy = std::clamp(camera.fovy, 0.1f, 179.9f);
    view->bank = camera.bank;
    C_MTXPerspective(
        view->projMtx, view->fovy, view->aspect, view->near_, view->far_);
    mDoMtx_lookAt(
        view->viewMtx, &view->lookat.eye, &view->lookat.center, &view->lookat.up,
        view->bank);
#if WIDESCREEN_SUPPORT
    mDoGph_gInf_c::setWideZoomProjection(view->projMtx);
#endif
    j3dSys.setViewMtx(view->viewMtx);
    cMtx_inverse(view->viewMtx, view->invViewMtx);
    MTXCopy(view->viewMtx, view->viewMtxNoTrans);
    view->viewMtxNoTrans[0][3] = 0.0f;
    view->viewMtxNoTrans[1][3] = 0.0f;
    view->viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(view->projMtx, view->viewMtx, view->projViewMtx);
    mDoLib_clipper::setup(
        view->fovy, view->aspect, view->near_, mDoLib_clipper::getFar());
}

void restorePresentationCamera() {
    if (!sViewBackup.valid || sViewBackup.view == nullptr) {
        return;
    }
    view_class* view = sViewBackup.view;
    std::memcpy(view, &sViewBackup.saved, sizeof(view_class));
    if (sViewBackup.camera != nullptr) {
        sViewBackup.camera->setRawRenderTransform(
            sViewBackup.cameraCenter, sViewBackup.cameraEye,
            sViewBackup.cameraFovy, sViewBackup.cameraBank);
    }
#if WIDESCREEN_SUPPORT
    mDoGph_gInf_c::setWideZoomProjection(view->projMtx);
#endif
    j3dSys.setViewMtx(view->viewMtx);
    mDoLib_clipper::setup(
        view->fovy, view->aspect, view->near_, mDoLib_clipper::getFar());
    sViewBackup = {};
}

void captureCameraKeyframe() {
    if (!sPresentationCamera.enabled || !sPresentationCamera.initialized) {
        return;
    }
    CameraKeyframe keyframe{};
    keyframe.frame = static_cast<uint32_t>(timelineFrame());
    keyframe.eye = sPresentationCamera.eye;
    keyframe.center = sPresentationCamera.center;
    keyframe.fovy = sPresentationCamera.fovy;
    keyframe.bank = sPresentationCamera.bank;
    sCameraKeyframes.push_back(keyframe);
    std::sort(sCameraKeyframes.begin(), sCameraKeyframes.end(),
              [](const CameraKeyframe& a, const CameraKeyframe& b) {
                  return a.frame < b.frame;
              });
}

void goToCameraKeyframe(size_t index) {
    if (index >= sCameraKeyframes.size()) {
        return;
    }
    stopCameraTrack();
    const CameraKeyframe& keyframe = sCameraKeyframes[index];
    sPresentationCamera.eye = keyframe.eye;
    sPresentationCamera.center = keyframe.center;
    sPresentationCamera.fovy = keyframe.fovy;
    sPresentationCamera.bank = keyframe.bank;
    sPresentationCamera.initialized = true;
    updateCameraAngles();
}

void deleteCameraKeyframe(size_t index) {
    if (index < sCameraKeyframes.size()) {
        sCameraKeyframes.erase(sCameraKeyframes.begin() + index);
    }
}

void clearCameraKeyframes() {
    stopCameraTrack();
    sCameraKeyframes.clear();
}

size_t cameraKeyframeCount() {
    return sCameraKeyframes.size();
}

const CameraKeyframe* cameraKeyframe(size_t index) {
    return index < sCameraKeyframes.size() ? &sCameraKeyframes[index] : nullptr;
}

void setCameraKeyframeFrame(size_t index, uint32_t frame) {
    if (index >= sCameraKeyframes.size()) {
        return;
    }
    sCameraKeyframes[index].frame =
        static_cast<uint32_t>(std::min<size_t>(frame, sFrames.size()));
    std::sort(sCameraKeyframes.begin(), sCameraKeyframes.end(),
              [](const CameraKeyframe& a, const CameraKeyframe& b) {
                  return a.frame < b.frame;
              });
}

bool cameraTrackPlaying() {
    return sCameraTrackPlaying;
}

bool cameraTrackPaused() {
    return sCameraTrackPaused;
}

void playCameraTrack() {
    if (!sPresentationCamera.enabled || sCameraKeyframes.size() < 2) {
        return;
    }
    sPresentationCamera.controlsEnabled = false;
    if (!sCameraTrackPlaying) {
        sCameraTrackPreviewFrame = static_cast<float>(sCameraKeyframes.front().frame);
    }
    sCameraTrackPlaying = true;
    sCameraTrackPaused = false;
}

void pauseCameraTrack(bool paused) {
    if (sCameraTrackPlaying) {
        sCameraTrackPaused = paused;
    }
}

void stopCameraTrack() {
    sCameraTrackPlaying = false;
    sCameraTrackPaused = false;
}

bool cameraTrackLoop() {
    return sCameraTrackLoop;
}

void setCameraTrackLoop(bool loop) {
    sCameraTrackLoop = loop;
}

bool cameraTrackEase() {
    return sCameraTrackEase;
}

void setCameraTrackEase(bool ease) {
    sCameraTrackEase = ease;
}

}  // namespace dusk::tas_movie
