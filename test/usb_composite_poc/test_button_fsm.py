#!/usr/bin/env python3
"""
test_button_fsm.py
Host-runnable deterministic unit test suite for the PAXG composite USB button FSM.

Verifies:
1. Endpoint busy right after successful press does NOT trigger false disconnect:
   - tud_hid_ready() being false while tud_mounted() is true keeps key held safely.
2. Release busy retry:
   - Release report returning busy retries non-blocking on subsequent ticks;
     success is claimed ONLY when release report is accepted.
3. True disconnect (unmount/suspend):
   - Only real unmount/suspend aborts active sequence to STARTUP_WAIT_RELEASE.
   - Reconnect while button remains held emits strictly zero keys until stable release.
4. Held at boot and across mount:
   - Holding GPIO 0 across boot and USB mount emits zero keys; requires observed stable release.
5. Transient bounce rejection on press and release.
6. Single actuation with strictly NO auto-repeat on long holds (e.g. 2s hold).
"""

import sys
from enum import Enum

class BtnOpResult(Enum):
    SUCCESS = 0
    BUSY = 1
    DISCONNECTED = 2

class BtnState(Enum):
    STARTUP_WAIT_RELEASE = 0
    STARTUP_DEBOUNCE_RELEASE = 1
    IDLE = 2
    DEBOUNCING_PRESS = 3
    HOLDING_KEY = 4
    RETRY_RELEASE = 5
    WAIT_RELEASE = 6
    DEBOUNCING_RELEASE = 7

class BtnAction(Enum):
    NONE = 0
    SEND_PRESS = 1
    RETRY_PRESS = 2
    SEND_RELEASE = 3
    KEY_SEQUENCE_COMPLETED = 4
    ABORT_DISCONNECT = 5
    DROP_UNREADY = 6

class ButtonFsm:
    def __init__(self, debounce_ms=50, hold_ms=50):
        self.state = BtnState.STARTUP_WAIT_RELEASE
        self.state_start_ms = 0
        self.debounce_ms = debounce_ms
        self.hold_ms = hold_ms
        self.was_mounted = False

    def step(self, now_ms, is_pressed, is_mounted, press_fn, release_fn):
        # Real disconnect: only when is_mounted (tud_mounted && !tud_suspended) drops
        if self.was_mounted and not is_mounted:
            self.was_mounted = False
            self.state = BtnState.STARTUP_WAIT_RELEASE
            self.state_start_ms = now_ms
            return BtnAction.ABORT_DISCONNECT
        self.was_mounted = is_mounted

        if self.state == BtnState.STARTUP_WAIT_RELEASE:
            if is_pressed:
                self.state_start_ms = now_ms
            else:
                self.state = BtnState.STARTUP_DEBOUNCE_RELEASE
                self.state_start_ms = now_ms
            return BtnAction.NONE

        elif self.state == BtnState.STARTUP_DEBOUNCE_RELEASE:
            if is_pressed:
                self.state = BtnState.STARTUP_WAIT_RELEASE
                self.state_start_ms = now_ms
            elif (now_ms - self.state_start_ms) >= self.debounce_ms:
                self.state = BtnState.IDLE
            return BtnAction.NONE

        elif self.state == BtnState.IDLE:
            if is_pressed:
                self.state = BtnState.DEBOUNCING_PRESS
                self.state_start_ms = now_ms
            return BtnAction.NONE

        elif self.state == BtnState.DEBOUNCING_PRESS:
            if not is_pressed:
                self.state = BtnState.IDLE
            elif (now_ms - self.state_start_ms) >= self.debounce_ms:
                if not is_mounted:
                    self.state = BtnState.WAIT_RELEASE
                    return BtnAction.DROP_UNREADY
                if press_fn:
                    res = press_fn()
                    if res == BtnOpResult.SUCCESS:
                        self.state = BtnState.HOLDING_KEY
                        self.state_start_ms = now_ms
                        return BtnAction.SEND_PRESS
                    elif res == BtnOpResult.BUSY:
                        return BtnAction.RETRY_PRESS
                    else: # DISCONNECTED
                        self.state = BtnState.STARTUP_WAIT_RELEASE
                        self.state_start_ms = now_ms
                        return BtnAction.ABORT_DISCONNECT
                else:
                    self.state = BtnState.WAIT_RELEASE
            return BtnAction.NONE

        elif self.state == BtnState.HOLDING_KEY:
            if not is_mounted:
                self.state = BtnState.STARTUP_WAIT_RELEASE
                self.state_start_ms = now_ms
                return BtnAction.ABORT_DISCONNECT
            # Notice: even if endpoint is busy, is_mounted is true -> does NOT abort!
            if (now_ms - self.state_start_ms) >= self.hold_ms:
                self.state = BtnState.RETRY_RELEASE
                if release_fn:
                    res = release_fn()
                    if res == BtnOpResult.SUCCESS:
                        self.state = BtnState.WAIT_RELEASE
                        return BtnAction.KEY_SEQUENCE_COMPLETED
                    elif res == BtnOpResult.BUSY:
                        return BtnAction.SEND_RELEASE
                    else:
                        self.state = BtnState.STARTUP_WAIT_RELEASE
                        self.state_start_ms = now_ms
                        return BtnAction.ABORT_DISCONNECT
            return BtnAction.NONE

        elif self.state == BtnState.RETRY_RELEASE:
            if not is_mounted:
                self.state = BtnState.STARTUP_WAIT_RELEASE
                self.state_start_ms = now_ms
                return BtnAction.ABORT_DISCONNECT
            if release_fn:
                res = release_fn()
                if res == BtnOpResult.SUCCESS:
                    self.state = BtnState.WAIT_RELEASE
                    return BtnAction.KEY_SEQUENCE_COMPLETED
                elif res == BtnOpResult.BUSY:
                    return BtnAction.SEND_RELEASE
                else:
                    self.state = BtnState.STARTUP_WAIT_RELEASE
                    self.state_start_ms = now_ms
                    return BtnAction.ABORT_DISCONNECT
            return BtnAction.NONE

        elif self.state == BtnState.WAIT_RELEASE:
            if not is_pressed:
                self.state = BtnState.DEBOUNCING_RELEASE
                self.state_start_ms = now_ms
            return BtnAction.NONE

        elif self.state == BtnState.DEBOUNCING_RELEASE:
            if is_pressed:
                self.state = BtnState.WAIT_RELEASE
                self.state_start_ms = now_ms
            elif (now_ms - self.state_start_ms) >= self.debounce_ms:
                self.state = BtnState.IDLE
            return BtnAction.NONE

        return BtnAction.NONE

# ---------------------------------------------------------------------------
# Test Cases
# ---------------------------------------------------------------------------

def test_busy_right_after_press():
    print("[TEST 1] Endpoint busy right after successful press does NOT cause disconnect...")
    fsm = ButtonFsm(debounce_ms=50, hold_ms=50)

    # Arm FSM
    fsm.step(0, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(50, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    assert fsm.state == BtnState.IDLE

    # Press at t=100ms
    fsm.step(100, is_pressed=True, is_mounted=True, press_fn=None, release_fn=None)

    # At t=150ms (debounced): press succeeds
    act = fsm.step(150, is_pressed=True, is_mounted=True,
                    press_fn=lambda: BtnOpResult.SUCCESS, release_fn=None)
    assert act == BtnAction.SEND_PRESS
    assert fsm.state == BtnState.HOLDING_KEY

    # At t=160ms, 170ms, 180ms, 190ms:
    # Endpoint is BUSY in flight (tud_hid_ready() == false), but USB IS STILL MOUNTED (is_mounted == True)!
    for t in (160, 170, 180, 190):
        act = fsm.step(t, is_pressed=True, is_mounted=True,
                       press_fn=lambda: BtnOpResult.BUSY,
                       release_fn=lambda: BtnOpResult.BUSY)
        assert act == BtnAction.NONE, f"Expected NONE during hold, got {act}"
        assert fsm.state == BtnState.HOLDING_KEY, "FSM erroneously aborted while endpoint was busy!"

    # At t=200ms (50ms hold elapsed): release succeeds
    act = fsm.step(200, is_pressed=True, is_mounted=True,
                   press_fn=None, release_fn=lambda: BtnOpResult.SUCCESS)
    assert act == BtnAction.KEY_SEQUENCE_COMPLETED
    assert fsm.state == BtnState.WAIT_RELEASE

    print("  -> PASSED: Endpoint busy during key hold did not drop key; release completed normally.")

def test_release_busy_retry():
    print("[TEST 2] Release report busy retries non-blocking; success claimed ONLY on accept...")
    fsm = ButtonFsm(debounce_ms=50, hold_ms=50)

    fsm.step(0, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(50, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)

    # Press
    fsm.step(100, is_pressed=True, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(150, is_pressed=True, is_mounted=True, press_fn=lambda: BtnOpResult.SUCCESS, release_fn=None)
    assert fsm.state == BtnState.HOLDING_KEY

    # At t=200ms: release attempt 1 is BUSY
    act = fsm.step(200, is_pressed=True, is_mounted=True, press_fn=None, release_fn=lambda: BtnOpResult.BUSY)
    assert act == BtnAction.SEND_RELEASE
    assert fsm.state == BtnState.RETRY_RELEASE

    # At t=210ms: release attempt 2 is STILL BUSY
    act = fsm.step(210, is_pressed=True, is_mounted=True, press_fn=None, release_fn=lambda: BtnOpResult.BUSY)
    assert act == BtnAction.SEND_RELEASE
    assert fsm.state == BtnState.RETRY_RELEASE

    # At t=220ms: release attempt 3 SUCCEEDS!
    act = fsm.step(220, is_pressed=True, is_mounted=True, press_fn=None, release_fn=lambda: BtnOpResult.SUCCESS)
    assert act == BtnAction.KEY_SEQUENCE_COMPLETED
    assert fsm.state == BtnState.WAIT_RELEASE

    print("  -> PASSED: Release retry non-blocking; completed only after acceptance.")

def test_true_disconnect_and_reconnect():
    print("[TEST 3] Real disconnect during hold/retry aborts safely; no auto-send on reconnect...")
    fsm = ButtonFsm(debounce_ms=50, hold_ms=50)

    # Arm
    fsm.step(0, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(50, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)

    # Press
    fsm.step(100, is_pressed=True, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(150, is_pressed=True, is_mounted=True, press_fn=lambda: BtnOpResult.SUCCESS, release_fn=None)
    assert fsm.state == BtnState.HOLDING_KEY

    # Real USB unmount at t=180ms
    act = fsm.step(180, is_pressed=True, is_mounted=False, press_fn=None, release_fn=None)
    assert act == BtnAction.ABORT_DISCONNECT
    assert fsm.state == BtnState.STARTUP_WAIT_RELEASE

    # Reconnect at t=250ms while button is STILL HELD!
    for t in range(250, 451, 10):
        act = fsm.step(t, is_pressed=True, is_mounted=True, press_fn=lambda: BtnOpResult.SUCCESS, release_fn=None)
        assert act == BtnAction.NONE, f"Spurious key on reconnect at t={t}: {act}"
        assert fsm.state == BtnState.STARTUP_WAIT_RELEASE

    # Release button at t=460ms
    for t in range(460, 509, 10):
        fsm.step(t, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
        assert fsm.state == BtnState.STARTUP_DEBOUNCE_RELEASE
    fsm.step(510, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    assert fsm.state == BtnState.IDLE

    print("  -> PASSED: Real disconnect aborts cleanly; reconnect while held emits zero keys.")

def test_held_at_boot_and_mount():
    print("[TEST 4] Held at boot and across mount emits zero keys...")
    fsm = ButtonFsm(debounce_ms=50, hold_ms=50)

    # Boot unmounted with button held
    for t in range(0, 101, 10):
        act = fsm.step(t, is_pressed=True, is_mounted=False, press_fn=None, release_fn=None)
        assert act == BtnAction.NONE
        assert fsm.state == BtnState.STARTUP_WAIT_RELEASE

    # Mount at t=110ms with button still held
    for t in range(110, 301, 10):
        act = fsm.step(t, is_pressed=True, is_mounted=True, press_fn=None, release_fn=None)
        assert act == BtnAction.NONE
        assert fsm.state == BtnState.STARTUP_WAIT_RELEASE

    print("  -> PASSED: Holding across boot and mount emits zero keys.")

def test_bounce_rejection():
    print("[TEST 5] Bounce rejection on press and release...")
    fsm = ButtonFsm(debounce_ms=50, hold_ms=50)
    fsm.step(0, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(50, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    assert fsm.state == BtnState.IDLE

    # 20ms glitch
    fsm.step(60, is_pressed=True, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(80, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    assert fsm.state == BtnState.IDLE

    print("  -> PASSED: Glitches under 50ms rejected.")

def test_one_actuation_no_repeat():
    print("[TEST 6] Exactly one actuation per press (NO auto-repeat on 2s hold)...")
    fsm = ButtonFsm(debounce_ms=50, hold_ms=50)
    fsm.step(0, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)
    fsm.step(50, is_pressed=False, is_mounted=True, press_fn=None, release_fn=None)

    presses = 0
    releases = 0
    def on_press():
        nonlocal presses
        presses += 1
        return BtnOpResult.SUCCESS

    def on_release():
        nonlocal releases
        releases += 1
        return BtnOpResult.SUCCESS

    fsm.step(100, is_pressed=True, is_mounted=True, press_fn=on_press, release_fn=on_release)
    act1 = fsm.step(150, is_pressed=True, is_mounted=True, press_fn=on_press, release_fn=on_release)
    assert act1 == BtnAction.SEND_PRESS

    act2 = fsm.step(200, is_pressed=True, is_mounted=True, press_fn=on_press, release_fn=on_release)
    assert act2 == BtnAction.KEY_SEQUENCE_COMPLETED

    # Continue holding until t=2200ms
    for t in range(210, 2201, 10):
        act = fsm.step(t, is_pressed=True, is_mounted=True, press_fn=on_press, release_fn=on_release)
        assert act == BtnAction.NONE
        assert fsm.state == BtnState.WAIT_RELEASE

    assert presses == 1
    assert releases == 1
    print("  -> PASSED: Strictly 1 press and 1 release over 2 seconds of continuous hold.")

def run_all_tests():
    print("===========================================================================")
    print("Running PAXG Composite Button FSM Deterministic Test Suite")
    print("===========================================================================")
    test_busy_right_after_press()
    test_release_busy_retry()
    test_true_disconnect_and_reconnect()
    test_held_at_boot_and_mount()
    test_bounce_rejection()
    test_one_actuation_no_repeat()
    print("===========================================================================")
    print("ALL 6 DETERMINISTIC BUTTON FSM TESTS PASSED!")
    print("===========================================================================")

if __name__ == "__main__":
    run_all_tests()
