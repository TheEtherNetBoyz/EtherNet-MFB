#include "f_op/f_op_camera_mng.h"
#include "SSystem/SComponent/c_xyz.h"
#include "d/d_com_inf_game.h"

#include "imgui.h"
#include "ImGuiConfig.hpp"
#include "ImGuiConsole.hpp"
#include "ImGuiMenuTools.hpp"
#include "dusk/settings.h"

#include <vector>

namespace {
struct CameraKeyframe {
    cXyz center;
    cXyz eye;
    f32 fovy;
    s16 bank;
};

std::vector<CameraKeyframe> sCameraKeyframes;
}

namespace dusk {
    void ImGuiMenuTools::ShowCameraOverlay() {
        if (!getSettings().backend.enableAdvancedSettings ||
            !ImGuiConsole::CheckMenuViewToggle(getSettings().hotkeys.debugCamera, m_showCameraOverlay))
        {
            return;
        }

        auto* cam = (camera_process_class*)dCam_getCamera();

        if (!m_showCameraOverlay || cam == nullptr)
            return;

        auto* dCam = &cam->mCamera;

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (m_cameraOverlayCorner != -1) {
            SetOverlayWindowLocation(m_cameraOverlayCorner);
            windowFlags |= ImGuiWindowFlags_NoMove;
        }

        // ImGui::SetNextWindowBgAlpha(0.65f);

        if (!ImGui::Begin("Camera Debug", nullptr, windowFlags)) {
            ImGui::End();
            return;
        }

        ImGui::SeparatorText("Camera Transform Data");

        cXyz center = dCam->mCenter;
        cXyz eye = dCam->mEye;

        if (ImGui::InputFloat3("Camera Center", &center.x)) {
            dCam->Reset(center, eye);
        }
        if (ImGui::InputFloat3("Camera Eye", &eye.x)) {
            dCam->Reset(center, eye);
        }

        if (ImGui::InputFloat("Camera FOV", &dCam->mFovy)) {
            dCam->mFovy = std::clamp(dCam->mFovy, 0.1f, 179.9f);
        }

        ImGui::SeparatorText("Options");

        bool eventRunning = (dComIfGp_event_runCheck() || dComIfGp_isPauseFlag()) && !getSettings().game.debugFlyCam;
        if (eventRunning) {
            ImGui::BeginDisabled();
        }
        config::ImGuiCheckbox("Fly Mode", getSettings().game.debugFlyCam);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (eventRunning) {
                ImGui::SetTooltip("Cannot enable while paused or during an active event.");
            } else {
                ImGui::SetTooltip("Detach camera and fly freely.\n"
                                  "WASD/Arrows/Left stick: move, Mouse/C-stick: look\n"
                                  "Ctrl/L: down, Space/R: up, Shift/Z: fast\n"
                                  "Q Key/Y: roll left, E Key/X: roll right\n"
                                  "P: toggle mouse look");
            }
        }
        if (eventRunning) {
            ImGui::EndDisabled();
        }

        if (!getSettings().game.debugFlyCam) {
            ImGui::BeginDisabled();
        }
        config::ImGuiCheckbox("Freeze Time", getSettings().game.debugFlyCamLockEvents);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!getSettings().game.debugFlyCam) {
                ImGui::SetTooltip("Enable Fly Mode first.");
            } else {
                ImGui::SetTooltip("Freezes the game while flying.");
            }
        }
        if (!getSettings().game.debugFlyCam) {
            ImGui::EndDisabled();
        }

        bool mouseLook = dCamera_c::isDebugFlyCamMouseLookEnabled();
        if (ImGui::Checkbox("Mouse Look (P)", &mouseLook)) {
            dCamera_c::setDebugFlyCamMouseLookEnabled(mouseLook);
        }

        ImGui::SeparatorText("Keyframes");

        if (!getSettings().game.debugFlyCam) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Capture Keyframe")) {
            sCameraKeyframes.push_back({dCam->mCenter, dCam->mEye, dCam->mFovy,
                                        dCam->mBank.Val()});
        }
        if (!getSettings().game.debugFlyCam) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear") && !sCameraKeyframes.empty()) {
            sCameraKeyframes.clear();
        }

        for (size_t i = 0; i < sCameraKeyframes.size();) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("Keyframe %zu", i + 1);
            ImGui::SameLine();
            if (ImGui::SmallButton("Go")) {
                const CameraKeyframe& keyframe = sCameraKeyframes[i];
                dCam->setDebugFlyCamTransform(keyframe.center, keyframe.eye, keyframe.fovy,
                                              keyframe.bank);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                sCameraKeyframes.erase(sCameraKeyframes.begin() + i);
                ImGui::PopID();
                continue;
            }
            ImGui::PopID();
            ++i;
        }

        ShowCornerContextMenu(m_cameraOverlayCorner, 0);

        ImGui::End();
    }
}
