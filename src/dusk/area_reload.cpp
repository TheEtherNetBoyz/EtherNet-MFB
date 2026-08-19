#include "area_reload.hpp"

#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "dusk/dusk.h"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "m_Do/m_Do_controller_pad.h"

#include <array>
#include <cstring>

namespace dusk {
namespace {

constexpr u32 kAreaReloadCombo = PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_START | PAD_BUTTON_A;

struct AreaReloadState {
    std::array<u8, sizeof(dSv_memory_c)> memory = {};
    u8 lightDrops[4] = {};
    bool comboHeld = false;
};

AreaReloadState s_state;

}  // namespace

void reload_area() {
    if (!IsGameLaunched || getSettings().game.speedrunMode.getValue() ||
        dComIfGp_isEnableNextStage()) {
        return;
    }

    const char* stage = dComIfGp_getNextStageName();
    s16 point = dComIfGp_getNextStagePoint();
    s8 room = dComIfGp_getNextStageRoomNo();
    s8 layer = dComIfGp_getNextStageLayer();
    if (stage == nullptr || stage[0] == '\0') {
        stage = dComIfGp_getStartStageName();
        point = dComIfGp_getStartStagePoint();
        room = dComIfGp_getStartStageRoomNo();
        layer = dComIfGp_getStartStageLayer();
    }
    if (stage == nullptr || stage[0] == '\0') {
        return;
    }
    char stageName[8] = {};
    std::strncpy(stageName, stage, sizeof(stageName) - 1);

    // Match tpgz's area reload behavior: retain the temporary area flags and
    // Tears of Light instead of letting the stage transition reset them.
    std::memcpy(s_state.memory.data(), &g_dComIfG_gameInfo.info.getMemory(),
                s_state.memory.size());
    for (u8 i = 0; i < 4; ++i) {
        s_state.lightDrops[i] = dComIfGs_getLightDropNum(i);
    }

    // A void-out reload starts from the saved turn-restart camera. Capture the
    // complete current camera transform so the fixed camera is restored rather
    // than only partially inheriting its previous state.
    if (dCamera_c* camera = dCam_getBody()) {
        auto& turnRestart = g_dComIfG_gameInfo.info.getTurnRestart();
        turnRestart.setCameraCtr(camera->Center());
        turnRestart.setCameraEye(camera->Eye());
        turnRestart.setCameraUp(camera->Up());
        turnRestart.setCameraFvy(camera->Fovy());
    }

    // Set the request directly so this reload does not apply the side effects
    // of a new gameplay warp.
    g_dComIfG_gameInfo.play.setNextStage(
        stageName, room, point, layer, dComIfGp_getNextStageWipe(),
        dComIfGp_getNextStageWipeSpeed());

    std::memcpy(&g_dComIfG_gameInfo.info.getMemory(), s_state.memory.data(),
                s_state.memory.size());
    for (u8 i = 0; i < 4; ++i) {
        dComIfGs_setLightDropNum(i, s_state.lightDrops[i]);
    }
}

void update_area_reload_input() {
    const u32 hold = mDoCPd_c::getUnfilteredHold(PAD_1);
    const u32 trig = mDoCPd_c::getUnfilteredTrig(PAD_1);
    const bool comboHeld = (hold & kAreaReloadCombo) == kAreaReloadCombo;
    const bool activate = comboHeld && (trig & PAD_BUTTON_A) != 0;

    if (getSettings().game.areaReload.getValue() && activate && !s_state.comboHeld) {
        reload_area();
    }

    s_state.comboHeld = comboHeld;
}

}  // namespace dusk
