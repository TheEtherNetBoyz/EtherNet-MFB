#include "global.h"

#include "JSystem/J3DGraphBase/J3DShape.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dolphin/gx/GXGet.h"
#include "dolphin/gx/GXPixel.h"
#include "dolphin/gx/GXTransform.h"
#include "m_Do/m_Do_mtx.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"
#include "mods/svc/window.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(WindowService, svc_window);

namespace {

constexpr uint32_t kRenderWidth = 640;
constexpr uint32_t kRenderHeight = 360;
constexpr float kTargetDistance = 100.0f;
constexpr float kLookSensitivity = 0.004f;
constexpr float kFastMultiplier = 4.0f;

// SDL scancodes use the USB HID keyboard-page values. Keeping the handful used here local lets
// this mod consume WindowService input without depending on SDL headers or linking SDL itself.
constexpr int32_t kScancodeA = 4;
constexpr int32_t kScancodeD = 7;
constexpr int32_t kScancodeS = 22;
constexpr int32_t kScancodeW = 26;
constexpr int32_t kScancodeEscape = 41;
constexpr int32_t kScancodeSpace = 44;
constexpr int32_t kScancodeLeftCtrl = 224;
constexpr int32_t kScancodeLeftShift = 225;

ConfigVarHandle g_controls = 0;
ConfigVarHandle g_moveSpeed = 0;
ConfigVarHandle g_fov = 0;

WindowHandle g_window = 0;
GfxPresentTargetHandle g_presentTarget = 0;
GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
WGPURenderPipeline g_presentPipeline = nullptr;
WGPUBindGroupLayout g_presentLayout = nullptr;
WGPUTextureFormat g_presentFormat = WGPUTextureFormat_Undefined;

bool g_resetViewRequested = false;
bool g_windowFocused = false;
bool g_mouseCaptured = false;

struct InputState {
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool fast = false;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
};

InputState g_input;

struct CameraState {
    bool initialized = false;
    cXyz eye;
    cXyz center;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovy = 60.0f;
    s16 bank = 0;
};

CameraState g_camera;

struct PresentPayload {
    WGPUTextureView color;
};

constexpr const char* kPresentShader = R"(
@group(0) @binding(0) var source_color: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    var out: VertexOutput;
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let size = vec2<i32>(textureDimensions(source_color));
    let texel = clamp(vec2<i32>(in.uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    return textureLoad(source_color, texel, 0i);
}
)";

float getFov();

void updateAngles() {
    const cXyz direction = g_camera.center - g_camera.eye;
    g_camera.yaw = std::atan2(direction.z, direction.x);
    const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    g_camera.pitch = std::atan2(direction.y, horizontal);
}

void updateCenter() {
    const float horizontal = std::cos(g_camera.pitch);
    g_camera.center.x =
        g_camera.eye.x + std::cos(g_camera.yaw) * horizontal * kTargetDistance;
    g_camera.center.y = g_camera.eye.y + std::sin(g_camera.pitch) * kTargetDistance;
    g_camera.center.z =
        g_camera.eye.z + std::sin(g_camera.yaw) * horizontal * kTargetDistance;
}

bool resetFreeCamera() {
    daAlink_c* player = daAlink_getAlinkActorClass();
    if (player == nullptr) {
        return false;
    }

    // Link is used only as a one-time spawn anchor. Start close enough to make
    // the relationship obvious, then leave Camera 2 completely independent.
    g_camera.center = player->current.pos + cXyz(0.0f, 70.0f, 0.0f);
    g_camera.eye = player->current.pos + cXyz(0.0f, 140.0f, 180.0f);
    g_camera.fovy = getFov();
    g_camera.bank = 0;
    g_camera.initialized = true;
    updateAngles();
    return true;
}

bool getControlsEnabled() {
    bool value = false;
    svc_config->get_bool(mod_ctx, g_controls, &value);
    return value;
}

float getMoveSpeed() {
    int64_t value = 900;
    svc_config->get_int(mod_ctx, g_moveSpeed, &value);
    return static_cast<float>(std::clamp<int64_t>(value, 1, 10000));
}

float getFov() {
    int64_t value = 60;
    svc_config->get_int(mod_ctx, g_fov, &value);
    return static_cast<float>(std::clamp<int64_t>(value, 1, 179));
}

void updateControls(float deltaSeconds) {
    if (!getControlsEnabled() || !g_camera.initialized || !g_windowFocused) {
        g_input.mouseDeltaX = 0.0f;
        g_input.mouseDeltaY = 0.0f;
        return;
    }

    float forward = (g_input.forward ? 1.0f : 0.0f) -
                    (g_input.backward ? 1.0f : 0.0f);
    float right = (g_input.right ? 1.0f : 0.0f) - (g_input.left ? 1.0f : 0.0f);
    float up = (g_input.up ? 1.0f : 0.0f) - (g_input.down ? 1.0f : 0.0f);
    const float lookX = g_input.mouseDeltaX;
    const float lookY = g_input.mouseDeltaY;
    const bool fast = g_input.fast;

    forward = std::clamp(forward, -1.0f, 1.0f);
    right = std::clamp(right, -1.0f, 1.0f);
    const float length = std::sqrt(forward * forward + right * right + up * up);
    if (length > 1.0f) {
        forward /= length;
        right /= length;
        up /= length;
    }

    const float speed = getMoveSpeed() * std::max(deltaSeconds, 0.0f) *
                        (fast ? kFastMultiplier : 1.0f);
    g_camera.eye.x +=
        (forward * std::cos(g_camera.yaw) - right * std::sin(g_camera.yaw)) * speed;
    g_camera.eye.y += up * speed;
    g_camera.eye.z +=
        (forward * std::sin(g_camera.yaw) + right * std::cos(g_camera.yaw)) * speed;
    g_camera.yaw += lookX * kLookSensitivity;
    g_camera.pitch -= lookY * kLookSensitivity;
    g_camera.pitch = std::clamp(g_camera.pitch, -1.553343f, 1.553343f);
    updateCenter();
    g_input.mouseDeltaX = 0.0f;
    g_input.mouseDeltaY = 0.0f;
}

bool drawListsReady() {
    return dComIfGd_getOpaListBG() != nullptr && dComIfGd_getOpaList() != nullptr &&
           dComIfGd_getOpaListDark() != nullptr && dComIfGd_getXluListBG() != nullptr &&
           dComIfGd_getListPacket() != nullptr;
}

void drawSceneLists() {
    dComIfGd_drawOpaListSky();
    dComIfGd_drawXluListSky();
    dComIfGd_drawOpaListBG();
    dComIfGd_drawOpaListDarkBG();
    dComIfGd_drawOpaListMiddle();
    dComIfGd_drawOpaList();
    dComIfGd_drawOpaListDark();
    dComIfGd_drawOpaListPacket();
    dComIfGd_drawXluListBG();
    dComIfGd_drawXluListDarkBG();
    dComIfGd_drawXluList();
    dComIfGd_drawXluListDark();
    dComIfGd_drawXluListZxlu();
}

void restoreGameRenderState(const Mtx savedView, const f32 savedProjection[7],
    const f32 savedViewport[6], const u32 savedScissor[4]) {
    j3dSys.setViewMtx(savedView);
    GXSetProjectionv(savedProjection);
    GXSetViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3],
        savedViewport[4], savedViewport[5]);
    GXSetScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);
    dKy_setLight();
}

void renderCamera2() {
    if (g_presentTarget == 0 || !g_camera.initialized || !drawListsReady()) {
        return;
    }

    f32 savedProjection[7];
    GXGetProjectionv(savedProjection);
    f32 savedViewport[6];
    GXGetViewportv(savedViewport);
    u32 savedScissor[4];
    GXGetScissor(&savedScissor[0], &savedScissor[1], &savedScissor[2], &savedScissor[3]);
    Mtx savedView;
    cMtx_copy(j3dSys.getViewMtx(), savedView);

    if (svc_gfx->create_pass(mod_ctx, kRenderWidth, kRenderHeight) != MOD_OK) {
        return;
    }

    Mtx cameraView;
    Mtx44 cameraProjection;
    cXyz up(0.0f, 1.0f, 0.0f);
    cMtx_lookAt(cameraView, &g_camera.eye, &g_camera.center, &up, g_camera.bank);
    C_MTXPerspective(cameraProjection, getFov(),
        static_cast<float>(kRenderWidth) / static_cast<float>(kRenderHeight), 1.0f, 100000.0f);

    j3dSys.setViewMtx(cameraView);
    if (daAlink_c* player = daAlink_getAlinkActorClass()) {
        player->refreshPlayerModelsForCurrentView();
    }
    GXSetProjectionFull(cameraProjection);
    GXSetViewport(0.0f, 0.0f, static_cast<float>(kRenderWidth),
        static_cast<float>(kRenderHeight), 0.0f, 1.0f);
    GXSetViewportRender(0.0f, 0.0f, static_cast<float>(kRenderWidth),
        static_cast<float>(kRenderHeight), 0.0f, 1.0f);
    GXSetScissorRender(0, 0, kRenderWidth, kRenderHeight);
    dKy_setLight();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    J3DShape::resetVcdVatCache();
    drawSceneLists();
    j3dSys.setViewMtx(savedView);
    if (daAlink_c* player = daAlink_getAlinkActorClass()) {
        player->refreshPlayerModelsForCurrentView();
    }
    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restoreGameRenderState(savedView, savedProjection, savedViewport, savedScissor);

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.depth = false;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr) {
        restoreGameRenderState(savedView, savedProjection, savedViewport, savedScissor);
        return;
    }

    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restoreGameRenderState(savedView, savedProjection, savedViewport, savedScissor);

    const PresentPayload payload{.color = resolved.color};
    svc_gfx->push_present(mod_ctx, g_presentTarget, &payload, sizeof(payload));
}

void releasePresentPipeline() {
    if (g_presentPipeline != nullptr) {
        wgpuRenderPipelineRelease(g_presentPipeline);
        g_presentPipeline = nullptr;
    }
    if (g_presentLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_presentLayout);
        g_presentLayout = nullptr;
    }
    g_presentFormat = WGPUTextureFormat_Undefined;
}

bool ensurePresentPipeline(const GfxPresentContext& ctx) {
    if (g_presentPipeline != nullptr && g_presentFormat == ctx.target_format) {
        return true;
    }
    releasePresentPipeline();

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {kPresentShader, WGPU_STRLEN};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"camera 2 present", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(ctx.device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = ctx.target_format;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {"camera 2 present", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.fragment = &fragment;
    g_presentPipeline = wgpuDeviceCreateRenderPipeline(ctx.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (g_presentPipeline == nullptr) {
        return false;
    }
    g_presentLayout = wgpuRenderPipelineGetBindGroupLayout(g_presentPipeline, 0);
    if (g_presentLayout == nullptr) {
        releasePresentPipeline();
        return false;
    }
    g_presentFormat = ctx.target_format;
    return true;
}

void onPresent(ModContext*, const GfxPresentContext* ctx, const void* payload,
    size_t payloadSize, void*) {
    WGPUBindGroup bindGroup = nullptr;
    if (payloadSize == sizeof(PresentPayload) && ensurePresentPipeline(*ctx)) {
        PresentPayload data;
        std::memcpy(&data, payload, sizeof(data));
        if (data.color != nullptr) {
            WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
            entry.binding = 0;
            entry.textureView = data.color;
            WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bindGroupDesc.layout = g_presentLayout;
            bindGroupDesc.entryCount = 1;
            bindGroupDesc.entries = &entry;
            bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
        }
    }

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = ctx->target_view;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = WGPUColor{0.02, 0.02, 0.025, 1.0};
    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"camera 2 present", WGPU_STRLEN};
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &passDesc);
    if (bindGroup != nullptr) {
        wgpuRenderPassEncoderSetPipeline(pass, g_presentPipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuBindGroupRelease(bindGroup);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

ModResult closeWindow() {
    if (g_window != 0 && g_mouseCaptured) {
        svc_window->set_relative_mouse_mode(mod_ctx, g_window, false);
        g_mouseCaptured = false;
    }
    if (g_presentTarget != 0) {
        const ModResult result = svc_gfx->unregister_present_target(mod_ctx, g_presentTarget);
        if (result != MOD_OK) {
            return result;
        }
        g_presentTarget = 0;
    }
    if (g_window != 0) {
        const ModResult result = svc_window->destroy_window(mod_ctx, g_window);
        if (result != MOD_OK) {
            return result;
        }
        g_window = 0;
    }
    g_windowFocused = false;
    g_input = {};
    return MOD_OK;
}

void setKeyState(const int32_t scancode, const bool down) {
    switch (scancode) {
    case kScancodeW:
        g_input.forward = down;
        break;
    case kScancodeS:
        g_input.backward = down;
        break;
    case kScancodeA:
        g_input.left = down;
        break;
    case kScancodeD:
        g_input.right = down;
        break;
    case kScancodeSpace:
        g_input.up = down;
        break;
    case kScancodeLeftCtrl:
        g_input.down = down;
        break;
    case kScancodeLeftShift:
        g_input.fast = down;
        break;
    default:
        break;
    }
}

void activateFreeCameraControls() {
    g_windowFocused = true;
    svc_config->set_bool(mod_ctx, g_controls, true);
}

void onWindowEvent(ModContext*, WindowHandle, const WindowEvent* event, void*) {
    if (event->type == WINDOW_EVENT_CLOSE_REQUESTED) {
        closeWindow();
    } else if (event->type == WINDOW_EVENT_FOCUS_GAINED) {
        activateFreeCameraControls();
    } else if (event->type == WINDOW_EVENT_FOCUS_LOST) {
        g_windowFocused = false;
        g_input = {};
    } else if (event->type == WINDOW_EVENT_KEY_DOWN) {
        if (event->scancode == kScancodeEscape) {
            svc_config->set_bool(mod_ctx, g_controls, false);
            g_input = {};
        } else {
            setKeyState(event->scancode, true);
        }
    } else if (event->type == WINDOW_EVENT_KEY_UP) {
        setKeyState(event->scancode, false);
    } else if (event->type == WINDOW_EVENT_MOUSE_BUTTON_DOWN) {
        // A click should recapture controls even if Escape released them while this
        // window remained focused (which does not generate another focus event).
        activateFreeCameraControls();
    } else if (event->type == WINDOW_EVENT_MOUSE_MOTION && g_mouseCaptured) {
        g_input.mouseDeltaX += event->mouse_delta_x;
        g_input.mouseDeltaY += event->mouse_delta_y;
    }
}

void syncMouseCapture() {
    if (g_window == 0) {
        g_mouseCaptured = false;
        return;
    }
    const bool wanted = g_windowFocused && getControlsEnabled();
    if (wanted == g_mouseCaptured) {
        return;
    }
    if (svc_window->set_relative_mouse_mode(mod_ctx, g_window, wanted) == MOD_OK) {
        g_mouseCaptured = wanted;
    }
}

ModResult openWindow() {
    if (g_window != 0) {
        return MOD_CONFLICT;
    }

    // Each newly enabled Camera 2 session starts near the current gameplay Link
    // instead of reusing a stale free-camera position from a previous window.
    g_resetViewRequested = true;

    WindowDesc windowDesc = WINDOW_DESC_INIT;
    windowDesc.title = "Camera 2";
    windowDesc.width = kRenderWidth;
    windowDesc.height = kRenderHeight;
    windowDesc.on_event = onWindowEvent;
    ModResult result = svc_window->create_window(mod_ctx, &windowDesc, &g_window);
    if (result != MOD_OK) {
        return result;
    }

    GfxPresentTargetDesc presentDesc = GFX_PRESENT_TARGET_DESC_INIT;
    presentDesc.label = "Camera 2 surface";
    presentDesc.render = onPresent;
    result = svc_gfx->register_window_present_target(
        mod_ctx, g_window, &presentDesc, &g_presentTarget);
    if (result != MOD_OK) {
        closeWindow();
        return result;
    }
    result = svc_window->show_window(mod_ctx, g_window);
    if (result != MOD_OK) {
        closeWindow();
    }
    return result;
}

void onToggleWindow(ModContext*, void*) {
    const ModResult result = g_window == 0 ? openWindow() : closeWindow();
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, g_window == 0 ? "failed to open Camera 2 window" :
                                                  "failed to close Camera 2 window");
    }
}

void onResetView(ModContext*, void*) {
    g_resetViewRequested = true;
}

void onSceneBegin(ModContext*, const GfxStageContext* stageCtx, void*) {
    if (stageCtx == nullptr || stageCtx->game_view == nullptr) {
        return;
    }
    if (!g_camera.initialized || g_resetViewRequested) {
        if (resetFreeCamera()) {
            g_resetViewRequested = false;
        }
    }
}

void onFrameBeforeHud(ModContext*, const GfxStageContext*, void*) {
    renderCamera2();
}

void addToggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

void addNumber(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

ModResult buildPanel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Camera 2 Window");
    UiControlDesc windowControl = UI_CONTROL_DESC_INIT;
    windowControl.kind = UI_CONTROL_BUTTON;
    windowControl.label = "Open / Close Camera 2";
    windowControl.on_pressed = onToggleWindow;
    svc_ui->pane_add_control(mod_ctx, panel, &windowControl, nullptr);

    UiControlDesc resetControl = UI_CONTROL_DESC_INIT;
    resetControl.kind = UI_CONTROL_BUTTON;
    resetControl.label = "Reset Free Camera";
    resetControl.help_rml =
        "Spawns Camera 2 just above and behind Link once; it remains independent afterward.";
    resetControl.on_pressed = onResetView;
    svc_ui->pane_add_control(mod_ctx, panel, &resetControl, nullptr);
    addToggle(panel, "Control Camera 2", g_controls,
        "Camera 2 is an independent free camera. Click its window to capture input. WASD moves, mouse looks, Space/Ctrl move vertically, Shift speeds up, and Escape releases the mouse.");
    addNumber(panel, "Move Speed", g_moveSpeed, 1, 10000, 50, nullptr,
        "Camera 2 movement speed in world units per second.");
    addNumber(panel, "Field of View", g_fov, 1, 179, 1, " degrees",
        "Camera 2 vertical field of view.");
    return MOD_OK;
}

ModResult registerBool(const char* name, bool defaultValue, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Camera 2 option");
    }
    return MOD_OK;
}

ModResult registerInt(const char* name, int64_t defaultValue, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Camera 2 option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = registerBool("controlsEnabled", false, g_controls, error);
    if (result != MOD_OK) return result;
    result = registerInt("moveSpeed", 900, g_moveSpeed, error);
    if (result != MOD_OK) return result;
    result = registerInt("fov", 60, g_fov, error);
    if (result != MOD_OK) return result;

    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = onSceneBegin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Camera 2 scene hook");
    }
    stageDesc.callback = onFrameBeforeHud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_frameBeforeHudHook) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Camera 2 frame hook");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = buildPanel;
    if (svc_ui->register_mods_panel(mod_ctx, &panelDesc) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Camera 2 controls");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    syncMouseCapture();
    updateControls(1.0f / 60.0f);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    if (g_sceneBeginHook != 0) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_sceneBeginHook);
        g_sceneBeginHook = 0;
    }
    if (g_frameBeforeHudHook != 0) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_frameBeforeHudHook);
        g_frameBeforeHudHook = 0;
    }
    closeWindow();
    releasePresentPipeline();
    g_controls = g_moveSpeed = g_fov = 0;
    return MOD_OK;
}

}
