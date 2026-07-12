#include "dusk/video_latency.h"

#if TARGET_PC

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

#include <dolphin/gx.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/mtx.h>

#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"
#include "dusk/settings.h"

namespace dusk::video_latency {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaximumDelayMs = 150;
// Enough history for 150 ms at over 400 presentation frames per second.
constexpr std::size_t kHistorySize = 64;

struct FrameSlot {
    bool valid = false;
    Clock::time_point capturedAt{};
    // Aurora keys GPU-side GXCopyTex results by destination pointer. This byte
    // only supplies a unique, stable identity; framebuffer pixels are not read
    // back into CPU memory.
    alignas(32) unsigned char textureIdentity = 0;
};

std::array<FrameSlot, kHistorySize> sHistory{};
std::size_t sWriteIndex = 0;
int sLastDelayMs = 0;
u16 sLastWidth = 0;
u16 sLastHeight = 0;

void clear_history() {
    for (FrameSlot& slot : sHistory) {
        if (slot.valid) {
            GXDestroyCopyTex(&slot.textureIdentity);
            slot.valid = false;
        }
    }
    sWriteIndex = 0;
}

void draw_frame(const FrameSlot& slot, u16 width, u16 height) {
    GXTexObj texture{};
    GXInitTexObj(&texture, const_cast<unsigned char*>(&slot.textureIdentity), width, height,
                 GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&texture, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE,
                    GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&texture, GX_TEXMAP0);

    GXSetNumChans(0);
    GXSetNumIndStages(0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetZCompLoc(GX_TRUE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);

    Mtx44 projection;
    MTXOrtho(projection, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 10.0f);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S8, 0);
    mDoGph_drawFilterQuad(1, 1);
}

}  // namespace

void process() {
    const int delayMs =
        std::clamp(getSettings().game.inputLagMs.getValue(), 0, kMaximumDelayMs);
    if (delayMs != sLastDelayMs) {
        clear_history();
        sLastDelayMs = delayMs;
    }
    if (delayMs == 0) {
        return;
    }

    const u16 width = static_cast<u16>(mDoGph_gInf_c::getWidth());
    const u16 height = static_cast<u16>(mDoGph_gInf_c::getHeight());
    if (width != sLastWidth || height != sLastHeight) {
        clear_history();
        sLastWidth = width;
        sLastHeight = height;
    }
    const Clock::time_point now = Clock::now();

    FrameSlot& captured = sHistory[sWriteIndex];
    if (captured.valid) {
        GXDestroyCopyTex(&captured.textureIdentity);
    }
    GXSetTexCopySrc(0, 0, width, height);
    GXSetTexCopyDst(width, height, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(&captured.textureIdentity, GX_FALSE);
    captured.valid = true;
    captured.capturedAt = now;
    sWriteIndex = (sWriteIndex + 1) % sHistory.size();

    const Clock::time_point target = now - std::chrono::milliseconds(delayMs);
    const FrameSlot* selected = nullptr;
    const FrameSlot* oldest = nullptr;
    for (const FrameSlot& slot : sHistory) {
        if (!slot.valid) {
            continue;
        }
        if (oldest == nullptr || slot.capturedAt < oldest->capturedAt) {
            oldest = &slot;
        }
        if (slot.capturedAt <= target &&
            (selected == nullptr || slot.capturedAt > selected->capturedAt)) {
            selected = &slot;
        }
    }

    // During warm-up there is not yet a frame old enough. Showing the oldest
    // available frame ramps naturally into the requested delay without blanking.
    draw_frame(selected != nullptr ? *selected : *oldest, width, height);
}

}  // namespace dusk::video_latency

#else

namespace dusk::video_latency {
void process() {}
}  // namespace dusk::video_latency

#endif
