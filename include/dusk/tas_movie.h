#pragma once

#include "SSystem/SComponent/c_API_controller_pad.h"
#include "SSystem/SComponent/c_math.h"
#include "SSystem/SComponent/c_xyz.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class view_class;

namespace dusk::tas_movie {

enum class State {
    Idle,
    WaitingToRecord,
    Recording,
    WaitingToPlay,
    Playing,
};

struct CameraKeyframe {
    uint32_t frame = 0;
    cXyz eye;
    cXyz center;
    float fovy = 60.0f;
    s16 bank = 0;
};

State state();
const char* stateName();
bool active();
bool waitingForAnchor();
bool hasMovie();
bool hasAnchor();
size_t recordedFrames();
size_t playbackFrame();
const std::string& anchor();

bool armRecording(std::string encodedState);
bool armPlayback();
bool armPlaybackToFrame(size_t frame);
bool branchRecordingFromPlayback();
void onPlaySceneCreateBegin();
void notifyAnchorReady();
void cancelAnchorLoad();
void stop();
void clear();

bool tick(interface_of_controller_pad* pads, bool ctrlRResetRequested);
void recordResetRequest();
bool rngCallDiverged();
size_t rngDivergenceFrame();
const cM_RndCallCounts& expectedRngCalls();
const cM_RndCallCounts& actualRngCalls();

bool paused();
void setPaused(bool paused);
void requestFrameAdvance();
bool turbo();
void setTurbo(bool turbo);
float simulationRate();
void setSimulationRate(float hz);
int simulationTicksForHostFrame(int normalTicks);

std::string serialize();
bool validateSerialized(std::string_view data);
bool loadSerialized(std::string_view data);

void setPresentationCameraEnabled(bool enabled);
bool presentationCameraEnabled();
void setPresentationCameraControlEnabled(bool enabled);
bool presentationCameraControlEnabled();
void setPresentationCameraMouseLookEnabled(bool enabled);
bool presentationCameraMouseLookEnabled();
float presentationCameraMoveSpeed();
void setPresentationCameraMoveSpeed(float speed);
float presentationCameraFov();
void setPresentationCameraFov(float fov);
float presentationCameraBankDegrees();
void setPresentationCameraBankDegrees(float degrees);
void copyPresentationCameraFromView(const view_class* view);
void updatePresentationCameraControls(float deltaSeconds);
void applyPresentationCamera(view_class* view);
void restorePresentationCamera();

void captureCameraKeyframe();
void goToCameraKeyframe(size_t index);
void deleteCameraKeyframe(size_t index);
void clearCameraKeyframes();
size_t cameraKeyframeCount();
const CameraKeyframe* cameraKeyframe(size_t index);
void setCameraKeyframeFrame(size_t index, uint32_t frame);
bool cameraTrackPlaying();
bool cameraTrackPaused();
void playCameraTrack();
void pauseCameraTrack(bool paused);
void stopCameraTrack();
bool cameraTrackLoop();
void setCameraTrackLoop(bool loop);
bool cameraTrackEase();
void setCameraTrackEase(bool ease);

}  // namespace dusk::tas_movie
