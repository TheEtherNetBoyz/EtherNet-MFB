#include "f_op/f_op_camera_mng.h"
#include "SSystem/SComponent/c_xyz.h"
#include "d/d_com_inf_game.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ImGuiConfig.hpp"
#include "ImGuiConsole.hpp"
#include "ImGuiMenuTools.hpp"
#include "dusk/io.hpp"
#include "dusk/main.h"
#include "dusk/rupee_slide_tools.hpp"
#include "dusk/settings.h"

#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using json = nlohmann::json;

struct CameraKeyframe {
    cXyz center;
    cXyz eye;
    f32 fovy;
    s16 bank;
    float duration = 2.0f;
};

std::vector<CameraKeyframe> sCameraKeyframes;
struct SavedCameraPath {
    std::string name;
    std::vector<CameraKeyframe> keyframes;
    bool loop = false;
    bool ease = true;
};

constexpr int kCameraPathsVersion = 1;
constexpr auto kCameraPathsFilename = "camera_paths.json";
std::vector<SavedCameraPath> sSavedCameraPaths;
bool sCameraPathsLoaded = false;
std::string sCameraPathStatus;
bool sCameraPlaybackActive = false;
bool sCameraPlaybackPaused = false;
bool sCameraPlaybackLoop = false;
bool sCameraPlaybackEase = true;
bool sMouseLookBeforePlayback = true;
size_t sCameraPlaybackSegment = 0;
float sCameraPlaybackSegmentTime = 0.0f;

std::filesystem::path CameraPathsFilePath() {
    return dusk::ConfigPath / kCameraPathsFilename;
}

json KeyframeToJson(const CameraKeyframe& keyframe) {
    return {
        {"center", {keyframe.center.x, keyframe.center.y, keyframe.center.z}},
        {"eye", {keyframe.eye.x, keyframe.eye.y, keyframe.eye.z}},
        {"fovy", keyframe.fovy},
        {"bank", keyframe.bank},
        {"duration", keyframe.duration},
    };
}

bool KeyframeFromJson(const json& value, CameraKeyframe& keyframe) {
    if (!value.is_object() || !value.contains("center") || !value.contains("eye") ||
        !value["center"].is_array() || value["center"].size() != 3 ||
        !value["eye"].is_array() || value["eye"].size() != 3)
    {
        return false;
    }

    try {
        keyframe.center.set(
            value["center"][0].get<float>(),
            value["center"][1].get<float>(),
            value["center"][2].get<float>());
        keyframe.eye.set(
            value["eye"][0].get<float>(),
            value["eye"][1].get<float>(),
            value["eye"][2].get<float>());
        keyframe.fovy = std::clamp(value.value("fovy", 60.0f), 0.1f, 179.9f);
        keyframe.bank = static_cast<s16>(value.value("bank", 0));
        keyframe.duration = std::clamp(value.value("duration", 2.0f), 0.01f, 600.0f);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

json CameraPathToJson(const SavedCameraPath& path) {
    json keyframes = json::array();
    for (const auto& keyframe : path.keyframes) {
        keyframes.push_back(KeyframeToJson(keyframe));
    }
    return {
        {"name", path.name},
        {"loop", path.loop},
        {"ease", path.ease},
        {"keyframes", std::move(keyframes)},
    };
}

bool CameraPathFromJson(const json& value, SavedCameraPath& path) {
    if (!value.is_object() || !value.contains("keyframes") ||
        !value["keyframes"].is_array())
    {
        return false;
    }

    try {
        path.name = value.value("name", "Imported Camera Path");
        path.loop = value.value("loop", false);
        path.ease = value.value("ease", true);
        path.keyframes.clear();
        for (const auto& item : value["keyframes"]) {
            CameraKeyframe keyframe;
            if (!KeyframeFromJson(item, keyframe)) {
                return false;
            }
            path.keyframes.push_back(keyframe);
        }
        return path.keyframes.size() >= 2;
    } catch (const json::exception&) {
        return false;
    }
}

void SaveCameraPathsFile() {
    json paths = json::array();
    for (const auto& path : sSavedCameraPaths) {
        paths.push_back(CameraPathToJson(path));
    }
    const json root = {
        {"version", kCameraPathsVersion},
        {"paths", std::move(paths)},
    };

    try {
        dusk::io::FileStream::WriteAllText(CameraPathsFilePath(), root.dump(2));
    } catch (const std::exception& e) {
        sCameraPathStatus = fmt::format("Failed to save camera paths: {}", e.what());
    }
}

void LoadCameraPathsFile() {
    sCameraPathsLoaded = true;
    const auto filePath = CameraPathsFilePath();
    if (!std::filesystem::exists(filePath)) {
        return;
    }

    try {
        const auto bytes = dusk::io::FileStream::ReadAllBytes(filePath);
        const auto root = json::parse(bytes);
        if (!root.is_object() || root.value("version", 0) != kCameraPathsVersion ||
            !root.contains("paths") || !root["paths"].is_array())
        {
            sCameraPathStatus = "Camera paths file has an unsupported format.";
            return;
        }

        sSavedCameraPaths.clear();
        for (const auto& value : root["paths"]) {
            SavedCameraPath path;
            if (CameraPathFromJson(value, path)) {
                sSavedCameraPaths.push_back(std::move(path));
            }
        }
    } catch (const std::exception& e) {
        sCameraPathStatus = fmt::format("Failed to load camera paths: {}", e.what());
    }
}

void CaptureCameraKeyframe(dCamera_c* camera) {
    if (camera == nullptr || !dusk::getSettings().game.debugFlyCam ||
        sCameraPlaybackActive)
    {
        return;
    }
    sCameraKeyframes.push_back({
        camera->mCenter, camera->mEye, camera->mFovy, camera->mBank.Val(), 2.0f,
    });
    sCameraPathStatus = fmt::format("Captured keyframe {}.", sCameraKeyframes.size());
    dusk::DuskToast(sCameraPathStatus);
}

cXyz Lerp(const cXyz& from, const cXyz& to, float t) {
    return from + (to - from) * t;
}

s16 LerpBank(s16 from, s16 to, float t) {
    const s16 shortestDifference = static_cast<s16>(to - from);
    return static_cast<s16>(from + static_cast<float>(shortestDifference) * t);
}

void StopCameraPlayback(bool restoreMouseLook = true) {
    if (sCameraPlaybackActive && restoreMouseLook) {
        dCamera_c::setDebugFlyCamMouseLookEnabled(sMouseLookBeforePlayback);
    }
    sCameraPlaybackActive = false;
    sCameraPlaybackPaused = false;
    sCameraPlaybackSegment = 0;
    sCameraPlaybackSegmentTime = 0.0f;
}

void StartCameraPlayback(dCamera_c* camera) {
    if (camera == nullptr || sCameraKeyframes.size() < 2) {
        return;
    }

    sMouseLookBeforePlayback = dCamera_c::isDebugFlyCamMouseLookEnabled();
    dCamera_c::setDebugFlyCamMouseLookEnabled(false);
    sCameraPlaybackActive = true;
    sCameraPlaybackPaused = false;
    sCameraPlaybackSegment = 0;
    sCameraPlaybackSegmentTime = 0.0f;

    const auto& first = sCameraKeyframes.front();
    camera->setDebugFlyCamTransform(first.center, first.eye, first.fovy, first.bank);
}

void UpdateCameraPlayback(dCamera_c* camera) {
    if (!sCameraPlaybackActive) {
        return;
    }
    if (camera == nullptr || sCameraKeyframes.size() < 2 ||
        !dusk::getSettings().game.debugFlyCam)
    {
        StopCameraPlayback();
        return;
    }

    // Playback owns the transform. Keep mouse look suppressed even if P is
    // pressed while the sequence is running.
    dCamera_c::setDebugFlyCamMouseLookEnabled(false);
    if (sCameraPlaybackPaused) {
        return;
    }

    sCameraPlaybackSegmentTime += std::max(ImGui::GetIO().DeltaTime, 0.0f);
    while (sCameraPlaybackActive) {
        size_t next = sCameraPlaybackSegment + 1;
        if (next >= sCameraKeyframes.size() && !sCameraPlaybackLoop) {
            const auto& last = sCameraKeyframes.back();
            camera->setDebugFlyCamTransform(last.center, last.eye, last.fovy, last.bank);
            StopCameraPlayback();
            return;
        }
        if (next >= sCameraKeyframes.size()) {
            next = 0;
        }

        const float duration = std::max(sCameraKeyframes[sCameraPlaybackSegment].duration, 0.01f);
        if (sCameraPlaybackSegmentTime < duration) {
            break;
        }
        sCameraPlaybackSegmentTime -= duration;
        sCameraPlaybackSegment = next;
    }

    size_t next = sCameraPlaybackSegment + 1;
    if (next >= sCameraKeyframes.size() && !sCameraPlaybackLoop) {
        return;
    }
    if (next >= sCameraKeyframes.size()) {
        next = 0;
    }

    const auto& from = sCameraKeyframes[sCameraPlaybackSegment];
    const auto& to = sCameraKeyframes[next];
    float t = std::clamp(sCameraPlaybackSegmentTime / std::max(from.duration, 0.01f),
                         0.0f, 1.0f);
    if (sCameraPlaybackEase) {
        t = t * t * (3.0f - 2.0f * t);
    }
    camera->setDebugFlyCamTransform(
        Lerp(from.center, to.center, t),
        Lerp(from.eye, to.eye, t),
        from.fovy + (to.fovy - from.fovy) * t,
        LerpBank(from.bank, to.bank, t));
}
}

namespace dusk {
    void ImGuiMenuTools::ShowCameraOverlay() {
        UpdateRupeeSlideTools();

        auto* cam = (camera_process_class*)dCam_getCamera();
        auto* dCam = cam != nullptr ? &cam->mCamera : nullptr;
        UpdateCameraPlayback(dCam);

        static bool captureHotkeyToggle = false;
        ImGuiConsole::CheckMenuViewToggle(
            getSettings().hotkeys.captureCameraKeyframe, captureHotkeyToggle);
        if (captureHotkeyToggle) {
            CaptureCameraKeyframe(dCam);
            captureHotkeyToggle = false;
        }

        if (!getSettings().backend.enableAdvancedSettings ||
            !ImGuiConsole::CheckMenuViewToggle(getSettings().hotkeys.debugCamera, m_showCameraOverlay) ||
            !m_showCameraOverlay || dCam == nullptr)
        {
            return;
        }

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
            CaptureCameraKeyframe(dCam);
        }
        if (!getSettings().game.debugFlyCam) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear") && !sCameraKeyframes.empty()) {
            StopCameraPlayback();
            sCameraKeyframes.clear();
        }

        const bool canPlay = getSettings().game.debugFlyCam && sCameraKeyframes.size() >= 2;
        if (!canPlay) {
            ImGui::BeginDisabled();
        }
        if (!sCameraPlaybackActive) {
            if (ImGui::Button("Play")) {
                StartCameraPlayback(dCam);
            }
        } else {
            if (ImGui::Button(sCameraPlaybackPaused ? "Resume" : "Pause")) {
                sCameraPlaybackPaused = !sCameraPlaybackPaused;
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                StopCameraPlayback();
            }
        }
        if (!canPlay) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &sCameraPlaybackLoop);
        ImGui::SameLine();
        ImGui::Checkbox("Ease", &sCameraPlaybackEase);

        if (sCameraPlaybackActive) {
            ImGui::Text("Playing segment %zu / %zu%s", sCameraPlaybackSegment + 1,
                        sCameraPlaybackLoop ? sCameraKeyframes.size()
                                            : sCameraKeyframes.size() - 1,
                        sCameraPlaybackPaused ? " (paused)" : "");
        } else if (sCameraKeyframes.size() < 2) {
            ImGui::TextDisabled("Capture at least two keyframes to play.");
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
                StopCameraPlayback();
                sCameraKeyframes.erase(sCameraKeyframes.begin() + i);
                ImGui::PopID();
                continue;
            }
            if (i + 1 < sCameraKeyframes.size() || sCameraPlaybackLoop) {
                ImGui::SetNextItemWidth(110.0f);
                ImGui::DragFloat("Seconds to next", &sCameraKeyframes[i].duration,
                                 0.05f, 0.01f, 600.0f, "%.2f s");
            }
            ImGui::PopID();
            ++i;
        }

        ImGui::SeparatorText("Saved Paths");
        if (!sCameraPathsLoaded) {
            LoadCameraPathsFile();
        }

        if (sCameraKeyframes.size() < 2 || sCameraPlaybackActive) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save Current Path")) {
            SavedCameraPath path;
            path.name = fmt::format("Camera Path {}", sSavedCameraPaths.size() + 1);
            path.keyframes = sCameraKeyframes;
            path.loop = sCameraPlaybackLoop;
            path.ease = sCameraPlaybackEase;
            sSavedCameraPaths.push_back(std::move(path));
            SaveCameraPathsFile();
            sCameraPathStatus = fmt::format("Saved '{}'.", sSavedCameraPaths.back().name);
        }
        if (sCameraKeyframes.size() < 2 || sCameraPlaybackActive) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (sCameraPlaybackActive) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Import Clipboard")) {
            const char* clipboard = ImGui::GetClipboardText();
            if (clipboard == nullptr || clipboard[0] == '\0') {
                sCameraPathStatus = "Clipboard is empty.";
            } else {
                try {
                    SavedCameraPath path;
                    if (!CameraPathFromJson(json::parse(clipboard), path)) {
                        sCameraPathStatus = "Clipboard does not contain a valid camera path.";
                    } else {
                        if (path.name.empty()) {
                            path.name = fmt::format("Imported Camera Path {}", sSavedCameraPaths.size() + 1);
                        }
                        sSavedCameraPaths.push_back(std::move(path));
                        SaveCameraPathsFile();
                        sCameraPathStatus =
                            fmt::format("Imported '{}'.", sSavedCameraPaths.back().name);
                    }
                } catch (const json::exception&) {
                    sCameraPathStatus = "Clipboard does not contain valid JSON.";
                }
            }
        }
        if (sCameraPlaybackActive) {
            ImGui::EndDisabled();
        }

        int pathToDelete = -1;
        for (int i = 0; i < static_cast<int>(sSavedCameraPaths.size()); ++i) {
            auto& path = sSavedCameraPaths[i];
            ImGui::PushID(10000 + i);
            ImGui::SetNextItemWidth(170.0f);
            if (ImGui::InputText("##pathName", &path.name,
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                SaveCameraPathsFile();
            }
            ImGui::SameLine();
            if (sCameraPlaybackActive) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("Load")) {
                StopCameraPlayback();
                sCameraKeyframes = path.keyframes;
                sCameraPlaybackLoop = path.loop;
                sCameraPlaybackEase = path.ease;
                sCameraPathStatus = fmt::format("Loaded '{}'.", path.name);
            }
            if (sCameraPlaybackActive) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                const std::string encoded = CameraPathToJson(path).dump();
                ImGui::SetClipboardText(encoded.c_str());
                sCameraPathStatus = fmt::format("Copied '{}'.", path.name);
            }
            ImGui::SameLine();
            if (sCameraPlaybackActive) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("Delete")) {
                pathToDelete = i;
            }
            if (sCameraPlaybackActive) {
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        if (pathToDelete >= 0) {
            sCameraPathStatus =
                fmt::format("Deleted '{}'.", sSavedCameraPaths[pathToDelete].name);
            sSavedCameraPaths.erase(sSavedCameraPaths.begin() + pathToDelete);
            SaveCameraPathsFile();
        }

        if (!sCameraPathStatus.empty()) {
            ImGui::TextWrapped("%s", sCameraPathStatus.c_str());
        }

        ShowCornerContextMenu(m_cameraOverlayCorner, 0);

        ImGui::End();
    }
}
