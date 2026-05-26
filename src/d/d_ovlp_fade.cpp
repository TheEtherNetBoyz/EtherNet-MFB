/**
 * d_ovlp_fade.cpp
 *
 */

#include "d/dolzel.h" // IWYU pragma: keep

#include "d/d_ovlp_fade.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_op/f_op_scene_req.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_Reset.h"
#if TARGET_PC
#include "dusk/settings.h"
#endif
#include <cstring>

class overlap1_class : public overlap_task_class {
public:
    /* 0xCC */ int field_0xcc;
    /* 0xCC */ int field_0xd0;
    /* 0xCC */ int field_0xd4;
};

static const int kFastFadeFrames = 20;
static const int kFastFadeInFrames = 25;
static const int kInstaFadeFrames = 0;
static const int kInstaFadeInFrames = 0;
static const int kInstaBlackFadeInFrames = 0;
static const int kFastStartLoadBeforeFadeDoneFrames = 1;

static bool dOvlpFd_isDmn07VanillaFastLoad() {
    return DUSK_IF_ELSE(dusk::getSettings().game.enableFastLoads.getValue() &&
                            !dusk::getSettings().game.enableInstaLoads.getValue() &&
                            !mDoRst::isReset() &&
                            strcmp(dComIfGp_getStartStageName(), "D_MN07") == 0 &&
                            strcmp(dComIfGp_getNextStageName(), "D_MN07") == 0,
                        false);
}

static bool dOvlpFd_isFastLoad() {
    return DUSK_IF_ELSE((dusk::getSettings().game.enableFastLoads.getValue() ||
                         dusk::getSettings().game.enableInstaLoads.getValue()) &&
                            !mDoRst::isReset() && !dOvlpFd_isDmn07VanillaFastLoad() &&
                            !fopScnRq_IsTitleToFileSelectVanillaFastLoad(),
                        false);
}

static bool dOvlpFd_isInstaLoad() {
    return DUSK_IF_ELSE(dusk::getSettings().game.enableInstaLoads.getValue() &&
                            !mDoRst::isReset() &&
                            !fopScnRq_IsTitleToFileSelectVanillaFastLoad(),
                        false);
}

static bool dOvlpFd_isRealInstaLoad() {
    return dOvlpFd_isInstaLoad();
}

static int dOvlpFd_getFadeFrames() {
    return DUSK_IF_ELSE(dOvlpFd_isInstaLoad() ? kInstaFadeFrames : kFastFadeFrames,
                        kFastFadeFrames);
}

static int dOvlpFd_getFadeInFrames() {
    u8 wipe = dComIfGp_getNextStageWipe();
    int hour = dKy_getdaytime_hour();
    BOOL isDaytime = (hour >= 6 && hour < 18) ? FALSE : TRUE;
    bool whiteWipe = wipe == 1 || wipe == 2 || wipe == 7 || wipe == 17 || wipe == 21 ||
                     ((wipe == 8 || wipe == 10 || wipe == 18) && isDaytime) ||
                     ((wipe == 9 || wipe == 11 || wipe == 19) && !isDaytime);

    return DUSK_IF_ELSE(dOvlpFd_isInstaLoad() ?
                            (whiteWipe ? kInstaFadeInFrames : kInstaBlackFadeInFrames) :
                            kFastFadeInFrames,
                        kFastFadeInFrames);
}

static int dOvlpFd_getDoneFrames() {
    return DUSK_IF_ELSE(dOvlpFd_isRealInstaLoad() ? 0 :
                            (dOvlpFd_isInstaLoad() ? kInstaFadeFrames - 1 :
                                                     kFastStartLoadBeforeFadeDoneFrames),
                        kFastStartLoadBeforeFadeDoneFrames);
}

static int dOvlpFd_Draw(overlap1_class* i_this) {
    return 1;
}

static void dOvlpFd_startFadeIn(int param_0) {
    JUTFader* fader = JFWDisplay::getManager()->getFader();
    JUT_ASSERT(0, fader != NULL);

    fader->setStatus(JUTFader::None, 0);
    fader->setStatus(JUTFader::None, -1);
    fader->startFadeIn(param_0);
}

static void dOvlpFd_clearFader() {
    dOvlpFd_startFadeIn(0);
    mDoGph_gInf_c::offFade();
}

static int dOvlpFd_FadeOut(overlap1_class* i_this) {
    if (dOvlpFd_isRealInstaLoad()) {
        dOvlpFd_clearFader();
        mDoAud_setFadeInStart(0);
        fopOvlpM_SceneIsStart();
        fopOvlpM_Done(i_this);
        return 1;
    }

    bool fastLoad = dOvlpFd_isFastLoad();
    int fastFadeFrames = dOvlpFd_getFadeInFrames();
    int var_r31 = fastLoad ? fastFadeFrames : i_this->field_0xd4;

    if (i_this->field_0xcc == 0) {
        mDoAud_setFadeInStart(0);

        if (!fastLoad && fpcM_GetProfName(i_this) == fpcNm_OVERLAP11_e) {
            i_this->field_0xcc = 10;
        } else if (fpcM_GetProfName(i_this) == fpcNm_OVERLAP11_e) {
            dOvlpFd_startFadeIn(var_r31);
            i_this->field_0xcc = var_r31;
        } else if (fpcM_GetProfName(i_this) != fpcNm_OVERLAP6_e) {
            dOvlpFd_startFadeIn(var_r31);
            i_this->field_0xcc = var_r31;
        } else if (!fastLoad) {
            dOvlpFd_startFadeIn(0);
            i_this->field_0xcc = 1;
        } else {
            dOvlpFd_startFadeIn(fastFadeFrames);
            i_this->field_0xcc = fastFadeFrames;
        }
    }

    if (!fastLoad && fpcM_GetProfName(i_this) == fpcNm_OVERLAP11_e && i_this->field_0xcc == 7) {
        dOvlpFd_startFadeIn(7);
    }

    fopOvlpM_SceneIsStart();

    if (--i_this->field_0xcc == 0) {
        fopOvlpM_Done(i_this);
        i_this->field_0xcc++;
    }

    return 1;
}

static int (*dOvlpFd_execute_f)(overlap1_class*);

static int dOvlpFd_Wait(overlap1_class* i_this) {
    if (fopOvlpM_IsOutReq(i_this)) {
        dOvlpFd_execute_f = dOvlpFd_FadeOut;
    }

    return 1;
}

static int dOvlpFd_FadeIn(overlap1_class* i_this) {
    if (dOvlpFd_isRealInstaLoad()) {
        dOvlpFd_clearFader();
        mDoAud_setFadeOutStart(0);
        dOvlpFd_execute_f = dOvlpFd_Wait;
        fopOvlpM_Done(i_this);
        return 1;
    }

    bool fastLoad = dOvlpFd_isFastLoad();
    int fastFadeFrames = dOvlpFd_getFadeFrames();
    int var_r30 = fastLoad ? fastFadeFrames : 30;

    if (i_this->field_0xd0 == 0) {
        if (fpcM_GetProfName(i_this) == fpcNm_OVERLAP0_e || fpcM_GetProfName(i_this) == fpcNm_OVERLAP7_e)
        {
            if (!fastLoad && dComIfGp_isEnableNextStage() == 1) {
                var_r30 = dComIfGp_getNextStageWipeSpeed();
            }

            if (!JFWDisplay::getManager()->startFadeOut(var_r30)) {
                return 1;
            }

            i_this->field_0xd0 = var_r30;
        } else if (fpcM_GetProfName(i_this) == fpcNm_OVERLAP10_e ||
                   fpcM_GetProfName(i_this) == fpcNm_OVERLAP11_e)
        {
            i_this->field_0xd0 = fastLoad ? fastFadeFrames : 1;
        } else {
            i_this->field_0xd0 = fastLoad ? fastFadeFrames : 30;
        }

        i_this->field_0xd4 = var_r30;
        mDoAud_setFadeOutStart(0);
    }

    if (--i_this->field_0xd0 == dOvlpFd_getDoneFrames() && fastLoad) {
        fopOvlpM_Done(i_this);
    }

    if (i_this->field_0xd0 == 0) {
        dOvlpFd_execute_f = dOvlpFd_Wait;
        mDoGph_gInf_c::startFadeOut(0);
        if (!fastLoad) {
            fopOvlpM_Done(i_this);
        }
    }

    return 1;
}

static int dOvlpFd_Execute(overlap1_class* i_this) {
    dOvlpFd_execute_f(i_this);
    return 1;
}

static int dOvlpFd_IsDelete(overlap1_class* i_this) {
    return 1;
}

static int dOvlpFd_Delete(overlap1_class* i_this) {
    return 1;
}

static int dOvlpFd_Create(void* i_this) {
    dOvlpFd_execute_f = dOvlpFd_FadeIn;
    return cPhs_COMPLEATE_e;
}

static leafdraw_method_class l_dOvlpFd_Method = {
    (process_method_func)dOvlpFd_Create,  (process_method_func)dOvlpFd_Delete,
    (process_method_func)dOvlpFd_Execute, (process_method_func)dOvlpFd_IsDelete,
    (process_method_func)dOvlpFd_Draw,
};

overlap_process_profile_definition g_profile_OVERLAP0 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP0_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP0_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};

overlap_process_profile_definition g_profile_OVERLAP1 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP1_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP1_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};

overlap_process_profile_definition g_profile_OVERLAP6 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP6_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP6_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};

overlap_process_profile_definition g_profile_OVERLAP7 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP7_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP7_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};

overlap_process_profile_definition g_profile_OVERLAP8 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP8_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP8_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};

overlap_process_profile_definition g_profile_OVERLAP9 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP9_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP9_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};

overlap_process_profile_definition g_profile_OVERLAP10 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP10_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP10_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};

overlap_process_profile_definition g_profile_OVERLAP11 = {
    /* Layer ID    */ fpcLy_ROOT_e,
    /* List ID     */ 0,
    /* List Prio   */ fpcPi_CURRENT_e,
    /* Proc Name   */ fpcNm_OVERLAP11_e,
    /* Proc SubMtd */ &g_fpcLf_Method.base,
    /* Size        */ sizeof(overlap1_class),
    /* Size Other  */ 0,
    /* Parameters  */ 0,
    /* Leaf SubMtd */ &g_fopOvlp_Method,
    /* Draw Prio   */ fpcDwPi_OVERLAP11_e,
    /* Fade SubMtd */ &l_dOvlpFd_Method,
};
