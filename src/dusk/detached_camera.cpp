#include "dusk/detached_camera.h"

#include "d/d_camera.h"
#include "f_op/f_op_view.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"

#include <SDL3/SDL_keyboard.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dusk::detached_camera {
namespace {

constexpr float kDefaultMoveSpeed = 900.0f;
constexpr float kFastMultiplier = 4.0f;
constexpr float kLookSensitivity = 0.004f;
constexpr float kTargetDistance = 100.0f;

struct DetachedCamera {
    bool enabled = false;
    bool controlsEnabled = false;
    bool initialized = false;
    bool mouseLookEnabled = true;
    bool mouseLookKeyWasDown = false;
    cXyz eye;
    cXyz center;
    float fovy = 60.0f;
    s16 bank = 0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float moveSpeed = kDefaultMoveSpeed;
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

DetachedCamera sCamera;
ViewBackup sViewBackup;

void updateAngles() {
    const cXyz direction = sCamera.center - sCamera.eye;
    sCamera.yaw = std::atan2(direction.z, direction.x);
    const float horizontal =
        std::sqrt(direction.x * direction.x + direction.z * direction.z);
    sCamera.pitch = std::atan2(direction.y, horizontal);
}

void updateCenter() {
    sCamera.center.x =
        sCamera.eye.x + std::cos(sCamera.yaw) * std::cos(sCamera.pitch) * kTargetDistance;
    sCamera.center.y = sCamera.eye.y + std::sin(sCamera.pitch) * kTargetDistance;
    sCamera.center.z =
        sCamera.eye.z + std::sin(sCamera.yaw) * std::cos(sCamera.pitch) * kTargetDistance;
}

}  // namespace

void setEnabled(bool enabled) {
    sCamera.enabled = enabled;
    if (!enabled) {
        sCamera.controlsEnabled = false;
        restore();
    }
}

bool isEnabled() {
    return sCamera.enabled;
}

void setControlsEnabled(bool enabled) {
    sCamera.controlsEnabled = enabled && sCamera.enabled;
    sCamera.mouseLookKeyWasDown = false;
}

bool areControlsEnabled() {
    return sCamera.controlsEnabled;
}

void setMouseLookEnabled(bool enabled) {
    sCamera.mouseLookEnabled = enabled;
}

bool isMouseLookEnabled() {
    return sCamera.mouseLookEnabled;
}

float getMoveSpeed() {
    return sCamera.moveSpeed;
}

void setMoveSpeed(float speed) {
    sCamera.moveSpeed = std::clamp(speed, 1.0f, 10000.0f);
}

float getFov() {
    return sCamera.fovy;
}

void setFov(float fov) {
    sCamera.fovy = std::clamp(fov, 1.0f, 179.0f);
}

float getBankDegrees() {
    return static_cast<float>(sCamera.bank) * (180.0f / 32768.0f);
}

void setBankDegrees(float degrees) {
    const float wrapped = std::remainder(degrees, 360.0f);
    sCamera.bank = static_cast<s16>(wrapped * (32768.0f / 180.0f));
}

void copyFromView(const view_class* view) {
    if (view == nullptr) {
        return;
    }

    sCamera.eye = view->lookat.eye;
    sCamera.center = view->lookat.center;
    sCamera.fovy = view->fovy;
    sCamera.bank = view->bank;
    sCamera.initialized = true;
    updateAngles();
}

void focusOn(const cXyz& target, float distance) {
    if (!sCamera.enabled || !sCamera.initialized) {
        return;
    }

    const float clampedDistance = std::max(distance, 1.0f);
    const float horizontal = std::cos(sCamera.pitch);
    const cXyz forward(
        std::cos(sCamera.yaw) * horizontal,
        std::sin(sCamera.pitch),
        std::sin(sCamera.yaw) * horizontal);

    sCamera.center = target;
    sCamera.eye = target - forward * clampedDistance;
    updateAngles();
}

void updateControls(float deltaSeconds) {
    if (!sCamera.enabled || !sCamera.controlsEnabled || !sCamera.initialized) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }

    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    if (keys == nullptr) {
        updateCenter();
        return;
    }
    const auto down = [&](SDL_Scancode key) {
        return static_cast<int>(key) < keyCount && keys[key];
    };

    const bool mouseLookKeyDown = down(SDL_SCANCODE_P);
    if (mouseLookKeyDown && !sCamera.mouseLookKeyWasDown) {
        sCamera.mouseLookEnabled = !sCamera.mouseLookEnabled;
    }
    sCamera.mouseLookKeyWasDown = mouseLookKeyDown;

    if (sCamera.mouseLookEnabled) {
        sCamera.yaw += io.MouseDelta.x * kLookSensitivity;
        sCamera.pitch -= io.MouseDelta.y * kLookSensitivity;
        sCamera.pitch = std::clamp(sCamera.pitch, -1.553343f, 1.553343f);
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

    const float speed =
        sCamera.moveSpeed * std::max(deltaSeconds, 0.0f) *
        (down(SDL_SCANCODE_LSHIFT) ? kFastMultiplier : 1.0f);
    sCamera.eye.x +=
        (forward * std::cos(sCamera.yaw) - right * std::sin(sCamera.yaw)) * speed;
    sCamera.eye.y += up * speed;
    sCamera.eye.z +=
        (forward * std::sin(sCamera.yaw) + right * std::cos(sCamera.yaw)) * speed;

    const float bankStep = 90.0f * std::max(deltaSeconds, 0.0f);
    setBankDegrees(getBankDegrees() +
                   ((down(SDL_SCANCODE_E) ? 1.0f : 0.0f) -
                    (down(SDL_SCANCODE_Q) ? 1.0f : 0.0f)) *
                       bankStep);
    updateCenter();
}

void apply(view_class* view) {
    if (!sCamera.enabled || view == nullptr || sViewBackup.valid) {
        return;
    }
    if (!sCamera.initialized) {
        copyFromView(view);
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
            sCamera.center, sCamera.eye, sCamera.fovy, sCamera.bank);
    }

    view->lookat.eye = sCamera.eye;
    view->lookat.center = sCamera.center;
    view->lookat.up.set(0.0f, 1.0f, 0.0f);
    view->fovy = std::clamp(sCamera.fovy, 0.1f, 179.9f);
    view->bank = sCamera.bank;
    C_MTXPerspective(view->projMtx, view->fovy, view->aspect, view->near_, view->far_);
    mDoMtx_lookAt(view->viewMtx, &view->lookat.eye, &view->lookat.center,
                  &view->lookat.up, view->bank);
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
}

void restore() {
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
    sViewBackup = {};
}

}  // namespace dusk::detached_camera
