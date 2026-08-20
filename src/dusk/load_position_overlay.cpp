#include "dusk/load_position_overlay.hpp"

#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JUtility/JUTResFont.h"
#include "JSystem/JUtility/TColor.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "dusk/config.hpp"
#include "dusk/settings.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_graphic.h"

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

// Drift is accumulated in Link-local space on every simulation tick. This ensures each
// displacement uses the rendered orientation from the tick in which it actually happened.
constexpr unsigned int kDriftSampleTicks = 5 * 60 * 30;
constexpr unsigned int kSlidePositionSaveTicks = 20 * 60 * 30;
bool s_havePreviousPosition = false;
float s_previousX = 0.0f;
float s_previousZ = 0.0f;
float s_accumulatedForward = 0.0f;
float s_accumulatedRight = 0.0f;
float s_publishedForward = 0.0f;
float s_publishedRight = 0.0f;
unsigned int s_ticksInWindow = 0;
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

void UpdateLoadPositionDriftNative() {
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

    if (++s_ticksInWindow >= kDriftSampleTicks) {
        s_publishedForward = s_accumulatedForward;
        s_publishedRight = s_accumulatedRight;
        s_accumulatedForward = 0.0f;
        s_accumulatedRight = 0.0f;
        s_ticksInWindow = 0;
    }
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
    static bool s_visible = true;
    static bool s_toggleComboHeld = false;
    const u32 hold = mDoCPd_c::getHold(PAD_1);
    const bool lHeld = (hold & PAD_TRIGGER_L) != 0 || mDoCPd_c::getHoldLockL(PAD_1) ||
                       mDoCPd_c::getAnalogL(PAD_1) > 0.6f;
    const bool startHeld = (hold & PAD_BUTTON_START) != 0;
    const bool toggleComboHeld = lHeld && startHeld;
    if (toggleComboHeld && !s_toggleComboHeld) {
        s_visible = !s_visible;
    }
    s_toggleComboHeld = toggleComboHeld;

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    const float xPos = player->current.pos.x;
    const float zPos = player->current.pos.z;

    if (!s_visible) {
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
