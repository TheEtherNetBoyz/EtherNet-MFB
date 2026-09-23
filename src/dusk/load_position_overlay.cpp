#include "dusk/load_position_overlay.hpp"

#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JUtility/JUTResFont.h"
#include "JSystem/JUtility/TColor.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "dusk/config.hpp"
#include "dusk/game_clock.h"
#include "dusk/settings.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_graphic.h"
#include "dolphin/pad.h"
#include <SDL3/SDL_gamepad.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dusk {
namespace {

JUTResFont* get_debug_font() {
    static JUTResFont s_font((const ResFONT*)JUTResFONT_Ascfont_fix12, nullptr);
    return s_font.isValid() ? &s_font : nullptr;
}

void draw_text(JUTFont* font, float x, float y, float size, const char* text) {
    if (font == nullptr) {
        return;
    }

    J2DTextBox tb;
    tb.setFont(font);
    tb.setFontSize(size, size);
    tb.setString(text);

    const JUtility::TColor white(0xFF, 0xFF, 0xFF, 0xFF);
    tb.setFontColor(white, white);
    tb.draw(x, y);
}

const char* drift_direction(float forward, float right) {
    constexpr float threshold = 0.000001f;
    static char direction[24];

    if (std::fabs(forward) < threshold && std::fabs(right) < threshold) {
        return "none";
    }

    const float absForward = std::fabs(forward);
    const float absRight = std::fabs(right);

    if (absForward >= absRight * 1.35f) {
        return forward >= 0.0f ? "forward" : "backward";
    }
    if (absRight >= absForward * 1.35f) {
        return right >= 0.0f ? "right" : "left";
    }

    std::snprintf(direction, sizeof(direction), "%s/%s",
                  forward >= 0.0f ? "forward" : "backward",
                  right >= 0.0f ? "right" : "left");
    return direction;
}

void project_drift(float dx, float dz, s16 facingAngle, float& forward, float& right) {
    const float forwardX = cM_ssin(facingAngle);
    const float forwardZ = cM_scos(facingAngle);
    forward = dx * forwardX + dz * forwardZ;
    // Link's anatomical right is the negative local-X axis of the rendered model.
    right = -dx * forwardZ + dz * forwardX;
}

// Drift is accumulated in Link-local space on every simulation tick. The published
// forward/right result is refreshed every five minutes of simulation time.
constexpr float kDriftSampleSeconds = 5.0f * 60.0f;
constexpr unsigned int kSlidePositionSaveTicks = 20 * 60 * 30;
bool s_havePreviousPosition = false;
float s_previousX = 0.0f;
float s_previousZ = 0.0f;
float s_accumulatedForward = 0.0f;
float s_accumulatedRight = 0.0f;
float s_publishedForward = 0.0f;
float s_publishedRight = 0.0f;
float s_driftWindowSeconds = 0.0f;
unsigned int s_slidePositionTicks = 0;
std::string s_slideTimerStage;
s8 s_slideTimerRoom = -1;
s8 s_slideTimerLayer = -1;
bool s_slideTimerAreaValid = false;
cXyz s_loggedSlidePosition;
s16 s_loggedSlideAngle = 0;
std::string s_loggedSlideStage;
s8 s_loggedSlideRoom = -1;
s8 s_loggedSlideLayer = -1;
bool s_loggedSlidePositionValid = false;
bool s_loggedSlidePositionLoaded = false;
bool s_overlayVisible = true;
bool s_overlayToggleComboHeld = false;
float s_overlayScale = 1.0f;
enum class OverlaySnap { None, TopLeft, TopRight, BottomLeft, BottomRight };
OverlaySnap s_overlaySnapRequest = OverlaySnap::None;

bool overlayUsesImGui() {
    const auto mode = getSettings().game.rupeeSlideOverlayMode.getValue();
    return mode == RupeeSlideOverlayMode::ImGui || mode == RupeeSlideOverlayMode::Both;
}

bool overlayUsesNative() {
    const auto mode = getSettings().game.rupeeSlideOverlayMode.getValue();
    return mode == RupeeSlideOverlayMode::Native || mode == RupeeSlideOverlayMode::Both;
}

bool overlayToggleComboHeld() {
    const u32 physicalHold = mDoCPd_c::getUnfilteredHold(PAD_1);
    const u32 hold = mDoCPd_c::getHold(PAD_1);
    const SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(PAD_1);
    const bool leftShoulderHeld =
        gamepad != nullptr && SDL_GetGamepadButton(
            const_cast<SDL_Gamepad*>(gamepad), SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const bool lHeld = (physicalHold & PAD_TRIGGER_L) != 0 ||
                       (hold & PAD_TRIGGER_L) != 0 || leftShoulderHeld ||
                       mDoCPd_c::getHoldLockL(PAD_1) || mDoCPd_c::getAnalogL(PAD_1) > 0.6f;
    const bool startHeld = (physicalHold & PAD_BUTTON_START) != 0 ||
                           (hold & PAD_BUTTON_START) != 0;
    return lHeld && startHeld;
}

void loadLoggedSlidePosition() {
    if (s_loggedSlidePositionLoaded) {
        return;
    }

    const auto& game = getSettings().game;
    s_loggedSlidePosition.set(
        game.rupeeSlidePositionX.getValue(), game.rupeeSlidePositionY.getValue(),
        game.rupeeSlidePositionZ.getValue());
    s_loggedSlideAngle = static_cast<s16>(game.rupeeSlideAngleY.getValue());
    s_loggedSlideStage = game.rupeeSlideStage.getValue();
    s_loggedSlideRoom = static_cast<s8>(game.rupeeSlideRoom.getValue());
    s_loggedSlideLayer = static_cast<s8>(game.rupeeSlideLayer.getValue());
    s_loggedSlidePositionValid = game.rupeeSlidePositionValid.getValue();
    s_loggedSlidePositionLoaded = true;
}

bool loggedSlidePositionMatchesCurrentArea() {
    const char* stageName = dComIfGp_getStartStageName();
    return s_loggedSlidePositionValid && stageName != nullptr &&
           s_loggedSlideStage == stageName &&
           s_loggedSlideRoom == dComIfGp_roomControl_getStayNo() &&
           s_loggedSlideLayer == dComIfGp_getStartStageLayer();
}

void persistCurrentSlidePosition(const daAlink_c* link) {
    const char* stageName = dComIfGp_getStartStageName();
    if (link == nullptr || stageName == nullptr) {
        return;
    }

    s_loggedSlidePosition = link->current.pos;
    s_loggedSlideAngle = link->shape_angle.y;
    s_loggedSlideStage = stageName;
    s_loggedSlideRoom = static_cast<s8>(dComIfGp_roomControl_getStayNo());
    s_loggedSlideLayer = dComIfGp_getStartStageLayer();
    s_loggedSlidePositionValid = true;

    auto& game = getSettings().game;
    game.rupeeSlidePositionX.setValue(s_loggedSlidePosition.x);
    game.rupeeSlidePositionY.setValue(s_loggedSlidePosition.y);
    game.rupeeSlidePositionZ.setValue(s_loggedSlidePosition.z);
    game.rupeeSlideAngleY.setValue(static_cast<u16>(s_loggedSlideAngle));
    game.rupeeSlideStage.setValue(s_loggedSlideStage);
    game.rupeeSlideRoom.setValue(s_loggedSlideRoom);
    game.rupeeSlideLayer.setValue(s_loggedSlideLayer);
    game.rupeeSlidePositionValid.setValue(true);
    config::save();
}

void updateLoggedSlidePosition(const daAlink_c* link) {
    loadLoggedSlidePosition();
    if (link == nullptr || getSettings().game.speedrunMode.getValue() ||
        dComIfGp_isEnableNextStage()) {
        return;
    }

    const char* stageName = dComIfGp_getStartStageName();
    if (stageName == nullptr) {
        return;
    }

    const s8 room = static_cast<s8>(dComIfGp_roomControl_getStayNo());
    const s8 layer = dComIfGp_getStartStageLayer();
    if (!s_slideTimerAreaValid || s_slideTimerStage != stageName ||
        s_slideTimerRoom != room || s_slideTimerLayer != layer) {
        s_slideTimerStage = stageName;
        s_slideTimerRoom = room;
        s_slideTimerLayer = layer;
        s_slideTimerAreaValid = true;
        s_slidePositionTicks = 0;
        return;
    }

    if (++s_slidePositionTicks >= kSlidePositionSaveTicks) {
        persistCurrentSlidePosition(link);
        s_slidePositionTicks = 0;
    }
}

}  // namespace

void UpdateLoadPositionOverlayInput() {
    const bool comboHeld = overlayToggleComboHeld();
    if (comboHeld && !s_overlayToggleComboHeld) {
        s_overlayVisible = !s_overlayVisible;
    }
    s_overlayToggleComboHeld = comboHeld;
}

void UpdateLoadPositionDriftNative() {
    UpdateLoadPositionOverlayInput();
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const daAlink_c* link = daAlink_getAlinkActorClass();
    if (player == nullptr || link == nullptr) {
        s_havePreviousPosition = false;
        return;
    }

    updateLoggedSlidePosition(link);

    const float xPos = player->current.pos.x;
    const float zPos = player->current.pos.z;
    if (!s_havePreviousPosition) {
        s_previousX = xPos;
        s_previousZ = zPos;
        s_havePreviousPosition = true;
        return;
    }

    const float dx = xPos - s_previousX;
    const float dz = zPos - s_previousZ;
    const s16 visualFacingAngle =
        static_cast<s16>(link->shape_angle.y + link->field_0x308c);
    float tickForward;
    float tickRight;
    project_drift(dx, dz, visualFacingAngle, tickForward, tickRight);
    s_accumulatedForward += tickForward;
    s_accumulatedRight += tickRight;
    s_previousX = xPos;
    s_previousZ = zPos;

    s_driftWindowSeconds += game_clock::sim_pace();
    if (s_driftWindowSeconds >= kDriftSampleSeconds) {
        s_publishedForward = s_accumulatedForward;
        s_publishedRight = s_accumulatedRight;
        s_accumulatedForward = 0.0f;
        s_accumulatedRight = 0.0f;
        s_driftWindowSeconds = 0.0f;
    }
}

void DrawLoadPositionOverlayImGui() {
    if (!overlayUsesImGui() || !s_overlayVisible) {
        return;
    }

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(8.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 92.0f), ImGuiCond_FirstUseEver);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Rupee Slide Position", nullptr, flags)) {
        ImGui::SetWindowFontScale(s_overlayScale);

        const float dragWidth = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##RupeeSlideDrag", ImVec2(dragWidth, 7.0f),
                               ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemActive()) {
            ImVec2 position = ImGui::GetWindowPos();
            position.x += ImGui::GetIO().MouseDelta.x;
            position.y += ImGui::GetIO().MouseDelta.y;
            ImGui::SetWindowPos(position, ImGuiCond_Always);
        }

        if (ImGui::BeginPopupContextWindow("Rupee Slide Position Menu")) {
            ImGui::TextUnformatted("Overlay layout");
            ImGui::Separator();
            ImGui::SliderFloat("Scale", &s_overlayScale, 0.75f, 2.0f, "%.2fx");
            ImGui::Separator();
            if (ImGui::MenuItem("Snap top-left")) {
                s_overlaySnapRequest = OverlaySnap::TopLeft;
            }
            if (ImGui::MenuItem("Snap top-right")) {
                s_overlaySnapRequest = OverlaySnap::TopRight;
            }
            if (ImGui::MenuItem("Snap bottom-left")) {
                s_overlaySnapRequest = OverlaySnap::BottomLeft;
            }
            if (ImGui::MenuItem("Snap bottom-right")) {
                s_overlaySnapRequest = OverlaySnap::BottomRight;
            }
            if (ImGui::MenuItem("Reset position")) {
                s_overlaySnapRequest = OverlaySnap::TopLeft;
            }
            ImGui::EndPopup();
        }

        if (s_overlaySnapRequest != OverlaySnap::None) {
            constexpr float margin = 12.0f;
            const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
            const ImVec2 windowSize = ImGui::GetWindowSize();
            ImVec2 position(margin, margin);
            if (s_overlaySnapRequest == OverlaySnap::TopRight ||
                s_overlaySnapRequest == OverlaySnap::BottomRight) {
                position.x = std::max(margin, displaySize.x - windowSize.x - margin);
            }
            if (s_overlaySnapRequest == OverlaySnap::BottomLeft ||
                s_overlaySnapRequest == OverlaySnap::BottomRight) {
                position.y = std::max(margin, displaySize.y - windowSize.y - margin);
            }
            ImGui::SetWindowPos(position, ImGuiCond_Always);
            s_overlaySnapRequest = OverlaySnap::None;
        }

        ImGui::Text("X Pos: %.3f | Z Pos: %.3f", player->current.pos.x, player->current.pos.z);
        ImGui::Text("Y Pos: %.3f | Angle: %u", player->current.pos.y,
                    static_cast<u16>(player->shape_angle.y));
        ImGui::Text("Drift: %s  F:%+.6f R:%+.6f",
                    drift_direction(s_publishedForward, s_publishedRight),
                    s_publishedForward, s_publishedRight);
    }
    ImGui::End();
}

bool GetLoggedRupeeSlidePosition(cXyz& position, s16& angle) {
    loadLoggedSlidePosition();
    if (!loggedSlidePositionMatchesCurrentArea()) {
        return false;
    }
    position = s_loggedSlidePosition;
    angle = s_loggedSlideAngle;
    return true;
}

void DrawLoadPositionOverlayNative() {
    if (!overlayUsesNative()) {
        return;
    }

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    const float xPos = player->current.pos.x;
    const float zPos = player->current.pos.z;

    if (!s_overlayVisible) {
        return;
    }

    J2DGrafContext* port = dComIfGp_getCurrentGrafPort();
    if (port == nullptr) {
        return;
    }
    port->setPort();

    JUTFont* font = get_debug_font();
    char line[96];
    constexpr float left = 8.0f;
    constexpr float top = 12.0f;
    constexpr float size = 7.0f;
    constexpr float lineHeight = 9.0f;
    const float x = mDoGph_gInf_c::ScaleHUDXLeft(left);
    const float y = mDoGph_gInf_c::getSafeMinYF() + top;

    std::snprintf(line, sizeof(line), "X Pos: %f | Z Pos:%f", xPos, zPos);
    draw_text(font, x, y, size, line);

    const float nextLineY = y + lineHeight;
    std::snprintf(line, sizeof(line), "Y Pos: %f | Angle:%u", player->current.pos.y,
                  static_cast<u16>(player->shape_angle.y));
    draw_text(font, x, nextLineY, size, line);
    std::snprintf(line, sizeof(line), "Drift: %s  F:%+.6f R:%+.6f",
                  drift_direction(s_publishedForward, s_publishedRight), s_publishedForward,
                  s_publishedRight);
    draw_text(font, x, nextLineY + lineHeight, size, line);
}

}
