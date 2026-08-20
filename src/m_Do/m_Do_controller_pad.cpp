/**
 * m_Do_controller_pad.cpp
 * JUTGamePad Wrapper and Conversion
 */

#include "m_Do/m_Do_controller_pad.h"
#include "JSystem/JAWExtSystem/JAWExtSystem.h"
#include "SSystem/SComponent/c_lib.h"
#include "d/d_com_inf_game.h"
#include "dusk/input_macro.h"
#include "dusk/tas_movie.h"
#include "f_ap/f_ap_game.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_main.h"
#include "tracy/Tracy.hpp"

#if TARGET_PC
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include "dusk/menu_pointer.h"
#include "dusk/settings.h"
#include "dusk/ui/touch_controls.hpp"
#endif

DUSK_GAME_DATA JUTGamePad* mDoCPd_c::m_gamePad[4];

DUSK_GAME_DATA interface_of_controller_pad mDoCPd_c::m_cpadInfo[4];
DUSK_GAME_DATA interface_of_controller_pad mDoCPd_c::m_debugCpadInfo[4];
u32 mDoCPd_c::m_unfilteredButtonFlags[4] = {};
u32 mDoCPd_c::m_unfilteredPressedButtonFlags[4] = {};

#if TARGET_PC
s16 mDoCPd_c::getStickAngle3D(u32 pad) {
    if (dusk::getSettings().game.enableMirrorMode) {
        return -getCpadInfo(pad).mMainStickAngle;
    }
    return getCpadInfo(pad).mMainStickAngle;
}

f32 mDoCPd_c::getSubStickX3D(u32 pad) {
    if (dusk::getSettings().game.enableMirrorMode) {
        return -getCpadInfo(pad).mCStickPosX;
    }
    return getCpadInfo(pad).mCStickPosX;
}
#endif

#if TARGET_PC
static bool sCtrlRResetHeld = false;
static constexpr u32 kPracticeMenuInputMask = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT |
                                              PAD_BUTTON_RIGHT | PAD_BUTTON_A | PAD_BUTTON_B |
                                              PAD_TRIGGER_L | PAD_TRIGGER_R;
static bool checkCtrlRSoftReset() {
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    const bool hasKeys = keyCount > SDL_SCANCODE_R && keyCount > SDL_SCANCODE_RCTRL;
    const bool comboHeld = hasKeys && keys[SDL_SCANCODE_R] &&
                           (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]);

    bool resetRequested = false;
    if (sCtrlRResetHeld && !comboHeld && !mDoRst::isReset()) {
        mDoRst_resetCallBack(-1, NULL);
        resetRequested = true;
    }

    sCtrlRResetHeld = comboHeld;
    return resetRequested;
}

static void clearPracticeMenuInput(interface_of_controller_pad* interface) {
    interface->mButtonFlags &= ~kPracticeMenuInputMask;
    interface->mPressedButtonFlags &= ~kPracticeMenuInputMask;
    interface->mTriggerLeft = 0.0f;
    interface->mTriggerRight = 0.0f;
    interface->mHoldLockL = false;
    interface->mTrigLockL = false;
    interface->mHoldLockR = false;
    interface->mTrigLockR = false;
}

static void clearTeleportLinkDpadInput(interface_of_controller_pad* interface) {
    constexpr u32 kTeleportDpadMask = PAD_BUTTON_UP | PAD_BUTTON_DOWN;
    interface->mButtonFlags &= ~kTeleportDpadMask;
    interface->mPressedButtonFlags &= ~kTeleportDpadMask;
}

#endif

void mDoCPd_c::create() {
    #if PLATFORM_GCN || PLATFORM_SHIELD
    m_gamePad[0] = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
    #endif

    if (DEBUG || mDoMain::developmentMode != 0) {
        #if PLATFORM_WII
        m_gamePad[0] = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
        #endif

        m_gamePad[1] = JKR_NEW JUTGamePad(JUTGamePad::EPort2);
        m_gamePad[2] = JKR_NEW JUTGamePad(JUTGamePad::EPort3);
        m_gamePad[3] = JKR_NEW JUTGamePad(JUTGamePad::EPort4);
    } else {
        #if PLATFORM_WII
        m_gamePad[0] = NULL;
        #endif

        m_gamePad[1] = NULL;
        m_gamePad[2] = NULL;
        m_gamePad[3] = NULL;
    }

    #if PLATFORM_GCN || PLATFORM_SHIELD
    if (!mDoRst::isReset()) {
        JUTGamePad::clearResetOccurred();
        JUTGamePad::setResetCallback(mDoRst_resetCallBack, NULL);
    }
    #endif
    JUTGamePad::setAnalogMode(3);

    interface_of_controller_pad* cpad = &m_cpadInfo[0];
    for (int i = 0; i < 4; i++) {
        cpad->mHoldLockL = cpad->mTrigLockL = false;
        cpad->mHoldLockR = cpad->mTrigLockR = false;
        cpad++;
    }
}

void mDoCPd_c::read() {
    ZoneScoped;
#if TARGET_PC
    dusk::ui::sync_virtual_input();
#endif
    JUTGamePad::read();

    if (!mDoRst::isReset() && mDoRst::is3ButtonReset()) {
        if (!JUTGamePad::getGamePad(mDoRst::get3ButtonResetPort())->isPushing3ButtonReset()) {
            mDoRst::off3ButtonReset();
        }
    }

#if TARGET_PC
    const bool ctrlRResetRequested = checkCtrlRSoftReset();
#endif

#if DEBUG
    if (m_gamePad[3]) {
        JAWExtSystem::padProc(*m_gamePad[3]);
    }
#endif

    JUTGamePad** pad = m_gamePad;
    interface_of_controller_pad* interface = m_cpadInfo;
#if DEBUG
    interface_of_controller_pad* interface2 = m_debugCpadInfo;
    if (dComIfG_isDebugMode()) {
        interface_of_controller_pad* tmp = interface;
        interface = interface2;
        interface2 = tmp;
    }
#endif

    for (u32 i = 0; i < 4; i++) {
        if (*pad == NULL) {
            cLib_memSet(interface, 0, sizeof(interface_of_controller_pad));
            m_unfilteredButtonFlags[i] = 0;
            m_unfilteredPressedButtonFlags[i] = 0;
        } else {
            convert(interface, *pad);
            m_unfilteredButtonFlags[i] = interface->mButtonFlags;
            m_unfilteredPressedButtonFlags[i] = interface->mPressedButtonFlags;
#if TARGET_PC
            const u32 suppressedButtons = dusk::menu_pointer::suppressed_pad_buttons(i);
            interface->mButtonFlags &= ~suppressedButtons;
            interface->mPressedButtonFlags &= ~suppressedButtons;
            dusk::menu_pointer::finish_pad_suppression_read(i);
#endif
            LRlockCheck(interface);
#if TARGET_PC
            if (i == PAD_1 && dusk::getTransientSettings().practiceMenuInputCapture) {
                clearPracticeMenuInput(interface);
            }
#endif
        }
#if DEBUG
        cLib_memSet(interface2, 0, sizeof(interface_of_controller_pad));
#endif
        pad++;
        interface++;
#if DEBUG
        interface2++;
#endif
    }

#if TARGET_PC
    const bool tasOwnsInput =
        dusk::tas_movie::state() == dusk::tas_movie::State::Recording ||
        dusk::tas_movie::state() == dusk::tas_movie::State::Playing;
    const bool replayResetRequested =
        tasOwnsInput ? dusk::tas_movie::tick(m_cpadInfo, ctrlRResetRequested)
                     : dusk::input_macro::tick(m_cpadInfo, ctrlRResetRequested);
    if (replayResetRequested && !mDoRst::isReset()) {
        mDoRst_resetCallBack(-1, NULL);
    }

    if (dusk::getSettings().game.teleportLink.getValue() &&
        !dusk::getSettings().game.speedrunMode.getValue() &&
        (m_unfilteredButtonFlags[PAD_1] & PAD_TRIGGER_R) != 0 &&
        (m_unfilteredButtonFlags[PAD_1] & PAD_TRIGGER_L) == 0)
    {
        clearTeleportLinkDpadInput(&m_cpadInfo[PAD_1]);
    }

    const u32 physicalHold = m_unfilteredButtonFlags[PAD_1];
    const u32 replayCombo = PAD_BUTTON_B | PAD_BUTTON_LEFT | PAD_TRIGGER_Z;
    if ((physicalHold & replayCombo) == replayCombo) {
        m_cpadInfo[PAD_1].mButtonFlags &= ~replayCombo;
        m_cpadInfo[PAD_1].mPressedButtonFlags &= ~replayCombo;
    }
    if ((physicalHold & PAD_BUTTON_A) != 0) {
        m_cpadInfo[PAD_1].mButtonFlags &= ~PAD_BUTTON_LEFT;
        m_cpadInfo[PAD_1].mPressedButtonFlags &= ~PAD_BUTTON_LEFT;
        if ((physicalHold & PAD_BUTTON_LEFT) != 0) {
            m_cpadInfo[PAD_1].mButtonFlags &= ~PAD_TRIGGER_Z;
            m_cpadInfo[PAD_1].mPressedButtonFlags &= ~PAD_TRIGGER_Z;
        }
    }
#endif
}

void mDoCPd_c::convert(interface_of_controller_pad* pInterface, JUTGamePad* pPad) {
    pInterface->mButtonFlags = pPad->getButton();
    pInterface->mPressedButtonFlags = pPad->getTrigger();
    pInterface->mMainStickPosX = pPad->getMainStickX();
    pInterface->mMainStickPosY = pPad->getMainStickY();
    pInterface->mMainStickValue = pPad->getMainStickValue();
    pInterface->mMainStickAngle = pPad->getMainStickAngle();
    pInterface->mCStickPosX = pPad->getSubStickX();
    pInterface->mCStickPosY = pPad->getSubStickY();
    pInterface->mCStickValue = pPad->getSubStickValue();
    pInterface->mCStickAngle = pPad->getSubStickAngle();

    mDoCPd_ANALOG_CONV(pPad->getAnalogA(), pInterface->mAnalogA);
    mDoCPd_ANALOG_CONV(pPad->getAnalogB(), pInterface->mAnalogB);
    mDoCPd_TRIGGER_CONV(pPad->getAnalogL(), pInterface->mTriggerLeft);
    mDoCPd_TRIGGER_CONV(pPad->getAnalogR(), pInterface->mTriggerRight);

    pInterface->mGamepadErrorFlags = pPad->getErrorStatus();
}

void mDoCPd_c::LRlockCheck(interface_of_controller_pad* interface) {
    f32 trigger = interface->mTriggerLeft;
    interface->mTrigLockL = false;
    interface->mTrigLockR = false;

    if (trigger > fapGmHIO_getLROnValue()) {
        if (interface->mHoldLockL != true) {
            interface->mTrigLockL = true;
        }
        interface->mHoldLockL = true;
    } else if (trigger < fapGmHIO_getLROffValue()) {
        interface->mHoldLockL = false;
    }

    trigger = interface->mTriggerRight;
    if (trigger > fapGmHIO_getLROnValue()) {
        if (interface->mHoldLockR != true) {
            interface->mTrigLockR = true;
        }
        interface->mHoldLockR = true;
    } else if (trigger < fapGmHIO_getLROffValue()) {
        interface->mHoldLockR = false;
    }
}

void mDoCPd_c::recalibrate(void) {
    JUTGamePad::clearForReset();
    JUTGamePad::CRumble::setEnabled(PAD_CHAN3_BIT | PAD_CHAN2_BIT | PAD_CHAN1_BIT | PAD_CHAN0_BIT);
}
