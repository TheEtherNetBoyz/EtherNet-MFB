#include "ImGuiMenuTools.hpp"

#include "SSystem/SComponent/c_counter.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "dusk/input_macro.h"
#include "dusk/io.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/tas_movie.h"
#include "f_op/f_op_camera_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_create_tag.h"

#include "absl/strings/escaping.h"
#include "fmt/format.h"
#include "imgui.h"
#include "nlohmann/json.hpp"
#include <SDL3/SDL_keyboard.h>

#include <filesystem>
#include <string>
#include <vector>

namespace dusk {
namespace {

using json = nlohmann::json;

struct SavedTasMovie {
    std::string name;
    std::string encoded;
};

constexpr auto kTasMoviesFilename = "tas_movies.json";
std::vector<SavedTasMovie> sSavedMovies;
std::string sStatus;
bool sLoaded = false;
int sSelectedState = 0;
int sSeekFrame = 0;
bool sPauseHotkeyWasDown = false;
bool sFrameAdvanceHotkeyWasDown = false;
bool sAnchorReadyCandidate = false;
u64 sAnchorReadyCreationEpoch = 0;
u32 sAnchorReadySimulationFrame = 0;

std::filesystem::path TasMoviesFilePath() {
    return ConfigPath / kTasMoviesFilename;
}

bool decodeMovie(const std::string& encoded, std::string& decoded) {
    return absl::Base64Unescape(encoded, &decoded) &&
           tas_movie::validateSerialized(decoded);
}

void saveMoviesFile() {
    json movies = json::array();
    for (const auto& movie : sSavedMovies) {
        movies.push_back(json{{"name", movie.name}, {"data", movie.encoded}});
    }
    try {
        io::FileStream::WriteAllText(TasMoviesFilePath(), movies.dump(2));
    } catch (const std::exception& e) {
        sStatus = fmt::format("Failed to save TAS movies: {}", e.what());
    }
}

void loadMoviesFile() {
    sLoaded = true;
    const auto path = TasMoviesFilePath();
    if (!std::filesystem::exists(path)) {
        return;
    }

    try {
        const auto bytes = io::FileStream::ReadAllBytes(path);
        const auto root = json::parse(bytes);
        if (!root.is_array()) {
            return;
        }
        for (const auto& value : root) {
            if (!value.contains("name") || !value.contains("data")) {
                continue;
            }
            SavedTasMovie movie{
                value["name"].get<std::string>(),
                value["data"].get<std::string>(),
            };
            std::string decoded;
            if (decodeMovie(movie.encoded, decoded)) {
                sSavedMovies.push_back(std::move(movie));
            }
        }
    } catch (const std::exception& e) {
        sStatus = fmt::format("Failed to load TAS movies: {}", e.what());
    }
}

}  // namespace

void ImGuiMenuTools::UpdateTasMovie() {
    m_stateShare.updateRuntime();
    tas_movie::updatePresentationCameraControls(std::max(ImGui::GetIO().DeltaTime, 0.0f));

    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    const bool pauseDown = keyCount > SDL_SCANCODE_L && keys[SDL_SCANCODE_L];
    const bool frameAdvanceDown = keyCount > SDL_SCANCODE_PERIOD && keys[SDL_SCANCODE_PERIOD];
    const bool movieRunning =
        tas_movie::state() == tas_movie::State::Recording ||
        tas_movie::state() == tas_movie::State::Playing;
    if (!ImGui::GetIO().WantTextInput && movieRunning) {
        if (pauseDown && !sPauseHotkeyWasDown) {
            tas_movie::setPaused(!tas_movie::paused());
        }
        if (frameAdvanceDown && !sFrameAdvanceHotkeyWasDown && tas_movie::paused()) {
            tas_movie::requestFrameAdvance();
        }
    }
    sPauseHotkeyWasDown = pauseDown;
    sFrameAdvanceHotkeyWasDown = frameAdvanceDown;

    const bool anchorCanSettle =
        tas_movie::waitingForAnchor() &&
        !m_stateShare.loadInProgress() &&
        !dComIfGp_isEnableNextStage() &&
        !fopOvlpM_IsDoingReq() &&
        daPy_getPlayerActorClass() != nullptr &&
        g_fpcCtTg_Queue.mSize == 0;
    if (!anchorCanSettle) {
        sAnchorReadyCandidate = false;
    } else if (
        !sAnchorReadyCandidate ||
        sAnchorReadyCreationEpoch != g_fpcCtTg_ActivityEpoch)
    {
        // An empty queue is not sufficient by itself: a complete actor batch
        // can be added and drained between two UI presentations. Snapshot both
        // creation activity and the simulation counter, then require a whole
        // simulation boundary with neither changing.
        sAnchorReadyCandidate = true;
        sAnchorReadyCreationEpoch = g_fpcCtTg_ActivityEpoch;
        sAnchorReadySimulationFrame = g_Counter.mCounter0;
    } else if (sAnchorReadySimulationFrame != g_Counter.mCounter0) {
        DuskLog.debug(
            "TAS anchor settled at simulation frame {} (creation epoch {})",
            g_Counter.mCounter0, g_fpcCtTg_ActivityEpoch);
        tas_movie::notifyAnchorReady();
        sAnchorReadyCandidate = false;
        sStatus = tas_movie::state() == tas_movie::State::Recording
                      ? "Anchor settled. Recording began on this simulation frame."
                      : "Anchor settled. Playback began on this simulation frame.";
    }
}

void ImGuiMenuTools::ShowTasMovie() {
    if (!m_showTasMovie) {
        return;
    }

    if (!sLoaded) {
        loadMoviesFile();
    }

    if (!ImGui::Begin("TAS Movie", &m_showTasMovie)) {
        ImGui::End();
        return;
    }

    const bool tasActive = tas_movie::active();
    const bool macroActive = input_macro::state() != input_macro::State::Idle;
    const auto& states = m_stateShare.savedStates();
    if (sSelectedState >= static_cast<int>(states.size())) {
        sSelectedState = 0;
    }

    ImGui::Text("State: %s", tas_movie::stateName());
    ImGui::Text("Frames: %zu", tas_movie::recordedFrames());
    if (tas_movie::state() == tas_movie::State::Playing) {
        ImGui::Text("Playback frame: %zu", tas_movie::playbackFrame());
    }
    if (tas_movie::rngCallDiverged()) {
        const auto& expected = tas_movie::expectedRngCalls();
        const auto& actual = tas_movie::actualRngCalls();
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
            "RNG call-order divergence at frame %zu", tas_movie::rngDivergenceFrame());
        ImGui::TextDisabled(
            "Expected P:%llu S:%llu, got P:%llu S:%llu",
            static_cast<unsigned long long>(expected.primary),
            static_cast<unsigned long long>(expected.secondary),
            static_cast<unsigned long long>(actual.primary),
            static_cast<unsigned long long>(actual.secondary));
    }

    ImGui::SeparatorText("Dusk State Anchor");
    if (states.empty()) {
        ImGui::TextDisabled("No Dusk states exist. Create one in State Manager first.");
    } else {
        const char* preview = states[sSelectedState].name.c_str();
        if (ImGui::BeginCombo("Start state", preview)) {
            for (int i = 0; i < static_cast<int>(states.size()); ++i) {
                if (ImGui::Selectable(states[i].name.c_str(), i == sSelectedState)) {
                    sSelectedState = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("The selected state is embedded in the movie.");
    }

    if (macroActive || tasActive || states.empty()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Record From State")) {
        const auto& selected = states[sSelectedState];
        if (tas_movie::armRecording(selected.encoded) &&
            m_stateShare.beginApplyEncodedState(selected.encoded,
                                                fmt::format("TAS: {}", selected.name)))
        {
            sStatus = "Loading the embedded state before recording...";
        } else {
            tas_movie::cancelAnchorLoad();
            sStatus = "Could not start the TAS state load.";
        }
    }
    if (macroActive || tasActive || states.empty()) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    const bool canPlay = tas_movie::hasMovie() && !tasActive && !macroActive;
    if (!canPlay) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Play From Start")) {
        if (tas_movie::armPlayback() &&
            m_stateShare.beginApplyEncodedState(tas_movie::anchor(), "TAS playback"))
        {
            sStatus = "Loading the embedded state; RNG will restore at new-scene creation...";
        } else {
            tas_movie::cancelAnchorLoad();
            sStatus = "Could not start TAS playback.";
        }
    }
    if (!canPlay) {
        ImGui::EndDisabled();
    }

    if (tasActive) {
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            tas_movie::stop();
            sStatus = "TAS stopped.";
        }
    }

    if (!canPlay) {
        ImGui::BeginDisabled();
    }
    sSeekFrame = std::clamp(
        sSeekFrame, 0, std::max(static_cast<int>(tas_movie::recordedFrames()) - 1, 0));
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Seek frame", &sSeekFrame);
    ImGui::SameLine();
    if (ImGui::Button("Replay Start to Frame")) {
        if (tas_movie::armPlaybackToFrame(static_cast<size_t>(sSeekFrame)) &&
            m_stateShare.beginApplyEncodedState(tas_movie::anchor(), "TAS seek"))
        {
            sStatus = fmt::format(
                "Reloading the anchor and fast-forwarding to frame {}...", sSeekFrame);
        } else {
            tas_movie::cancelAnchorLoad();
            sStatus = "Could not start TAS seek.";
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Reload the embedded Dusk state, then simulate every recorded frame up to this one.");
    }
    if (!canPlay) {
        ImGui::EndDisabled();
    }

    if (macroActive) {
        ImGui::TextDisabled("Stop the current Input Macro before using TAS Movies.");
    }

    const bool running =
        tas_movie::state() == tas_movie::State::Recording ||
        tas_movie::state() == tas_movie::State::Playing;
    if (!running) {
        ImGui::BeginDisabled();
    }
    bool paused = tas_movie::paused();
    if (ImGui::Checkbox("Pause simulation", &paused)) {
        tas_movie::setPaused(paused);
    }
    ImGui::SameLine();
    if (!paused) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Advance 1 Frame")) {
        tas_movie::requestFrameAdvance();
    }
    if (!paused) {
        ImGui::EndDisabled();
    }
    ImGui::TextDisabled("L toggles simulation. Period advances one frame.");
    bool turbo = tas_movie::turbo();
    if (ImGui::Checkbox("Turbo playback/recording", &turbo)) {
        tas_movie::setTurbo(turbo);
    }
    float simulationRate = tas_movie::simulationRate();
    if (ImGui::SliderFloat("Viewing tick rate", &simulationRate, 1.0f, 120.0f, "%.0f Hz")) {
        tas_movie::setSimulationRate(simulationRate);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runtime viewing speed only. This is not saved in the movie and does not change recorded frames.");
    }
    if (tas_movie::state() != tas_movie::State::Playing || !paused) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Rerecord From This Frame")) {
        if (tas_movie::branchRecordingFromPlayback()) {
            sStatus = "Branched the movie here. Unpause to record replacement input.";
        }
    }
    if (tas_movie::state() != tas_movie::State::Playing || !paused) {
        ImGui::EndDisabled();
    }
    if (!running) {
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Saved Movies");
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##tasmovies", ImVec2(0.0f, rowHeight * 5.0f), true);
    if (sSavedMovies.empty()) {
        ImGui::TextDisabled("No saved TAS movies.");
    }
    int deleteMovie = -1;
    for (int i = 0; i < static_cast<int>(sSavedMovies.size()); ++i) {
        ImGui::PushID(i);
        ImGui::Selectable(
            sSavedMovies[i].name.c_str(), false, ImGuiSelectableFlags_None, ImVec2(160.0f, 0.0f));
        ImGui::SameLine();
        if (tasActive) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Load")) {
            std::string decoded;
            if (decodeMovie(sSavedMovies[i].encoded, decoded) &&
                tas_movie::loadSerialized(decoded))
            {
                sStatus = fmt::format("Loaded '{}'.", sSavedMovies[i].name);
            } else {
                sStatus = fmt::format("'{}' is not a valid TAS movie.", sSavedMovies[i].name);
            }
        }
        if (tasActive) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(sSavedMovies[i].encoded.c_str());
            sStatus = fmt::format("Copied '{}'.", sSavedMovies[i].name);
        }
        ImGui::SameLine();
        if (tasActive) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Del")) {
            deleteMovie = i;
        }
        if (tasActive) {
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    if (deleteMovie >= 0) {
        sStatus = fmt::format("Deleted '{}'.", sSavedMovies[deleteMovie].name);
        sSavedMovies.erase(sSavedMovies.begin() + deleteMovie);
        saveMoviesFile();
    }
    ImGui::EndChild();

    if (!tas_movie::hasMovie() || tasActive) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save Movie")) {
        const std::string data = tas_movie::serialize();
        if (!data.empty()) {
            SavedTasMovie movie{
                fmt::format("TAS Movie {}", sSavedMovies.size() + 1),
                absl::Base64Escape(data),
            };
            sSavedMovies.push_back(std::move(movie));
            saveMoviesFile();
            sStatus = fmt::format("Saved '{}'.", sSavedMovies.back().name);
        }
    }
    if (!tas_movie::hasMovie() || tasActive) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (tasActive) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Import Clipboard")) {
        const char* clipboard = ImGui::GetClipboardText();
        std::string decoded;
        if (clipboard == nullptr || !decodeMovie(clipboard, decoded)) {
            sStatus = "Clipboard does not contain a valid TAS movie.";
        } else {
            sSavedMovies.push_back({
                fmt::format("Imported TAS {}", sSavedMovies.size() + 1),
                clipboard,
            });
            saveMoviesFile();
            sStatus = fmt::format("Imported '{}'.", sSavedMovies.back().name);
        }
    }
    if (tasActive) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (tasActive || !tas_movie::hasMovie()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Clear Loaded")) {
        tas_movie::clear();
        sStatus = "Cleared the loaded TAS movie.";
    }
    if (tasActive || !tas_movie::hasMovie()) {
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Detached Presentation Camera");
    bool presentationEnabled = tas_movie::presentationCameraEnabled();
    if (ImGui::Checkbox("Enable render-only camera", &presentationEnabled)) {
        tas_movie::setPresentationCameraEnabled(presentationEnabled);
        if (presentationEnabled) {
            auto* camera = dComIfGp_getCamera(0);
            if (camera != nullptr) {
                tas_movie::copyPresentationCameraFromView(&camera->view);
            }
        }
    }
    if (!presentationEnabled) {
        ImGui::BeginDisabled();
    }
    bool controlsEnabled = tas_movie::presentationCameraControlEnabled();
    if (ImGui::Checkbox("Control detached camera", &controlsEnabled)) {
        tas_movie::setPresentationCameraControlEnabled(controlsEnabled);
    }
    bool dualCulling = tas_movie::presentationCameraDualCullingEnabled();
    if (ImGui::Checkbox("Keep gameplay-camera actors visible", &dualCulling)) {
        tas_movie::setPresentationCameraDualCullingEnabled(dualCulling);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Render-only dual-frustum culling. Gameplay, audio, and room streaming remain attached to Link.");
    }
    float moveSpeed = tas_movie::presentationCameraMoveSpeed();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::DragFloat("Move speed", &moveSpeed, 10.0f, 1.0f, 10000.0f, "%.0f")) {
        tas_movie::setPresentationCameraMoveSpeed(moveSpeed);
    }
    float fov = tas_movie::presentationCameraFov();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderFloat("FOV", &fov, 1.0f, 179.0f, "%.1f")) {
        tas_movie::setPresentationCameraFov(fov);
    }
    float bank = tas_movie::presentationCameraBankDegrees();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderFloat("Bank", &bank, -180.0f, 180.0f, "%.1f deg")) {
        tas_movie::setPresentationCameraBankDegrees(bank);
    }
    ImGui::TextDisabled("Standalone camera: regular Fly Mode should remain off.");
    ImGui::TextDisabled("WASD move, Space/Ctrl vertical, Shift fast, Q/E roll.");
    ImGui::TextDisabled("Right-click+mouse looks. K captures a keyframe.");
    if (ImGui::Button("Copy Gameplay Camera")) {
        auto* camera = dComIfGp_getCamera(0);
        if (camera != nullptr) {
            tas_movie::copyPresentationCameraFromView(&camera->view);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture Camera Keyframe")) {
        tas_movie::captureCameraKeyframe();
        sStatus = fmt::format("Captured camera keyframe at frame {}.",
                              tas_movie::state() == tas_movie::State::Playing
                                  ? tas_movie::playbackFrame()
                                  : tas_movie::recordedFrames());
    }
    if (!presentationEnabled) {
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Camera Track");
    const bool canPlayCamera =
        presentationEnabled && tas_movie::cameraKeyframeCount() >= 2;
    if (!canPlayCamera) {
        ImGui::BeginDisabled();
    }
    if (!tas_movie::cameraTrackPlaying()) {
        if (ImGui::Button("Play Camera Track")) {
            tas_movie::playCameraTrack();
        }
    } else {
        if (ImGui::Button(tas_movie::cameraTrackPaused() ? "Resume Camera" : "Pause Camera")) {
            tas_movie::pauseCameraTrack(!tas_movie::cameraTrackPaused());
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop Camera")) {
            tas_movie::stopCameraTrack();
        }
    }
    if (!canPlayCamera) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    bool cameraLoop = tas_movie::cameraTrackLoop();
    if (ImGui::Checkbox("Loop##camera", &cameraLoop)) {
        tas_movie::setCameraTrackLoop(cameraLoop);
    }
    ImGui::SameLine();
    bool cameraEase = tas_movie::cameraTrackEase();
    if (ImGui::Checkbox("Ease##camera", &cameraEase)) {
        tas_movie::setCameraTrackEase(cameraEase);
    }
    if (tas_movie::cameraKeyframeCount() < 2) {
        ImGui::TextDisabled("Capture at least two keyframes to play the camera track.");
    } else {
        ImGui::TextDisabled(
            "Preview plays at 30 movie frames/second; TAS playback follows recorded frame numbers.");
    }

    int editKeyframe = -1;
    uint32_t editedFrame = 0;
    int deleteKeyframe = -1;
    for (size_t i = 0; i < tas_movie::cameraKeyframeCount(); ++i) {
        const auto* keyframe = tas_movie::cameraKeyframe(i);
        if (keyframe == nullptr) {
            continue;
        }
        ImGui::PushID(static_cast<int>(i) + 10000);
        int frame = static_cast<int>(keyframe->frame);
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("Frame", &frame);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Committing each typed digit can reorder the keyframe list while
            // this loop is using it. Keep the edit local until focus leaves.
            editKeyframe = static_cast<int>(i);
            editedFrame = static_cast<uint32_t>(std::max(frame, 0));
        }
        ImGui::SameLine();
        if (ImGui::Button("Go")) {
            tas_movie::goToCameraKeyframe(i);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            deleteKeyframe = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (deleteKeyframe >= 0) {
        tas_movie::deleteCameraKeyframe(static_cast<size_t>(deleteKeyframe));
        if (editKeyframe == deleteKeyframe) {
            editKeyframe = -1;
        } else if (editKeyframe > deleteKeyframe) {
            --editKeyframe;
        }
    }
    if (editKeyframe >= 0) {
        tas_movie::setCameraKeyframeFrame(
            static_cast<size_t>(editKeyframe), editedFrame);
    }
    if (tas_movie::cameraKeyframeCount() > 0) {
        if (ImGui::Button("Clear Camera Track")) {
            tas_movie::clearCameraKeyframes();
        }
    }

    if (!sStatus.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", sStatus.c_str());
    }

    ImGui::End();
}

}  // namespace dusk
