#pragma once

#include <cstdint>

namespace paxg {
namespace fishing {

/**
 * @brief States of the Fishing Finite State Machine.
 */
enum class FishingState : int {
    Idle = 0,
    Casting = 1,
    Waiting = 2,
    BiteDetected = 3,
    Reeling = 4,
    Cooldown = 5,
    Timeout = 6,
    Paused = 7,
    Error = 8,
};

/**
 * @brief Discrete transition events driving the state machine.
 */
enum class FishingEvent : int {
    Start = 0,         // User initiates fishing from Idle
    CastComplete,      // Casting duration (1500ms) expired -> enter Waiting
    BiteDetected,      // Audio detector confirmed valid bite acoustic burst -> enter BiteDetected
    BiteTimeout,       // Wait timer exceeded configured wait timeout -> enter Timeout
    ReelTrigger,       // BiteDetected presentation duration (300ms) expired -> enter Reeling
    ReelComplete,      // Reeling duration (1200ms) expired -> enter Cooldown
    CooldownComplete,  // Cooldown duration expired -> loop back to Casting
    TimeoutComplete,   // Timeout display duration (1500ms) expired -> enter Cooldown
    Pause,             // User paused active simulation -> enter Paused
    Resume,            // User resumed simulation -> enter Casting
    Stop,              // User stopped simulation or reset from error -> enter Idle
    Error,             // Hardware/USB/Audio failure occurred -> enter Error safe state
};

inline const char* getFishingStateName(FishingState state) {
    switch (state) {
        case FishingState::Idle: return "IDLE";
        case FishingState::Casting: return "CASTING";
        case FishingState::Waiting: return "WAITING";
        case FishingState::BiteDetected: return "BITE DETECTED";
        case FishingState::Reeling: return "REELING";
        case FishingState::Cooldown: return "COOLDOWN";
        case FishingState::Timeout: return "TIMEOUT";
        case FishingState::Paused: return "PAUSED";
        case FishingState::Error: return "ERROR";
        default: return "UNKNOWN";
    }
}

inline const char* getFishingEventName(FishingEvent ev) {
    switch (ev) {
        case FishingEvent::Start: return "START";
        case FishingEvent::CastComplete: return "CAST_COMPLETE";
        case FishingEvent::BiteDetected: return "BITE_DETECTED";
        case FishingEvent::BiteTimeout: return "BITE_TIMEOUT";
        case FishingEvent::ReelTrigger: return "REEL_TRIGGER";
        case FishingEvent::ReelComplete: return "REEL_COMPLETE";
        case FishingEvent::CooldownComplete: return "COOLDOWN_COMPLETE";
        case FishingEvent::TimeoutComplete: return "TIMEOUT_COMPLETE";
        case FishingEvent::Pause: return "PAUSE";
        case FishingEvent::Resume: return "RESUME";
        case FishingEvent::Stop: return "STOP";
        case FishingEvent::Error: return "ERROR";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Deterministic, platform-independent Finite State Machine for Fishing Mode.
 *
 * Enforces all state transition rules. Pure logic with zero side effects:
 * hardware peripherals (USB HID, displays, NVS) and timers are managed by caller.
 */
class FishingStateMachine {
public:
    explicit FishingStateMachine(FishingState initialState = FishingState::Idle)
        : state_(initialState) {}

    FishingState getState() const { return state_; }

    void reset(FishingState state = FishingState::Idle) {
        state_ = state;
    }

    /**
     * @brief Process an incoming event according to strict transition rules.
     * @param ev The transition event.
     * @return true if the event was valid in the current state and transition occurred;
     *         false if the event is disallowed, leaving state unmodified.
     */
    bool handleEvent(FishingEvent ev) {
        switch (state_) {
            case FishingState::Idle:
                if (ev == FishingEvent::Start) {
                    state_ = FishingState::Casting;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    // Idempotent stop in Idle (triggers stats reset in caller)
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::Casting:
                if (ev == FishingEvent::CastComplete) {
                    state_ = FishingState::Waiting;
                    return true;
                }
                if (ev == FishingEvent::Pause) {
                    state_ = FishingState::Paused;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::Waiting:
                if (ev == FishingEvent::BiteDetected) {
                    state_ = FishingState::BiteDetected;
                    return true;
                }
                if (ev == FishingEvent::BiteTimeout) {
                    state_ = FishingState::Timeout;
                    return true;
                }
                if (ev == FishingEvent::Pause) {
                    state_ = FishingState::Paused;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::BiteDetected:
                if (ev == FishingEvent::ReelTrigger) {
                    state_ = FishingState::Reeling;
                    return true;
                }
                if (ev == FishingEvent::Pause) {
                    state_ = FishingState::Paused;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::Reeling:
                if (ev == FishingEvent::ReelComplete) {
                    state_ = FishingState::Cooldown;
                    return true;
                }
                if (ev == FishingEvent::Pause) {
                    state_ = FishingState::Paused;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::Cooldown:
                if (ev == FishingEvent::CooldownComplete) {
                    state_ = FishingState::Casting;
                    return true;
                }
                if (ev == FishingEvent::Pause) {
                    state_ = FishingState::Paused;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::Timeout:
                if (ev == FishingEvent::TimeoutComplete) {
                    state_ = FishingState::Cooldown;
                    return true;
                }
                if (ev == FishingEvent::Pause) {
                    state_ = FishingState::Paused;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::Paused:
                if (ev == FishingEvent::Resume) {
                    state_ = FishingState::Casting;
                    return true;
                }
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    state_ = FishingState::Error;
                    return true;
                }
                break;

            case FishingState::Error:
                if (ev == FishingEvent::Stop) {
                    state_ = FishingState::Idle;
                    return true;
                }
                if (ev == FishingEvent::Error) {
                    // Idempotent error
                    return true;
                }
                break;
        }

        return false;  // Invalid transition rejected
    }

private:
    FishingState state_;
};

}  // namespace fishing
}  // namespace paxg
