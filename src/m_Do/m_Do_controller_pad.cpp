/**
 * m_Do_controller_pad.cpp
 * JUTGamePad Wrapper and Conversion
 */

#include "m_Do/m_Do_controller_pad.h"
#include "JSystem/JAWExtSystem/JAWExtSystem.h"
#include "SSystem/SComponent/c_lib.h"
#include "d/d_com_inf_game.h"
#include "dusk/input_macro.h"
#include "f_ap/f_ap_game.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_main.h"
#include "tracy/Tracy.hpp"

#if TARGET_PC
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <chrono>

#include "dusk/menu_pointer.h"
#include "dusk/ui/touch_controls.hpp"
#endif

JUTGamePad* mDoCPd_c::m_gamePad[4];

interface_of_controller_pad mDoCPd_c::m_cpadInfo[4];
interface_of_controller_pad mDoCPd_c::m_debugCpadInfo[4];

#if TARGET_PC
static bool sCtrlRResetHeld = false;
static constexpr u32 kPracticeMenuInputMask = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT |
                                              PAD_BUTTON_RIGHT | PAD_BUTTON_A | PAD_BUTTON_B |
                                              PAD_TRIGGER_L | PAD_TRIGGER_R;
static constexpr int kMaxInputLagMs = 150;
static constexpr size_t kInputDelayHistorySize = 64;

using InputDelayClock = std::chrono::steady_clock;

struct InputDelaySample {
    bool valid = false;
    InputDelayClock::time_point time{};
    std::array<interface_of_controller_pad, 4> pads{};
};

static std::array<InputDelaySample, kInputDelayHistorySize> sInputDelayHistory;
static size_t sInputDelayHistoryWriteIndex = 0;
static std::array<interface_of_controller_pad, 4> sInputDelayLastDeliveredPads{};
static bool sInputDelayHasLastDeliveredPads = false;

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

static void resetInputDelayHistory() {
    for (InputDelaySample& sample : sInputDelayHistory) {
        sample.valid = false;
    }
    sInputDelayHistoryWriteIndex = 0;
    sInputDelayHasLastDeliveredPads = false;
}

static void finalizeDelayedInput(interface_of_controller_pad* pads) {
    for (size_t i = 0; i < sInputDelayLastDeliveredPads.size(); ++i) {
        const interface_of_controller_pad previous =
            sInputDelayHasLastDeliveredPads ? sInputDelayLastDeliveredPads[i] : interface_of_controller_pad{};

        pads[i].mPressedButtonFlags = pads[i].mButtonFlags & ~previous.mButtonFlags;
        pads[i].mTrigLockL = pads[i].mHoldLockL && !previous.mHoldLockL;
        pads[i].mTrigLockR = pads[i].mHoldLockR && !previous.mHoldLockR;

        sInputDelayLastDeliveredPads[i] = pads[i];
    }
    sInputDelayHasLastDeliveredPads = true;
}

static void applyInputDelay(interface_of_controller_pad* pads) {
    const int delayMs = std::clamp(dusk::getSettings().game.inputLagMs.getValue(), 0, kMaxInputLagMs);
    if (delayMs <= 0) {
        resetInputDelayHistory();
        return;
    }

    const InputDelayClock::time_point now = InputDelayClock::now();
    InputDelaySample& writeSample = sInputDelayHistory[sInputDelayHistoryWriteIndex];
    writeSample.valid = true;
    writeSample.time = now;
    for (size_t i = 0; i < writeSample.pads.size(); ++i) {
        writeSample.pads[i] = pads[i];
    }
    sInputDelayHistoryWriteIndex = (sInputDelayHistoryWriteIndex + 1) % sInputDelayHistory.size();

    const InputDelayClock::time_point target =
        now - std::chrono::milliseconds(delayMs);
    const InputDelaySample* bestSample = nullptr;
    const InputDelaySample* oldestSample = nullptr;

    for (const InputDelaySample& sample : sInputDelayHistory) {
        if (!sample.valid) {
            continue;
        }

        if (oldestSample == nullptr || sample.time < oldestSample->time) {
            oldestSample = &sample;
        }

        if (sample.time <= target &&
            (bestSample == nullptr || sample.time > bestSample->time))
        {
            bestSample = &sample;
        }
    }

    if (bestSample == nullptr) {
        bestSample = oldestSample;
    }

    if (bestSample == nullptr) {
        return;
    }

    for (size_t i = 0; i < bestSample->pads.size(); ++i) {
        pads[i] = bestSample->pads[i];
    }
    finalizeDelayedInput(pads);
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
        } else {
            convert(interface, *pad);
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
    applyInputDelay(m_cpadInfo);
    if (dusk::input_macro::tick(m_cpadInfo, ctrlRResetRequested) && !mDoRst::isReset()) {
        mDoRst_resetCallBack(-1, NULL);
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
