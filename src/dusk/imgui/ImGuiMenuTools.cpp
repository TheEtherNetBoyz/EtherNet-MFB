#include "fmt/format.h"
#include "imgui.h"
#include "aurora/gfx.h"

#include "ImGuiConfig.hpp"
#include "dusk/audio/DuskAudioSystem.h"
#include "dusk/audio/DuskDsp.hpp"
#include "dusk/hotkeys.h"
#include "dusk/settings.h"
#include "ImGuiConsole.hpp"
#include "ImGuiMenuTools.hpp"

#include "ImGuiEngine.hpp"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "d/d_com_inf_game.h"
#include "dusk/data.hpp"
#include "dusk/dusk.h"
#include "dusk/livesplit.h"
#include "dusk/main.h"
#include "dusk/ui/menu_bar.hpp"
#include "dusk/ui/ui.hpp"
#include "dusk/vector_rsqrt.h"
#include "m_Do/m_Do_main.h"

#include <aurora/lib/internal.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_misc.h>
#include <algorithm>
#include <cstdint>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace aurora::gx {
extern bool enableLodBias;
}

namespace dusk {
    ImGuiMenuTools::ImGuiMenuTools() {}

    namespace {
        bool MenuCheckbox(const char* label, ConfigVar<bool>& value, bool enabled = true) {
            return config::ImGuiMenuItem(label, nullptr, value, enabled);
        }
        void RefreshRmlMenuBar() {
            for (auto& doc : ui::get_document_stack()) {
                if (dynamic_cast<ui::MenuBar*>(doc.get())) {
                    doc = std::make_unique<ui::MenuBar>();
                    break;
                }
            }
        }

        void ClearSpeedrunOverrides() {
            config::EnumerateRegistered([](config::ConfigVarBase& cvar) {
                cvar.clearSpeedrunOverride();
            });
        }

        void ResetForSpeedrunMode() {
            auto& s = getSettings();
            mDoMain::developmentMode = -1;

            s.game.enableTurboKeybind.setSpeedrunValue(false);

            s.game.damageMultiplier.setSpeedrunValue(1);
            s.game.instantDeath.setSpeedrunValue(false);
            s.game.noHeartDrops.setSpeedrunValue(false);
            s.game.autoSave.setSpeedrunValue(false);
            s.game.sunsSong.setSpeedrunValue(false);

            s.game.infiniteHearts.setSpeedrunValue(false);
            s.game.infiniteArrows.setSpeedrunValue(false);
            s.game.infiniteSeeds.setSpeedrunValue(false);
            s.game.infiniteBombs.setSpeedrunValue(false);
            s.game.infiniteOil.setSpeedrunValue(false);
            s.game.infiniteOxygen.setSpeedrunValue(false);
            s.game.infiniteRupees.setSpeedrunValue(false);
            s.game.enableIndefiniteItemDrops.setSpeedrunValue(false);
            s.game.moonJump.setSpeedrunValue(false);
            s.game.superClawshot.setSpeedrunValue(false);
            s.game.alwaysGreatspin.setSpeedrunValue(false);
            s.game.enableFastIronBoots.setSpeedrunValue(false);
            s.game.canTransformAnywhere.setSpeedrunValue(false);
            s.game.fastRoll.setSpeedrunValue(false);
            s.game.fastSpinner.setSpeedrunValue(false);
            s.game.freeMagicArmor.setSpeedrunValue(false);
            s.game.invincibleEnemies.setSpeedrunValue(false);

            s.game.pauseOnFocusLost.setSpeedrunValue(false);
            aurora_set_pause_on_focus_lost(false);

            s.backend.enableAdvancedSettings.setSpeedrunValue(false);
            s.game.recordingMode.setSpeedrunValue(false);
            s.game.debugFlyCam.setSpeedrunValue(false);
            s.game.moveLink.setSpeedrunValue(false);
            getTransientSettings().moveLinkActive = false;
        }

        void RestoreFromSpeedrunMode() {
            ClearSpeedrunOverrides();
            aurora_set_pause_on_focus_lost(getSettings().game.pauseOnFocusLost.getValue());
        }

        bool SpeedrunModeCheckbox() {
            auto& s = getSettings();
            bool copy = s.game.speedrunMode.getValue();
            if (!ImGui::MenuItem("Speedrun Mode", nullptr, &copy)) {
                return false;
            }

            s.game.speedrunMode.setValue(copy);
            if (copy) {
                ResetForSpeedrunMode();
            } else {
                RestoreFromSpeedrunMode();
                if (s.game.liveSplitEnabled) {
                    speedrun::disconnectLiveSplit();
                }
            }
            RefreshRmlMenuBar();
            config::Save();
            return true;
        }

        bool LiveSplitCheckbox() {
            auto& s = getSettings();
            bool copy = s.game.liveSplitEnabled.getValue();
            if (!ImGui::MenuItem("LiveSplit", nullptr, &copy, !IsMobile && s.game.speedrunMode)) {
                return false;
            }

            s.game.liveSplitEnabled.setValue(copy);
            if (copy) {
                speedrun::connectLiveSplit();
            } else {
                speedrun::disconnectLiveSplit();
            }
            config::Save();
            return true;
        }

        void LoadModeCheckbox(const char* label, ConfigVar<bool>& value, ConfigVar<bool>& other) {
            bool copy = value.getValue();
            if (ImGui::MenuItem(label, nullptr, &copy)) {
                value.setValue(copy);
                if (copy) {
                    other.setValue(false);
                }
                config::Save();
            }
        }

        void SliderIntItem(const char* label, ConfigVar<int>& value, int min, int max) {
            int copy = std::clamp(value.getValue(), min, max);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt(label, &copy, min, max)) {
                value.setValue(std::clamp(copy, min, max));
                config::Save();
            }
        }

        void SliderFloatItem(const char* label, ConfigVar<float>& value, float min, float max,
                             const char* format = "%.2f", bool enabled = true) {
            float copy = value.getValue();
            ImGui::SetNextItemWidth(160.0f);
            if (!enabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SliderFloat(label, &copy, min, max, format)) {
                value.setValue(copy);
                config::Save();
            }
            if (!enabled) {
                ImGui::EndDisabled();
            }
        }

        void ApplyAspectRatioSettings() {
            switch (getSettings().video.forcedAspectRatio.getValue()) {
            case AspectRatioMode::Ratio16x9:
                AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
                AuroraSetForcedAspectRatio(16, 9);
                break;
            case AspectRatioMode::Ratio21x9:
                AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
                AuroraSetForcedAspectRatio(43, 18);
                break;
            case AspectRatioMode::Ratio3x2:
                AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
                AuroraSetForcedAspectRatio(3, 2);
                break;
            case AspectRatioMode::Off:
            default:
                AuroraSetForcedAspectRatio(0, 0);
                AuroraSetViewportPolicy(getSettings().video.lockAspectRatio.getValue() ?
                                            AURORA_VIEWPORT_FIT :
                                            AURORA_VIEWPORT_STRETCH);
                break;
            }
        }

        int AspectRatioModeIndex() {
            switch (getSettings().video.forcedAspectRatio.getValue()) {
            case AspectRatioMode::Ratio3x2:
                return 2;
            case AspectRatioMode::Ratio16x9:
                return 3;
            case AspectRatioMode::Ratio21x9:
                return 4;
            case AspectRatioMode::Off:
            default:
                return getSettings().video.lockAspectRatio.getValue() ? 1 : 0;
            }
        }

        void ForcedAspectRatioControl() {
            int copy = AspectRatioModeIndex();
            const char* items[] = {"Off", "4:3", "3:2", "16:9", "21:9"};
            ImGui::TextUnformatted("Force Aspect Ratio");
            ImGui::SameLine(170.0f);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("##ForceAspectRatio", &copy, items, IM_ARRAYSIZE(items))) {
                getSettings().video.lockAspectRatio.setValue(copy == 1);
                switch (copy) {
                case 2:
                    getSettings().video.forcedAspectRatio.setValue(AspectRatioMode::Ratio3x2);
                    break;
                case 3:
                    getSettings().video.forcedAspectRatio.setValue(AspectRatioMode::Ratio16x9);
                    break;
                case 4:
                    getSettings().video.forcedAspectRatio.setValue(AspectRatioMode::Ratio21x9);
                    break;
                default:
                    getSettings().video.forcedAspectRatio.setValue(AspectRatioMode::Off);
                    break;
                }
                ApplyAspectRatioSettings();
                config::Save();
            }
        }

        void InternalResolutionSlider() {
            auto& value = getSettings().game.internalResolutionScale;
            int copy = value.getValue();
            ImGui::TextUnformatted("Internal Resolution");
            ImGui::SameLine(170.0f);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderInt("##InternalResolution", &copy, 0, 12, copy == 0 ? "Auto" : "%dx")) {
                value.setValue(copy);
                VISetFrameBufferScale(static_cast<float>(copy));
                config::Save();
            }
        }

        void ShadowResolutionSlider() {
            auto& value = getSettings().game.shadowResolutionMultiplier;
            int copy = value.getValue();
            ImGui::TextUnformatted("Shadow Resolution");
            ImGui::SameLine(170.0f);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderInt("##ShadowResolution", &copy, 1, 8, "%dx")) {
                value.setValue(copy);
                config::Save();
            }
        }

        void BloomModeControl() {
            auto& value = getSettings().game.bloomMode;
            int copy = static_cast<int>(value.getValue());
            const char* items[] = {"Off", "Classic", "Dusk"};
            ImGui::TextUnformatted("Bloom Mode");
            ImGui::SameLine(170.0f);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("##BloomMode", &copy, items, IM_ARRAYSIZE(items))) {
                value.setValue(static_cast<BloomMode>(copy));
                config::Save();
            }
        }

        void BloomMultiplierSlider() {
            auto& value = getSettings().game.bloomMultiplier;
            float copy = value.getValue();
            ImGui::TextUnformatted("Bloom Multiplier");
            ImGui::SameLine(170.0f);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderFloat("##BloomMultiplier", &copy, 0.0f, 3.0f, "%.2f")) {
                value.setValue(copy);
                config::Save();
            }
        }

        constexpr int kFrameRateLimitValues[] = {30, 60, 120, 240, 360, 480, 0};
        constexpr const char* kFrameRateLimitNames[] = {
            "30 FPS", "60 FPS", "120 FPS", "240 FPS", "360 FPS", "480 FPS", "Unlocked",
        };

        int FrameRateLimitIndex() {
            auto& s = getSettings();
            if (!s.game.enableFrameInterpolation.getValue()) {
                return 0;
            }
            const int limit = s.game.frameRateLimit.getValue();
            for (int i = 1; i < IM_ARRAYSIZE(kFrameRateLimitValues); ++i) {
                if (kFrameRateLimitValues[i] == limit) {
                    return i;
                }
            }
            return IM_ARRAYSIZE(kFrameRateLimitValues) - 1;
        }

        void FrameRateLimitSlider() {
            auto& s = getSettings();
            int index = FrameRateLimitIndex();
            ImGui::TextUnformatted("Frame Rate Limit");
            ImGui::SameLine(170.0f);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderInt("##FrameRateLimit", &index, 0,
                    IM_ARRAYSIZE(kFrameRateLimitValues) - 1, kFrameRateLimitNames[index]))
            {
                s.game.enableFrameInterpolation.setValue(index != 0);
                s.game.frameRateLimit.setValue(index == 0 ? 0 : kFrameRateLimitValues[index]);
                config::Save();
            }
        }

        void DrawGameMenu() {
            auto& s = getSettings();
            if (ImGui::BeginMenu("General")) {
                MenuCheckbox("Mirror Mode", s.game.enableMirrorMode);
                MenuCheckbox("Minimal HUD", s.game.minimalHUD);
                MenuCheckbox("Achievement Notifications", s.game.enableAchievementToasts);
                MenuCheckbox("Controller Notifications", s.game.enableControllerToasts);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Quality of Life")) {
                MenuCheckbox("Quick Transform (R+Y)", s.game.enableQuickTransform);
                MenuCheckbox("Bigger Wallets", s.game.biggerWallets);
                MenuCheckbox("Disable Rupee Cutscenes", s.game.disableRupeeCutscenes);
                MenuCheckbox("Skip All Cutscenes", s.game.skipAllCutscenes);
                MenuCheckbox("No Rupee Returns", s.game.noReturnRupees);
                MenuCheckbox("No Sword Recoil", s.game.noSwordRecoil);
                MenuCheckbox("Faster Climbing", s.game.fastClimbing);
                MenuCheckbox("No Climbing Miss Animation", s.game.noMissClimbing);
                MenuCheckbox("Faster Tears of Light", s.game.fastTears);
                MenuCheckbox("No 2nd Fish for Cat", s.game.no2ndFishForCat);
                MenuCheckbox("Sun's Song (R+X)", s.game.sunsSong, !s.game.speedrunMode);
                ImGui::Separator();
                LoadModeCheckbox("Fast Loads", s.game.enableFastLoads, s.game.enableInstaLoads);
                LoadModeCheckbox("Insta Loads", s.game.enableInstaLoads, s.game.enableFastLoads);
                MenuCheckbox("Autosave", s.game.autoSave, !s.game.speedrunMode);
                MenuCheckbox("Instant Saves", s.game.instantSaves);
                MenuCheckbox("Hold B for Instant Text", s.game.instantText);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Difficulty")) {
                ImGui::BeginDisabled(s.game.speedrunMode);
                SliderIntItem("Damage Multiplier", s.game.damageMultiplier, 1, 8);
                MenuCheckbox("Instant Death", s.game.instantDeath);
                MenuCheckbox("No Heart Drops", s.game.noHeartDrops);
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Misc")) {
                MenuCheckbox("Restore Wii 1.0 Glitches", s.game.restoreWiiGlitches);
                if (MenuCheckbox("PPC Fast InvSqrt", s.game.usePpcFastInvSqrt)) {
                    dusk_set_native_vector_rsqrt(!s.game.usePpcFastInvSqrt.getValue());
                }
                MenuCheckbox("Rotating Link Doll", s.game.enableLinkDollRotation);
                MenuCheckbox("Hide TV Settings Screen", s.game.hideTvSettingsScreen);
                MenuCheckbox("Pause On Focus Lost", s.game.pauseOnFocusLost, !s.game.speedrunMode);
                MenuCheckbox("Recording Mode", s.game.recordingMode, !s.game.speedrunMode);
                MenuCheckbox("Skip Pre-Launch UI", s.backend.skipPreLaunchUI);
                ImGui::Separator();
                SpeedrunModeCheckbox();
                LiveSplitCheckbox();
                MenuCheckbox("Show RTA", s.game.showSpeedrunRTATimer, s.game.speedrunMode);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Input")) {
                MenuCheckbox("Allow Background Input", s.game.allowBackgroundInput);
                MenuCheckbox("Free Camera", s.game.freeCamera);
                MenuCheckbox("Custom Camera Speeds", s.game.enableCameraSpeedControls);
                SliderFloatItem("Camera Speed", s.game.regularCameraSensitivityLevel, 1.0f, 10.0f, "%.1f",
                    s.game.enableCameraSpeedControls && !s.game.freeCamera);
                SliderFloatItem("Freecam Speed", s.game.freeCameraSensitivityLevel, 1.0f, 10.0f, "%.1f",
                    s.game.enableCameraSpeedControls && s.game.freeCamera);
                SliderFloatItem("Aiming Speed", s.game.aimingCameraSensitivityLevel, 1.0f, 10.0f, "%.1f",
                    s.game.enableCameraSpeedControls);
                ImGui::Separator();
                MenuCheckbox("Invert Camera X Axis", s.game.invertCameraXAxis);
                MenuCheckbox("Invert Camera Y Axis", s.game.invertCameraYAxis);
                MenuCheckbox("Invert First Person X Axis", s.game.invertFirstPersonXAxis);
                MenuCheckbox("Invert First Person Y Axis", s.game.invertFirstPersonYAxis);
                ImGui::Separator();
                MenuCheckbox("Gyro Aim", s.game.enableGyroAim);
                MenuCheckbox("Gyro Rollgoal", s.game.enableGyroRollgoal);
                SliderFloatItem("Gyro Sensitivity X", s.game.gyroSensitivityX, 0.1f, 5.0f);
                SliderFloatItem("Gyro Sensitivity Y", s.game.gyroSensitivityY, 0.1f, 5.0f);
                SliderFloatItem("Gyro Rollgoal Sensitivity", s.game.gyroSensitivityRollgoal, 0.1f, 5.0f);
                SliderFloatItem("Gyro Smoothing", s.game.gyroSmoothing, 0.0f, 1.0f);
                SliderFloatItem("Gyro Deadband", s.game.gyroDeadband, 0.0f, 0.5f);
                MenuCheckbox("Invert Gyro Pitch", s.game.gyroInvertPitch);
                MenuCheckbox("Invert Gyro Yaw", s.game.gyroInvertYaw);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Cheats")) {
                ImGui::BeginDisabled(s.game.speedrunMode);
                MenuCheckbox("Infinite Hearts", s.game.infiniteHearts);
                MenuCheckbox("Infinite Arrows", s.game.infiniteArrows);
                MenuCheckbox("Infinite Seeds", s.game.infiniteSeeds);
                MenuCheckbox("Infinite Bombs", s.game.infiniteBombs);
                MenuCheckbox("Infinite Oil", s.game.infiniteOil);
                MenuCheckbox("Infinite Oxygen", s.game.infiniteOxygen);
                MenuCheckbox("Infinite Rupees", s.game.infiniteRupees);
                ImGui::Separator();
                MenuCheckbox("No Item Timer", s.game.enableIndefiniteItemDrops);
                MenuCheckbox("Moon Jump", s.game.moonJump);
                MenuCheckbox("Super Clawshot", s.game.superClawshot);
                MenuCheckbox("Always Greatspin", s.game.alwaysGreatspin);
                MenuCheckbox("Fast Iron Boots", s.game.enableFastIronBoots);
                MenuCheckbox("Transform Anywhere", s.game.canTransformAnywhere);
                MenuCheckbox("Fast Roll", s.game.fastRoll);
                MenuCheckbox("Fast Spinner", s.game.fastSpinner);
                MenuCheckbox("Free Magic Armor", s.game.freeMagicArmor);
                MenuCheckbox("Invincible Enemies", s.game.invincibleEnemies);
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
        }

        void DrawGraphicsMenu() {
            auto& s = getSettings();
            if (MenuCheckbox("Enable VSync", s.video.enableVsync)) {
                aurora_enable_vsync(s.video.enableVsync.getValue());
            }
            MenuCheckbox("Show FPS Counter", s.video.enableFpsOverlay);
            ImGui::Separator();
            FrameRateLimitSlider();
            MenuCheckbox("Low Latency Presentation", s.game.lowLatencyPresentation);
            MenuCheckbox("Depth of Field", s.game.enableDepthOfField);
            MenuCheckbox("Map Background", s.game.enableMapBackground);
            MenuCheckbox("Disable Water Refraction", s.game.disableWaterRefraction);
            MenuCheckbox("Disable Cutscene Pillarboxing", s.game.disableCutscenePillarboxing);
            ImGui::Separator();
            InternalResolutionSlider();
            ShadowResolutionSlider();
            ForcedAspectRatioControl();
            BloomModeControl();
            BloomMultiplierSlider();
        }

        void DrawAudioMenu() {
            auto& s = getSettings();
            int masterVolume = s.audio.masterVolume.getValue();
            ImGui::TextUnformatted("Master Volume");
            ImGui::SameLine(170.0f);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderInt("##MasterVolume", &masterVolume, 0, 100, "%d%%")) {
                s.audio.masterVolume.setValue(masterVolume);
                audio::SetMasterVolume(audio::MasterVolumeToLinear(masterVolume / 100.0f));
                config::Save();
            }
            ImGui::Separator();
            if (MenuCheckbox("Enable Reverb", s.audio.enableReverb)) {
                audio::SetEnableReverb(s.audio.enableReverb.getValue());
            }
            if (MenuCheckbox("Enable Spatial Sound", s.audio.enableHrtf)) {
                audio::EnableHrtf = s.audio.enableHrtf.getValue();
            }
            MenuCheckbox("Menu Sounds", s.audio.menuSounds);
            MenuCheckbox("No Low HP Sound", s.game.noLowHpSound);
            MenuCheckbox("Non-Stop Midna's Lament", s.game.midnasLamentNonStop);
        }
    }

    void ImGuiMenuTools::togglePracticeSaves() {
        m_showPracticeSaves = !m_showPracticeSaves;
    }

    void ImGuiMenuTools::draw() {
        if (ImGui::BeginMenu("Game")) {
            DrawGameMenu();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Graphics")) {
            DrawGraphicsMenu();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Audio")) {
            DrawAudioMenu();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools")) {
            if (!dusk::IsGameLaunched) {
                ImGui::BeginDisabled();
            }

            ImGui::BeginDisabled(getSettings().game.speedrunMode);

            ImGui::MenuItem("Save Editor", hotkeys::SHOW_SAVE_EDITOR, &m_showSaveEditor);
            ImGui::MenuItem("Practice Saves", nullptr, &m_showPracticeSaves);
            ImGui::MenuItem("State Share", hotkeys::SHOW_STATE_SHARE, &m_showStateShare);
            MenuCheckbox("Move Link", getSettings().game.moveLink);

            ImGui::EndDisabled();

            ImGui::Separator();
            config::ImGuiMenuItem("Show Input Viewer", nullptr, getSettings().game.showInputViewer);
            config::ImGuiMenuItem("Show Gyro Input Viewer", nullptr,
                getSettings().game.showInputViewerGyro, getSettings().game.showInputViewer);

            if (!dusk::IsGameLaunched) {
                ImGui::EndDisabled();
            }

            ImGui::Separator();
            config::ImGuiMenuItem("Turbo Speed Key", hotkeys::TURBO,
                getSettings().game.enableTurboKeybind, !getSettings().game.speedrunMode);

#if DUSK_CAN_OPEN_DATA_FOLDER
            ImGui::Separator();
            if (ImGui::MenuItem("Open Data Folder")) {
                data::open_data_path();
            }
#endif

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug")) {
            ImGui::BeginDisabled(getSettings().game.speedrunMode);

            config::ImGuiMenuItem("Advanced Settings", nullptr, getSettings().backend.enableAdvancedSettings);
            config::ImGuiMenuItem("Show Pipeline Compilation", nullptr, getSettings().backend.showPipelineCompilation);
            ImGui::Separator();

            bool developmentMode = mDoMain::developmentMode == 1;
            if (ImGui::Checkbox("Development Mode", &developmentMode)) {
                mDoMain::developmentMode = developmentMode ? 1 : -1;
            }

            ImGui::Separator();

            auto& collisionView = getTransientSettings().collisionView;
            if (ImGui::BeginMenu("Graphics Settings")) {
                bool disableWaterRefraction = getSettings().game.disableWaterRefraction;
                if (ImGui::Checkbox("Disable Water Refraction", &disableWaterRefraction)) {
                    getSettings().game.disableWaterRefraction.setValue(disableWaterRefraction);
                    config::Save();
                }
                ImGui::Checkbox("Enable LOD Bias", &aurora::gx::enableLodBias);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Collision View")) {
                ImGui::Checkbox("Enable Terrain view", &collisionView.enableTerrainView);
                ImGui::Checkbox("Enable wireframe view", &collisionView.enableWireframe);
                ImGui::SliderFloat("Opacity##terrain", &collisionView.terrainViewOpacity, 0.0f, 100.0f);
                ImGui::SliderFloat("Draw Range", &collisionView.drawRange, 0.0f, 1000.0f);
                ImGui::Separator();
                ImGui::Checkbox("Enable Attack Collider view", &collisionView.enableAtView);
                ImGui::Checkbox("Enable Target Collider view", &collisionView.enableTgView);
                ImGui::Checkbox("Enable Push Collider view", &collisionView.enableCoView);
                ImGui::SliderFloat("Opacity##colliders", &collisionView.colliderViewOpacity, 0.0f, 100.0f);
                ImGui::EndMenu();
            }

            if (!dusk::IsGameLaunched) {
                ImGui::BeginDisabled();
            }

            ImGui::MenuItem("Process Management", hotkeys::SHOW_PROCESS_MANAGEMENT, &m_showProcessManagement);
            ImGui::MenuItem("Debug Overlay", hotkeys::SHOW_DEBUG_OVERLAY, &m_showDebugOverlay);
            ImGui::MenuItem("Heap Viewer", hotkeys::SHOW_HEAP_VIEWER, &m_showHeapOverlay);
            ImGui::MenuItem("Player Info", hotkeys::SHOW_PLAYER_INFO, &m_showPlayerInfo);
            ImGui::MenuItem("Debug Camera", hotkeys::SHOW_DEBUG_CAMERA, &m_showCameraOverlay);
            ImGui::MenuItem("Audio Debug", hotkeys::SHOW_AUDIO_DEBUG, &m_showAudioDebug);
            ImGui::MenuItem("Bloom", nullptr, &m_showBloomWindow);
            ImGui::MenuItem("Stub Log", nullptr, &m_showStubLog);
            ImGui::MenuItem("Actor Spawner", nullptr, &m_showActorSpawner);

            if (!dusk::IsGameLaunched) {
                ImGui::EndDisabled();
            }

            ImGui::MenuItem("OSReport Force", nullptr, &OSReportReallyForceEnable);

            ImGui::EndDisabled();

            ImGui::EndMenu();
        }
    }

    void ImGuiMenuTools::ShowDebugOverlay() {
        if (!getSettings().backend.enableAdvancedSettings ||
            !ImGuiConsole::CheckMenuViewToggle(getSettings().hotkeys.debugOverlay, m_showDebugOverlay))
        {
            return;
        }

        ImGui::PushFont(ImGuiEngine::fontMono);

        ImGuiIO& io = ImGui::GetIO();
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;
        if (m_debugOverlayCorner != -1) {
            SetOverlayWindowLocation(m_debugOverlayCorner);
            windowFlags |= ImGuiWindowFlags_NoMove;
        }

        ImGui::SetNextWindowBgAlpha(0.65f);
        if (ImGui::Begin("Debug Overlay", nullptr, windowFlags)) {
            ImGuiStringViewText(fmt::format(FMT_STRING("FPS: {:.2f}\n"), io.Framerate));
            if (frameUsagePct > 0.f) {
                ImGuiStringViewText(fmt::format(FMT_STRING("Frame usage: {:.1f}%\n"), frameUsagePct));
            }

            ImGui::Separator();

            ImGuiStringViewText(fmt::format(FMT_STRING("Backend: {}\n"), backend_name(aurora_get_backend())));

            ImGui::Separator();

            const auto& stats = lastFrameAuroraStats;

            ImGuiStringViewText(
                fmt::format(FMT_STRING("Queued pipelines:  {}\n"), stats.queuedPipelines));
            ImGuiStringViewText(
                fmt::format(FMT_STRING("Done pipelines:    {}\n"), stats.createdPipelines));
            ImGuiStringViewText(
                fmt::format(FMT_STRING("Draw call count:   {}\n"), stats.drawCallCount));
            ImGuiStringViewText(fmt::format(FMT_STRING("Merged draw calls: {}\n"),
                stats.mergedDrawCallCount));
            ImGuiStringViewText(fmt::format(FMT_STRING("Vertex size:       {}\n"),
                BytesToString(stats.lastVertSize)));
            ImGuiStringViewText(fmt::format(FMT_STRING("Uniform size:      {}\n"),
                BytesToString(stats.lastUniformSize)));
            ImGuiStringViewText(fmt::format(FMT_STRING("Index size:        {}\n"),
                BytesToString(stats.lastIndexSize)));
            ImGuiStringViewText(fmt::format(FMT_STRING("Storage size:      {}\n"),
                BytesToString(stats.lastStorageSize)));
            ImGuiStringViewText(fmt::format(FMT_STRING("Tex upload size:   {}\n"),
                BytesToString(stats.lastTextureUploadSize)));
            ImGuiStringViewText(fmt::format(
                FMT_STRING("Total:             {}\n"),
                BytesToString(stats.lastVertSize + stats.lastUniformSize +
                    stats.lastIndexSize + stats.lastStorageSize +
                    stats.lastTextureUploadSize)));

            // TODO: persist to config
            ShowCornerContextMenu(m_debugOverlayCorner, m_cameraOverlayCorner);
        }
        ImGui::End();

        ImGui::PopFont();
    }

    void ImGuiMenuTools::ShowPlayerInfo() {
        if (!getSettings().backend.enableAdvancedSettings ||
            !ImGuiConsole::CheckMenuViewToggle(getSettings().hotkeys.playerInfo, m_showPlayerInfo))
        {
            return;
        }

        ImGui::PushFont(ImGuiEngine::fontMono);

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;
        if (m_playerInfoOverlayCorner != -1) {
            SetOverlayWindowLocation(m_playerInfoOverlayCorner);
            windowFlags |= ImGuiWindowFlags_NoMove;
        }

        ImGui::SetNextWindowBgAlpha(0.65f);

        if (ImGui::Begin("Player Info", nullptr, windowFlags)) {
            daAlink_c* player = (daAlink_c*)dComIfGp_getPlayer(0);
            daHorse_c* horse = dComIfGp_getHorseActor();

            ImGui::Text("Link");
            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Position: {: .4f}, {: .4f}, {: .4f}\n", player->current.pos.x, player->current.pos.y, player->current.pos.z)
                : "Position: ?, ?, ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Angle: {0}\n", player->shape_angle.y)
                : "Angle: ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Speed: {: .4f}\n", player->speedF)
                : "Speed: ?\n"
            );

            ImGui::Separator();
            ImGui::Text("Epona");
            ImGuiStringViewText(
                horse != nullptr
                ? fmt::format("Position: {: .4f}, {: .4f}, {: .4f}\n", horse->current.pos.x, horse->current.pos.y, horse->current.pos.z)
                : "Position: ?, ?, ?\n"
            );

            ImGuiStringViewText(
                horse != nullptr
                ? fmt::format("Angle: {0}\n", horse->shape_angle.y)
                : "Angle: ?\n"
            );

            ImGuiStringViewText(
                horse != nullptr
                ? fmt::format("Speed: {: .4f}\n", horse->speedF)
                : "Speed: ?\n"
            );

            ShowCornerContextMenu(m_playerInfoOverlayCorner, m_debugOverlayCorner);
        }

        ImGui::End();
        ImGui::PopFont();
    }
}
