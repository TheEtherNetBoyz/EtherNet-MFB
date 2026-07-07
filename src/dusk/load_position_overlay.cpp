#include "dusk/load_position_overlay.hpp"

#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JUtility/JUTResFont.h"
#include "JSystem/JUtility/TColor.h"
#include "SSystem/SComponent/c_math.h"
#include "d/d_com_inf_game.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_graphic.h"

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

const char* drift_direction(float dx, float dz, s16 facingAngle) {
    constexpr float threshold = 0.000001f;
    static char direction[24];

    if (std::fabs(dx) < threshold && std::fabs(dz) < threshold) {
        return "none";
    }

    const float forwardX = cM_ssin(facingAngle);
    const float forwardZ = cM_scos(facingAngle);
    const float rightX = forwardZ;
    const float rightZ = -forwardX;
    const float forwardDot = dx * forwardX + dz * forwardZ;
    const float rightDot = dx * rightX + dz * rightZ;
    const float absForward = std::fabs(forwardDot);
    const float absRight = std::fabs(rightDot);

    if (absForward >= absRight * 1.35f) {
        return forwardDot >= 0.0f ? "forward" : "backward";
    }
    if (absRight >= absForward * 1.35f) {
        return rightDot >= 0.0f ? "right" : "left";
    }

    std::snprintf(direction, sizeof(direction), "%s/%s",
                  forwardDot >= 0.0f ? "forward" : "backward",
                  rightDot >= 0.0f ? "right" : "left");
    return direction;
}

}  // namespace

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
    if (!s_visible) {
        return;
    }

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    static bool s_haveMax = false;
    static float s_maxX = 0.0f;
    static float s_maxZ = 0.0f;
    constexpr int kDriftSampleFrames = 20 * 60 * 30;
    static bool s_haveDriftSample = false;
    static float s_sampleX = 0.0f;
    static float s_sampleZ = 0.0f;
    static float s_driftX = 0.0f;
    static float s_driftZ = 0.0f;
    static int s_sampleTimer = 0;

    const float xPos = player->current.pos.x;
    const float zPos = player->current.pos.z;
    if (!s_haveMax) {
        s_maxX = xPos;
        s_maxZ = zPos;
        s_haveMax = true;
    } else {
        s_maxX = std::max(s_maxX, xPos);
        s_maxZ = std::max(s_maxZ, zPos);
    }

    if (!s_haveDriftSample) {
        s_sampleX = xPos;
        s_sampleZ = zPos;
        s_driftX = 0.0f;
        s_driftZ = 0.0f;
        s_sampleTimer = 0;
        s_haveDriftSample = true;
    } else if (++s_sampleTimer >= kDriftSampleFrames) {
        s_driftX = xPos - s_sampleX;
        s_driftZ = zPos - s_sampleZ;
        s_sampleX = xPos;
        s_sampleZ = zPos;
        s_sampleTimer = 0;
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
    constexpr float size = 9.0f;
    constexpr float lineHeight = 12.0f;
    const float x = mDoGph_gInf_c::ScaleHUDXLeft(left);
    const float y = mDoGph_gInf_c::getSafeMinYF() + top;

    std::snprintf(line, sizeof(line), "X Pos: %f | Z Pos:%f", xPos, zPos);
    draw_text(font, x, y, size, line);
    std::snprintf(line, sizeof(line), "X Max:%f | Z Max:%f", s_maxX, s_maxZ);
    draw_text(font, x, y + lineHeight, size, line);
    std::snprintf(line, sizeof(line), "Drift: %s  dX:%+.6f dZ:%+.6f",
                  drift_direction(s_driftX, s_driftZ, player->shape_angle.y), s_driftX,
                  s_driftZ);
    draw_text(font, x, y + lineHeight * 2.0f, size, line);
}

}
