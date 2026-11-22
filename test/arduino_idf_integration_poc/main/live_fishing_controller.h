#pragma once

#include <cstdint>
#include <cstddef>
#include "modules/fishing_state_machine.h"

namespace paxg {
namespace fishing {

/**
 * @brief Reason for stopping active fishing operation.
 */
enum class StopReason : int {
    None = 0,
    UserStopped,
    UserDisarmed,
    UsbUnmounted,
    AudioDropout,
    StreamDiscontinuity,
    QueueOverflowDrop,
    Timeout,
    PressFailure,
    ErrorState,
    CycleCompleted,
};

inline const char* getStopReasonName(StopReason reason) {
    switch (reason) {
        case StopReason::None: return "NONE";
        case StopReason::UserStopped: return "USER_STOP";
        case StopReason::UserDisarmed: return "USER_DISARMED";
        case StopReason::UsbUnmounted: return "USB_UNMOUNTED";
        case StopReason::AudioDropout: return "AUDIO_DROPOUT";
        case StopReason::StreamDiscontinuity: return "STREAM_DISCONTINUITY";
        case StopReason::QueueOverflowDrop: return "QUEUE_OVERFLOW";
        case StopReason::Timeout: return "BITE_TIMEOUT";
        case StopReason::PressFailure: return "PRESS_FAILURE";
        case StopReason::ErrorState: return "ERROR_STATE";
        case StopReason::CycleCompleted: return "CYCLE_COMPLETED";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Non-blocking HID keyboard interface for production composite driver and test mocks.
 */
class IHidKeyboard {
public:
    virtual ~IHidKeyboard() = default;
    virtual bool isMounted() = 0;
    virtual bool isReady() = 0;
    virtual bool press(uint8_t modifier, uint8_t keycode) = 0;
    virtual bool release() = 0;
};

/**
 * @brief Clock abstraction for wrap-safe time retrieval.
 */
class IClock {
public:
    virtual ~IClock() = default;
    virtual uint32_t nowMs() = 0;
};

/**
 * @brief Audio stream telemetry and trigger interface.
 */
class IAudioStream {
public:
    virtual ~IAudioStream() = default;
    virtual uint64_t getPacketsReceived() const = 0;
    virtual uint64_t getDiscontinuities() const = 0;
    virtual uint32_t getQueueDrops() const = 0;
    virtual bool isTriggered() const = 0;
    virtual void clearTrigger() = 0;
    virtual float getRms() const = 0;
};

/**
 * @brief Internal non-blocking keypress state.
 */
enum class KeyState : int {
    Idle = 0,
    PendingPress,    // Key scheduled to be pressed, retrying until accepted by HID
    Pressed,         // Key successfully accepted by HID, actively held down
    Releasing,       // Hold duration expired or stop requested, retrying release until accepted
};

/**
 * @brief Live fishing controller with explicit safety arming, fail-closed guards,
 *        and single-key (SPACE = 0x2c) cast/reel coordination.
 *
 * Safety & Invariant Guarantees:
 * 1. Boot default: DISARMED and IDLE. Zero automated keys sent on boot.
 * 2. Positive Post-Arm Packet Progress: Starting or key dispatch strictly requires
 *    actual packet progress (packetsReceived > armBaseline) and fresh arrival within
 *    STREAM_TIMEOUT_MS. Never allows starting with zero audio packets received.
 * 3. Deliberate Arming & Post-Arm Release: Requires holding UP + DOWN for >= 1000ms.
 *    After arming, BOTH buttons must be observed released before any start gesture is valid.
 * 4. Startup Held-Button Immunity: Any button held at boot/init is ignored until both released.
 * 5. Disarm on Disconnect/Suspend in ANY State: Disconnect/unmount forces immediate disarm
 *    and stop even while in Idle. Re-enumeration never auto-re-arms.
 * 6. Non-blocking Key Release Retry & Stop Lockout: On stop/disarm/dropout while a key is
 *    pressed or pending release, the release is retried on subsequent ticks until accepted.
 *    No rearm, restart, or additional press is permitted while a key is not Idle.
 * 7. Explicit Accepted Key Press: Cast and reel counters advance ONLY upon confirmed press.
 *    If press cannot be accepted within PRESS_RETRY_TIMEOUT_MS, controller fails closed and disarms.
 * 8. Single-Key Spacebar Timeout Recast: Because Space toggles cast/reel, recast
 *    is strictly prohibited until the full 30s game cast duration plus margin (32s in Waiting)
 *    has safely elapsed. On timeout, state transitions to Timeout -> Cooldown -> Casting,
 *    clearing latched audio and recasting only if still armed, mounted, and healthy.
 * 9. Wrap-Safe Arithmetic: All time calculations use unsigned (now - start) >= duration.
 */
class LiveFishingController {
public:
    static constexpr uint8_t HID_KEY_SPACE = 0x2c;           // USB HID usage 44
    static constexpr uint32_t ARM_HOLD_DURATION_MS = 1000UL;   // 1s dual-button hold
    static constexpr uint32_t START_PRESS_DURATION_MS = 50UL;  // Debounce for UP start
    static constexpr uint32_t STOP_PRESS_DURATION_MS = 50UL;   // Debounce for DOWN stop
    static constexpr uint32_t KEY_PRESS_HOLD_MS = 80UL;        // Non-blocking key hold
    static constexpr uint32_t PRESS_RETRY_TIMEOUT_MS = 500UL;  // Max duration to retry press
    static constexpr uint32_t CASTING_DURATION_MS = 1500UL;    // Cast presentation delay
    static constexpr uint32_t BITE_DETECTED_DURATION_MS = 300UL; // Presentation pause
    static constexpr uint32_t REELING_DURATION_MS = 1200UL;    // Reel animation delay
    static constexpr uint32_t TIMEOUT_DURATION_MS = 1500UL;    // Timeout presentation indication delay before Cooldown
    static constexpr uint32_t COOLDOWN_DURATION_MS = 2000UL;   // Inter-cast cooldown (>= 2s safety margin)
    static constexpr uint32_t GAME_CAST_DURATION_MS = 30000UL; // User-reported 30s cast duration from accepted cast press
    static constexpr uint32_t WAIT_TIMEOUT_MS = GAME_CAST_DURATION_MS; // Backwards-compatible alias
    static constexpr uint32_t STREAM_TIMEOUT_MS = 1000UL;      // 1s audio freshness limit

    LiveFishingController(IHidKeyboard* hid, IClock* clock, IAudioStream* audioStream);

    void init();

    /**
     * @brief Periodic update called from main application loop.
     * @param btnUpPressed Raw or debounced state of Button UP (active true).
     * @param btnDownPressed Raw or debounced state of Button DOWN (active true).
     */
    void update(bool btnUpPressed, bool btnDownPressed);

    // Operational inspection
    bool isArmed() const { return armed_; }
    FishingState getState() const { return fsm_.getState(); }
    KeyState getKeyState() const { return keyState_; }
    StopReason getLastStopReason() const { return lastStopReason_; }
    uint32_t getTotalCasts() const { return totalCasts_; }
    uint32_t getTotalBites() const { return totalBites_; }
    uint32_t getTotalReels() const { return totalReels_; }
    uint32_t getTotalTimeouts() const { return totalTimeouts_; }
    uint32_t getStateDurationMs() const;
    uint32_t getWaitRemainingMs() const;
    uint32_t getCastAcceptedTime() const { return castAcceptedTime_; }
    bool isCycleCompleted() const { return lastStopReason_ == StopReason::CycleCompleted; }

    // Manual control interfaces
    bool requestArmToggle();
    bool requestStart();
    void requestStop(StopReason reason = StopReason::UserStopped);

private:
    void handleButtons(bool btnUp, bool btnDown, uint32_t now);
    void updateKeypress(uint32_t now);
    void checkSafetyGuards(uint32_t now);
    void processFsm(uint32_t now);
    bool beginKeyPress(uint8_t keycode, uint32_t now);
    void pollAudioFreshness(uint32_t now);

    IHidKeyboard* hid_;
    IClock* clock_;
    IAudioStream* audioStream_;
    FishingStateMachine fsm_;

    bool armed_ = false;
    bool startupProtectionActive_ = true;
    bool requireButtonsReleaseAfterArm_ = false;

    // Button tracking
    uint32_t dualButtonHoldStart_ = 0;
    bool armToggleHandled_ = false;
    uint32_t btnUpPressStart_ = 0;
    bool startHandled_ = false;
    uint32_t btnDownPressStart_ = 0;
    bool stopHandled_ = false;

    // FSM timing & state tracking
    uint32_t stateStartTime_ = 0;
    uint32_t castAcceptedTime_ = 0;
    StopReason lastStopReason_ = StopReason::None;

    // Audio stream baseline for freshness and discontinuity checks
    uint64_t lastPacketsReceived_ = 0;
    uint32_t lastPacketArrivalMs_ = 0;
    uint64_t armPacketsBaseline_ = 0;
    uint64_t initialDiscontinuities_ = 0;
    uint32_t initialQueueDrops_ = 0;

    // Non-blocking HID key state
    KeyState keyState_ = KeyState::Idle;
    uint8_t activeKeycode_ = 0;
    uint32_t keyPressStartTime_ = 0;
    uint32_t keyPressAttemptStart_ = 0;

    // Session metrics
    uint32_t totalCasts_ = 0;
    uint32_t totalBites_ = 0;
    uint32_t totalReels_ = 0;
    uint32_t totalTimeouts_ = 0;
};

}  // namespace fishing
}  // namespace paxg
