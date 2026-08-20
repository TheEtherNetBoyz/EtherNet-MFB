#pragma once

#include <array>

#include "dusk/config_var.hpp"
#include "dusk/ui/controls.hpp"

namespace dusk {

using config::ConfigVar;
using config::ActionBindConfigVar;

enum class BloomMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
    Legacy = 3,
};

enum class DepthOfFieldMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class TwilightSkyboxMode : u8 {
    Day = 0,
    Night = 1,
};

enum class TwilightWeather : u8 {
    Current = 0,
    Clear = 1,
    Rain = 2,
    Snow = 3,
    Lightning = 4,
    WindStorm = 5,
};

enum class Resampler : int {
    Bilinear = 0,
    Area = 1,
    Composite = 2,
};

enum class GameLanguage : u8 {
    English = OS_LANGUAGE_ENGLISH,
    German = OS_LANGUAGE_GERMAN,
    French = OS_LANGUAGE_FRENCH,
    Spanish = OS_LANGUAGE_SPANISH,
    Italian = OS_LANGUAGE_ITALIAN,
    Japanese = 6,
};

enum class DiscVerificationState : u8 {
    Unknown = 0,
    Success,
    HashMismatch,
};

enum class GyroMode : u8 {
    Sensor = 0,
    Mouse = 1,
};

enum class AspectRatioMode : int {
    Off = 0,
    Ratio16x9 = 1,
    Ratio21x9 = 2,
    Ratio3x2 = 3,
};

enum class FrameInterpMode : u8 {
    Off = 0,
    Capped = 1,
    Unlimited = 2,
};

enum HotkeyModifier : int {
    HOTKEY_MOD_NONE = 0,
    HOTKEY_MOD_CTRL = 1 << 0,
    HOTKEY_MOD_SHIFT = 1 << 1,
    HOTKEY_MOD_ALT = 1 << 2,
};

enum class TouchTargeting : u8 {
    Hybrid = 0,
    Hold = 1,
    Switch = 2,
};

enum class MenuScaling : u8 {
    GameCube = 0,
    Wii = 1,
    Dusklight = 2,
};

enum class MagicArmorMode : u8 {
    NORMAL = 0,
    ON_DAMAGE = 1,
    DOUBLE_DEFENSE = 2,
    INVINCIBLE = 3,
    COSMETIC = 4,
};

enum class DiscLoadingDelayMode : u8 {
    Off = 0,
    On = 1,
    Timed = 2,
};

namespace config {
template <>
struct ConfigEnumRange<BloomMode> {
    static constexpr auto min = BloomMode::Off;
    static constexpr auto max = BloomMode::Legacy;
};

template <>
struct ConfigEnumRange<DepthOfFieldMode> {
    static constexpr auto min = DepthOfFieldMode::Off;
    static constexpr auto max = DepthOfFieldMode::Dusk;
};

template <>
struct ConfigEnumRange<TwilightSkyboxMode> {
    static constexpr auto min = TwilightSkyboxMode::Day;
    static constexpr auto max = TwilightSkyboxMode::Night;
};

template <>
struct ConfigEnumRange<TwilightWeather> {
    static constexpr auto min = TwilightWeather::Current;
    static constexpr auto max = TwilightWeather::WindStorm;
};

template <>
struct ConfigEnumRange<Resampler> {
    static constexpr auto min = Resampler::Bilinear;
    static constexpr auto max = Resampler::Composite;
};

template <>
struct ConfigEnumRange<GameLanguage> {
    static constexpr auto min = GameLanguage::English;
    static constexpr auto max = GameLanguage::Japanese;
};

template <>
struct ConfigEnumRange<DiscVerificationState> {
    static constexpr auto min = DiscVerificationState::Unknown;
    static constexpr auto max = DiscVerificationState::HashMismatch;
};

template <>
struct ConfigEnumRange<GyroMode> {
    static constexpr auto min = GyroMode::Sensor;
    static constexpr auto max = GyroMode::Mouse;
};

template <>
struct ConfigEnumRange<AspectRatioMode> {
    static constexpr auto min = AspectRatioMode::Off;
    static constexpr auto max = AspectRatioMode::Ratio3x2;
};

template <>
struct ConfigEnumRange<FrameInterpMode> {
    static constexpr auto min = FrameInterpMode::Off;
    static constexpr auto max = FrameInterpMode::Unlimited;
};

template <>
struct ConfigEnumRange<TouchTargeting> {
    static constexpr auto min = TouchTargeting::Hybrid;
    static constexpr auto max = TouchTargeting::Switch;
};

template <>
struct ConfigEnumRange<MenuScaling> {
    static constexpr auto min = MenuScaling::GameCube;
    static constexpr auto max = MenuScaling::Dusklight;
};

template <>
struct ConfigEnumRange<MagicArmorMode> {
    static constexpr auto min = MagicArmorMode::NORMAL;
    static constexpr auto max = MagicArmorMode::COSMETIC;
};

template <>
struct ConfigEnumRange<DiscLoadingDelayMode> {
    static constexpr auto min = DiscLoadingDelayMode::Off;
    static constexpr auto max = DiscLoadingDelayMode::Timed;
};

template <>
struct ConfigValueTraits<ui::ControlLayout> {
    static constexpr bool enabled = true;
};
}  // namespace config

// Persistent user settings

struct UserSettings {
    struct HotkeyBinding {
        ConfigVar<int> key;
        ConfigVar<int> modifiers;
        ConfigVar<int> controllerButton;
    };

    // Program settings

    struct {
        // Video
        ConfigVar<bool> enableFullscreen;
        ConfigVar<bool> enableVsync;
        ConfigVar<bool> lockAspectRatio;
        ConfigVar<AspectRatioMode> forcedAspectRatio;
        ConfigVar<bool> enableFpsOverlay;
        ConfigVar<int> fpsOverlayCorner;
        ConfigVar<int> maxFrameRate;
        ConfigVar<bool> rememberWindowSize;
        ConfigVar<int> lastWindowWidth;
        ConfigVar<int> lastWindowHeight;
    } video;

    struct {
        ConfigVar<std::string> settingsFavorites;
        ConfigVar<int> menuWidthDp;
        ConfigVar<int> menuHeightDp;
    } ui;

    struct {
        // Audio
        ConfigVar<int> masterVolume;
        ConfigVar<int> mainMusicVolume;
        ConfigVar<int> subMusicVolume;
        ConfigVar<int> soundEffectsVolume;
        ConfigVar<int> fanfareVolume;
        ConfigVar<bool> enableReverb;
        ConfigVar<bool> enableHrtf;
        ConfigVar<bool> menuSounds;
    } audio;

    // Game settings

    struct {
        ConfigVar<GameLanguage> language;

        // QoL
        ConfigVar<bool> enableQuickTransform;
        ConfigVar<bool> humanMidnaWarp;
        ConfigVar<bool> hideTvSettingsScreen;
        ConfigVar<bool> biggerWallets;
        ConfigVar<bool> noReturnRupees;
        ConfigVar<bool> disableRupeeCutscenes;
        ConfigVar<bool> skipAllCutscenes;
        ConfigVar<bool> noSwordRecoil;
        ConfigVar<int> damageMultiplier;
        ConfigVar<bool> noHeartDrops;
        ConfigVar<bool> instantDeath;
        ConfigVar<bool> fastClimbing;
        ConfigVar<bool> noMissClimbing;
        ConfigVar<bool> fastTears;
        ConfigVar<bool> no2ndFishForCat;
        ConfigVar<bool> enableFastLoads;
        ConfigVar<bool> enableInstaLoads;
        ConfigVar<DiscLoadingDelayMode> discLoadingDelayMode;
        ConfigVar<int> discLoadingDelaySeconds;
        ConfigVar<bool> theEtherNetBoyzExperience;
        ConfigVar<bool> instantMovement;
        ConfigVar<bool> buttonFishing;
        ConfigVar<bool> instantSaves;
        ConfigVar<bool> instantText;
        ConfigVar<bool> sunsSong;
        ConfigVar<bool> autoSave;
        ConfigVar<bool> enhancedMapMenus;
        ConfigVar<bool> aimingReticle;

        // Preferences
        ConfigVar<bool> enableMirrorMode;
        ConfigVar<bool> minimalHUD;
        ConfigVar<float> hudScale;
        ConfigVar<bool> pauseOnFocusLost;
        ConfigVar<bool> enableLinkDollRotation;
        ConfigVar<bool> enableAchievementToasts;
        ConfigVar<bool> enableControllerToasts;
        ConfigVar<bool> enableDiscordPresence;
        ConfigVar<MenuScaling> menuScalingMode;

        // Graphics
        ConfigVar<BloomMode> bloomMode;
        ConfigVar<float> bloomMultiplier;
        ConfigVar<DepthOfFieldMode> depthOfFieldMode;
        ConfigVar<bool> disableWaterRefraction;
        ConfigVar<bool> enableTextureReplacements;
        ConfigVar<FrameInterpMode> enableFrameInterpolation;
        ConfigVar<int> frameRateLimit;
        ConfigVar<bool> lowLatencyPresentation;
        ConfigVar<int> internalResolutionScale;
        ConfigVar<int> shadowResolutionMultiplier;
        ConfigVar<Resampler> resampler;
        ConfigVar<bool> enableTwilightVisuals;
        ConfigVar<float> twilightVisualBrightness;
        ConfigVar<TwilightSkyboxMode> twilightSkyboxMode;
        ConfigVar<TwilightWeather> twilightWeather;
        ConfigVar<bool> enableMapBackground;
        ConfigVar<bool> disableCutscenePillarboxing;
        ConfigVar<bool> enableHighQualityMinimapTextures;

        // Audio
        ConfigVar<bool> noLowHpSound;
        ConfigVar<bool> midnasLamentNonStop;

        // Input
        ConfigVar<bool> enableGyroAim;
        ConfigVar<bool> enableGyroRollgoal;
        ConfigVar<float> gyroSensitivityX;
        ConfigVar<float> gyroSensitivityY;
        ConfigVar<float> gyroSensitivityRollgoal;
        ConfigVar<float> gyroSmoothing;
        ConfigVar<float> gyroDeadband;
        ConfigVar<bool> gyroInvertPitch;
        ConfigVar<bool> gyroInvertYaw;
        ConfigVar<bool> enableMouseCamera;
        ConfigVar<bool> enableMouseAim;
        ConfigVar<float> mouseAimSensitivity;
        ConfigVar<float> mouseCameraSensitivity;
        ConfigVar<bool> invertMouseY;
        ConfigVar<bool> freeCamera;
        ConfigVar<bool> enableTouchControls;
        ConfigVar<TouchTargeting> touchTargeting;
        ConfigVar<bool> enableMenuPointer;
        ConfigVar<ui::ControlLayout> touchControlsLayout;
        ConfigVar<bool> invertCameraXAxis;
        ConfigVar<bool> invertCameraYAxis;
        ConfigVar<bool> invertFirstPersonXAxis;
        ConfigVar<bool> invertFirstPersonYAxis;
        ConfigVar<bool> invertAirSwimX;
        ConfigVar<bool> invertAirSwimY;
        ConfigVar<float> freeCameraSensitivity;
        ConfigVar<bool> enableCameraSpeedControls;
        ConfigVar<float> aimingCameraSensitivityLevel;
        ConfigVar<float> freeCameraSensitivityLevel;
        ConfigVar<float> regularCameraSensitivityLevel;
        ConfigVar<float> freeCameraXSensitivity;
        ConfigVar<float> freeCameraYSensitivity;
        ConfigVar<float> touchCameraXSensitivity;
        ConfigVar<float> touchCameraYSensitivity;
        ConfigVar<bool> debugFlyCam;
        ConfigVar<bool> debugFlyCamLockEvents;
        ConfigVar<bool> allowBackgroundInput;
        ConfigVar<int> inputLagMs;
        std::array<ConfigVar<bool>, 4> enableLED;
        ConfigVar<bool> swapDirectSelect;

        // Cheats
        ConfigVar<bool> infiniteHearts;
        ConfigVar<bool> infiniteArrows;
        ConfigVar<bool> infiniteSeeds;
        ConfigVar<bool> infiniteBombs;
        ConfigVar<bool> infiniteOil;
        ConfigVar<bool> infiniteOxygen;
        ConfigVar<bool> infiniteRupees;
        ConfigVar<bool> enableIndefiniteItemDrops;
        ConfigVar<bool> moonJump;
        ConfigVar<bool> superClawshot;
        ConfigVar<bool> alwaysGreatspin;
        ConfigVar<bool> enableFastIronBoots;
        ConfigVar<bool> canTransformAnywhere;
        ConfigVar<bool> fastRoll;
        ConfigVar<bool> fastSpinner;
        ConfigVar<MagicArmorMode> armorRupeeDrain;
        ConfigVar<bool> invincibleEnemies;
        ConfigVar<bool> transformWithoutShadowCrystal;

        // Technical
        ConfigVar<bool> restoreWiiGlitches;
        ConfigVar<bool> usePpcFastInvSqrt;

        // Controls
        ConfigVar<bool> enableTurboKeybind;
        ConfigVar<bool> enableResetKeybind;

        // Tools
        ConfigVar<bool> speedrunMode;
        ConfigVar<bool> liveSplitEnabled;
        ConfigVar<bool> showSpeedrunRTATimer;
        ConfigVar<bool> moveLink;
        ConfigVar<bool> teleportLink;
        ConfigVar<bool> areaReload;
        ConfigVar<bool> gorgeVoidChecker;
        ConfigVar<bool> recordingMode;
        ConfigVar<bool> removeQuestMapMarkers;
        ConfigVar<bool> showInputViewer;
        ConfigVar<bool> showInputViewerGyro;
        ConfigVar<bool> nativeInputViewer;
        ConfigVar<bool> nativeLinkDebugInfo;
        ConfigVar<std::string> triggerViewDefinitions;
        // When true, the practice-tools menu renders natively (J2D/GX), which
        // scales with resolution but is controller-only. When false, it uses
        // the imgui menu (mouse-capable).
        ConfigVar<bool> nativePracticeMenu;
    } game;

    struct {
        ConfigVar<std::string> isoPath;
        ConfigVar<DiscVerificationState> isoVerification;
        ConfigVar<std::string> graphicsBackend;
        ConfigVar<bool> skipPreLaunchUI;
        ConfigVar<bool> wasPresetChosen;
        ConfigVar<bool> showPipelineCompilation;
        ConfigVar<bool> checkForUpdates;
        ConfigVar<int> cardFileType;
        ConfigVar<bool> enableAdvancedSettings;
    } backend;

    struct {
        HotkeyBinding toggleImGuiMenu;
        HotkeyBinding toggleThirtyFps;
        HotkeyBinding turboSpeed;
        HotkeyBinding toggleFullscreen;
        HotkeyBinding hideShowImGuiMenu;
        HotkeyBinding processManagement;
        HotkeyBinding debugOverlay;
        HotkeyBinding heapViewer;
        HotkeyBinding playerInfo;
        HotkeyBinding saveEditor;
        HotkeyBinding stateShare;
        HotkeyBinding debugCamera;
        HotkeyBinding captureCameraKeyframe;
        HotkeyBinding audioDebug;
        HotkeyBinding useTexturePack;
        HotkeyBinding gyroAim;
        HotkeyBinding showInputViewer;
        HotkeyBinding moveLink;
        HotkeyBinding cycleBloomMode;
        HotkeyBinding toggleDiscLoadingDelay;
    } hotkeys;

    // Arrays of size 4 for 4 ports
    struct {
        std::array<ActionBindConfigVar, 4> firstPersonCamera;
        std::array<ActionBindConfigVar, 4> callMidna;
        std::array<ActionBindConfigVar, 4> openMapScreen;
        std::array<ActionBindConfigVar, 4> toggleMinimap;
        std::array<ActionBindConfigVar, 4> openDusklightMenu;
        std::array<ActionBindConfigVar, 4> turboSpeedButton;
    } actionBindings;
};

UserSettings& getSettings();

void registerSettings();

// Transient settings

struct CollisionViewSettings {
    bool enableTerrainView;
    bool enableWireframe;
    bool enableTriggerView;
    bool enableAtView;
    bool enableTgView;
    bool enableCoView;
    float terrainViewOpacity;
    float colliderViewOpacity;
    float drawRange;
};

struct TransientSettings {
    CollisionViewSettings collisionView;
    bool skipFrameRateLimit;
    bool forceThirtyFpsLimit;
    bool turboMode;
    bool moveLinkActive;
    bool stateShareLoadActive;
    bool practiceMenuInputCapture;
};

TransientSettings& getTransientSettings();

void updateDiscLoadingDelay();
void toggleDiscLoadingDelay();

}  // namespace dusk
