#include "settings.hpp"

#include "aurora/gfx.h"
#include "bool_button.hpp"
#include "controller_config.hpp"
#include "dusk/app_info.hpp"
#include "dusk/audio/DuskAudioSystem.h"
#include "dusk/audio/DuskDsp.hpp"
#include "dusk/config.hpp"
#include "dusk/hotkeys.h"
#include "dusk/data.hpp"
#include "dusk/imgui/ImGuiEngine.hpp"
#include "dusk/io.hpp"
#include "dusk/language.hpp"
#include "dusk/presentation.hpp"
#include <borealis/io.hpp>
#include <borealis/file_select.hpp>
#include "dusk/livesplit.h"
#include "dusk/main.h"
#include "dusk/discord_presence.hpp"
#include "dusk/vector_rsqrt.h"
#include "dusk/speedrun.h"
#include "graphics_tuner.hpp"
#include "m_Do/m_Do_main.h"
#include "menu_bar.hpp"
#include "modal.hpp"
#include "number_button.hpp"
#include "menu_bar.hpp"
#include "pane.hpp"
#include "prelaunch.hpp"
#include "touch_controls_editor.hpp"
#include "ui.hpp"

#include <aurora/lib/window.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <fmt/format.h>

#if BOREALIS_HAS_SENTRY
#include <borealis/sentry.hpp>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <optional>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(TARGET_ANDROID) || defined(__ANDROID__) || \
    (defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
#define TOUCH_CONTROLS_AVAILABLE true
#else
#define TOUCH_CONTROLS_AVAILABLE false
#endif

namespace dusk::ui {
namespace {

class FloatButton : public BaseStringButton {
public:
    struct Props {
        Rml::String key;
        std::function<float()> getValue;
        std::function<void(float)> setValue;
        std::function<bool()> isDisabled;
        std::function<bool()> isModified;
        float min = 0.0f;
        float max = 1.0f;
        float step = 0.1f;
    };

    FloatButton(Rml::Element* parent, Props props)
        : BaseStringButton(parent, {.key = std::move(props.key), .type = "number"}),
          mGetValue(std::move(props.getValue)), mSetValue(std::move(props.setValue)),
          mIsDisabled(std::move(props.isDisabled)), mIsModified(std::move(props.isModified)),
          mMin(props.min), mMax(props.max), mStep(props.step) {}

    bool modified() const override {
        if (mIsModified) {
            return mIsModified();
        }
        return BaseStringButton::modified();
    }

    bool disabled() const override {
        if (mIsDisabled) {
            return mIsDisabled();
        }
        return BaseStringButton::disabled();
    }

protected:
    Rml::String format_value() override {
        return fmt::format("{:.1f}", mGetValue());
    }

    Rml::String input_value() override {
        return fmt::format("{:.1f}", mGetValue());
    }

    void set_value(Rml::String value) override {
        if (!mSetValue) {
            return;
        }

        float parsedValue = 0.0f;
        const char* begin = value.data();
        const char* end = begin + value.size();
        char* parseEnd = nullptr;
        parsedValue = std::strtof(begin, &parseEnd);
        if (parseEnd == begin || parseEnd != end) {
            return;
        }

        mSetValue(std::clamp(parsedValue, mMin, mMax));
    }

    bool handle_nav_command(NavCommand cmd) override {
        if (!is_editing() && (cmd == NavCommand::Left || cmd == NavCommand::Right)) {
            const float newValue = std::clamp(
                mGetValue() + (cmd == NavCommand::Right ? mStep : -mStep), mMin, mMax);
            if (newValue != mGetValue()) {
                mSetValue(newValue);
                mDoAud_seStartMenu(kSoundItemChange);
            }
            return true;
        }
        return BaseStringButton::handle_nav_command(cmd);
    }

private:
    std::function<float()> mGetValue;
    std::function<void(float)> mSetValue;
    std::function<bool()> mIsDisabled;
    std::function<bool()> mIsModified;
    float mMin;
    float mMax;
    float mStep;
};

constexpr std::array kLanguageNames = {
    "English",
    "German",
    "French",
    "Spanish",
    "Italian",
};
constexpr std::array kCardFileTypes = {
    "Card Image",
    "GCI Folder",
};

constexpr std::array kFpsOverlayCornerNames = {
    "Top Left",
    "Top Right",
    "Bottom Left",
    "Bottom Right",
};

constexpr std::array kInterpolationModes = {
    "Off",
    "Capped",
    "Unlimited",
};

constexpr std::array kTouchTargetingLabels = {
    "Hybrid",
    "Hold",
    "Switch",
};

constexpr std::array kTouchTargetingDescriptions = {
    "Tap once to lock on when a target is found. Double-tap when none is found to hold L.",
    "L stays held only while your finger is on the button.",
    "Tap L to keep it held. Tap again to release it.",
};

constexpr std::array kGyroInputModeLabels = {
    "Sensor",
    "Mouse",
};

enum class HotkeyAction {
    ToggleImGuiMenu,
    ToggleThirtyFps,
    TurboSpeed,
    ToggleFullscreen,
    HideShowImGuiMenu,
    ProcessManagement,
    DebugOverlay,
    HeapViewer,
    PlayerInfo,
    SaveEditor,
    StateShare,
    DebugCamera,
    CaptureCameraKeyframe,
    AudioDebug,
    UseTexturePack,
    GyroAim,
    ShowInputViewer,
    MoveLink,
    CycleBloomMode,
};

struct HotkeyEntry {
    HotkeyAction action;
    const char* label;
    const char* helpText;
};

constexpr std::array kHotkeyEntries = {
    HotkeyEntry{HotkeyAction::ToggleImGuiMenu, "Toggle ImGui Menu", "Show or hide the ImGui menu bar."},
    HotkeyEntry{HotkeyAction::ToggleThirtyFps, "Toggle 30 FPS Cap", "Force 30 FPS mode until toggled again."},
    HotkeyEntry{HotkeyAction::TurboSpeed, "Turbo Speed Key", "Hold to use turbo frame pacing."},
    HotkeyEntry{HotkeyAction::ToggleFullscreen, "Toggle Fullscreen", "Switch between windowed and fullscreen."},
    HotkeyEntry{HotkeyAction::HideShowImGuiMenu, "Hide/Show ImGui Menu", "Alternate shortcut for showing or hiding the ImGui menu bar."},
    HotkeyEntry{HotkeyAction::ProcessManagement, "Process Management", "Show process and task information."},
    HotkeyEntry{HotkeyAction::DebugOverlay, "Debug Overlay", "Show frame, backend, and renderer statistics."},
    HotkeyEntry{HotkeyAction::HeapViewer, "Heap Viewer", "Show memory heap information."},
    HotkeyEntry{HotkeyAction::PlayerInfo, "Player Info", "Show Link and Epona position, angle, and speed."},
    HotkeyEntry{HotkeyAction::SaveEditor, "Save Editor", "Open the save editor."},
    HotkeyEntry{HotkeyAction::StateShare, "State Share", "Open the state share window."},
    HotkeyEntry{HotkeyAction::DebugCamera, "Debug Camera", "Show the developer camera tools."},
    HotkeyEntry{HotkeyAction::CaptureCameraKeyframe, "Capture Camera Keyframe", "Capture the current Fly Mode camera transform."},
    HotkeyEntry{HotkeyAction::AudioDebug, "Audio Debug", "Open the audio debug window."},
    HotkeyEntry{HotkeyAction::UseTexturePack, "Use Texture Pack", "Enable or disable texture replacements."},
    HotkeyEntry{HotkeyAction::GyroAim, "Gyro Aim", "Enable or disable gyro aiming for supported actions."},
    HotkeyEntry{HotkeyAction::ShowInputViewer, "Show Input Viewer", "Show or hide the controller input overlay."},
    HotkeyEntry{HotkeyAction::MoveLink, "Move Link", "Allow or block the Move Link activation combo."},
    HotkeyEntry{HotkeyAction::CycleBloomMode, "Cycle Bloom Mode", "Cycle through Off, Classic, Dusklight, and Legacy bloom modes."},
};

UserSettings::HotkeyBinding& hotkey_binding(HotkeyAction action) {
    auto& hotkeys = getSettings().hotkeys;
    switch (action) {
    case HotkeyAction::ToggleImGuiMenu:
        return hotkeys.toggleImGuiMenu;
    case HotkeyAction::ToggleThirtyFps:
        return hotkeys.toggleThirtyFps;
    case HotkeyAction::TurboSpeed:
        return hotkeys.turboSpeed;
    case HotkeyAction::ToggleFullscreen:
        return hotkeys.toggleFullscreen;
    case HotkeyAction::HideShowImGuiMenu:
        return hotkeys.hideShowImGuiMenu;
    case HotkeyAction::ProcessManagement:
        return hotkeys.processManagement;
    case HotkeyAction::DebugOverlay:
        return hotkeys.debugOverlay;
    case HotkeyAction::HeapViewer:
        return hotkeys.heapViewer;
    case HotkeyAction::PlayerInfo:
        return hotkeys.playerInfo;
    case HotkeyAction::SaveEditor:
        return hotkeys.saveEditor;
    case HotkeyAction::StateShare:
        return hotkeys.stateShare;
    case HotkeyAction::DebugCamera:
        return hotkeys.debugCamera;
    case HotkeyAction::CaptureCameraKeyframe:
        return hotkeys.captureCameraKeyframe;
    case HotkeyAction::AudioDebug:
        return hotkeys.audioDebug;
    case HotkeyAction::UseTexturePack:
        return hotkeys.useTexturePack;
    case HotkeyAction::GyroAim:
        return hotkeys.gyroAim;
    case HotkeyAction::ShowInputViewer:
        return hotkeys.showInputViewer;
    case HotkeyAction::MoveLink:
        return hotkeys.moveLink;
    case HotkeyAction::CycleBloomMode:
        return hotkeys.cycleBloomMode;
    }
    return hotkeys.toggleImGuiMenu;
}

int current_hotkey_modifiers() {
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    int modifiers = HOTKEY_MOD_NONE;
    if (keys == nullptr) {
        return modifiers;
    }
    if ((SDL_SCANCODE_LCTRL < keyCount && keys[SDL_SCANCODE_LCTRL]) ||
        (SDL_SCANCODE_RCTRL < keyCount && keys[SDL_SCANCODE_RCTRL])) {
        modifiers |= HOTKEY_MOD_CTRL;
    }
    if ((SDL_SCANCODE_LSHIFT < keyCount && keys[SDL_SCANCODE_LSHIFT]) ||
        (SDL_SCANCODE_RSHIFT < keyCount && keys[SDL_SCANCODE_RSHIFT])) {
        modifiers |= HOTKEY_MOD_SHIFT;
    }
    if ((SDL_SCANCODE_LALT < keyCount && keys[SDL_SCANCODE_LALT]) ||
        (SDL_SCANCODE_RALT < keyCount && keys[SDL_SCANCODE_RALT])) {
        modifiers |= HOTKEY_MOD_ALT;
    }
    return modifiers;
}

bool is_modifier_scancode(int scancode) {
    return scancode == SDL_SCANCODE_LCTRL || scancode == SDL_SCANCODE_RCTRL ||
           scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT ||
           scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT;
}

bool keyboard_neutral() {
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    if (keys == nullptr) {
        return true;
    }
    for (int i = 0; i < keyCount; ++i) {
        if (keys[i]) {
            return false;
        }
    }
    return true;
}

bool controller_neutral() {
    for (int port = PAD_CHAN0; port < PAD_CHANMAX; ++port) {
        if (PADGetNativeButtonPressed(port) != -1) {
            return false;
        }
    }
    return true;
}

bool hotkey_input_neutral() {
    return keyboard_neutral() && controller_neutral();
}

int keyboard_key_pressed() {
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    if (keys == nullptr) {
        return SDL_SCANCODE_UNKNOWN;
    }
    for (int i = 1; i < keyCount; ++i) {
        if (keys[i] && !is_modifier_scancode(i)) {
            return i;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

int controller_button_pressed() {
    for (int port = PAD_CHAN0; port < PAD_CHANMAX; ++port) {
        const int button = PADGetNativeButtonPressed(port);
        if (button != -1) {
            return button;
        }
    }
    return static_cast<int>(PAD_NATIVE_BUTTON_INVALID);
}

void clear_hotkey_binding(UserSettings::HotkeyBinding& binding) {
    binding.key.setValue(SDL_SCANCODE_UNKNOWN);
    binding.modifiers.setValue(HOTKEY_MOD_NONE);
    binding.controllerButton.setValue(PAD_NATIVE_BUTTON_INVALID);
}

Rml::String hotkey_binding_name(const UserSettings::HotkeyBinding& binding) {
    const int scancode = binding.key.getValue();
    const int controllerButton = binding.controllerButton.getValue();
    if (scancode == SDL_SCANCODE_UNKNOWN &&
        static_cast<u32>(controllerButton) == PAD_NATIVE_BUTTON_INVALID) {
        return "Unbound";
    }

    Rml::String out;
    if (scancode != SDL_SCANCODE_UNKNOWN) {
        const int modifiers = binding.modifiers.getValue();
        if (modifiers & HOTKEY_MOD_CTRL) {
            out += "Ctrl+";
        }
        if (modifiers & HOTKEY_MOD_SHIFT) {
            out += "Shift+";
        }
        if (modifiers & HOTKEY_MOD_ALT) {
            out += "Alt+";
        }

        if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT) {
            out += "Unknown";
        } else {
            const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
            out += name != nullptr && name[0] != '\0' ? name : "Unknown";
        }
    }

    if (static_cast<u32>(controllerButton) != PAD_NATIVE_BUTTON_INVALID) {
        if (!out.empty()) {
            out += " / ";
        }
        out += native_button_name(nullptr, static_cast<u32>(controllerButton));
        return out;
    }

    return out;
}

class HotkeyConfigWindow : public Window {
public:
    HotkeyConfigWindow() {
        add_tab("Configure Hotkeys", [this](Rml::Element* content) {
            auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
            auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

            leftPane.add_section("Hotkeys");
            for (const auto& entry : kHotkeyEntries) {
                auto& button = leftPane.add_select_button({
                    .key = entry.label,
                    .getValue = [this, action = entry.action] {
                        return mPendingHotkey == action ? Rml::String{"Press key..."} :
                                                          hotkey_binding_name(hotkey_binding(action));
                    },
                    .isModified = [action = entry.action] {
                        const auto& binding = hotkey_binding(action);
                        return binding.key.getValue() != binding.key.getDefaultValue() ||
                               binding.modifiers.getValue() != binding.modifiers.getDefaultValue() ||
                               binding.controllerButton.getValue() != binding.controllerButton.getDefaultValue();
                    },
                });
                button.on_pressed([this, action = entry.action] {
                    mPendingHotkey = action;
                    mSuppressCaptureUntilNeutral = true;
                });
                leftPane.register_control(
                    button,
                    rightPane, [helpText = entry.helpText](Pane& pane) {
                        pane.add_text(helpText);
                        pane.add_rml("<br/>Press A/Enter to bind a new key or controller button."
                                     " Press Escape while binding to clear it.");
                    });
            }
        });
    }

    void update() override {
        poll_pending_hotkey();
        Window::update();
    }

private:
    void poll_pending_hotkey() {
        if (!mPendingHotkey.has_value()) {
            return;
        }

        if (mSuppressCaptureUntilNeutral) {
            if (!hotkey_input_neutral()) {
                return;
            }
            mSuppressCaptureUntilNeutral = false;
        }

        int keyCount = 0;
        const bool* keys = SDL_GetKeyboardState(&keyCount);
        if (keys != nullptr && SDL_SCANCODE_ESCAPE < keyCount && keys[SDL_SCANCODE_ESCAPE]) {
            auto& binding = hotkey_binding(*mPendingHotkey);
            clear_hotkey_binding(binding);
            config::save();
            mPendingHotkey.reset();
            return;
        }

        const int controllerButton = controller_button_pressed();
        if (static_cast<u32>(controllerButton) != PAD_NATIVE_BUTTON_INVALID) {
            auto& binding = hotkey_binding(*mPendingHotkey);
            binding.key.setValue(SDL_SCANCODE_UNKNOWN);
            binding.modifiers.setValue(HOTKEY_MOD_NONE);
            binding.controllerButton.setValue(controllerButton);
            config::save();
            mPendingHotkey.reset();
            return;
        }

        const int scancode = keyboard_key_pressed();
        if (scancode == SDL_SCANCODE_UNKNOWN) {
            return;
        }

        auto& binding = hotkey_binding(*mPendingHotkey);
        binding.key.setValue(scancode);
        binding.modifiers.setValue(current_hotkey_modifiers());
        binding.controllerButton.setValue(PAD_NATIVE_BUTTON_INVALID);
        config::save();
        mPendingHotkey.reset();
    }

    std::optional<HotkeyAction> mPendingHotkey;
    bool mSuppressCaptureUntilNeutral = false;
};

constexpr std::array kMenuScalingModeLabels = {
    "GameCube",
    "Wii",
    "Dusklight",
};

constexpr std::array kMagicArmorModes = {
    "Normal",
    "On Damage",
    "Double Defense",
    "Invincible",
    "Cosmetic",
};

bool try_parse_backend(std::string_view backend, AuroraBackend& outBackend) {
    if (backend == "auto") {
        outBackend = BACKEND_AUTO;
        return true;
    }
    if (backend == "d3d11") {
        outBackend = BACKEND_D3D11;
        return true;
    }
    if (backend == "d3d12") {
        outBackend = BACKEND_D3D12;
        return true;
    }
    if (backend == "metal") {
        outBackend = BACKEND_METAL;
        return true;
    }
    if (backend == "vulkan") {
        outBackend = BACKEND_VULKAN;
        return true;
    }
    if (backend == "opengl") {
        outBackend = BACKEND_OPENGL;
        return true;
    }
    if (backend == "opengles") {
        outBackend = BACKEND_OPENGLES;
        return true;
    }
    if (backend == "webgpu") {
        outBackend = BACKEND_WEBGPU;
        return true;
    }
    if (backend == "null") {
        outBackend = BACKEND_NULL;
        return true;
    }

    return false;
}

std::string_view backend_name(AuroraBackend backend) {
    switch (backend) {
    default:
        return "Auto";
    case BACKEND_D3D12:
        return "D3D12";
    case BACKEND_D3D11:
        return "D3D11";
    case BACKEND_METAL:
        return "Metal";
    case BACKEND_VULKAN:
        return "Vulkan";
    case BACKEND_OPENGL:
        return "OpenGL";
    case BACKEND_OPENGLES:
        return "OpenGL ES";
    case BACKEND_WEBGPU:
        return "WebGPU";
    case BACKEND_NULL:
        return "Null";
    }
}

std::string_view backend_id(AuroraBackend backend) {
    switch (backend) {
    default:
        return "auto";
    case BACKEND_D3D12:
        return "d3d12";
    case BACKEND_D3D11:
        return "d3d11";
    case BACKEND_METAL:
        return "metal";
    case BACKEND_VULKAN:
        return "vulkan";
    case BACKEND_OPENGL:
        return "opengl";
    case BACKEND_OPENGLES:
        return "opengles";
    case BACKEND_WEBGPU:
        return "webgpu";
    case BACKEND_NULL:
        return "null";
    }
}

std::vector<AuroraBackend> available_backends() {
    std::vector<AuroraBackend> backends;
    backends.emplace_back(BACKEND_AUTO);
    size_t backendCount = 0;
    const AuroraBackend* raw = aurora_get_available_backends(&backendCount);
    for (size_t i = 0; i < backendCount; ++i) {
        // Do not expose NULL or D3D11
        if (raw[i] != BACKEND_NULL && raw[i] != BACKEND_D3D11) {
            backends.emplace_back(raw[i]);
        }
    }
    return backends;
}

AuroraBackend configured_backend() {
    AuroraBackend configuredBackend = BACKEND_AUTO;
    const auto configuredId = getSettings().backend.graphicsBackend.getValue();
    if (!try_parse_backend(configuredId, configuredBackend)) {
        configuredBackend = BACKEND_AUTO;
    }
    return configuredBackend;
}

bool is_graphics_backend_restart_pending() {
    return getSettings().backend.graphicsBackend.getValue() !=
           prelaunch_state().initialGraphicsBackend;
}

Rml::String graphics_backend_display_name() {
    if (is_graphics_backend_restart_pending()) {
        return Rml::String{backend_name(configured_backend())};
    }
    return Rml::String{backend_name(aurora_get_backend())};
}

Rml::String configured_data_path_display_name() {
    const auto path = data::abbreviated_path_string(data::configured_data_path());
    if (path.empty()) {
        return "(none)";
    }

    auto display = borealis::file_select::display_name(path);
    if (display.empty()) {
        return path;
    }
    return display;
}

class DataFolderPathText : public Component {
public:
    explicit DataFolderPathText(Rml::Element* parent) : Component(append(parent, "div")) {}

    void update() override {
        const Rml::String rml =
            "<span class=\"data-folder-current\">Current data folder:<br/>" +
            escape(data::abbreviated_path_string(data::configured_data_path())) + "</span>";
        if (rml != mCurrentRml) {
            mRoot->SetInnerRML(rml);
            mCurrentRml = rml;
        }
        Component::update();
    }

private:
    Rml::String mCurrentRml;
};

void show_data_folder_error_modal(std::string_view message) {
    auto dismiss = [](Modal& modal) {
        mDoAud_seStartMenu(kSoundWindowClose);
        modal.pop();
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = "Data Folder Not Changed",
        .bodyRml = escape(message),
        .actions =
            {
                ModalAction{
                    .label = "OK",
                    .onPressed = dismiss,
                },
            },
        .onDismiss = dismiss,
        .icon = "warning",
    }));
    if (auto* doc = top_document()) {
        doc->focus();
    }
}

void data_folder_dialog_callback(borealis::file_select::Result result) {
    if (result.status == borealis::file_select::Status::Canceled) {
        return;
    }
    if (result.status != borealis::file_select::Status::Selected || result.locations.empty()) {
        show_data_folder_error_modal("Dusklight could not open the folder picker.");
        return;
    }

    std::string dataPathError;
    if (data::set_custom_data_path(result.locations.front(), &dataPathError)) {
        mDoAud_seStartMenu(kSoundItemChange);
        return;
    }

    if (dataPathError.empty()) {
        dataPathError =
            fmt::format("{} could not use the selected folder as its data folder.", AppName);
    }
    show_data_folder_error_modal(dataPathError);
}

const Rml::String kInternalResolutionHelpText =
    "Configure the resolution used for rendering the game. Higher values are more demanding on "
    "your graphics hardware.";
const Rml::String kShadowResolutionHelpText =
    "Configure the shadow-map resolution. Higher values improve shadow quality but increase GPU "
    "and memory usage.";
const Rml::String kResamplerHelpText =
    "Configure the sampling method used when scaling the internal resolution for final presentation.";
const Rml::String kBloomHelpText =
    "Configure the post-processing bloom effect. Classic matches the console; Dusklight uses "
    "a higher-quality bloom pass; Legacy restores the April 9 bloom rendering path.";
const Rml::String kBloomBrightnessHelpText =
    "Configure bloom intensity. Higher values make bright areas glow more strongly.";
const Rml::String kDepthOfFieldHelpText =
    "Configure the post-processing depth-of-field effect. Classic uses the original depth-of-field pass;"
    " Dusklight uses a higher-quality depth-of-field pass.";
const Rml::String kUnlockFramerateHelpText =
    "<br/>Uses inter-frame interpolation to enable higher frame rates.<br/><br/>May introduce minor "
    "visual artifacts or animation glitches.";
const Rml::String kTextureReplacementHelpText =
    "Enable installed texture replacements.";

constexpr std::array kFrameRateLimitValues = {30, 60, 120, 240, 360, 480, 0};
constexpr std::array kFrameRateLimitNames = {
    "30 FPS",
    "60 FPS",
    "120 FPS",
    "240 FPS",
    "360 FPS",
    "480 FPS",
    "Unlocked",
};

constexpr std::array kAspectRatioModeNames = {
    "Off",
    "4:3",
    "3:2",
    "16:9",
    "21:9",
};

void apply_aspect_ratio_settings() {
    switch (getSettings().video.forcedAspectRatio.getValue()) {
    case AspectRatioMode::Ratio16x9:
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
        VILockAspectRatio(16, 9);
        break;
    case AspectRatioMode::Ratio21x9:
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
        VILockAspectRatio(43, 18);
        break;
    case AspectRatioMode::Ratio3x2:
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
        VILockAspectRatio(3, 2);
        break;
    case AspectRatioMode::Off:
    default:
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(getSettings().video.lockAspectRatio.getValue() ?
                                    AURORA_VIEWPORT_FIT :
                                    AURORA_VIEWPORT_STRETCH);
        break;
    }
}

int aspect_ratio_mode_index() {
    switch (getSettings().video.forcedAspectRatio.getValue()) {
    case AspectRatioMode::Ratio3x2:
        return 2;
    case AspectRatioMode::Ratio16x9:
        return 3;
    case AspectRatioMode::Ratio21x9:
        return 4;
    case AspectRatioMode::Off:
    default:
        break;
    }

    return getSettings().video.lockAspectRatio.getValue() ? 1 : 0;
}

void set_aspect_ratio_mode_index(int index) {
    index = std::clamp(index, 0, static_cast<int>(kAspectRatioModeNames.size()) - 1);
    getSettings().video.lockAspectRatio.setValue(index == 1);
    switch (index) {
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
    apply_aspect_ratio_settings();
    config::save();
}

int frame_rate_limit_index() {
    if (getSettings().game.enableFrameInterpolation.getValue() == FrameInterpMode::Off) {
        return 0;
    }

    const int limit = getSettings().game.frameRateLimit.getValue();
    for (int i = 1; i < static_cast<int>(kFrameRateLimitValues.size()); ++i) {
        if (kFrameRateLimitValues[i] == limit) {
            return i;
        }
    }
    return static_cast<int>(kFrameRateLimitValues.size()) - 1;
}

void set_frame_rate_limit_index(int index) {
    index = std::clamp(index, 0, static_cast<int>(kFrameRateLimitValues.size()) - 1);
    getSettings().game.enableFrameInterpolation.setValue(index == 0 ? FrameInterpMode::Off :
        (kFrameRateLimitValues[index] == 0 ? FrameInterpMode::Unlimited : FrameInterpMode::Capped));
    getSettings().game.frameRateLimit.setValue(index == 0 ? 0 : kFrameRateLimitValues[index]);
    if (index > 0 && kFrameRateLimitValues[index] > 0) {
        getSettings().video.maxFrameRate.setValue(kFrameRateLimitValues[index]);
    }
    presentation::update_frame_rate_preference();
    config::save();
}

int float_setting_percent(ConfigVar<float>& var) {
    return static_cast<int>(var.getValue() * 100.0f + 0.5f);
}

bool gyro_enabled() {
    return getSettings().game.enableGyroAim || getSettings().game.enableGyroRollgoal;
}

Rml::String touch_targeting_label(TouchTargeting targeting) {
    const auto index = static_cast<std::size_t>(targeting);
    if (index >= kTouchTargetingLabels.size()) {
        return "Unknown";
    }
    return kTouchTargetingLabels[index];
}

struct ConfigBoolProps {
    Rml::String key;
    Rml::String icon;
    Rml::String helpText;
    std::function<void(bool)> onChange;
    std::function<bool()> isDisabled;
};

SelectButton& config_bool_select(
    Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var, ConfigBoolProps props) {
    auto& button = leftPane.add_child<BoolButton>(BoolButton::Props{
        .key = std::move(props.key),
        .icon = std::move(props.icon),
        .getValue = [&var] { return var.getValue(); },
        .setValue =
            [&var, callback = std::move(props.onChange)](bool value) {
                if (value == var.getValue()) {
                    return;
                }
                var.setValue(value);
                config::save();
                if (callback) {
                    callback(value);
                }
            },
        .isDisabled = std::move(props.isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
    });
    leftPane.register_control(
        button, rightPane, [helpText = std::move(props.helpText)](Pane& pane) {
            pane.clear();
            pane.add_rml(helpText);
        });
    return button;
}

void add_speedrun_disabled_option(Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var,
    const Rml::String& key, const Rml::String& helpText) {
    config_bool_select(leftPane, rightPane, var, {
        .key = key,
        .helpText = helpText,
        .isDisabled = [] { return getSettings().game.speedrunMode.getValue(); },
    });
}

SelectButton& config_percent_select(Pane& leftPane, Pane& rightPane, ConfigVar<float>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step = 5,
    std::function<bool()> isDisabled = {}) {
    auto& button = leftPane.add_child<NumberButton>(NumberButton::Props{
        .key = std::move(key),
        .getValue = [&var] { return float_setting_percent(var); },
        .setValue =
            [&var, min, max](int value) {
                var.setValue(std::clamp(value, min, max) / 100.0f);
                config::save();
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .min = min,
        .max = max,
        .step = step,
        .suffix = "%",
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_rml(helpText);
    });
    return button;
}

template <typename T>
SelectButton& config_enum_select(Pane& leftPane, Pane& rightPane, ConfigVar<T>& var,
    Rml::String key, Rml::String helpText, const char* const* labels, int labelCount) {
    auto& button = leftPane.add_select_button({
        .key = std::move(key),
        .getValue = [&var, labels, labelCount] {
            const int index = static_cast<int>(var.getValue());
            return Rml::String{index >= 0 && index < labelCount ? labels[index] : "Unknown"};
        },
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .submit = true,
    });
    leftPane.register_control(button, rightPane,
        [&var, labels, labelCount, helpText = std::move(helpText)](Pane& pane) {
            pane.clear();
            for (int i = 0; i < labelCount; ++i) {
                pane.add_button({
                    .text = labels[i],
                    .isSelected = [&var, i] { return static_cast<int>(var.getValue()) == i; },
                }).on_pressed([&var, i] {
                    mDoAud_seStartMenu(kSoundItemChange);
                    var.setValue(static_cast<T>(i));
                    config::save();
                });
            }
            pane.add_text(helpText);
        });
    return button;
}

SelectButton& config_int_select(Pane& leftPane, Pane& rightPane, ConfigVar<int>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step = 5,
    std::function<bool()> isDisabled = {}, std::function<void(int)> onChange = {},
    std::string suffix = "") {
    auto& button = leftPane.add_child<NumberButton>(NumberButton::Props{
        .key = std::move(key),
        .getValue = [&var] { return var.getValue(); },
        .setValue =
            [&var, min, max, callback = std::move(onChange)](int value) {
                const int clampedValue = std::clamp(value, min, max);
                var.setValue(clampedValue);
                config::save();
                if (callback) {
                    callback(clampedValue);
                }
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .min = min,
        .max = max,
        .step = step,
        .suffix = std::move(suffix),
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_text(helpText);
    });
    return button;
}

SelectButton& config_milliseconds_select(Pane& leftPane, Pane& rightPane, ConfigVar<int>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step = 1,
    std::function<bool()> isDisabled = {}) {
    return config_int_select(leftPane, rightPane, var, std::move(key), std::move(helpText), min, max,
        step, std::move(isDisabled), {}, " ms");
}

SelectButton& config_level_select(Pane& leftPane, Pane& rightPane, ConfigVar<float>& var,
    Rml::String key, Rml::String helpText, std::function<bool()> isDisabled = {}) {
    auto& button = leftPane.add_child<FloatButton>(FloatButton::Props{
        .key = std::move(key),
        .getValue = [&var] {
            return std::clamp(var.getValue(), 1.0f, 10.0f);
        },
        .setValue =
            [&var](float value) {
                var.setValue(std::clamp(value, 1.0f, 10.0f));
                config::save();
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .min = 1.0f,
        .max = 10.0f,
        .step = 0.1f,
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_text(helpText);
    });
    return button;
}

template <typename T>
void graphics_tuner_control(Window& window, Pane& leftPane, Pane& rightPane, ConfigVar<T>& var,
    const GraphicsTunerProps& props) {
    leftPane.register_control(
        leftPane
            .add_select_button({
                .key = props.title,
                .getValue =
                    [&var, option = props.option] {
                        if constexpr (std::is_same_v<T, float>) {
                            return format_graphics_setting_value(
                                option, float_setting_percent(var));
                        } else {
                            return format_graphics_setting_value(
                                option, static_cast<int>(var.getValue()));
                        }
                    },
                .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
                .submit = false,
            })
            .on_nav_command([&window, props](Rml::Event&, NavCommand cmd) {
                if (cmd == NavCommand::Confirm || cmd == NavCommand::Left ||
                    cmd == NavCommand::Right) {
                    window.push(std::make_unique<GraphicsTuner>(props));
                    return true;
                }
                return false;
            }),
        rightPane, [helpText = props.helpText](Pane& pane) {
            pane.clear();
            pane.add_text(helpText);
        });
}

void confirm_return_to_startup() {
    auto dismiss = [](Modal& modal) {
        mDoAud_seStartMenu(kSoundWindowClose);
        modal.pop();
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = "Return to Startup Screen",
        .bodyRml = "Dusklight will restart and return to the startup screen, where you can change "
                   "your disc image, language, graphics backend, and other startup options."
                   "<br/><br/>Any unsaved progress will be lost.",
        .actions =
            {
                ModalAction{
                    .label = "Cancel",
                    .onPressed = dismiss,
                },
                ModalAction{
                    .label = "Restart",
                    .onPressed = [](Modal&) { dusk::RequestReturnToPrelaunch(); },
                },
            },
        .onDismiss = dismiss,
        .icon = "warning",
    }));
    if (auto* doc = top_document()) {
        doc->focus();
    }
}

}  // namespace

SettingsWindow::SettingsWindow(bool prelaunch) : mPrelaunch(prelaunch) {
    if (prelaunch) {
        add_tab("Prelaunch", [this](Rml::Element* content) {
            auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
            auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

            leftPane.register_control(
                leftPane
                    .add_select_button({
                        .key = "Disc Image",
                        .getValue =
                            [] {
                                const auto& path = prelaunch_state().configuredDiscPath;
                                std::string display;
                                if (path.empty()) {
                                    display = "(none)";
                                } else {
                                    display = borealis::file_select::display_name(path);
                                    if (display.empty()) {
                                        display = path;
                                    }
                                }
                                return display;
                            },
                        .isModified =
                            [] {
                                const auto& state = prelaunch_state();
                                const auto& active = state.activeDiscPath;
                                return !active.empty() && state.configuredDiscPath != active;
                            },
                    })
                    .on_pressed([] { open_iso_picker(); }),
                rightPane, [](Pane& pane) {
                    pane.add_rml("Set the disc image that Dusklight uses to launch the game.<br/><br/>"
                                 "Changes require a restart.");
                });
            if (data::manager().capabilities().canChangeLocation &&
                borealis::file_select::capabilities().canOpenFolder)
            {
                leftPane.register_control(
                    leftPane.add_select_button({
                        .key = "Data Folder",
                        .getValue = [] { return configured_data_path_display_name(); },
                        .isModified = [] { return data::is_data_path_restart_pending(); },
                    }),
                    rightPane, [](Pane& pane) {
                        pane.add_text("The data folder is where Dusklight stores settings, saves, "
                                      "logs, texture replacements, and other app data.");
                        pane.add_child<DataFolderPathText>();
#if DUSK_CAN_OPEN_DATA_FOLDER
                        pane.add_button("Open Data Folder").on_pressed([] {
                            if (data::open_data_path()) {
                                mDoAud_seStartMenu(kSoundClick);
                            }
                        });
#endif
                        pane.add_button("Change Data Folder").on_pressed([] {
                            const auto defaultLocation =
                                borealis::io::fs_path_to_string(data::configured_data_path());
                            borealis::file_select::open_folder(
                                {
                                    .parentWindow = aurora::window::get_sdl_window(),
                                    .defaultLocation = defaultLocation,
                                },
                                &data_folder_dialog_callback);
                        });
#if defined(_WIN32)
                        pane.add_button("Portable Mode").on_pressed([] {
                            if (data::set_portable_data_path()) {
                                mDoAud_seStartMenu(kSoundItemChange);
                            }
                        });
#endif
                        pane.add_button(
                                {
                                    .text = "Reset to Default",
                                    .isDisabled = [] { return data::is_default_data_path(); },
                                })
                            .on_pressed([] {
                                if (data::reset_data_path()) {
                                    mDoAud_seStartMenu(kSoundItemChange);
                                }
                            });
                        pane.add_rml("Data will be migrated automatically on restart.");
                    });
            }
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Language",
                    .getValue =
                        [] {
                            return language::language_name(getSettings().game.language.getValue());
                        },
                    .isDisabled =
                        [] {
                            const auto& state = prelaunch_state();
                            if (!state.configuredDiscCanLaunch) {
                                return true;
                            }
                            return language::available_languages(state.configuredDiscInfo).size() <= 1;
                        },
                    .isModified =
                        [] {
                            return getSettings().game.language.getValue() !=
                                   prelaunch_state().initialLanguage;
                        },
                }),
                rightPane, [](Pane& pane) {
                    const auto& state = prelaunch_state();
                    const auto languages = state.configuredDiscCanLaunch
                                               ? language::available_languages(state.configuredDiscInfo)
                                               : language::available_languages({});
                    for (const GameLanguage language : languages) {
                        pane.add_button({
                                            .text = language::language_name(language),
                                            .isSelected =
                                                [language] {
                                                    return getSettings().game.language.getValue() ==
                                                           language;
                                                },
                                        })
                            .on_pressed([language] {
                                mDoAud_seStartMenu(kSoundItemChange);
                                getSettings().game.language.setValue(language);
                                config::save();
                            });
                    }
                    pane.add_rml("<br/>Changes require a restart.");
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Graphics Backend",
                    .getValue = [] { return graphics_backend_display_name(); },
                    .isModified = [] { return is_graphics_backend_restart_pending(); },
                }),
                rightPane, [](Pane& pane) {
                    const auto availableBackends = available_backends();
                    for (const auto backend : availableBackends) {
                        pane
                            .add_button({
                                .text = Rml::String{backend_name(backend)},
                                .isSelected = [backend] { return configured_backend() == backend; },
                            })
                            .on_pressed([backend] {
                                mDoAud_seStartMenu(kSoundItemChange);
                                getSettings().backend.graphicsBackend.setValue(
                                    std::string{backend_id(backend)});
                                config::save();
                            });
                    }
                    pane.add_rml("<br/>Changes require a restart.");
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Save File Type",
                    .getValue =
                        [] {
                            return kCardFileTypes[getSettings().backend.cardFileType.getValue()];
                        },
                    .isModified =
                        [] {
                            return getSettings().backend.cardFileType.getValue() !=
                                   prelaunch_state().initialCardFileType;
                        },
                }),
                rightPane, [](Pane& pane) {
                    for (int i = 0; i < kCardFileTypes.size(); i++) {
                        pane
                            .add_button({
                                .text = kCardFileTypes[i],
                                .isSelected =
                                    [i] {
                                        return getSettings().backend.cardFileType.getValue() == i;
                                    },
                            })
                            .on_pressed([i] {
                                mDoAud_seStartMenu(kSoundItemChange);
                                getSettings().backend.cardFileType.setValue(i);
                                config::save();
                            });
                    }
                });
        });
    }

    add_tab("Video", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Display");

        leftPane.register_control(leftPane.add_button("Toggle Fullscreen").on_pressed([] {
            mDoAud_seStartMenu(kSoundItemChange);
            getSettings().video.enableFullscreen.setValue(!getSettings().video.enableFullscreen);
            VISetWindowFullscreen(getSettings().video.enableFullscreen);
            config::save();
        }),
            rightPane, [](Pane& pane) { pane.clear(); });
        leftPane.register_control(leftPane.add_button("Restore Default Window Size").on_pressed([] {
            mDoAud_seStartMenu(kSoundItemChange);
            getSettings().video.enableFullscreen.setValue(false);
            VISetWindowFullscreen(false);
            VISetWindowSize(FB_WIDTH * 2, FB_HEIGHT * 2);
            VICenterWindow();
        }),
            rightPane, [](Pane& pane) { pane.clear(); });
        config_bool_select(leftPane, rightPane, getSettings().video.enableVsync,
            {
                .key = "Enable VSync",
                .helpText = "Synchronizes the frame rate to your monitor's refresh rate.",
                .onChange = [](bool value) { aurora_enable_vsync(value); },
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Force Aspect Ratio",
                .getValue = [] { return Rml::String{kAspectRatioModeNames[aspect_ratio_mode_index()]}; },
                .isModified =
                    [] {
                        return getSettings().video.lockAspectRatio.getValue() !=
                                   getSettings().video.lockAspectRatio.getDefaultValue() ||
                               getSettings().video.forcedAspectRatio.getValue() !=
                                   getSettings().video.forcedAspectRatio.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kAspectRatioModeNames.size()); ++i) {
                    pane.add_button({
                        .text = kAspectRatioModeNames[i],
                        .isSelected = [i] { return aspect_ratio_mode_index() == i; },
                    }).on_pressed([i] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        set_aspect_ratio_mode_index(i);
                    });
                }
                pane.add_text("Force the rendered game to a specific display aspect ratio.");
            });
        config_bool_select(leftPane, rightPane, getSettings().game.pauseOnFocusLost,
            {
                .key = "Pause on Focus Lost",
                .helpText = "Pause the game when window focus is lost.",
                .isDisabled = [] { return IsMobile || getSettings().game.speedrunMode; },
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Show FPS Counter",
                .getValue =
                    [] {
                        if (!getSettings().video.enableFpsOverlay.getValue()) {
                            return Rml::String{"Off"};
                        }
                        const int idx = getSettings().video.fpsOverlayCorner.getValue();
                        return Rml::String{kFpsOverlayCornerNames[idx]};
                    },
                .isModified =
                    [] {
                        const auto& enable = getSettings().video.enableFpsOverlay;
                        const auto& corner = getSettings().video.fpsOverlayCorner;
                        return enable.getValue() != enable.getDefaultValue() ||
                               (enable.getValue() && corner.getValue() != corner.getDefaultValue());
                    },
            }),
            rightPane, [](Pane& pane) {
                pane.add_button(
                        {
                            .text = "Off",
                            .isSelected =
                                [] { return !getSettings().video.enableFpsOverlay.getValue(); },
                        })
                    .on_pressed([] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        getSettings().video.enableFpsOverlay.setValue(false);
                        config::save();
                    });
                for (int i = 0; i < static_cast<int>(kFpsOverlayCornerNames.size()); ++i) {
                    pane.add_button(
                            {
                                .text = kFpsOverlayCornerNames[i],
                                .isSelected =
                                    [i] {
                                        return getSettings().video.enableFpsOverlay.getValue() &&
                                               getSettings().video.fpsOverlayCorner.getValue() == i;
                                    },
                            })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().video.enableFpsOverlay.setValue(true);
                            getSettings().video.fpsOverlayCorner.setValue(i);
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/>Display the current framerate in a corner of the screen while playing.");
            });
        config_bool_select(leftPane, rightPane, getSettings().video.rememberWindowSize,
            {
                .key = "Remember Window Size",
                .helpText = "Save and restore the previous session's window size when opening Dusklight.",
                .onChange =
                    [](bool value) {
                        if (value && !dusk::getSettings().video.enableFullscreen) {
                            const auto windowSize = aurora::window::get_window_size();
                            dusk::getSettings().video.lastWindowWidth.setValue(windowSize.width);
                            dusk::getSettings().video.lastWindowHeight.setValue(windowSize.height);
                            dusk::config::save();
                        }
                    },
                .isDisabled = [] { return IsMobile; },
            });
        leftPane.add_section("Resolution");
        graphics_tuner_control(*this, leftPane, rightPane,
            getSettings().game.internalResolutionScale,
            GraphicsTunerProps{
                .option = GraphicsOption::InternalResolution,
                .title = "Internal Resolution",
                .helpText = kInternalResolutionHelpText,
                .valueMin = 0,
                .valueMax = 12,
                .defaultValue = 0,
            });
        graphics_tuner_control(*this, leftPane, rightPane,
            getSettings().game.shadowResolutionMultiplier,
            GraphicsTunerProps{
                .option = GraphicsOption::ShadowResolution,
                .title = "Shadow Resolution",
                .helpText = kShadowResolutionHelpText,
                .valueMin = 1,
                .valueMax = 8,
                .defaultValue = 1,
            });
        graphics_tuner_control(*this, leftPane, rightPane, getSettings().game.resampler,
            GraphicsTunerProps{
                .option = GraphicsOption::Resampler,
                .title = "Output Resampling",
                .helpText = kResamplerHelpText,
                .valueMin = static_cast<int>(Resampler::Bilinear),
                .valueMax = static_cast<int>(Resampler::Area),
                .defaultValue = static_cast<int>(Resampler::Bilinear),
            });
        leftPane.add_section("Twilight Visuals");
        config_bool_select(leftPane, rightPane, getSettings().game.enableTwilightVisuals,
            {
                .key = "Enable Twilight Visuals",
                .helpText = "Apply the Faron, Eldin, and Lanayru twilight environment layers anywhere. "
                            "This changes visuals only; actors, gameplay, audio, and time remain unchanged."
            });
        config_percent_select(leftPane, rightPane, getSettings().game.twilightVisualBrightness,
            "Twilight Visual Brightness",
            "Adjusts the strength of the Twilight bloom when Twilight Visuals is enabled. 100% matches the vanilla Twilight strength.",
            0, 400, 10);
        static constexpr const char* kTwilightSkyboxModes[] = {"Day", "Night"};
        config_enum_select(leftPane, rightPane, getSettings().game.twilightSkyboxMode,
            "Twilight Skybox", "Choose the authored Twilight skybox time of day.",
            kTwilightSkyboxModes, ARRAY_SIZEU(kTwilightSkyboxModes));
        static constexpr const char* kTwilightWeather[] = {"Current", "Clear", "Rain", "Snow", "Lightning", "Wind Storm"};
        config_enum_select(leftPane, rightPane, getSettings().game.twilightWeather,
            "Twilight Weather", "Choose whether Twilight uses the current map weather or a visual weather override.",
            kTwilightWeather, ARRAY_SIZEU(kTwilightWeather));

        leftPane.add_section("Post-Processing");
        graphics_tuner_control(*this, leftPane, rightPane, getSettings().game.bloomMode,
            GraphicsTunerProps{
                .option = GraphicsOption::BloomMode,
                .title = "Bloom",
                .helpText = kBloomHelpText,
                .valueMin = static_cast<int>(BloomMode::Off),
                .valueMax = static_cast<int>(BloomMode::Legacy),
                .defaultValue = static_cast<int>(BloomMode::Classic),
            });
        graphics_tuner_control(*this, leftPane, rightPane, getSettings().game.bloomMultiplier,
            GraphicsTunerProps{
                .option = GraphicsOption::BloomMultiplier,
                .title = "Bloom Brightness",
                .helpText = kBloomBrightnessHelpText,
                .valueMin = 0,
                .valueMax = 100,
                .defaultValue = 100,
                .step = 10,
            });
        graphics_tuner_control(*this, leftPane, rightPane, getSettings().game.depthOfFieldMode,
            GraphicsTunerProps{
                .option = GraphicsOption::DepthOfFieldMode,
                .title = "Depth of Field",
                .helpText = kDepthOfFieldHelpText,
                .valueMin = static_cast<int>(DepthOfFieldMode::Off),
                .valueMax = static_cast<int>(DepthOfFieldMode::Dusk),
                .defaultValue = static_cast<int>(DepthOfFieldMode::Classic),
            });

        leftPane.add_section("Rendering");
        graphics_tuner_control(*this, leftPane, rightPane,
            getSettings().game.enableTextureReplacements,
            GraphicsTunerProps{
                .option = GraphicsOption::TextureReplacements,
                .title = "Enable Texture Replacements",
                .helpText = kTextureReplacementHelpText,
                .valueMin = static_cast<int>(false),
                .valueMax = static_cast<int>(true),
                .defaultValue = static_cast<int>(false),
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Frame Rate Limit",
                .getValue = [] { return Rml::String{kFrameRateLimitNames[frame_rate_limit_index()]}; },
                .isModified =
                    [] {
                        return getSettings().game.enableFrameInterpolation.getValue() !=
                                   getSettings().game.enableFrameInterpolation.getDefaultValue() ||
                               getSettings().game.frameRateLimit.getValue() !=
                                   getSettings().game.frameRateLimit.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kFrameRateLimitNames.size()); ++i) {
                    pane.add_button({
                        .text = kFrameRateLimitNames[i],
                        .isSelected = [i] { return frame_rate_limit_index() == i; },
                    }).on_pressed([i] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        set_frame_rate_limit_index(i);
                    });
                }
                pane.add_text(kUnlockFramerateHelpText);
            });
        config_bool_select(leftPane, rightPane, getSettings().game.lowLatencyPresentation,
            {
                .key = "Low Latency Presentation",
                .helpText = "Reduces the extra presentation delay used by the PC port's normal frame pacing. "
                            "Only available in 30 FPS mode.",
                .isDisabled = [] {
                    return getSettings().game.enableFrameInterpolation.getValue() != FrameInterpMode::Off &&
                           !getTransientSettings().forceThirtyFpsLimit;
                },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.enableMapBackground,
            {
                .key = "Enable Mini-Map Shadows",
                .helpText = "Render a thick shadow around the mini-map. May impact performance."
            });
        config_bool_select(leftPane, rightPane, getSettings().game.disableCutscenePillarboxing,
            {
                .key = "Disable Cutscene Pillarboxing",
                .helpText = "Disable black bars on the left and right sides of the screen "
                            "during some cutscenes, particularly on ultra-wide displays. "
                            "Visuals beyond the original intended framing may appear buggy."
            });
    });

    add_tab("Input", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addOption = [&](const Rml::String& key, ConfigVar<bool>& value,
                             const Rml::String& helpText, std::function<bool()> isDisabled = {}) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                    .isDisabled = std::move(isDisabled),
                });
        };

        leftPane.add_section("Inputs");
        leftPane.register_control(leftPane.add_button("Configure Inputs").on_pressed([this] {
            push(std::make_unique<ControllerConfigWindow>());
        }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Open controller binding configuration.");
            });
        config_bool_select(leftPane, rightPane, getSettings().game.allowBackgroundInput,
            {
                .key = "Allow Background Input",
                .helpText = "Allow controller input even when the game window is not focused.",
                .onChange = [](bool value) { aurora_set_background_input(value); },
            });
        config_milliseconds_select(leftPane, rightPane, getSettings().game.inputLagMs,
            "Video Latency", "Delays the rendered game image by 0-150ms without changing "
                             "which simulation frame receives controller input.",
            0, 150, 1);

#if TOUCH_CONTROLS_AVAILABLE
        leftPane.add_section("Touch");
        addOption("Touch Controls", getSettings().game.enableTouchControls,
            "Enables controls overlay for touch screens.<br/><br/>Press and drag on the left side "
            "of the screen to move, and on the right side of the screen to control the camera.");
        auto& customizeTouchLayout = leftPane.add_button(ControlledButton::Props{
            .text = "Customize Layout",
            .isDisabled = [] { return !getSettings().game.enableTouchControls; },
        });
        leftPane.register_control(customizeTouchLayout.on_pressed(
                                      [this] { push(std::make_unique<TouchControlsEditor>()); }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Open the touch controls layout editor.");
            });
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Touch Targeting",
                                      .getValue =
                                          [] {
                                              return touch_targeting_label(
                                                  getSettings().game.touchTargeting.getValue());
                                          },
                                      .isDisabled =
                                          [] { return !getSettings().game.enableTouchControls; },
                                      .isModified =
                                          [] {
                                              const auto& targeting =
                                                  getSettings().game.touchTargeting;
                                              return targeting.getValue() !=
                                                     targeting.getDefaultValue();
                                          },
                                  }),
            rightPane, [](Pane& pane) {
                pane.clear();
                for (int i = 0; i < static_cast<int>(kTouchTargetingLabels.size()); ++i) {
                    pane.add_button({
                            .text = kTouchTargetingLabels[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.touchTargeting.getValue() ==
                                           static_cast<TouchTargeting>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.touchTargeting.setValue(
                                static_cast<TouchTargeting>(i));
                            config::save();
                        });
                }
                pane.add_rml(fmt::format("<br/>Hybrid: {}<br/>Hold: {}<br/>Switch: {}",
                    kTouchTargetingDescriptions[0], kTouchTargetingDescriptions[1],
                    kTouchTargetingDescriptions[2]));
            });
        config_percent_select(leftPane, rightPane, getSettings().game.touchCameraXSensitivity,
            "Touch Camera X Sensitivity",
            "Adjusts touch camera horizontal sensitivity.<br/><br/>Applies to touch input only.",
            25, 400, 5, [] { return !getSettings().game.enableTouchControls; });
        config_percent_select(leftPane, rightPane, getSettings().game.touchCameraYSensitivity,
            "Touch Camera Y Sensitivity",
            "Adjusts touch camera vertical sensitivity.<br/><br/>Applies to touch input only.", 25,
            400, 5, [] { return !getSettings().game.enableTouchControls; });
#endif

        leftPane.add_section("Camera");
        addOption("Free Camera", getSettings().game.freeCamera,
            "Enables free camera control, letting you control the camera fully with the C-Stick.");
        addOption("Invert Camera X Axis", getSettings().game.invertCameraXAxis,
            "Invert horizontal camera movement.<br/><br/>Applies to the control stick only.");
        addOption("Invert Camera Y Axis", getSettings().game.invertCameraYAxis,
            "Invert vertical camera movement.<br/><br/>Applies to the control stick only.",
            [] { return !getSettings().game.freeCamera; });
        addOption("Custom Camera Speeds", getSettings().game.enableCameraSpeedControls,
            "Enable camera speed controls. When disabled, all camera speeds use the default value.");
        config_level_select(leftPane, rightPane, getSettings().game.regularCameraSensitivityLevel,
            "Camera Speed", "Adjusts horizontal C-Stick camera rotation speed.",
            [] {
                return !getSettings().game.enableCameraSpeedControls ||
                       getSettings().game.freeCamera;
            });
        config_level_select(leftPane, rightPane, getSettings().game.freeCameraSensitivityLevel,
            "Freecam Speed",
            "Adjusts free camera movement speed.",
            [] {
                return !getSettings().game.enableCameraSpeedControls ||
                       !getSettings().game.freeCamera;
            });
        config_level_select(leftPane, rightPane, getSettings().game.aimingCameraSensitivityLevel,
            "Aiming Speed",
            "Adjusts stick and gyro sensitivity while aiming items or using first person camera.",
            [] { return !getSettings().game.enableCameraSpeedControls; });
        addOption("Invert First Person X Axis", getSettings().game.invertFirstPersonXAxis,
            "Invert horizontal movement while aiming with items or first person camera.<br/><br/>Applies to the control stick only.");
        addOption("Invert First Person Y Axis", getSettings().game.invertFirstPersonYAxis,
            "Invert vertical movement while aiming with items or first person camera.<br/><br/>Applies to the control stick only.");

        leftPane.add_section("Gyro");
        addOption("Gyro Aim", getSettings().game.enableGyroAim,
            "Enables gyro controls while in look mode, aiming a hawk, and aiming "
            "supported items.<br/><br/>Supported items include the Slingshot, Gale Boomerang, "
            "Hero's Bow, Clawshot(s), Ball and Chain, and Dominion Rod.");
        addOption("Gyro Rollgoal", getSettings().game.enableGyroRollgoal,
            "Enables gyro controls for Rollgoal in Hena's Cabin.");
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSensitivityY,
            "Gyro Pitch Sensitivity", "Controls vertical gyro aiming sensitivity.", 25, 400, 5,
            [] { return !gyro_enabled(); });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSensitivityX,
            "Gyro Yaw Sensitivity", "Controls horizontal gyro aiming sensitivity.", 25, 400, 5,
            [] { return !gyro_enabled(); });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSensitivityRollgoal,
            "Rollgoal Sensitivity", "Controls how strongly gyro input tilts the Rollgoal table.",
            25, 400, 5,
            [] { return !getSettings().game.enableGyroRollgoal; });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroDeadband, "Gyro Deadband",
            "Ignores small gyro movement to reduce drift and jitter.", 0, 50, 1,
            [] { return !gyro_enabled(); });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSmoothing,
            "Gyro Smoothing", "Higher values smooth gyro input over time.", 0, 100, 1,
            [] { return !gyro_enabled(); });
        addOption("Invert Gyro Pitch", getSettings().game.gyroInvertPitch,
            "Invert vertical gyro aiming.", [] { return !gyro_enabled(); });
        addOption("Invert Gyro Yaw", getSettings().game.gyroInvertYaw,
            "Invert horizontal gyro aiming.", [] { return !gyro_enabled(); });
        
        leftPane.add_section("Gameplay");
        addOption("Swap Direct Select Input", getSettings().game.swapDirectSelect,
            "Swap the controls for using Direct Select on the item wheel, making Direct Select the default and holding L to scroll the wheel.");

        leftPane.add_section("Mouse");
        addOption("Mouse Aim", getSettings().game.enableMouseAim,
            "Enables mouse input while in look mode, aiming a hawk, and aiming "
            "supported items.<br/><br/>Supported items include the Slingshot, Gale Boomerang, "
            "Hero's Bow, Clawshot(s), Ball and Chain, and Dominion Rod.");
        addOption("Mouse Camera", getSettings().game.enableMouseCamera,
            "Enables mouse input for controlling the third-person camera.");
        config_percent_select(leftPane, rightPane, getSettings().game.mouseAimSensitivity,
            "Mouse Aim Sensitivity", "Controls mouse aim sensitivity.", 25, 400, 5,
            [] { return !getSettings().game.enableMouseAim; });
        config_percent_select(leftPane, rightPane, getSettings().game.mouseCameraSensitivity,
            "Mouse Camera Sensitivity", "Controls mouse camera sensitivity.", 25, 400, 5,
            [] { return !getSettings().game.enableMouseCamera; });
        addOption("Invert Mouse Y", getSettings().game.invertMouseY,
            "Invert vertical mouse control for both aiming and camera.",
            [] { return !getSettings().game.enableMouseAim || !getSettings().game.enableMouseCamera; });

        leftPane.add_section("Gameplay");
        addOption("Mouse/Touch in Menus", getSettings().game.enableMenuPointer,
            "Enables mouse and touch input for supported in-game menus.");
        addOption("Invert Air/Swim X Axis", getSettings().game.invertAirSwimX,
            "Invert horizontal movement while flying or swimming.");
        addOption("Invert Air/Swim Y Axis", getSettings().game.invertAirSwimY,
            "Invert vertical movement while flying or swimming.");
        addOption("Swap Direct Select Input", getSettings().game.swapDirectSelect,
            "Swap the controls for using Direct Select on the item wheel, making Direct Select the default and holding L to scroll the wheel.");

        leftPane.add_section("Tools");
        addOption("Turbo Speed Key", getSettings().game.enableTurboKeybind,
            "Hold Tab to temporarily disable the frame rate limit.",
            [] { return getSettings().game.speedrunMode.getValue(); });
        addOption("Reset Key (" + Rml::String{hotkeys::DO_RESET} + ")",
            getSettings().game.enableResetKeybind,
            "Press " + Rml::String{hotkeys::DO_RESET} + " to reset the game.");

        leftPane.add_section("Hotkeys");
        leftPane.register_control(leftPane.add_button("Configure Hotkeys").on_pressed([this] {
            push(std::make_unique<HotkeyConfigWindow>());
        }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Open hotkey configuration.");
            });
    });

    add_tab("Audio", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        // TODO: Individual sliders for Main Music, Sub Music, Sound Effects, and Fanfare.
        leftPane.add_section("Volume");
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props{
                .key = "Master Volume",
                .getValue = [] { return getSettings().audio.masterVolume.getValue(); },
                .setValue =
                    [](int value) {
                        getSettings().audio.masterVolume.setValue(value);
                        config::save();
                        audio::SetMasterVolume(audio::MasterVolumeToLinear(value / 100.0f));
                    },
                .isModified =
                    [] {
                        return getSettings().audio.masterVolume.getValue() !=
                               getSettings().audio.masterVolume.getDefaultValue();
                    },
                .max = 100,
                .suffix = "%",
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Adjusts the volume of all sounds in the game.");
            });

        leftPane.add_section("Effects");
        config_bool_select(leftPane, rightPane, getSettings().audio.enableReverb,
            {
                .key = "Enable Reverb",
                .helpText = "Enables the reverb effect in game audio.",
                .onChange = [](bool value) { audio::SetEnableReverb(value); },
            });
        config_bool_select(leftPane, rightPane, getSettings().audio.enableHrtf,
            {
                .key = "Enable Spatial Sound",
                .helpText =
                    "Emulate surround sound via HRTF. Recommended only for use with headphones!",
                .onChange = [](bool value) { audio::EnableHrtf = value; },
            });
        config_bool_select(leftPane, rightPane, getSettings().audio.menuSounds,
            {
                .key = "Dusklight Menu Sounds",
                .helpText = "Play sound effects when navigating the Dusklight menu.",
            });

        leftPane.add_section("Tweaks");
        config_bool_select(leftPane, rightPane, getSettings().game.noLowHpSound,
            {
                .key = "No Low HP Sound",
                .helpText = "Disable the beeping sound when having low health.",
            });
        config_bool_select(leftPane, rightPane, getSettings().game.midnasLamentNonStop,
            {
                .key = "Non-Stop Midna's Lament",
                .helpText = "Prevents enemy music while Midna's Lament is playing.",
            });
    });

    add_tab("Gameplay", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addOption = [&](const Rml::String& key, ConfigVar<bool>& value,
                             const Rml::String& helpText) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                });
        };
        auto addSpeedrunDisabledOption = [&](const Rml::String& key, ConfigVar<bool>& value,
                                             const Rml::String& helpText) {
            add_speedrun_disabled_option(leftPane, rightPane, value, key, helpText);
        };

        leftPane.add_section("General");
        addOption("Mirror Mode", getSettings().game.enableMirrorMode,
            "Mirrors the world horizontally, matching the Wii version of the game.");
        addOption("Minimal HUD", getSettings().game.minimalHUD,
            "Disables the elements of the main HUD of the game.<br/>Useful for a more immersive "
            "experience.");
        config_percent_select(leftPane, rightPane, getSettings().game.hudScale,
            "HUD Scale",
            "Scales the size of the gameplay HUD (hearts, buttons, mini-map, etc.). Does not affect dialog boxes or menus.",
            50, 200, 5,
            [] { return getSettings().game.minimalHUD.getValue(); });
        addOption("Restore Wii 1.0 Glitches", getSettings().game.restoreWiiGlitches,
            "Restores patched glitches from Wii USA 1.0, the first released version.");
        config_bool_select(leftPane, rightPane, getSettings().game.usePpcFastInvSqrt,
            {
                .key = "PPC Fast InvSqrt",
                .helpText =
                    "Uses the default GameCube/Wii reciprocal square root estimate for fast "
                    "square-root and vector distance math. Turn this off to use the modern "
                    "platform sqrt implementation.",
                .onChange = [](bool value) { dusk_set_native_vector_rsqrt(!value); },
            });
        addOption("Enable Rotating Link Doll", getSettings().game.enableLinkDollRotation,
            "Enables rotating Link in the collection menu with the C-Stick.");
        addOption("Hide Owl Statue Markers", getSettings().game.removeQuestMapMarkers,
            "Removes completed Owl Statue markers from the map and Minimap.");

        leftPane.add_section("Difficulty");
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props{
                .key = "Damage Multiplier",
                .getValue = [] { return std::clamp(getSettings().game.damageMultiplier.getValue(), 1, 8); },
                .setValue =
                    [](int value) {
                        getSettings().game.damageMultiplier.setValue(std::clamp(value, 1, 8));
                        config::save();
                    },
                .isDisabled = [] { return getSettings().game.speedrunMode.getValue(); },
                .isModified =
                    [] {
                        return getSettings().game.damageMultiplier.getValue() !=
                               getSettings().game.damageMultiplier.getDefaultValue();
                    },
                .min = 1,
                .max = 8,
                .suffix = "×",
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Multiplies incoming damage.");
            });
        addSpeedrunDisabledOption(
            "Instant Death", getSettings().game.instantDeath, "Any hit will instantly kill you.");
        addSpeedrunDisabledOption("No Heart Drops", getSettings().game.noHeartDrops,
            "Hearts will never drop from enemies, pots, and various other places.");

        leftPane.add_section("Quality of Life");
        addOption("Bigger Wallets", getSettings().game.biggerWallets,
            "Wallet sizes are like in the HD version. (500, 1000, 2000)");
        addOption("Disable Rupee Cutscenes", getSettings().game.disableRupeeCutscenes,
            "Rupees will not play cutscenes after you have collected them the first time.");
        addOption("Skip All Cutscenes", getSettings().game.skipAllCutscenes,
            "Allows additional cutscene skips.");
        addOption("Faster Climbing", getSettings().game.fastClimbing,
            "Quicker climbing on ladders and vines like the HD version.");
        addOption("Faster Tears of Light", getSettings().game.fastTears,
            "Tears of Light dropped by Shadow Insects pop out faster like the HD version.");
        config_bool_select(leftPane, rightPane, getSettings().game.enableFastLoads,
            {
                .key = "Fast Loads",
                .helpText = "Shortens area transition waits and fades.",
                .onChange = [](bool value) {
                    if (value) {
                        getSettings().game.enableInstaLoads.setValue(false);
                        config::save();
                    }
                },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.enableInstaLoads,
            {
                .key = "Insta Loads",
                .helpText = "Loads areas in 1 frame, will break some cutscenes if you skip too fast.",
                .onChange = [](bool value) {
                    if (value) {
                        getSettings().game.enableFastLoads.setValue(false);
                        config::save();
                    }
                },
            });
        addOption("Instant Movement", getSettings().game.instantMovement,
            "Experimental: gives you control of Link the instant a load finishes, regardless "
            "of load type, by skipping the start demo. Suppresses many post-load cutscenes -- "
            "intended for randomizer. Will break stages that rely on a start cutscene."
            "<br/><br/>Warning: This WILL break certain parts of the game, do not report bugs "
            "regarding this feature");
        config_bool_select(leftPane, rightPane, getSettings().game.autoSave,
            {
                .key = "Autosave",
                .helpText = "Autosaves the game when going to a new area, opening a dungeon door, "
                            "or getting a new item.",
                .isDisabled = [] { return getSettings().game.speedrunMode.getValue(); },
            });
        addOption("Instant Saves", getSettings().game.instantSaves,
            "Skips the delay when writing to the Memory Card.");
        addOption("Hold B for Instant Text", getSettings().game.instantText,
            "Makes text scroll immediately by holding B.");
        addOption("No Climbing Miss Animation", getSettings().game.noMissClimbing,
            "Prevents Link from playing a struggle animation when grabbing ledges or "
            "climbing on vines.");
        addOption("No Rupee Returns", getSettings().game.noReturnRupees,
            "Always collect Rupees even if your Wallet is too full.");
        addOption("No Sword Recoil", getSettings().game.noSwordRecoil,
            "Link will not recoil when his sword hits walls.");
        addOption("No 2nd Fish for Cat", getSettings().game.no2ndFishForCat,
            "Skip needing to catch a second fish for Sera's cat.");
        addOption("Button Fishing", getSettings().game.buttonFishing,
            "Allow fishing with the Fishing Rod using the button the item is assigned to.");
        addOption("Show Poe Count on Map", getSettings().game.enhancedMapMenus,
            "Displays collected/total number of Poe Souls for a region on the map.");
        addSpeedrunDisabledOption("Sun's Song (R+X)", getSettings().game.sunsSong,
            "Allows Wolf Link to howl and change the time of day.");
        addOption("Quick Transform (R+Y)", getSettings().game.enableQuickTransform,
            "Transform instantly by pressing R and Y simultaneously.");
        addOption("Warp as Human", getSettings().game.humanMidnaWarp,
            "Map/Midna warps no longer force Wolf Link transformation.");
        addOption("Aiming Reticle", getSettings().game.aimingReticle,
            "Shows the aiming reticle for bow and slingshot.");

        addSpeedrunDisabledOption("Area Reload (L+R+Start+A)", getSettings().game.areaReload,
            "Reloads the current area at its last entrance while preserving temporary area state.");

        leftPane.add_section("Speedrunning");
        config_bool_select(leftPane, rightPane, getSettings().game.speedrunMode,
            {
                .key = "Speedrun Mode",
                .helpText =
                    "Enables speedrunning options while restricting certain gameplay modifiers.",
                .onChange =
                    [](bool enabled) {
                        if (enabled) {
                            resetForSpeedrunMode();
                        } else {
                            restoreFromSpeedrunMode();
                            if (getSettings().game.liveSplitEnabled) {
                                speedrun::disconnectLiveSplit();
                            }
                        }
                        MenuBar::rebuild();
                    },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.liveSplitEnabled,
            {
                .key = "LiveSplit Connection",
                .helpText = "Connect to LiveSplit server on localhost:16834. For this to work you must right click LiveSplit, and turn on Control -> Start TCP Server."
                " To see IGT in LiveSplit you must change your comparison to Game Time.",
                .onChange =
                    [](bool enabled) {
                        if (enabled) {
                            speedrun::connectLiveSplit();
                        } else {
                            speedrun::disconnectLiveSplit();
                        }
                    },
                .isDisabled = [] { return IsMobile || !getSettings().game.speedrunMode; },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.showSpeedrunRTATimer,
            {
                .key = "Show RTA",
                .helpText = "Display the RTA timer. IGT is always visible.",
                .isDisabled = [] { return !getSettings().game.speedrunMode; },
            });
    });

    add_tab("Cheats", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addCheat = [&](const Rml::String& key, ConfigVar<bool>& value,
                            const Rml::String& helpText) {
            add_speedrun_disabled_option(leftPane, rightPane, value, key, helpText);
        };

        leftPane.add_section("Resources");
        addCheat("Infinite Hearts", getSettings().game.infiniteHearts, "Keeps your health full.");
        addCheat(
            "Infinite Arrows", getSettings().game.infiniteArrows, "Keeps your arrow count full.");
        addCheat("Infinite Seeds", getSettings().game.infiniteSeeds, "Keeps your slingshot pellets (seeds) full.");
        addCheat("Infinite Bombs", getSettings().game.infiniteBombs, "Keeps all bomb bags full.");
        addCheat("Infinite Oil", getSettings().game.infiniteOil, "Keeps your lantern oil full.");
        addCheat("Infinite Oxygen", getSettings().game.infiniteOxygen,
            "Keeps your underwater oxygen meter full.");
        addCheat(
            "Infinite Rupees", getSettings().game.infiniteRupees, "Keeps your rupee count full.");
        addCheat("No Item Timer", getSettings().game.enableIndefiniteItemDrops,
            "Item drops such as rupees and hearts will never disappear after they drop.");

        leftPane.add_section("Abilities");
        addCheat(
            "Moon Jump (R+A)", getSettings().game.moonJump, "Hold R and A to rise into the air.");
        addCheat("Super Clawshot", getSettings().game.superClawshot,
            "Extends Clawshot behavior beyond the normal game rules.");
        addCheat("Always Greatspin", getSettings().game.alwaysGreatspin,
            "Allows the Great Spin attack without requiring full health.");
        addCheat("Fast Iron Boots", getSettings().game.enableFastIronBoots,
            "Speeds up movement while wearing the Iron Boots.");
        addCheat("Can Transform Anywhere", getSettings().game.canTransformAnywhere,
            "Allows transforming even if NPCs are looking.");
        addCheat("Fast Roll", getSettings().game.fastRoll,
            "Makes Link's roll animation and movement twice as fast.");
        addCheat("Fast Spinner", getSettings().game.fastSpinner,
            "Speeds up Spinner movement while holding R.");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Magic Armor Behavior",
                .getValue =
                    [] {
                        return kMagicArmorModes[static_cast<u8>(getSettings().game.armorRupeeDrain.getValue())];
                    },
                .isDisabled = [] { return getSettings().game.speedrunMode.getValue(); },
                .isModified =
                    [] {
                        return getSettings().game.armorRupeeDrain.getValue() !=
                               getSettings().game.armorRupeeDrain.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < kMagicArmorModes.size(); i++) {
                    pane.add_button({
                            .text = kMagicArmorModes[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.armorRupeeDrain.getValue() == static_cast<MagicArmorMode>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.armorRupeeDrain.setValue(static_cast<MagicArmorMode>(i));
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/>Control the behavior of the Magic Armor.");
            });
        addCheat("Invincible Enemies", getSettings().game.invincibleEnemies,
            "Prevents enemies from taking damage.");
        addCheat("Transform without Shadow Crystal", getSettings().game.transformWithoutShadowCrystal,
            "Allows Link to transform without the Shadow Crystal (Only using Quick Transform.)");
    });

    add_tab("Interface", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Dusklight");
#if DUSK_CAN_OPEN_DATA_FOLDER
        leftPane.register_control(
            leftPane.add_button("Open Data Folder").on_pressed([] {
                mDoAud_seStartMenu(kSoundClick);
                data::open_data_path();
            }),
            rightPane, [](Pane& pane) {
                pane.add_text(
                    "Open the folder where Dusklight stores settings, saves, logs, texture "
                    "replacements, and other app data.");
            });
#endif
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Notifications",
                .getValue = [] {
                    const bool ach = getSettings().game.enableAchievementToasts.getValue();
                    const bool ctl = getSettings().game.enableControllerToasts.getValue();
                    if (!ach && !ctl) {
                        return Rml::String{"Off"};
                    }
                    if (ach && ctl) {
                        return Rml::String{"All"};
                    }
                    return Rml::String{"Some"};
                },
                .isModified = [] {
                    const auto& ach = getSettings().game.enableAchievementToasts;
                    const auto& ctl = getSettings().game.enableControllerToasts;
                    return ach.getValue() != ach.getDefaultValue() || ctl.getValue() != ctl.getDefaultValue();
                },
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_button("Select All").on_pressed([] {
                    mDoAud_seStartMenu(kSoundItemChange);
                    getSettings().game.enableAchievementToasts.setValue(true);
                    getSettings().game.enableControllerToasts.setValue(true);
                    config::save();
                });
                pane.add_button("Select None").on_pressed([] {
                    mDoAud_seStartMenu(kSoundItemChange);
                    getSettings().game.enableAchievementToasts.setValue(false);
                    getSettings().game.enableControllerToasts.setValue(false);
                    config::save();
                });

                pane.add_section("Types");
                pane.add_button(
                    {
                        .text = "Achievements",
                        .isSelected =
                        [] {
                            return getSettings().game.enableAchievementToasts.getValue();
                        },
                    })
                    .on_pressed([] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        auto& v = getSettings().game.enableAchievementToasts;
                        v.setValue(!v.getValue());
                        config::save();
                    });
                pane.add_button(
                    {
                        .text = "Controller",
                        .isSelected =
                            [] { return getSettings().game.enableControllerToasts.getValue(); },
                    })
                    .on_pressed([] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        auto& v = getSettings().game.enableControllerToasts;
                        v.setValue(!v.getValue());
                        config::save();
                    });
                pane.add_rml("<br/>Choose which notifications can be displayed.");
            });
#if BOREALIS_HAS_SENTRY
        auto& crashReporting = leftPane.add_child<BoolButton>(BoolButton::Props{
            .key = "Crash Reporting",
            .getValue =
                [] { return borealis::sentry::get_consent() == borealis::sentry::Consent::Given; },
            .setValue = [](bool enabled) { borealis::sentry::set_consent(enabled); },
            .isDisabled =
                [] {
                    return borealis::sentry::get_consent() ==
                           borealis::sentry::Consent::Unavailable;
                },
            .isModified = [] { return false; },
        });
        leftPane.register_control(crashReporting, rightPane, [](Pane& pane) {
            pane.clear();
            pane.add_rml("Dusklight can automatically send crash reports to the developers. Crash "
                         "reports contain the following:<br/>• Operating system version<br/>• CPU "
                         "architecture<br/>• GPU model & driver version<br/>• File paths (may "
                         "include account username)<br/>• Stack trace");
        });
#endif
        config_bool_select(leftPane, rightPane, getSettings().backend.skipPreLaunchUI,
            {
                .key = "Skip Dusklight Main Menu",
                .helpText = "When starting Dusklight, skip the main menu and boot straight into the "
                            "game if a disc image is available.",
            });
        config_bool_select(leftPane, rightPane, getSettings().backend.showPipelineCompilation,
            {
                .key = "Show Pipeline Compilation",
                .helpText = "Show an overlay when shaders are being compiled for your hardware.",
            });
        config_bool_select(leftPane, rightPane, getSettings().backend.checkForUpdates,
            {
                .key = "Check for Updates",
                .helpText = "Checks GitHub releases for a new Dusklight version on startup.<br/><br/>"
                            "No personal information is transmitted or collected.",
            });
#if BOREALIS_HAS_DISCORD
        config_bool_select(leftPane, rightPane, getSettings().game.enableDiscordPresence,
            {
                .key = "Enable Discord Rich Presence",
                .helpText = "Enable Dusk to integrate with Discord Rich Presence. This allows Discord to show your status in-game.",
                .onChange = [](bool enabled) {
                    if (enabled) {
                        dusk::discord::initialize();
                    } else {
                        dusk::discord::shutdown();
                    }
                },
            });
#endif
        config_bool_select(leftPane, rightPane, getSettings().backend.enableAdvancedSettings,
            {
                .key = "Enable Advanced Settings",
                .icon = "warning",
                .helpText = "Show advanced settings and debugging tools with "
                            "Shift+F1.<br/><br/><icon class=\"warning\"/> WARNING: Debugging tools "
                            "can easily break your game. Do not use on a regular save!",
                .onChange = [](bool) { MenuBar::rebuild(); },
                .isDisabled = [] { return getSettings().game.speedrunMode.getValue(); },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.showInputViewer,
            {
                .key = "Show Input Viewer",
                .helpText = "Display a controller input overlay while playing.",
            });
        config_bool_select(leftPane, rightPane, getSettings().game.showInputViewerGyro,
            {
                .key = "Show Gyro Input Viewer",
                .helpText = "Show gyro sensor values in the input viewer.",
                .isDisabled = [] { return !getSettings().game.showInputViewer; },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.nativePracticeMenu,
            {
                .key = "Native Practice Menu",
                .helpText = "Draw the practice tools menu with the game's own engine so it scales "
                            "with the resolution (controller only). When off, the practice menu "
                            "uses the ImGui interface, which supports the mouse but does not scale "
                            "at low resolutions.",
            });
        leftPane.add_section("Game");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Menu Scaling Mode",
                .getValue =
                    [] {
                        return kMenuScalingModeLabels[static_cast<u8>(
                            getSettings().game.menuScalingMode.getValue())];
                    },
                .isModified =
                    [] {
                        const auto& mode = getSettings().game.menuScalingMode;
                        return mode.getValue() != mode.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kMenuScalingModeLabels.size()); ++i) {
                    pane
                        .add_button({
                            .text = kMenuScalingModeLabels[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.menuScalingMode.getValue() ==
                                           static_cast<MenuScaling>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.menuScalingMode.setValue(
                                static_cast<MenuScaling>(i));
                            config::save();
                        });
                }
                pane.add_rml("<br/>Changes how the Collection and File Select menus scale to your "
                             "aspect ratio.");
            });
        config_bool_select(leftPane, rightPane, getSettings().game.hideTvSettingsScreen,
            {
                .key = "Skip TV Settings Screen",
                .helpText = "Skips the TV calibration screen shown when loading a save.",
            });
        add_speedrun_disabled_option(leftPane, rightPane, getSettings().game.recordingMode,
            "Recording Mode",
            "Disables the game HUD and all background music.<br/><br/>Useful for recording footage.");

        if (!mPrelaunch) {
            if constexpr (dusk::SupportsProcessRestart) {
                leftPane.add_section("System");
                leftPane.register_control(
                    leftPane.add_button("Return to Startup Screen").on_pressed([] {
                        mDoAud_seStartMenu(kSoundClick);
                        confirm_return_to_startup();
                    }),
                    rightPane, [](Pane& pane) {
                        pane.add_text(
                            "Restart Dusklight and return to the startup screen, where you can "
                            "change your disc image, language, graphics backend, and other startup "
                            "options.");
                    });
            }
        }
    });
}

void SettingsWindow::update() {
    if (mPrelaunch && top_document() == this) {
        try_push_verification_modal(*this);
        try_push_language_unavailable_modal(*this);
    }

    Window::update();
}

void SettingsWindow::hide(bool close) {
    config::save();
    Window::hide(close);
}

}  // namespace dusk::ui
