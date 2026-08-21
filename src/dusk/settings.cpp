#include "dusk/settings.h"
#include "dusk/config.hpp"
#include <aurora/aurora.h>
#include <aurora/dvd.h>

#include <algorithm>
#include <SDL3/SDL_scancode.h>

namespace dusk {

UserSettings g_userSettings = {
    .video = {
        .enableFullscreen {"video.enableFullscreen", false},
        .enableVsync {"video.enableVsync", true},
        .lockAspectRatio {"video.lockAspectRatio", false},
        .forcedAspectRatio {"video.forcedAspectRatio", AspectRatioMode::Off},
        .enableFpsOverlay {"game.enableFpsOverlay", false},
        .fpsOverlayCorner {"game.fpsOverlayCorner", 0},
        .maxFrameRate {"video.maxFrameRate", 240},
        .rememberWindowSize {"video.rememberWindowSize", false},
        .lastWindowWidth {"video.lastWindowWidth", 0},
        .lastWindowHeight {"video.lastWindowHeight", 0},
    },

    .ui = {
        .settingsFavorites {"ui.settingsFavorites", ""},
        .menuWidthDp {"ui.menuWidthDp", 1088},
        .menuHeightDp {"ui.menuHeightDp", 768},
        .menuSizeCustomized {"ui.menuSizeCustomized", false},
    },

    .audio = {
        .masterVolume {"audio.masterVolume", 60},
        .mainMusicVolume {"audio.mainMusicVolume", 100},
        .subMusicVolume {"audio.subMusicVolume", 100},
        .soundEffectsVolume {"audio.soundEffectsVolume", 100},
        .fanfareVolume {"audio.fanfareVolume", 100},
        .enableReverb {"audio.enableReverb", true},
        .enableHrtf {"audio.enableHrtf", false},
        .menuSounds {"audio.menuSounds", true},
    },

    .game = {
        .language { "game.language", GameLanguage::English },

        // Quality of Life
        .enableQuickTransform {"game.enableQuickTransform", false},
        .humanMidnaWarp {"game.humanMidnaWarp", false},
        .hideTvSettingsScreen {"game.hideTvSettingsScreen", true},
        .biggerWallets {"game.biggerWallets", false},
        .noReturnRupees {"game.noReturnRupees", false},
        .disableRupeeCutscenes {"game.disableRupeeCutscenes", false},
        .skipAllCutscenes {"game.skipAllCutscenes", false},
        .noSwordRecoil {"game.noSwordRecoil", false},
        .damageMultiplier {"game.damageMultiplier", 1},
        .noHeartDrops {"game.noHeartDrops", false},
        .instantDeath {"game.instantDeath", false},
        .fastClimbing {"game.fastClimbing", false},
        .noMissClimbing {"game.noMissClimbing", false},
        .fastTears {"game.fastTears", false},
        .no2ndFishForCat {"game.no2ndFishForCat", false},
        .enableFastLoads {"game.enableFastLoads", false},
        .enableInstaLoads {"game.enableInstaLoads", false},
        .discLoadingDelayMode {"game.loadDelayMode", DiscLoadingDelayMode::Off},
        .discLoadingDelaySeconds {"game.discLoadingDelaySeconds", 1},
        .theEtherNetBoyzExperience {"game.theEtherNetBoyzExperience", false},
        .instantMovement {"game.instantMovement", false},
        .buttonFishing {"game.buttonFishing", false},
        .instantSaves {"game.instantSaves", false},
        .instantText {"game.instantText", false},
        .sunsSong {"game.sunsSong", false},
        .autoSave {"game.autoSave", false},
        .enhancedMapMenus {"game.enhancedMapMenus", false},
        .aimingReticle {"game.aimingReticle", false},

        // Preferences
        .enableMirrorMode {"game.enableMirrorMode", false},
        .minimalHUD {"game.minimalHUD", false},
        .hudScale {"game.hudScale", 1.0f},
        .pauseOnFocusLost {"game.pauseOnFocusLost", false},
        .enableLinkDollRotation {"game.enableLinkDollRotation", false},
        .enableAchievementToasts {"game.enableAchievementToasts", true},
        .enableControllerToasts {"game.enableControllerToasts", true},
        .enableDiscordPresence {"game.enableDiscordPresence", true},
        .menuScalingMode {"game.menuScalingMode", MenuScaling::Wii},

        // Graphics
        .bloomMode {"game.bloomMode", BloomMode::Dusk},
        .bloomMultiplier {"game.bloomMultiplier", 1.0f},
        .depthOfFieldMode{"game.depthOfFieldMode", DepthOfFieldMode::Dusk},
        .disableWaterRefraction {"game.disableWaterRefraction", false},
        .enableTextureReplacements {"game.enableTextureReplacements", true},
        .enableFrameInterpolation {"game.enableFrameInterpolation", FrameInterpMode::Off},
        .frameRateLimit {"game.frameRateLimit", 0},
        .lowLatencyPresentation {"game.lowLatencyPresentation", true},
        .internalResolutionScale {"game.internalResolutionScale", 0},
        .shadowResolutionMultiplier {"game.shadowResolutionMultiplier", 1},
        .resampler {"game.resampler", Resampler::Bilinear},
        .enableTwilightVisuals {"game.enableTwilightVisuals", false},
        .enableTwilightVisualMusic {"game.enableTwilightVisualMusic", true},
        .twilightVisualBrightness {"game.twilightVisualBrightness", 1.0f},
        .twilightSkyboxMode {"game.twilightSkyboxMode", TwilightSkyboxMode::TwilightDay},
        .twilightWeather {"game.twilightWeather", TwilightWeather::Current},
        .enableMapBackground {"game.enableMapBackground", true},
        .disableCutscenePillarboxing {"game.disableCutscenePillarboxing", false},
        .enableHighQualityMinimapTextures {"game.enableHighQualityMinimapTextures", true},

        // Audio
        .noLowHpSound {"game.noLowHpSound", false},
        .midnasLamentNonStop {"game.midnasLamentNonStop", false},

        // Input
        .enableGyroAim {"game.enableGyroAim", false},
        .enableGyroRollgoal {"game.enableGyroRollgoal", false},
        .gyroSensitivityX {"game.gyroSensitivityX", 1.0f},
        .gyroSensitivityY {"game.gyroSensitivityY", 1.0f},
        .gyroSensitivityRollgoal {"game.gyroSensitivityRollgoal", 1.0f},
        .gyroSmoothing {"game.gyroSmoothing", 0.65f},
        .gyroDeadband {"game.gyroDeadband", 0.04f},
        .gyroInvertPitch {"game.gyroInvertPitch", false},
        .gyroInvertYaw {"game.gyroInvertYaw", false},
        .enableMouseCamera {"game.enableMouseCamera", false},
        .enableMouseAim {"game.enableMouseAim", false},
        .mouseAimSensitivity {"game.mouseAimSensitivity", 1.0f},
        .mouseCameraSensitivity {"game.mouseCameraSensitivity", 1.0f},
        .invertMouseY {"game.invertMouseY", false},
        .freeCamera {"game.freeCamera", false},
        .enableTouchControls {"game.enableTouchControls", false},
        .touchTargeting {"game.touchTargeting", TouchTargeting::Hybrid},
        .enableMenuPointer {"game.enableMenuPointer", true},
        .touchControlsLayout {"game.touchControlsLayout", ui::ControlLayout{}},
        .invertCameraXAxis {"game.invertCameraXAxis", false},
        .invertCameraYAxis {"game.invertCameraYAxis", false},
        .invertFirstPersonXAxis {"game.invertFirstPersonXAxis", false},
        .invertFirstPersonYAxis {"game.invertFirstPersonYAxis", false},
        .invertAirSwimX {"game.invertAirSwimX", false},
        .invertAirSwimY {"game.invertAirSwimY", false},
        .freeCameraSensitivity {"game.freeCameraSensitivity", 1.0f},
        .enableCameraSpeedControls {"game.enableCameraSpeedControls", false},
        .aimingCameraSensitivityLevel {"game.aimingCameraSensitivityLevel", 5.0f},
        .freeCameraSensitivityLevel {"game.freeCameraSensitivityLevel", 5.0f},
        .regularCameraSensitivityLevel {"game.regularCameraSensitivityLevel", 5.0f},
        .freeCameraXSensitivity {"game.freeCameraXSensitivity", 1.0f},
        .freeCameraYSensitivity {"game.freeCameraYSensitivity", 1.0f},
        .touchCameraXSensitivity {"game.touchCameraXSensitivity", 1.0f},
        .touchCameraYSensitivity {"game.touchCameraYSensitivity", 1.0f},
        .debugFlyCam {"game.debugFlyCam", false},
        .debugFlyCamLockEvents {"game.debugFlyCamLockEvents", true},
        .allowBackgroundInput {"game.allowBackgroundInput", true},
        .inputLagMs {"game.inputLagMs", 0},
        .enableLED {
            ConfigVar<bool>{"game.enableLED_port0", true},
            ConfigVar<bool>{"game.enableLED_port1", true},
            ConfigVar<bool>{"game.enableLED_port2", true},
            ConfigVar<bool>{"game.enableLED_port3", true},
        },
        .swapDirectSelect {"game.swapDirectSelect", false},

        // Cheats
        .infiniteHearts {"game.infiniteHearts", false},
        .infiniteArrows {"game.infiniteArrows", false},
        .infiniteSeeds {"game.infiniteSeeds", false},
        .infiniteBombs {"game.infiniteBombs", false},
        .infiniteOil {"game.infiniteOil", false},
        .infiniteOxygen {"game.infiniteOxygen", false},
        .infiniteRupees {"game.infiniteRupees", false},
        .enableIndefiniteItemDrops {"game.enableIndefiniteItemDrops", false},
        .moonJump {"game.moonJump", false},
        .superClawshot {"game.superClawshot", false},
        .alwaysGreatspin {"game.alwaysGreatspin", false},
        .enableFastIronBoots {"game.enableFastIronBoots", false},
        .canTransformAnywhere {"game.canTransformAnywhere", false},
        .fastRoll {"game.fastRoll", false},
        .fastSpinner {"game.fastSpinner", false},
        .armorRupeeDrain {"game.armorRupeeDrain", MagicArmorMode::NORMAL},
        .invincibleEnemies {"game.invincibleEnemies", false},
        .transformWithoutShadowCrystal {"game.transformWithoutShadowCrystal", false},

        // Technical
        .restoreWiiGlitches {"game.restoreWiiGlitches", false},
        .usePpcFastInvSqrt {"game.usePpcFastInvSqrt", true},

        // Controls
        .enableTurboKeybind {"game.enableTurboKeybind", false},
        .enableResetKeybind {"game.enableResetKeybind", false},

        // Tools
        .speedrunMode {"game.speedrunMode", false},
        .liveSplitEnabled {"game.liveSplitEnabled", false},
        .showSpeedrunRTATimer {"game.showSpeedrunRTATimer", true},
        .moveLink {"game.moveLink", false},
        .teleportLink {"game.teleportLink", false},
        .areaReload {"game.areaReload", false},
        .gorgeVoidChecker {"game.gorgeVoidChecker", false},
        .recordingMode {"game.recordingMode", false},
        .removeQuestMapMarkers {"game.removeQuestMapMarkers", false},
        .showInputViewer {"game.showInputViewer", false},
        .showInputViewerGyro {"game.showInputViewerGyro", false},
        .nativeInputViewer {"game.nativeInputViewer", false},
        .nativeLinkDebugInfo {"game.nativeLinkDebugInfo", false},
        .triggerViewDefinitions {
            "tools.triggerViewDefinitions",
            "[]"
        },
        .nativePracticeMenu {"game.nativePracticeMenu", true}
    },

    .backend = {
        .isoPath {"backend.isoPath", ""},
        .isoVerification {"backend.isoVerification", DiscVerificationState::Unknown},
        .graphicsBackend {"backend.graphicsBackend", "auto"},
        .skipPreLaunchUI {"backend.skipPreLaunchUI", false},
        .wasPresetChosen {"backend.wasPresetChosen", false},
        .showPipelineCompilation {"backend.showPipelineCompilation", true},
        .checkForUpdates {"backend.checkForUpdates", false},
        .cardFileType {"backend.cardFileType", static_cast<int>(CARD_GCIFOLDER)},
        .enableAdvancedSettings {"backend.enableAdvancedSettings", false},
    },

    .hotkeys = {
        .toggleImGuiMenu = {
            ConfigVar<int>{"hotkeys.toggleImGuiMenu.key", SDL_SCANCODE_GRAVE},
            ConfigVar<int>{"hotkeys.toggleImGuiMenu.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.toggleImGuiMenu.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .toggleThirtyFps = {
            ConfigVar<int>{"hotkeys.toggleThirtyFps.key", SDL_SCANCODE_BACKSLASH},
            ConfigVar<int>{"hotkeys.toggleThirtyFps.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.toggleThirtyFps.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .turboSpeed = {
            ConfigVar<int>{"hotkeys.turboSpeed.key", SDL_SCANCODE_TAB},
            ConfigVar<int>{"hotkeys.turboSpeed.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.turboSpeed.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .toggleFullscreen = {
            ConfigVar<int>{"hotkeys.toggleFullscreen.key", SDL_SCANCODE_F11},
            ConfigVar<int>{"hotkeys.toggleFullscreen.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.toggleFullscreen.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .hideShowImGuiMenu = {
            ConfigVar<int>{"hotkeys.hideShowImGuiMenu.key", SDL_SCANCODE_F1},
            ConfigVar<int>{"hotkeys.hideShowImGuiMenu.modifiers", HOTKEY_MOD_SHIFT},
            ConfigVar<int>{"hotkeys.hideShowImGuiMenu.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .processManagement = {
            ConfigVar<int>{"hotkeys.processManagement.key", SDL_SCANCODE_F2},
            ConfigVar<int>{"hotkeys.processManagement.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.processManagement.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .debugOverlay = {
            ConfigVar<int>{"hotkeys.debugOverlay.key", SDL_SCANCODE_F3},
            ConfigVar<int>{"hotkeys.debugOverlay.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.debugOverlay.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .heapViewer = {
            ConfigVar<int>{"hotkeys.heapViewer.key", SDL_SCANCODE_F4},
            ConfigVar<int>{"hotkeys.heapViewer.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.heapViewer.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .playerInfo = {
            ConfigVar<int>{"hotkeys.playerInfo.key", SDL_SCANCODE_F5},
            ConfigVar<int>{"hotkeys.playerInfo.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.playerInfo.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .saveEditor = {
            ConfigVar<int>{"hotkeys.saveEditor.key", SDL_SCANCODE_F6},
            ConfigVar<int>{"hotkeys.saveEditor.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.saveEditor.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .stateShare = {
            ConfigVar<int>{"hotkeys.stateShare.key", SDL_SCANCODE_F8},
            ConfigVar<int>{"hotkeys.stateShare.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.stateShare.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .debugCamera = {
            ConfigVar<int>{"hotkeys.debugCamera.key", SDL_SCANCODE_F9},
            ConfigVar<int>{"hotkeys.debugCamera.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.debugCamera.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .captureCameraKeyframe = {
            ConfigVar<int>{"hotkeys.captureCameraKeyframe.key", SDL_SCANCODE_K},
            ConfigVar<int>{"hotkeys.captureCameraKeyframe.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.captureCameraKeyframe.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .audioDebug = {
            ConfigVar<int>{"hotkeys.audioDebug.key", SDL_SCANCODE_F10},
            ConfigVar<int>{"hotkeys.audioDebug.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.audioDebug.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .useTexturePack = {
            ConfigVar<int>{"hotkeys.useTexturePack.key", SDL_SCANCODE_UNKNOWN},
            ConfigVar<int>{"hotkeys.useTexturePack.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.useTexturePack.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .gyroAim = {
            ConfigVar<int>{"hotkeys.gyroAim.key", SDL_SCANCODE_UNKNOWN},
            ConfigVar<int>{"hotkeys.gyroAim.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.gyroAim.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .showInputViewer = {
            ConfigVar<int>{"hotkeys.showInputViewer.key", SDL_SCANCODE_UNKNOWN},
            ConfigVar<int>{"hotkeys.showInputViewer.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.showInputViewer.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .moveLink = {
            ConfigVar<int>{"hotkeys.moveLink.key", SDL_SCANCODE_UNKNOWN},
            ConfigVar<int>{"hotkeys.moveLink.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.moveLink.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .cycleBloomMode = {
            ConfigVar<int>{"hotkeys.cycleBloomMode.key", SDL_SCANCODE_UNKNOWN},
            ConfigVar<int>{"hotkeys.cycleBloomMode.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.cycleBloomMode.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
        .toggleDiscLoadingDelay = {
            ConfigVar<int>{"hotkeys.toggleDiscLoadingDelay.key", SDL_SCANCODE_UNKNOWN},
            ConfigVar<int>{"hotkeys.toggleDiscLoadingDelay.modifiers", HOTKEY_MOD_NONE},
            ConfigVar<int>{"hotkeys.toggleDiscLoadingDelay.controllerButton", PAD_NATIVE_BUTTON_INVALID},
        },
    },

    // Not sure if there's a better way to declare this
    .actionBindings = {
        .firstPersonCamera {
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .callMidna {
            ActionBindConfigVar{"actionBindings.callMidna_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openMapScreen {
            ActionBindConfigVar{"actionBindings.openMapScreen_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .toggleMinimap {
            ActionBindConfigVar{"actionBindings.toggleMinimap_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openDusklightMenu {
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .turboSpeedButton {
            ActionBindConfigVar{"actionBindings.turboButton_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port3", PAD_NATIVE_BUTTON_INVALID},
        },
    }
};

UserSettings& getSettings() {
    return g_userSettings;
}

void registerSettings() {
    Register(g_userSettings.ui.settingsFavorites);
    Register(g_userSettings.ui.menuWidthDp);
    Register(g_userSettings.ui.menuHeightDp);
    Register(g_userSettings.ui.menuSizeCustomized);

    // Video
    Register(g_userSettings.video.enableFullscreen);
    Register(g_userSettings.video.enableVsync);
    Register(g_userSettings.video.lockAspectRatio);
    Register(g_userSettings.video.forcedAspectRatio);
    Register(g_userSettings.video.enableFpsOverlay);
    Register(g_userSettings.video.fpsOverlayCorner);
    Register(g_userSettings.video.maxFrameRate);
    Register(g_userSettings.video.rememberWindowSize);
    Register(g_userSettings.video.lastWindowWidth);
    Register(g_userSettings.video.lastWindowHeight);

    // Audio
    Register(g_userSettings.audio.masterVolume);
    Register(g_userSettings.audio.mainMusicVolume);
    Register(g_userSettings.audio.subMusicVolume);
    Register(g_userSettings.audio.soundEffectsVolume);
    Register(g_userSettings.audio.fanfareVolume);
    Register(g_userSettings.audio.enableReverb);
    Register(g_userSettings.audio.enableHrtf);
    Register(g_userSettings.audio.menuSounds);

    // Game
    Register(g_userSettings.game.language);
    Register(g_userSettings.game.enableQuickTransform);
    Register(g_userSettings.game.humanMidnaWarp);
    Register(g_userSettings.game.transformWithoutShadowCrystal);
    Register(g_userSettings.game.hideTvSettingsScreen);
    Register(g_userSettings.game.biggerWallets);
    Register(g_userSettings.game.noReturnRupees);
    Register(g_userSettings.game.disableRupeeCutscenes);
    Register(g_userSettings.game.skipAllCutscenes);
    Register(g_userSettings.game.noSwordRecoil);
    Register(g_userSettings.game.damageMultiplier);
    Register(g_userSettings.game.noHeartDrops);
    Register(g_userSettings.game.instantDeath);
    Register(g_userSettings.game.fastClimbing);
    Register(g_userSettings.game.fastTears);
    Register(g_userSettings.game.no2ndFishForCat);
    Register(g_userSettings.game.enableFastLoads);
    Register(g_userSettings.game.enableInstaLoads);
    Register(g_userSettings.game.discLoadingDelayMode);
    Register(g_userSettings.game.discLoadingDelaySeconds);
    Register(g_userSettings.game.theEtherNetBoyzExperience);
    Register(g_userSettings.game.instantMovement);
    Register(g_userSettings.game.buttonFishing);
    Register(g_userSettings.game.instantSaves);
    Register(g_userSettings.game.instantText);
    Register(g_userSettings.game.sunsSong);
    Register(g_userSettings.game.autoSave);
    Register(g_userSettings.game.enhancedMapMenus);
    Register(g_userSettings.game.aimingReticle);
    Register(g_userSettings.game.enableMirrorMode);
    Register(g_userSettings.game.invertCameraXAxis);
    Register(g_userSettings.game.invertCameraYAxis);
    Register(g_userSettings.game.invertFirstPersonXAxis);
    Register(g_userSettings.game.invertFirstPersonYAxis);
    Register(g_userSettings.game.invertAirSwimX);
    Register(g_userSettings.game.invertAirSwimY);
    Register(g_userSettings.game.freeCameraSensitivity);
    Register(g_userSettings.game.enableCameraSpeedControls);
    Register(g_userSettings.game.aimingCameraSensitivityLevel);
    Register(g_userSettings.game.freeCameraSensitivityLevel);
    Register(g_userSettings.game.regularCameraSensitivityLevel);
    Register(g_userSettings.game.freeCameraXSensitivity);
    Register(g_userSettings.game.freeCameraYSensitivity);
    Register(g_userSettings.game.touchCameraXSensitivity);
    Register(g_userSettings.game.touchCameraYSensitivity);
    Register(g_userSettings.game.minimalHUD);
    Register(g_userSettings.game.hudScale);
    Register(g_userSettings.game.pauseOnFocusLost,
        [](const bool& value, const bool&) { aurora_set_pause_on_focus_lost(value); });
    Register(g_userSettings.game.enableDiscordPresence);
    Register(g_userSettings.game.bloomMode);
    Register(g_userSettings.game.bloomMultiplier);
    Register(g_userSettings.game.depthOfFieldMode);
    Register(g_userSettings.game.disableWaterRefraction);
    Register(g_userSettings.game.enableTextureReplacements);
    Register(g_userSettings.game.internalResolutionScale);
    Register(g_userSettings.game.resampler);
    Register(g_userSettings.game.shadowResolutionMultiplier);
    Register(g_userSettings.game.enableTwilightVisuals);
    Register(g_userSettings.game.enableTwilightVisualMusic);
    Register(g_userSettings.game.twilightVisualBrightness);
    Register(g_userSettings.game.twilightSkyboxMode);
    Register(g_userSettings.game.twilightWeather);
    Register(g_userSettings.game.enableMapBackground);
    Register(g_userSettings.game.disableCutscenePillarboxing);
    Register(g_userSettings.game.enableHighQualityMinimapTextures);
    Register(g_userSettings.game.enableFastIronBoots);
    Register(g_userSettings.game.canTransformAnywhere);
    Register(g_userSettings.game.fastRoll);
    Register(g_userSettings.game.armorRupeeDrain);
    Register(g_userSettings.game.restoreWiiGlitches);
    Register(g_userSettings.game.usePpcFastInvSqrt);
    Register(g_userSettings.game.enableLinkDollRotation);
    Register(g_userSettings.game.enableAchievementToasts);
    Register(g_userSettings.game.enableControllerToasts);
    Register(g_userSettings.game.noMissClimbing);
    Register(g_userSettings.game.noLowHpSound);
    Register(g_userSettings.game.midnasLamentNonStop);
    Register(g_userSettings.game.enableTurboKeybind);
    Register(g_userSettings.game.enableResetKeybind);
    Register(g_userSettings.game.speedrunMode);
    Register(g_userSettings.game.liveSplitEnabled);
    Register(g_userSettings.game.showSpeedrunRTATimer);
    Register(g_userSettings.game.moveLink);
    Register(g_userSettings.game.teleportLink);
    Register(g_userSettings.game.areaReload);
    Register(g_userSettings.game.gorgeVoidChecker);
    Register(g_userSettings.game.recordingMode);
    Register(g_userSettings.game.menuScalingMode);
    Register(g_userSettings.game.removeQuestMapMarkers);
    Register(g_userSettings.game.showInputViewer);
    Register(g_userSettings.game.showInputViewerGyro);
    Register(g_userSettings.game.nativeInputViewer);
    Register(g_userSettings.game.nativeLinkDebugInfo);
    Register(g_userSettings.game.triggerViewDefinitions);
    Register(g_userSettings.game.nativePracticeMenu);
    Register(g_userSettings.game.fastSpinner);
    Register(g_userSettings.game.infiniteHearts);
    Register(g_userSettings.game.infiniteArrows);
    Register(g_userSettings.game.infiniteSeeds);
    Register(g_userSettings.game.infiniteBombs);
    Register(g_userSettings.game.infiniteOil);
    Register(g_userSettings.game.infiniteOxygen);
    Register(g_userSettings.game.infiniteRupees);
    Register(g_userSettings.game.enableIndefiniteItemDrops);
    Register(g_userSettings.game.moonJump);
    Register(g_userSettings.game.superClawshot);
    Register(g_userSettings.game.alwaysGreatspin);
    Register(g_userSettings.game.invincibleEnemies);
    Register(g_userSettings.game.enableFrameInterpolation);
    Register(g_userSettings.game.frameRateLimit);
    Register(g_userSettings.game.lowLatencyPresentation);
    Register(g_userSettings.game.enableGyroAim);
    Register(g_userSettings.game.enableGyroRollgoal);
    Register(g_userSettings.game.gyroSensitivityX);
    Register(g_userSettings.game.gyroSensitivityY);
    Register(g_userSettings.game.gyroSensitivityRollgoal);
    Register(g_userSettings.game.gyroDeadband);
    Register(g_userSettings.game.gyroSmoothing);
    Register(g_userSettings.game.gyroInvertPitch);
    Register(g_userSettings.game.gyroInvertYaw);
    Register(g_userSettings.game.enableMouseCamera);
    Register(g_userSettings.game.enableMouseAim);
    Register(g_userSettings.game.mouseAimSensitivity);
    Register(g_userSettings.game.mouseCameraSensitivity);
    Register(g_userSettings.game.invertMouseY);
    Register(g_userSettings.game.freeCamera);
    Register(g_userSettings.game.enableTouchControls);
    Register(g_userSettings.game.touchTargeting);
    Register(g_userSettings.game.enableMenuPointer);
    Register(g_userSettings.game.touchControlsLayout);
    Register(g_userSettings.game.debugFlyCam);
    Register(g_userSettings.game.debugFlyCamLockEvents);
    Register(g_userSettings.game.allowBackgroundInput);
    Register(g_userSettings.game.inputLagMs);
    Register(g_userSettings.game.enableLED[0]);
    Register(g_userSettings.game.enableLED[1]);
    Register(g_userSettings.game.enableLED[2]);
    Register(g_userSettings.game.enableLED[3]);
    Register(g_userSettings.game.swapDirectSelect);

    Register(g_userSettings.backend.isoPath);
    Register(g_userSettings.backend.isoVerification);
    Register(g_userSettings.backend.graphicsBackend);
    Register(g_userSettings.backend.skipPreLaunchUI);
    Register(g_userSettings.backend.wasPresetChosen);
    Register(g_userSettings.backend.showPipelineCompilation);
    Register(g_userSettings.backend.checkForUpdates);
    Register(g_userSettings.backend.cardFileType);
    Register(g_userSettings.backend.enableAdvancedSettings);

    Register(g_userSettings.hotkeys.toggleImGuiMenu.key);
    Register(g_userSettings.hotkeys.toggleImGuiMenu.modifiers);
    Register(g_userSettings.hotkeys.toggleImGuiMenu.controllerButton);
    Register(g_userSettings.hotkeys.toggleThirtyFps.key);
    Register(g_userSettings.hotkeys.toggleThirtyFps.modifiers);
    Register(g_userSettings.hotkeys.toggleThirtyFps.controllerButton);
    Register(g_userSettings.hotkeys.turboSpeed.key);
    Register(g_userSettings.hotkeys.turboSpeed.modifiers);
    Register(g_userSettings.hotkeys.turboSpeed.controllerButton);
    Register(g_userSettings.hotkeys.toggleFullscreen.key);
    Register(g_userSettings.hotkeys.toggleFullscreen.modifiers);
    Register(g_userSettings.hotkeys.toggleFullscreen.controllerButton);
    Register(g_userSettings.hotkeys.hideShowImGuiMenu.key);
    Register(g_userSettings.hotkeys.hideShowImGuiMenu.modifiers);
    Register(g_userSettings.hotkeys.hideShowImGuiMenu.controllerButton);
    Register(g_userSettings.hotkeys.processManagement.key);
    Register(g_userSettings.hotkeys.processManagement.modifiers);
    Register(g_userSettings.hotkeys.processManagement.controllerButton);
    Register(g_userSettings.hotkeys.debugOverlay.key);
    Register(g_userSettings.hotkeys.debugOverlay.modifiers);
    Register(g_userSettings.hotkeys.debugOverlay.controllerButton);
    Register(g_userSettings.hotkeys.heapViewer.key);
    Register(g_userSettings.hotkeys.heapViewer.modifiers);
    Register(g_userSettings.hotkeys.heapViewer.controllerButton);
    Register(g_userSettings.hotkeys.playerInfo.key);
    Register(g_userSettings.hotkeys.playerInfo.modifiers);
    Register(g_userSettings.hotkeys.playerInfo.controllerButton);
    Register(g_userSettings.hotkeys.saveEditor.key);
    Register(g_userSettings.hotkeys.saveEditor.modifiers);
    Register(g_userSettings.hotkeys.saveEditor.controllerButton);
    Register(g_userSettings.hotkeys.stateShare.key);
    Register(g_userSettings.hotkeys.stateShare.modifiers);
    Register(g_userSettings.hotkeys.stateShare.controllerButton);
    Register(g_userSettings.hotkeys.debugCamera.key);
    Register(g_userSettings.hotkeys.debugCamera.modifiers);
    Register(g_userSettings.hotkeys.debugCamera.controllerButton);
    Register(g_userSettings.hotkeys.captureCameraKeyframe.key);
    Register(g_userSettings.hotkeys.captureCameraKeyframe.modifiers);
    Register(g_userSettings.hotkeys.captureCameraKeyframe.controllerButton);
    Register(g_userSettings.hotkeys.audioDebug.key);
    Register(g_userSettings.hotkeys.audioDebug.modifiers);
    Register(g_userSettings.hotkeys.audioDebug.controllerButton);
    Register(g_userSettings.hotkeys.useTexturePack.key);
    Register(g_userSettings.hotkeys.useTexturePack.modifiers);
    Register(g_userSettings.hotkeys.useTexturePack.controllerButton);
    Register(g_userSettings.hotkeys.gyroAim.key);
    Register(g_userSettings.hotkeys.gyroAim.modifiers);
    Register(g_userSettings.hotkeys.gyroAim.controllerButton);
    Register(g_userSettings.hotkeys.showInputViewer.key);
    Register(g_userSettings.hotkeys.showInputViewer.modifiers);
    Register(g_userSettings.hotkeys.showInputViewer.controllerButton);
    Register(g_userSettings.hotkeys.moveLink.key);
    Register(g_userSettings.hotkeys.moveLink.modifiers);
    Register(g_userSettings.hotkeys.moveLink.controllerButton);
    Register(g_userSettings.hotkeys.cycleBloomMode.key);
    Register(g_userSettings.hotkeys.cycleBloomMode.modifiers);
    Register(g_userSettings.hotkeys.cycleBloomMode.controllerButton);
    Register(g_userSettings.hotkeys.toggleDiscLoadingDelay.key);
    Register(g_userSettings.hotkeys.toggleDiscLoadingDelay.modifiers);
    Register(g_userSettings.hotkeys.toggleDiscLoadingDelay.controllerButton);

    Register(g_userSettings.actionBindings.firstPersonCamera[0]);
    Register(g_userSettings.actionBindings.firstPersonCamera[1]);
    Register(g_userSettings.actionBindings.firstPersonCamera[2]);
    Register(g_userSettings.actionBindings.firstPersonCamera[3]);
    Register(g_userSettings.actionBindings.callMidna[0]);
    Register(g_userSettings.actionBindings.callMidna[1]);
    Register(g_userSettings.actionBindings.callMidna[2]);
    Register(g_userSettings.actionBindings.callMidna[3]);
    Register(g_userSettings.actionBindings.openMapScreen[0]);
    Register(g_userSettings.actionBindings.openMapScreen[1]);
    Register(g_userSettings.actionBindings.openMapScreen[2]);
    Register(g_userSettings.actionBindings.openMapScreen[3]);
    Register(g_userSettings.actionBindings.toggleMinimap[0]);
    Register(g_userSettings.actionBindings.toggleMinimap[1]);
    Register(g_userSettings.actionBindings.toggleMinimap[2]);
    Register(g_userSettings.actionBindings.toggleMinimap[3]);
    Register(g_userSettings.actionBindings.openDusklightMenu[0]);
    Register(g_userSettings.actionBindings.openDusklightMenu[1]);
    Register(g_userSettings.actionBindings.openDusklightMenu[2]);
    Register(g_userSettings.actionBindings.openDusklightMenu[3]);
    Register(g_userSettings.actionBindings.turboSpeedButton[0]);
    Register(g_userSettings.actionBindings.turboSpeedButton[1]);
    Register(g_userSettings.actionBindings.turboSpeedButton[2]);
    Register(g_userSettings.actionBindings.turboSpeedButton[3]);
}

// Transient settings

static TransientSettings g_transientSettings = {
    .collisionView = {
        .enableTerrainView = false,
        .enableWireframe = false,
        .enableTriggerView = false,
        .enableAtView = false,
        .enableTgView = false,
        .enableCoView = false,
        .terrainViewOpacity = 50.0f,
        .colliderViewOpacity = 50.0f,
        .drawRange = 100.0f,
    },
    .skipFrameRateLimit = false,
    .forceThirtyFpsLimit = false,
    .turboMode = false,
    .moveLinkActive = false,
    .stateShareLoadActive = false,
    .practiceMenuInputCapture = false,
};

TransientSettings& getTransientSettings() {
    return g_transientSettings;
}

void updateDiscLoadingDelay() {
    const int delaySeconds = std::clamp(getSettings().game.discLoadingDelaySeconds.getValue(), 1, 10);
    const auto mode = getSettings().game.speedrunMode.getValue() ? DiscLoadingDelayMode::Off :
                                                                   getSettings().game.discLoadingDelayMode.getValue();

    aurora_dvd_set_read_delay_seconds(static_cast<u32>(delaySeconds));
    const u32 dvdMode = mode == DiscLoadingDelayMode::Off ? AURORA_DVD_READ_DELAY_OFF :
        (mode == DiscLoadingDelayMode::On ? AURORA_DVD_READ_DELAY_BLOCKED :
                                            AURORA_DVD_READ_DELAY_TIMED);
    aurora_dvd_set_read_delay_mode(dvdMode);
}

void toggleDiscLoadingDelay() {
    if (getSettings().game.speedrunMode.getValue()) {
        return;
    }
    auto& mode = getSettings().game.discLoadingDelayMode;
    const auto nextMode = static_cast<u8>(mode.getValue()) >= static_cast<u8>(DiscLoadingDelayMode::Timed)
        ? DiscLoadingDelayMode::Off
        : static_cast<DiscLoadingDelayMode>(static_cast<u8>(mode.getValue()) + 1);
    mode.setValue(nextMode);
    config::save();
    updateDiscLoadingDelay();
}

}
