/**
 * d_a_tag_hinit.cpp
 *
 */

#include "d/dolzel_rel.h" // IWYU pragma: keep

#include "d/actor/d_a_tag_hinit.h"
#include "f_op/f_op_actor_mng.h"
#if TARGET_PC
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/multiplayer.hpp"
#endif

int daTagHinit_c::create() {
    fopAcM_ct(this, daTagHinit_c);

    field_0x569 = shape_angle.x;

    if (field_0x569 != 0xFF && fopAcM_isSwitch(this, field_0x569)) {
#if TARGET_PC
        stage_stag_info_class* stagInfo = dComIfGp_getStageStagInfo();
        const int saveStage = stagInfo != nullptr ? dStage_stagInfo_GetSaveTbl(stagInfo) : -1;
        uint32_t remoteAgeTicks = 0;
        const bool recentRemoteSwitch = saveStage >= 0 &&
            dusk::multiplayer::was_switch_recently_remote_set(saveStage, field_0x569,
                                                              &remoteAgeTicks);
        DuskLog.warn("daTagHinit_c::create REJECT_SWITCH_ON tagPid={} stage={} startRoom={} "
                     "stayRoom={} tagRoom={} saveStage={} outSwitch={} rawShapeX={} "
                     "rawParam=0x{:08x} recentRemoteSwitch={} remoteAgeTicks={}",
                     fopAcM_GetID(this), dComIfGp_getStartStageName(),
                     dComIfGp_getStartStageRoomNo(), dComIfGp_roomControl_getStayNo(),
                     fopAcM_GetRoomNo(this), saveStage, field_0x569, shape_angle.x,
                     fopAcM_GetParam(this), recentRemoteSwitch, remoteAgeTicks);
        if (recentRemoteSwitch) {
            DuskLog.warn("Multiplayer ACTOR_REJECTED_BY_REMOTE_SWITCH actor=Tag_Hinit tagPid={} "
                         "stage={} saveStage={} room={} switch={} remoteAgeTicks={}",
                         fopAcM_GetID(this), dComIfGp_getStartStageName(), saveStage,
                         fopAcM_GetRoomNo(this), field_0x569, remoteAgeTicks);
        }
#endif
        return cPhs_ERROR_e;
    }

    field_0x568 = (shape_angle.x >> 8) & 0xFF;
    field_0x56c = fopAcM_GetParam(this);
    field_0x56e = (fopAcM_GetParam(this) >> 0x10);
#if TARGET_PC
    DuskLog.warn("daTagHinit_c::create COMPLETE tagPid={} stage={} startRoom={} stayRoom={} "
                 "tagRoom={} outSwitch={} reqSwitch={} reqEvent={} blockEvent={} rawShapeX={} "
                 "rawParam=0x{:08x} outSwitchOn={} reqSwitchOn={} reqEventOn={} blockEventOn={}",
                 fopAcM_GetID(this), dComIfGp_getStartStageName(), dComIfGp_getStartStageRoomNo(),
                 dComIfGp_roomControl_getStayNo(), fopAcM_GetRoomNo(this), field_0x569,
                 field_0x568, field_0x56c, field_0x56e, shape_angle.x, fopAcM_GetParam(this),
                 field_0x569 != 0xFF ? fopAcM_isSwitch(this, field_0x569) : 0,
                 field_0x568 != 0xFF ? fopAcM_isSwitch(this, field_0x568) : 0,
                 field_0x56c != 0xFFFF ?
                     dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[field_0x56c]) :
                     1,
                 field_0x56e != 0xFFFF ?
                     dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[field_0x56e]) :
                     0);
#endif

    return cPhs_COMPLEATE_e;
}

static int daTagHinit_Create(fopAc_ac_c* i_this) {
    daTagHinit_c* hInit = static_cast<daTagHinit_c*>(i_this);
    int id = fopAcM_GetID(i_this);
    return hInit->create();
}

daTagHinit_c::~daTagHinit_c() {}

static int daTagHinit_Delete(daTagHinit_c* i_this) {
    int id = fopAcM_GetID(i_this);
    i_this->~daTagHinit_c();
    return 1;
}

int daTagHinit_c::execute() {
    if ((field_0x56c == 0xFFFF ||
         dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[field_0x56c])) &&
        (field_0x56e == 0xFFFF ||
         !dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[field_0x56e])) &&
        (field_0x568 == 0xFF || fopAcM_isSwitch(this, field_0x568)))
    {
        if (field_0x569 != 0xFF) {
            fopAcM_onSwitch(this, field_0x569);
        }

        daHorse_c* horse = dComIfGp_getHorseActor();
#if TARGET_PC
        DuskLog.warn("daTagHinit_c::execute POSITION_HORSE tagPid={} horsePid={} stage={} "
                     "startRoom={} stayRoom={} tagRoom={} horseRoom={} tag_pos=({}, {}, {}) "
                     "tag_angle={} horse_no_draw_wait_before={} horse_pos_before=({}, {}, {}) "
                     "horse_angle_before={}",
                     fopAcM_GetID(this), horse != nullptr ? fopAcM_GetID(horse) : -1,
                     dComIfGp_getStartStageName(), dComIfGp_getStartStageRoomNo(),
                     dComIfGp_roomControl_getStayNo(), fopAcM_GetRoomNo(this),
                     horse != nullptr ? fopAcM_GetRoomNo(horse) : -128, current.pos.x,
                     current.pos.y, current.pos.z, shape_angle.y,
                     horse != nullptr ? horse->checkHorseCallWait() : 0,
                     horse != nullptr ? horse->current.pos.x : 0.0f,
                     horse != nullptr ? horse->current.pos.y : 0.0f,
                     horse != nullptr ? horse->current.pos.z : 0.0f,
                     horse != nullptr ? horse->shape_angle.y : 0);
#endif
        horse->setHorsePosAndAngle(&current.pos, shape_angle.y);
        horse->offNoDrawWait();
        fopAcM_delete(this);
    }

    return 1;
}

static int daTagHinit_Execute(daTagHinit_c* i_this) {
    return i_this->execute();
}

static int daTagHinit_Draw(daTagHinit_c*) {
    return 1;
}

static DUSK_CONST actor_method_class l_daTagHinit_Method = {
    (process_method_func)daTagHinit_Create,  (process_method_func)daTagHinit_Delete,
    (process_method_func)daTagHinit_Execute, (process_method_func)NULL,
    (process_method_func)daTagHinit_Draw,

};

DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_Tag_Hinit = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 3,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_Tag_Hinit_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daTagHinit_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_Tag_Hinit_e,
    /* Actor SubMtd */ &l_daTagHinit_Method,
    /* Status       */ fopAcStts_UNK_0x40000_e | fopAcStts_NOPAUSE_e,
    /* Group        */ fopAc_ENV_e,
    /* Cull Type    */ fopAc_CULLBOX_CUSTOM_e,
};
