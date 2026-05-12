#include "fmt/format.h"
#include "imgui.h"

#include "ImGuiConsole.hpp"
#include "ImGuiConfig.hpp"

#include "dusk/audio/DuskAudioSystem.h"
#include "dusk/config.hpp"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "dusk/audio/DuskDsp.hpp"
#include "m_Do/m_Do_main.h"
#include <dolphin/vi.h>

namespace dusk {
    ImGuiMenuGame::ImGuiMenuGame() {}

    namespace {
        bool MenuCheckbox(const char* label, ConfigVar<bool>& value, bool enabled = true) {
            return config::ImGuiMenuItem(label, nullptr, value, enabled);
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
            int copy = value.getValue();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt(label, &copy, min, max)) {
                value.setValue(copy);
                config::Save();
            }
        }

        void SliderFloatItem(const char* label, ConfigVar<float>& value, float min, float max,
                             const char* format = "%.2f")
        {
            float copy = value.getValue();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderFloat(label, &copy, min, max, format)) {
                value.setValue(copy);
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
            "30 FPS",
            "60 FPS",
            "120 FPS",
            "240 FPS",
            "360 FPS",
            "480 FPS",
            "Unlocked",
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

        void DrawGameGeneralMenu() {
            auto& s = getSettings();
            MenuCheckbox("Mirror Mode", s.game.enableMirrorMode);
            MenuCheckbox("Minimal HUD", s.game.minimalHUD);
            MenuCheckbox("Achievement Notifications", s.game.enableAchievementToasts);
            MenuCheckbox("Controller Notifications", s.game.enableControllerToasts);
        }

        void DrawQualityOfLifeMenu() {
            auto& s = getSettings();
            MenuCheckbox("Quick Transform (R+Y)", s.game.enableQuickTransform);
            MenuCheckbox("Bigger Wallets", s.game.biggerWallets);
            MenuCheckbox("Disable Rupee Cutscenes", s.game.disableRupeeCutscenes);
            MenuCheckbox("No Rupee Returns", s.game.noReturnRupees);
            MenuCheckbox("No Sword Recoil", s.game.noSwordRecoil);
            MenuCheckbox("Faster Climbing", s.game.fastClimbing);
            MenuCheckbox("No Climbing Miss Animation", s.game.noMissClimbing);
            MenuCheckbox("Faster Tears of Light", s.game.fastTears);
            MenuCheckbox("No 2nd Fish for Cat", s.game.no2ndFishForCat);
            MenuCheckbox("Sun's Song (R+X)", s.game.sunsSong);
            ImGui::Separator();
            LoadModeCheckbox("Fast Loads", s.game.enableFastLoads, s.game.enableInstaLoads);
            LoadModeCheckbox("Insta Loads", s.game.enableInstaLoads, s.game.enableFastLoads);
            MenuCheckbox("Autosave", s.game.autoSave);
            MenuCheckbox("Instant Saves", s.game.instantSaves);
            MenuCheckbox("Hold B for Instant Text", s.game.instantText);
        }

        void DrawDifficultyMenu() {
            auto& s = getSettings();
            ImGui::BeginDisabled(s.game.speedrunMode);
            SliderIntItem("Damage Multiplier", s.game.damageMultiplier, 1, 8);
            MenuCheckbox("Instant Death", s.game.instantDeath);
            MenuCheckbox("No Heart Drops", s.game.noHeartDrops);
            ImGui::EndDisabled();
        }

        void DrawMiscMenu() {
            auto& s = getSettings();
            MenuCheckbox("Restore Wii 1.0 Glitches", s.game.restoreWiiGlitches);
            MenuCheckbox("Rotating Link Doll", s.game.enableLinkDollRotation);
            MenuCheckbox("Hide TV Settings Screen", s.game.hideTvSettingsScreen);
            MenuCheckbox("Pause On Focus Lost", s.game.pauseOnFocusLost);
            MenuCheckbox("Recording Mode", s.game.recordingMode);
            ImGui::Separator();
            MenuCheckbox("Speedrun Mode", s.game.speedrunMode);
            MenuCheckbox("LiveSplit", s.game.liveSplitEnabled);
        }

        void DrawGraphicsMenu() {
            auto& s = getSettings();
            FrameRateLimitSlider();
            MenuCheckbox("Depth of Field", s.game.enableDepthOfField);
            MenuCheckbox("Map Background", s.game.enableMapBackground);
            MenuCheckbox("Disable Water Refraction", s.game.disableWaterRefraction);
            ImGui::Separator();
            InternalResolutionSlider();
            ShadowResolutionSlider();
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
                audio::SetMasterVolume(masterVolume / 100.0f);
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

        void DrawInputMenu() {
            auto& s = getSettings();
            MenuCheckbox("Allow Background Input", s.game.allowBackgroundInput);
            MenuCheckbox("Free Camera", s.game.freeCamera);
            SliderFloatItem("Free Camera Sensitivity", s.game.freeCameraSensitivity, 0.1f, 5.0f);
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
            ImGui::Separator();
            MenuCheckbox("Turbo Key", s.game.enableTurboKeybind);
        }

        void DrawCheatsMenu() {
            auto& s = getSettings();
            ImGui::BeginDisabled(s.game.speedrunMode);
            MenuCheckbox("Infinite Hearts", s.game.infiniteHearts);
            MenuCheckbox("Infinite Arrows", s.game.infiniteArrows);
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
            MenuCheckbox("Fast Spinner", s.game.fastSpinner);
            MenuCheckbox("Free Magic Armor", s.game.freeMagicArmor);
            ImGui::EndDisabled();
        }

        void DrawTechnicalMenu() {
            auto& s = getSettings();
            MenuCheckbox("Advanced Settings", s.backend.enableAdvancedSettings);
            MenuCheckbox("Show Pipeline Compilation", s.backend.showPipelineCompilation);
            MenuCheckbox("Check For Updates", s.backend.checkForUpdates);
            MenuCheckbox("Skip Pre-Launch UI", s.backend.skipPreLaunchUI);
        }
    }

    void ImGuiMenuGame::draw() {
        if (ImGui::BeginMenu("Game")) {
            if (ImGui::BeginMenu("General")) {
                DrawGameGeneralMenu();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Quality of Life")) {
                DrawQualityOfLifeMenu();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Difficulty")) {
                DrawDifficultyMenu();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Misc")) {
                DrawMiscMenu();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Enhancements")) {
            if (ImGui::BeginMenu("Graphics")) {
                DrawGraphicsMenu();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Input")) {
                DrawInputMenu();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Cheats")) {
                DrawCheatsMenu();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Audio")) {
            DrawAudioMenu();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Technical")) {
            DrawTechnicalMenu();
            ImGui::EndMenu();
        }
    }

    static std::string GetFormattedTime(OSTime ticks) {
        OSCalendarTime time;
        OSTicksToCalendarTime(ticks, &time);

        return fmt::format("{0:02}:{1:02}:{2:02}.{3:03}", time.hour, time.min, time.sec, time.msec);
    }

    void ImGuiMenuGame::resetForSpeedrunMode() {
        // reset settings that should be off for speedrun mode
        mDoMain::developmentMode = -1;

        getSettings().game.damageMultiplier.setValue(1);
        getSettings().game.instantDeath.setValue(false);
        getSettings().game.noHeartDrops.setValue(false);

        getSettings().game.infiniteHearts.setValue(false);
        getSettings().game.infiniteArrows.setValue(false);
        getSettings().game.infiniteBombs.setValue(false);
        getSettings().game.infiniteOil.setValue(false);
        getSettings().game.infiniteOxygen.setValue(false);
        getSettings().game.infiniteRupees.setValue(false);
        getSettings().game.enableIndefiniteItemDrops.setValue(false);

        getSettings().game.moonJump.setValue(false);
        getSettings().game.superClawshot.setValue(false);
        getSettings().game.alwaysGreatspin.setValue(false);
        getSettings().game.enableFastIronBoots.setValue(false);
        getSettings().game.canTransformAnywhere.setValue(false);
        getSettings().game.fastSpinner.setValue(false);
        getSettings().game.freeMagicArmor.setValue(false);

        getSettings().game.enableTurboKeybind.setValue(false);
        getSettings().game.debugFlyCam.setValue(false);
        getSettings().game.autoSave.setValue(false);
    }

    SpeedrunInfo m_speedrunInfo;

    void ImGuiMenuGame::drawSpeedrunTimerOverlay() {
        if (!getSettings().game.speedrunMode) {
            return;
        }

        // L+R+A+Start to reset timer
        if (mDoCPd_c::getHoldL(PAD_1) && mDoCPd_c::getHoldR(PAD_1) && mDoCPd_c::getHoldA(PAD_1) && mDoCPd_c::getTrigZ(PAD_1)) {
            m_speedrunInfo.reset();
        }

        // L+R+A+Z to manually stop timer
        if (mDoCPd_c::getHoldL(PAD_1) && mDoCPd_c::getHoldR(PAD_1) && mDoCPd_c::getHoldA(PAD_1) && mDoCPd_c::getTrigY(PAD_1)) {
            if (m_speedrunInfo.m_isRunStarted) {
                m_speedrunInfo.m_endTimestamp = OSGetTime() - m_speedrunInfo.m_startTimestamp;
                m_speedrunInfo.m_isRunStarted = false;
            }
        }

        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoScrollbar;

        if (ImGui::Begin("##SpeedrunTimerWindow", nullptr, flags)) {
            OSTime elapsedTime = 0;
            if (m_speedrunInfo.m_isRunStarted) {
                elapsedTime = OSGetTime() - m_speedrunInfo.m_startTimestamp;
            } else if (m_speedrunInfo.m_endTimestamp != 0) {
                elapsedTime = m_speedrunInfo.m_endTimestamp;
            }

            ImGui::Text("RTA");
            ImGui::SameLine(60.0f);
            ImGuiStringViewText(GetFormattedTime(elapsedTime));

            if (!m_speedrunInfo.m_isPauseIGT) {
                m_speedrunInfo.m_igtTimer = elapsedTime - m_speedrunInfo.m_totalLoadTime;
            }

            ImGui::Text("IGT");
            ImGui::SameLine(60.0f);
            ImGuiStringViewText(GetFormattedTime(m_speedrunInfo.m_igtTimer));
        }
        ImGui::End();
    }
}
