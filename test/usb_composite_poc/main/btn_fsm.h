/**
 * @file btn_fsm.h
 * @brief Deterministic, non-blocking button state machine with safe USB HID synchronization.
 *
 * Designed for ESP32-S3 physical button input with:
 * - Clear separation of true USB mount/suspend state from transient endpoint busy state.
 * - Observed stable release required after startup/disconnect before accepting any press.
 * - Guaranteed single-shot actuation (no auto-repeat).
 * - Debouncing on both press and release.
 * - Non-blocking release retry while mounted; no held key lost when endpoint is busy.
 * - Safe reset strictly on real unmount/suspend without auto-send on reconnect.
 * - Platform-independent logic (pure C99, standard types only).
 */

#ifndef BTN_FSM_H
#define BTN_FSM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_OP_SUCCESS = 0,     /**< Report queued/accepted by endpoint */
    BTN_OP_BUSY,            /**< Endpoint temporarily busy; retry non-blocking */
    BTN_OP_DISCONNECTED     /**< Real unmount or suspend */
} btn_op_result_t;

typedef btn_op_result_t (*btn_press_fn_t)(void *user_data);
typedef btn_op_result_t (*btn_release_fn_t)(void *user_data);

typedef enum {
    BTN_STATE_STARTUP_WAIT_RELEASE = 0, /**< Waiting for button to be released after boot/mount */
    BTN_STATE_STARTUP_DEBOUNCE_RELEASE, /**< Debouncing initial release after boot/mount */
    BTN_STATE_IDLE,                     /**< Button is stably released and armed for a new press */
    BTN_STATE_DEBOUNCING_PRESS,         /**< Debouncing a physical press */
    BTN_STATE_HOLDING_KEY,              /**< Press sent; holding key for minimum hold time */
    BTN_STATE_RETRY_RELEASE,            /**< Actively sending release report; retry nonblocking until accepted */
    BTN_STATE_WAIT_RELEASE,             /**< Keystroke finished (or dropped); waiting for physical release */
    BTN_STATE_DEBOUNCING_RELEASE        /**< Debouncing physical release */
} btn_fsm_state_t;

typedef enum {
    BTN_ACTION_NONE = 0,
    BTN_ACTION_SEND_PRESS,              /**< Successfully sent key press report */
    BTN_ACTION_RETRY_PRESS,             /**< Press endpoint temporarily busy; retrying */
    BTN_ACTION_SEND_RELEASE,            /**< Attempted release report, currently retrying */
    BTN_ACTION_KEY_SEQUENCE_COMPLETED,  /**< Release report accepted; single sequence complete */
    BTN_ACTION_ABORT_DISCONNECT,        /**< Real USB disconnect/unmount; state safely cleared */
    BTN_ACTION_DROP_UNREADY             /**< Button pressed while USB was not mounted; dropped */
} btn_fsm_action_t;

typedef struct {
    btn_fsm_state_t state;
    uint32_t state_start_ms;
    uint32_t debounce_ms;
    uint32_t hold_ms;
    bool was_mounted;
} btn_fsm_t;

/**
 * @brief Initialize button FSM.
 *
 * Always begins in BTN_STATE_STARTUP_WAIT_RELEASE to ensure that holding the button
 * across boot or USB mount will NEVER trigger an accidental keystroke.
 */
static inline void btn_fsm_init(btn_fsm_t *fsm, uint32_t debounce_ms, uint32_t hold_ms)
{
    if (fsm == NULL) return;
    fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
    fsm->state_start_ms = 0;
    fsm->debounce_ms = debounce_ms;
    fsm->hold_ms = hold_ms;
    fsm->was_mounted = false;
}

static inline const char *btn_fsm_state_str(btn_fsm_state_t state)
{
    switch (state) {
        case BTN_STATE_STARTUP_WAIT_RELEASE:     return "STARTUP_WAIT_RELEASE";
        case BTN_STATE_STARTUP_DEBOUNCE_RELEASE: return "STARTUP_DEBOUNCE_RELEASE";
        case BTN_STATE_IDLE:                     return "IDLE";
        case BTN_STATE_DEBOUNCING_PRESS:         return "DEBOUNCING_PRESS";
        case BTN_STATE_HOLDING_KEY:              return "HOLDING_KEY";
        case BTN_STATE_RETRY_RELEASE:            return "RETRY_RELEASE";
        case BTN_STATE_WAIT_RELEASE:             return "WAIT_RELEASE";
        case BTN_STATE_DEBOUNCING_RELEASE:        return "DEBOUNCING_RELEASE";
        default:                                 return "UNKNOWN";
    }
}

/**
 * @brief Step the button state machine.
 *
 * @param fsm Pointer to FSM structure.
 * @param now_ms Current timestamp in milliseconds.
 * @param is_pressed True if physical button is currently held down (GPIO active-low reading == 0).
 * @param is_mounted True if USB is actively mounted and configured by host (tud_mounted() && !tud_suspended()).
 * @param press_fn Callback to send press report (returns btn_op_result_t).
 * @param release_fn Callback to send release report (returns btn_op_result_t).
 * @param user_data Context passed to callbacks.
 * @return btn_fsm_action_t Action performed during this step.
 */
static inline btn_fsm_action_t btn_fsm_step(btn_fsm_t *fsm,
                                            uint32_t now_ms,
                                            bool is_pressed,
                                            bool is_mounted,
                                            btn_press_fn_t press_fn,
                                            btn_release_fn_t release_fn,
                                            void *user_data)
{
    if (fsm == NULL) return BTN_ACTION_NONE;

    // Detect REAL USB unmount or suspend (true disconnect)
    if (fsm->was_mounted && !is_mounted) {
        fsm->was_mounted = false;
        fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
        fsm->state_start_ms = now_ms;
        return BTN_ACTION_ABORT_DISCONNECT;
    }
    fsm->was_mounted = is_mounted;

    switch (fsm->state) {
        case BTN_STATE_STARTUP_WAIT_RELEASE:
            if (is_pressed) {
                // Button held down at startup or across mount: wait for release
                fsm->state_start_ms = now_ms;
            } else {
                fsm->state = BTN_STATE_STARTUP_DEBOUNCE_RELEASE;
                fsm->state_start_ms = now_ms;
            }
            return BTN_ACTION_NONE;

        case BTN_STATE_STARTUP_DEBOUNCE_RELEASE:
            if (is_pressed) {
                // Bounce detected before stable release window
                fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
                fsm->state_start_ms = now_ms;
            } else if ((now_ms - fsm->state_start_ms) >= fsm->debounce_ms) {
                // Observed stable release after startup confirmed! Armed for real input.
                fsm->state = BTN_STATE_IDLE;
            }
            return BTN_ACTION_NONE;

        case BTN_STATE_IDLE:
            if (is_pressed) {
                fsm->state = BTN_STATE_DEBOUNCING_PRESS;
                fsm->state_start_ms = now_ms;
            }
            return BTN_ACTION_NONE;

        case BTN_STATE_DEBOUNCING_PRESS:
            if (!is_pressed) {
                // Transient bounce rejected
                fsm->state = BTN_STATE_IDLE;
            } else if ((now_ms - fsm->state_start_ms) >= fsm->debounce_ms) {
                // Stable physical press confirmed
                if (!is_mounted) {
                    // Pressed while USB not mounted: drop actuation and wait for release
                    fsm->state = BTN_STATE_WAIT_RELEASE;
                    return BTN_ACTION_DROP_UNREADY;
                }
                if (press_fn != NULL) {
                    btn_op_result_t res = press_fn(user_data);
                    if (res == BTN_OP_SUCCESS) {
                        fsm->state = BTN_STATE_HOLDING_KEY;
                        fsm->state_start_ms = now_ms;
                        return BTN_ACTION_SEND_PRESS;
                    } else if (res == BTN_OP_BUSY) {
                        // Endpoint temporarily busy; stay in debouncing state and retry next tick
                        return BTN_ACTION_RETRY_PRESS;
                    } else { // BTN_OP_DISCONNECTED
                        fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
                        fsm->state_start_ms = now_ms;
                        return BTN_ACTION_ABORT_DISCONNECT;
                    }
                } else {
                    fsm->state = BTN_STATE_WAIT_RELEASE;
                }
            }
            return BTN_ACTION_NONE;

        case BTN_STATE_HOLDING_KEY:
            // Check real disconnect
            if (!is_mounted) {
                fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
                fsm->state_start_ms = now_ms;
                return BTN_ACTION_ABORT_DISCONNECT;
            }
            // CRITICAL: Even if endpoint is temporarily busy (e.g. right after press report),
            // is_mounted is still true! We do NOT abort! We safely count hold time!
            if ((now_ms - fsm->state_start_ms) >= fsm->hold_ms) {
                // Hold time elapsed; attempt non-blocking release
                fsm->state = BTN_STATE_RETRY_RELEASE;
                if (release_fn != NULL) {
                    btn_op_result_t res = release_fn(user_data);
                    if (res == BTN_OP_SUCCESS) {
                        // Release accepted on first attempt!
                        fsm->state = BTN_STATE_WAIT_RELEASE;
                        return BTN_ACTION_KEY_SEQUENCE_COMPLETED;
                    } else if (res == BTN_OP_BUSY) {
                        // Endpoint temporarily busy; will retry on next tick
                        return BTN_ACTION_SEND_RELEASE;
                    } else { // BTN_OP_DISCONNECTED
                        fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
                        fsm->state_start_ms = now_ms;
                        return BTN_ACTION_ABORT_DISCONNECT;
                    }
                }
            }
            return BTN_ACTION_NONE;

        case BTN_STATE_RETRY_RELEASE:
            if (!is_mounted) {
                fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
                fsm->state_start_ms = now_ms;
                return BTN_ACTION_ABORT_DISCONNECT;
            }
            if (release_fn != NULL) {
                btn_op_result_t res = release_fn(user_data);
                if (res == BTN_OP_SUCCESS) {
                    // Release accepted on retry!
                    fsm->state = BTN_STATE_WAIT_RELEASE;
                    return BTN_ACTION_KEY_SEQUENCE_COMPLETED;
                } else if (res == BTN_OP_BUSY) {
                    // Still busy; retry non-blocking on subsequent ticks
                    return BTN_ACTION_SEND_RELEASE;
                } else { // BTN_OP_DISCONNECTED
                    fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
                    fsm->state_start_ms = now_ms;
                    return BTN_ACTION_ABORT_DISCONNECT;
                }
            }
            return BTN_ACTION_NONE;

        case BTN_STATE_WAIT_RELEASE:
            if (!is_pressed) {
                fsm->state = BTN_STATE_DEBOUNCING_RELEASE;
                fsm->state_start_ms = now_ms;
            }
            // If still pressed: stay here indefinitely (NO AUTO-REPEAT)
            return BTN_ACTION_NONE;

        case BTN_STATE_DEBOUNCING_RELEASE:
            if (is_pressed) {
                // Bounce during release: return to wait release
                fsm->state = BTN_STATE_WAIT_RELEASE;
                fsm->state_start_ms = now_ms;
            } else if ((now_ms - fsm->state_start_ms) >= fsm->debounce_ms) {
                // Stable physical release confirmed: ready for next press
                fsm->state = BTN_STATE_IDLE;
            }
            return BTN_ACTION_NONE;

        default:
            fsm->state = BTN_STATE_STARTUP_WAIT_RELEASE;
            fsm->state_start_ms = now_ms;
            return BTN_ACTION_NONE;
    }
}

#ifdef __cplusplus
}
#endif

#endif // BTN_FSM_H
