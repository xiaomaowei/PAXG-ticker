#include "mode_navigator.h"

namespace paxg {
namespace ui {

ModeNavigator::ModeNavigator(ScreenMode initialMode)
    : currentMode_(initialMode),
      modeChanged_(true),
      prevRawDown_(false),
      downChangeTime_(0),
      debouncedDown_(false),
      downPressTime_(0),
      downWasStopAction_(false),
      prevRawUp_(false),
      upChangeTime_(0),
      debouncedUp_(false) {}

void ModeNavigator::switchToMode(ScreenMode target, paxg::fishing::LiveFishingController& controller) {
    if (currentMode_ == target) {
        return;
    }

    if (currentMode_ == ScreenMode::RealFishing) {
        // Leaving fishing mode ALWAYS disarms and flushes pending keystrokes
        controller.requestStop(paxg::fishing::StopReason::UserStopped);
    } else {
        // Entering fishing mode ALWAYS starts strictly disarmed and idle
        controller.requestStop(paxg::fishing::StopReason::None);
    }

    currentMode_ = target;
    modeChanged_ = true;
    downWasStopAction_ = false;
}

void ModeNavigator::update(bool rawUp, bool rawDown, uint32_t nowMs, paxg::fishing::LiveFishingController& controller) {
    // 1. Debounce rawDown input (>= 50ms stability required)
    if (rawDown != prevRawDown_) {
        prevRawDown_ = rawDown;
        downChangeTime_ = nowMs;
    }

    bool downPressEdge = false;
    bool downReleaseEdge = false;
    if ((nowMs - downChangeTime_) >= DEBOUNCE_MS) {
        if (rawDown != debouncedDown_) {
            debouncedDown_ = rawDown;
            if (debouncedDown_) {
                downPressEdge = true;
                downPressTime_ = nowMs;
            } else {
                downReleaseEdge = true;
            }
        }
    }

    // 2. Debounce rawUp input (>= 50ms stability required)
    if (rawUp != prevRawUp_) {
        prevRawUp_ = rawUp;
        upChangeTime_ = nowMs;
    }
    if ((nowMs - upChangeTime_) >= DEBOUNCE_MS) {
        if (rawUp != debouncedUp_) {
            debouncedUp_ = rawUp;
        }
    }

    // 3. Mode-specific handling & controller dispatch
    if (currentMode_ == ScreenMode::PaxgMarket) {
        // While in PAXG screen, update controller with false buttons to service
        // audio, guards, and release retry while guaranteeing zero keystrokes.
        controller.update(false, false);

        // DOWN button release switches from PAXG Market screen to Real Fishing screen
        if (downReleaseEdge) {
            const uint32_t pressDuration = nowMs - downPressTime_;
            if (pressDuration < SHORT_PRESS_MAX_MS) {
                switchToMode(ScreenMode::RealFishing, controller);
            }
        }
    } else {
        // In Real Fishing screen:
        // Check if DOWN press is an emergency STOP action or a mode EXIT gesture.
        if (downPressEdge) {
            const bool wasActive = (controller.isArmed() ||
                                   controller.getState() != paxg::fishing::FishingState::Idle);
            if (wasActive) {
                // If fishing was running or armed, this press STOPS and DISARMS.
                downWasStopAction_ = true;
            } else {
                // If already stopped, idle, and disarmed, this press may be an exit gesture.
                downWasStopAction_ = false;
            }
        }

        // Pass debounced button states directly to controller
        controller.update(debouncedUp_, debouncedDown_);

        // On DOWN release:
        if (downReleaseEdge) {
            if (downWasStopAction_) {
                // Stop action consumed: release DOES NOT switch mode
                downWasStopAction_ = false;
            } else {
                // Controller was already idle and disarmed before this press:
                // short press exits to PAXG Market screen
                const uint32_t pressDuration = nowMs - downPressTime_;
                if (pressDuration < SHORT_PRESS_MAX_MS &&
                    !controller.isArmed() &&
                    controller.getState() == paxg::fishing::FishingState::Idle) {
                    switchToMode(ScreenMode::PaxgMarket, controller);
                }
            }
        }
    }
}

} // namespace ui
} // namespace paxg
