#include "live_fishing_controller.h"

namespace paxg {
namespace fishing {

LiveFishingController::LiveFishingController(IHidKeyboard* hid, IClock* clock, IAudioStream* audioStream)
    : hid_(hid),
      clock_(clock),
      audioStream_(audioStream),
      fsm_(FishingState::Idle) {
}

void LiveFishingController::init() {
    armed_ = false;
    startupProtectionActive_ = true;
    requireButtonsReleaseAfterArm_ = false;
    fsm_.reset(FishingState::Idle);
    keyState_ = KeyState::Idle;
    activeKeycode_ = 0;
    dualButtonHoldStart_ = 0;
    armToggleHandled_ = false;
    btnUpPressStart_ = 0;
    startHandled_ = false;
    btnDownPressStart_ = 0;
    stopHandled_ = false;
    stateStartTime_ = clock_ ? clock_->nowMs() : 0;
    castAcceptedTime_ = 0;
    lastStopReason_ = StopReason::None;
    // lastPacketArrivalMs_ initialized to 0: requires actual fresh packet arrival post-init
    lastPacketArrivalMs_ = 0;
    lastPacketsReceived_ = audioStream_ ? audioStream_->getPacketsReceived() : 0;
    armPacketsBaseline_ = 0;
    initialDiscontinuities_ = audioStream_ ? audioStream_->getDiscontinuities() : 0;
    initialQueueDrops_ = audioStream_ ? audioStream_->getQueueDrops() : 0;
    if (audioStream_) {
        audioStream_->clearTrigger();
    }
}

uint32_t LiveFishingController::getStateDurationMs() const {
    if (!clock_) return 0;
    return clock_->nowMs() - stateStartTime_;
}

uint32_t LiveFishingController::getWaitRemainingMs() const {
    if (fsm_.getState() != FishingState::Waiting) {
        return 0;
    }
    if (!clock_) return 0;
    const uint32_t elapsed = clock_->nowMs() - castAcceptedTime_;
    if (elapsed >= GAME_CAST_DURATION_MS) {
        return 0;
    }
    return (GAME_CAST_DURATION_MS - elapsed);
}

void LiveFishingController::pollAudioFreshness(uint32_t now) {
    if (audioStream_) {
        const uint64_t pkts = audioStream_->getPacketsReceived();
        if (pkts > lastPacketsReceived_) {
            lastPacketsReceived_ = pkts;
            lastPacketArrivalMs_ = now;
        }
    }
}

void LiveFishingController::update(bool btnUpPressed, bool btnDownPressed) {
    if (!clock_) return;
    const uint32_t now = clock_->nowMs();

    // 0. Continually poll audio packet activity
    pollAudioFreshness(now);

    // 1. Process physical button gestures
    handleButtons(btnUpPressed, btnDownPressed, now);

    // 2. Continuous fail-closed safety monitoring (including disconnect while Idle)
    checkSafetyGuards(now);

    // 3. Process active non-blocking keypress release and retries
    updateKeypress(now);

    // 4. Process deterministic FSM transitions
    processFsm(now);
}

void LiveFishingController::handleButtons(bool btnUp, bool btnDown, uint32_t now) {
    // Startup protection: any button held at power-on/init is ignored until BOTH are released
    if (startupProtectionActive_) {
        if (!btnUp && !btnDown) {
            startupProtectionActive_ = false;
        } else {
            return;
        }
    }

    // Post-arm requirement: user must release BOTH buttons after arming before START can trigger
    if (requireButtonsReleaseAfterArm_) {
        if (!btnUp && !btnDown) {
            requireButtonsReleaseAfterArm_ = false;
        } else {
            return;
        }
    }

    // Dual-button deliberate hold: Toggle Armed / Disarmed
    if (btnUp && btnDown) {
        if (dualButtonHoldStart_ == 0) {
            dualButtonHoldStart_ = now;
        } else if ((now - dualButtonHoldStart_ >= ARM_HOLD_DURATION_MS) && !armToggleHandled_) {
            requestArmToggle();
            armToggleHandled_ = true;
        }
        // Suppress individual button actions when both are engaged
        btnUpPressStart_ = 0;
        startHandled_ = false;
        btnDownPressStart_ = 0;
        stopHandled_ = false;
        return;
    } else {
        dualButtonHoldStart_ = 0;
        armToggleHandled_ = false;
    }

    // Button DOWN: Stop / Disarm (Highest priority single-button action)
    if (btnDown && !btnUp) {
        if (btnDownPressStart_ == 0) {
            btnDownPressStart_ = now;
        } else if ((now - btnDownPressStart_ >= STOP_PRESS_DURATION_MS) && !stopHandled_) {
            requestStop(StopReason::UserStopped);
            armed_ = false; // Physical stop always explicitly disarms for safety
            stopHandled_ = true;
        }
    } else if (!btnDown) {
        btnDownPressStart_ = 0;
        stopHandled_ = false;
    }

    // Button UP: Start fishing from Idle (only valid if armed and released)
    if (btnUp && !btnDown) {
        if (btnUpPressStart_ == 0) {
            btnUpPressStart_ = now;
        } else if ((now - btnUpPressStart_ >= START_PRESS_DURATION_MS) && !startHandled_) {
            if (armed_ && fsm_.getState() == FishingState::Idle && keyState_ == KeyState::Idle) {
                requestStart();
            }
            startHandled_ = true;
        }
    } else if (!btnUp) {
        btnUpPressStart_ = 0;
        startHandled_ = false;
    }
}

bool LiveFishingController::requestArmToggle() {
    // If a key release is currently pending on the bus, refuse arming
    if (keyState_ != KeyState::Idle) {
        return false;
    }

    if (!armed_) {
        armed_ = true;
        requireButtonsReleaseAfterArm_ = true;
        armPacketsBaseline_ = audioStream_ ? audioStream_->getPacketsReceived() : 0;
    } else {
        disarm();
    }
    return armed_;
}

void LiveFishingController::disarm() {
    armed_ = false;
    requireButtonsReleaseAfterArm_ = false;
    requestStop(StopReason::UserDisarmed);
}

bool LiveFishingController::requestStart() {
    if (!armed_ || !hid_ || !clock_ || !audioStream_) {
        return false;
    }

    const uint32_t now = clock_->nowMs();

    // Refuse to start if full-bag stop condition is confirmed/latched
    if (audioStream_->isBagFull()) {
        armed_ = false;
        lastStopReason_ = StopReason::BagFull;
        return false;
    }

    // Lockout if a key report is currently pending release or press
    if (keyState_ != KeyState::Idle) {
        return false;
    }

    if (!hid_->isMounted()) {
        return false;
    }

    pollAudioFreshness(now);

    const uint64_t currentPackets = audioStream_->getPacketsReceived();

    // 1. Require positive actual packet progress post-arm
    if (currentPackets <= armPacketsBaseline_) {
        return false;
    }

    // 2. Require actual fresh packet arrival within STREAM_TIMEOUT_MS
    if (lastPacketArrivalMs_ == 0 || (now - lastPacketArrivalMs_) > STREAM_TIMEOUT_MS) {
        return false;
    }

    if (fsm_.getState() != FishingState::Idle) {
        return false;
    }

    // Capture baseline metrics at start of active session
    initialDiscontinuities_ = audioStream_->getDiscontinuities();
    initialQueueDrops_ = audioStream_->getQueueDrops();

    // Start fishing sequence
    if (fsm_.handleEvent(FishingEvent::Start)) {
        stateStartTime_ = now;
        lastStopReason_ = StopReason::None;
        audioStream_->clearTrigger();
        if (!beginKeyPress(HID_KEY_SPACE, now)) {
            // If press could not be initiated, fail closed
            requestStop(StopReason::PressFailure);
            armed_ = false;
            return false;
        }
        return true;
    }

    return false;
}

void LiveFishingController::requestStop(StopReason reason) {
    armed_ = false; // Stop, dropout, disconnect, or error always disarms for safety

    if (fsm_.getState() != FishingState::Idle) {
        fsm_.handleEvent(FishingEvent::Stop);
    }
    fsm_.reset(FishingState::Idle);

    // If key was pending press, cancel it
    if (keyState_ == KeyState::PendingPress) {
        keyState_ = KeyState::Idle;
        activeKeycode_ = 0;
    } else if (keyState_ == KeyState::Pressed || keyState_ == KeyState::Releasing) {
        // Attempt immediate release; if endpoint is busy, remain in Releasing to retry
        if (hid_ && hid_->isMounted()) {
            if (hid_->isReady() && hid_->release()) {
                keyState_ = KeyState::Idle;
                activeKeycode_ = 0;
            } else {
                keyState_ = KeyState::Releasing;
            }
        } else {
            // Bus unmounted: local state reset safe
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
        }
    }

    if (audioStream_) {
        audioStream_->clearTrigger();
    }

    lastStopReason_ = reason;
}

void LiveFishingController::checkSafetyGuards(uint32_t now) {
    // 0. Guard against BagFull condition AT ALL TIMES (even while in Idle)
    if (audioStream_ && audioStream_->isBagFull()) {
        if (armed_ || fsm_.getState() != FishingState::Idle) {
            requestStop(StopReason::BagFull);
            armed_ = false;
            return;
        } else if (lastStopReason_ != StopReason::BagFull) {
            lastStopReason_ = StopReason::BagFull;
        }
    }

    // 1. Guard against USB unmount / suspend AT ALL TIMES (even while in Idle)
    if (!hid_ || !hid_->isMounted()) {
        if (armed_ || fsm_.getState() != FishingState::Idle || keyState_ != KeyState::Idle) {
            requestStop(StopReason::UsbUnmounted);
            armed_ = false; // Disarm on unmount; never auto-rearm upon reconnect!
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
            return;
        }
    }

    if (fsm_.getState() == FishingState::Idle) {
        return;
    }

    // 2. Guard against audio stream drop-out while actively fishing
    if (audioStream_) {
        if (lastPacketArrivalMs_ == 0 || (now - lastPacketArrivalMs_) > STREAM_TIMEOUT_MS) {
            requestStop(StopReason::AudioDropout);
            armed_ = false;
            return;
        }

        // 3. Fail closed on stream discontinuity or queue overflow drop
        if (audioStream_->getDiscontinuities() > initialDiscontinuities_) {
            requestStop(StopReason::StreamDiscontinuity);
            armed_ = false;
            return;
        }

        if (audioStream_->getQueueDrops() > initialQueueDrops_) {
            requestStop(StopReason::QueueOverflowDrop);
            armed_ = false;
            return;
        }
    }
}

bool LiveFishingController::beginKeyPress(uint8_t keycode, uint32_t now) {
    if (!armed_ || !hid_ || !hid_->isMounted()) {
        return false;
    }
    if (keyState_ != KeyState::Idle) {
        return false;
    }

    activeKeycode_ = keycode;
    keyPressAttemptStart_ = now;
    keyState_ = KeyState::PendingPress;

    // Attempt non-blocking press immediately if ready
    if (hid_->isReady()) {
        if (hid_->press(0, keycode)) {
            keyState_ = KeyState::Pressed;
            keyPressStartTime_ = now;
            if (fsm_.getState() == FishingState::Casting) {
                totalCasts_++;
                castAcceptedTime_ = now;
            } else if (fsm_.getState() == FishingState::Reeling) {
                totalReels_++;
            }
            return true;
        }
    }

    // If endpoint not ready, remain in PendingPress to retry in updateKeypress()
    return true;
}

void LiveFishingController::updateKeypress(uint32_t now) {
    if (!hid_) return;

    if (keyState_ == KeyState::PendingPress) {
        if (!hid_->isMounted()) {
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
            requestStop(StopReason::UsbUnmounted);
            armed_ = false;
            return;
        }

        // If reel key is pending press and 30s cast duration has expired:
        // Cancel pending reel press to prevent sending late Space after bobber despawn!
        if (fsm_.getState() == FishingState::Reeling && (now - castAcceptedTime_) >= GAME_CAST_DURATION_MS) {
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
            totalTimeouts_++;
            if (audioStream_) {
                audioStream_->clearTrigger();
            }
            fsm_.reset(FishingState::Timeout);
            stateStartTime_ = now;
            return;
        }

        if (hid_->isReady() && hid_->press(0, activeKeycode_)) {
            keyState_ = KeyState::Pressed;
            keyPressStartTime_ = now;
            if (fsm_.getState() == FishingState::Casting) {
                totalCasts_++;
                castAcceptedTime_ = now;
            } else if (fsm_.getState() == FishingState::Reeling) {
                totalReels_++;
            }
        } else if ((now - keyPressAttemptStart_) >= PRESS_RETRY_TIMEOUT_MS) {
            // Bounded retry expired: fail closed rather than claiming cast/reel
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
            requestStop(StopReason::PressFailure);
            armed_ = false;
        }
    } else if (keyState_ == KeyState::Pressed) {
        if (!hid_->isMounted()) {
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
            requestStop(StopReason::UsbUnmounted);
            armed_ = false;
            return;
        }

        if ((now - keyPressStartTime_) >= KEY_PRESS_HOLD_MS) {
            if (hid_->isReady() && hid_->release()) {
                keyState_ = KeyState::Idle;
                activeKeycode_ = 0;
            } else {
                keyState_ = KeyState::Releasing;
            }
        }
    } else if (keyState_ == KeyState::Releasing) {
        if (!hid_->isMounted()) {
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
            return;
        }

        if (hid_->isReady() && hid_->release()) {
            keyState_ = KeyState::Idle;
            activeKeycode_ = 0;
        }
    }
}

void LiveFishingController::processFsm(uint32_t now) {
    switch (fsm_.getState()) {
        case FishingState::Casting:
            // Cast presentation wait; key must be completely released
            if ((now - stateStartTime_ >= CASTING_DURATION_MS) && (keyState_ == KeyState::Idle)) {
                if (audioStream_) {
                    audioStream_->clearTrigger(); // Purge any casting acoustic transients
                }
                if (fsm_.handleEvent(FishingEvent::CastComplete)) {
                    stateStartTime_ = now;
                }
            }
            break;

        case FishingState::Waiting: {
            const uint32_t elapsedSinceCast = now - castAcceptedTime_;
            if (audioStream_ && audioStream_->isBagFull()) {
                requestStop(StopReason::BagFull);
                armed_ = false;
                return;
            }
            // Deadline check takes strict priority at or after GAME_CAST_DURATION_MS (30,000ms):
            // Accept bite ONLY while elapsed since accepted cast press is strictly under 30000ms.
            // Once 30s has expired, any acoustic trigger is considered outdated sound.
            if (elapsedSinceCast >= GAME_CAST_DURATION_MS) {
                totalTimeouts_++;
                if (audioStream_) {
                    audioStream_->clearTrigger();
                }
                if (fsm_.handleEvent(FishingEvent::BiteTimeout)) {
                    stateStartTime_ = now;
                } else {
                    fsm_.reset(FishingState::Timeout);
                    stateStartTime_ = now;
                }
            } else if (audioStream_ && audioStream_->isTriggered()) {
                audioStream_->clearTrigger();
                if (fsm_.handleEvent(FishingEvent::BiteDetected)) {
                    stateStartTime_ = now;
                    totalBites_++;
                }
            }
            break;
        }

        case FishingState::BiteDetected: {
            const uint32_t elapsedSinceCast = now - castAcceptedTime_;
            // If bite detected immediately before expiry but 300ms reel delay would send after 30s:
            // suppress/cancel pending reel at deadline and enter timeout path, never issue late reel!
            if (elapsedSinceCast >= GAME_CAST_DURATION_MS) {
                totalTimeouts_++;
                if (audioStream_) {
                    audioStream_->clearTrigger();
                }
                fsm_.reset(FishingState::Timeout);
                stateStartTime_ = now;
                return;
            }

            // Humanized presentation pause before reeling
            if ((now - stateStartTime_) >= BITE_DETECTED_DURATION_MS) {
                // Confirm audio stream is still fresh before attempting reel
                if (lastPacketArrivalMs_ == 0 || (now - lastPacketArrivalMs_) > STREAM_TIMEOUT_MS) {
                    requestStop(StopReason::AudioDropout);
                    armed_ = false;
                    return;
                }

                // Check again right before reel dispatch
                if ((now - castAcceptedTime_) >= GAME_CAST_DURATION_MS) {
                    totalTimeouts_++;
                    if (audioStream_) {
                        audioStream_->clearTrigger();
                    }
                    fsm_.reset(FishingState::Timeout);
                    stateStartTime_ = now;
                    return;
                }

                if (fsm_.handleEvent(FishingEvent::ReelTrigger)) {
                    stateStartTime_ = now;
                    if (!beginKeyPress(HID_KEY_SPACE, now)) {
                        requestStop(StopReason::PressFailure);
                        armed_ = false;
                    }
                }
            }
            break;
        }

        case FishingState::Reeling:
            // If reel key is still pending press and 30s deadline has expired, cancel without press
            if (keyState_ == KeyState::PendingPress && (now - castAcceptedTime_) >= GAME_CAST_DURATION_MS) {
                keyState_ = KeyState::Idle;
                activeKeycode_ = 0;
                totalTimeouts_++;
                if (audioStream_) {
                    audioStream_->clearTrigger();
                }
                fsm_.reset(FishingState::Timeout);
                stateStartTime_ = now;
                return;
            }

            // Wait for reel animation and confirmed key release completion
            if ((now - stateStartTime_ >= REELING_DURATION_MS) && (keyState_ == KeyState::Idle)) {
                if (audioStream_) {
                    audioStream_->clearTrigger(); // Purge any audio triggers before entering cooldown
                }
                if (fsm_.handleEvent(FishingEvent::ReelComplete)) {
                    stateStartTime_ = now;
                }
            }
            break;

        case FishingState::Timeout:
            // Non-blocking timeout indication presentation before Cooldown
            if ((now - stateStartTime_) >= TIMEOUT_DURATION_MS) {
                if (audioStream_) {
                    audioStream_->clearTrigger(); // Purge any acoustic triggers during timeout
                }
                if (fsm_.handleEvent(FishingEvent::TimeoutComplete)) {
                    stateStartTime_ = now;
                }
            }
            break;

        case FishingState::Cooldown:
            // Non-blocking 2.0s cooldown before next cast in continuous mode;
            // ensure key release complete before attempting next cast
            if ((now - stateStartTime_ >= COOLDOWN_DURATION_MS) && (keyState_ == KeyState::Idle)) {
                // In continuous mode: recasting requires remaining armed, mounted, and fresh audio
                if (armed_ && hid_ && hid_->isMounted() &&
                    lastPacketArrivalMs_ != 0 &&
                    ((now - lastPacketArrivalMs_) <= STREAM_TIMEOUT_MS)) {
                    if (audioStream_) {
                        if (audioStream_->getDiscontinuities() > initialDiscontinuities_) {
                            requestStop(StopReason::StreamDiscontinuity);
                            armed_ = false;
                            return;
                        }
                        if (audioStream_->getQueueDrops() > initialQueueDrops_) {
                            requestStop(StopReason::QueueOverflowDrop);
                            armed_ = false;
                            return;
                        }
                        audioStream_->clearTrigger(); // Purge any transient audio triggers before recast
                    }
                    if (fsm_.handleEvent(FishingEvent::CooldownComplete)) {
                        stateStartTime_ = now;
                        if (!beginKeyPress(HID_KEY_SPACE, now)) {
                            requestStop(StopReason::PressFailure);
                            armed_ = false;
                        }
                    }
                } else {
                    requestStop(StopReason::AudioDropout);
                    armed_ = false;
                }
            }
            break;

        case FishingState::Idle:
        case FishingState::Paused:
        case FishingState::Error:
        default:
            break;
    }
}

}  // namespace fishing
}  // namespace paxg
