#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "live_fishing_controller.h"

using namespace paxg::fishing;

// Mock HID Keyboard implementation
class MockHidKeyboard : public IHidKeyboard {
public:
    bool mounted = true;
    bool ready = true;
    bool currentlyPressed = false;
    uint8_t lastModifier = 0;
    uint8_t lastKeycode = 0;
    uint32_t pressCount = 0;
    uint32_t releaseCount = 0;

    bool isMounted() override { return mounted; }
    bool isReady() override { return ready; }

    bool press(uint8_t modifier, uint8_t keycode) override {
        if (!mounted || !ready) return false;
        currentlyPressed = true;
        lastModifier = modifier;
        lastKeycode = keycode;
        pressCount++;
        return true;
    }

    bool release() override {
        if (!mounted) return false;
        if (!ready) return false;
        currentlyPressed = false;
        releaseCount++;
        return true;
    }

    void reset() {
        mounted = true;
        ready = true;
        currentlyPressed = false;
        lastModifier = 0;
        lastKeycode = 0;
        pressCount = 0;
        releaseCount = 0;
    }
};

// Mock Clock implementation
class MockClock : public IClock {
public:
    uint32_t currentTimeMs = 10000;

    uint32_t nowMs() override { return currentTimeMs; }
    void advance(uint32_t deltaMs) { currentTimeMs += deltaMs; }
    void reset(uint32_t t = 10000) { currentTimeMs = t; }
};

// Mock Audio Stream implementation
class MockAudioStream : public IAudioStream {
public:
    uint64_t packetsReceived = 0;
    uint64_t discontinuities = 0;
    uint32_t queueDrops = 0;
    bool triggered = false;
    float rms = 100.0f;

    uint64_t getPacketsReceived() const override { return packetsReceived; }
    uint64_t getDiscontinuities() const override { return discontinuities; }
    uint32_t getQueueDrops() const override { return queueDrops; }
    bool isTriggered() const override { return triggered; }
    void clearTrigger() override { triggered = false; }
    float getRms() const override { return rms; }

    void reset() {
        packetsReceived = 0;
        discontinuities = 0;
        queueDrops = 0;
        triggered = false;
        rms = 100.0f;
    }
};

// 1. Default Boot Unarmed Passive: no keys sent even with high audio / triggers
void test_default_boot_unarmed_passive() {
    printf("[RUN]  test_default_boot_unarmed_passive\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();

    controller.update(false, false);
    assert(!controller.isArmed());
    assert(controller.getState() == FishingState::Idle);

    audio.packetsReceived += 50;
    audio.triggered = true;
    audio.rms = 5000.0f;

    for (int i = 0; i < 100; ++i) {
        clock.advance(10);
        controller.update(false, false);
    }

    assert(hid.pressCount == 0);
    assert(hid.releaseCount == 0);
    assert(!controller.isArmed());
    assert(controller.getState() == FishingState::Idle);
    printf("  [PASS] test_default_boot_unarmed_passive\n");
}

// 2. Review Point 1: Positive Post-Arm Packet Progress & Freshness Required Before Start
void test_require_positive_packet_progress_and_freshness_before_start() {
    printf("[RUN]  test_require_positive_packet_progress_and_freshness_before_start\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    // Initial audio stream has ZERO packets ever received
    audio.packetsReceived = 0;

    // Arm controller
    assert(controller.requestArmToggle());
    assert(controller.isArmed());

    // Attempt start within timeout window (e.g. at 100ms) with ZERO packets received
    clock.advance(100);
    controller.update(false, false);
    bool startSuccess = controller.requestStart();

    // MUST FAIL: zero post-arm packets received!
    assert(!startSuccess);
    assert(controller.getState() == FishingState::Idle);
    assert(hid.pressCount == 0);
    assert(controller.getTotalCasts() == 0);

    // Now deliver positive packet progress
    audio.packetsReceived += 20;
    clock.advance(10);
    controller.update(false, false); // poll freshness

    // Now start succeeds
    startSuccess = controller.requestStart();
    assert(startSuccess);
    assert(controller.getState() == FishingState::Casting);
    assert(hid.pressCount == 1);
    assert(controller.getTotalCasts() == 1);

    printf("  [PASS] test_require_positive_packet_progress_and_freshness_before_start\n");
}

// 3. Review Point 2: Pending Release Retry on Stop/Disarm & Rearm Lockout
void test_release_retry_on_stop_disarm_and_rearm_lockout() {
    printf("[RUN]  test_release_retry_on_stop_disarm_and_rearm_lockout\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    assert(controller.requestStart());
    assert(controller.getKeyState() == KeyState::Pressed);
    assert(hid.currentlyPressed);

    // Make HID busy when user requests physical stop
    hid.ready = false;
    controller.requestStop(StopReason::UserStopped);

    // FSM stops, but key cannot complete release yet: MUST stay in KeyState::Releasing!
    assert(controller.getState() == FishingState::Idle);
    assert(controller.getKeyState() == KeyState::Releasing);
    assert(hid.releaseCount == 0);

    // While release is pending, user MUST be locked out from re-arming or starting!
    assert(!controller.requestArmToggle());
    assert(!controller.isArmed());
    assert(!controller.requestStart());

    // Tick multiple updates while still busy
    for (int i = 0; i < 5; ++i) {
        clock.advance(10);
        controller.update(false, false);
        assert(controller.getKeyState() == KeyState::Releasing);
        assert(hid.releaseCount == 0);
    }

    // Now endpoint becomes ready
    hid.ready = true;
    clock.advance(10);
    controller.update(false, false);

    // Release successfully accepted!
    assert(hid.releaseCount == 1);
    assert(controller.getKeyState() == KeyState::Idle);
    assert(!hid.currentlyPressed);

    // Now that key is idle, arming is permitted again
    assert(controller.requestArmToggle());
    assert(controller.isArmed());

    // Test unmount during busy release safely clearing local state
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);
    controller.requestStart();
    assert(controller.getKeyState() == KeyState::Pressed);

    hid.ready = false;
    controller.requestStop(StopReason::UserStopped);
    assert(controller.getKeyState() == KeyState::Releasing);

    // Host disconnects / unmounts
    hid.mounted = false;
    clock.advance(10);
    controller.update(false, false);
    // Unmount safely clears local key state
    assert(controller.getKeyState() == KeyState::Idle);

    printf("  [PASS] test_release_retry_on_stop_disarm_and_rearm_lockout\n");
}

// 4. Review Point 3: Explicit Accepted Key Press & Bounded Retry / Fail-Closed
void test_press_failure_bounded_retry_fail_closed() {
    printf("[RUN]  test_press_failure_bounded_retry_fail_closed\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;

    // Part A: Transient busy press succeeds within timeout
    {
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);

        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);

        // HID is not ready when cast is triggered
        hid.ready = false;
        assert(controller.requestStart());
        assert(controller.getKeyState() == KeyState::PendingPress);
        assert(controller.getTotalCasts() == 0); // Not counted yet!

        // 100ms elapses while still busy
        clock.advance(100);
        controller.update(false, false);
        assert(controller.getKeyState() == KeyState::PendingPress);
        assert(controller.getTotalCasts() == 0);

        // HID becomes ready -> press accepted
        hid.ready = true;
        clock.advance(10);
        controller.update(false, false);
        assert(controller.getKeyState() == KeyState::Pressed);
        assert(hid.pressCount == 1);
        assert(controller.getTotalCasts() == 1); // Now confirmed!
    }

    // Part B: Permanent busy press fails closed and disarms after 500ms
    {
        hid.reset();
        clock.reset();
        audio.reset();
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);

        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);

        hid.ready = false; // Permanently busy
        assert(controller.requestStart());
        assert(controller.getKeyState() == KeyState::PendingPress);
        assert(controller.getTotalCasts() == 0);

        // Advance 550ms (> PRESS_RETRY_TIMEOUT_MS 500ms)
        clock.advance(550);
        controller.update(false, false);

        // Must fail closed: stopped, disarmed, PressFailure, 0 casts claimed!
        assert(!controller.isArmed());
        assert(controller.getState() == FishingState::Idle);
        assert(controller.getLastStopReason() == StopReason::PressFailure);
        assert(controller.getTotalCasts() == 0);
        assert(controller.getKeyState() == KeyState::Idle);
    }

    printf("  [PASS] test_press_failure_bounded_retry_fail_closed\n");
}

// 5. Review Point 4: Disarm on Disconnect/Suspend Even While in Idle
void test_disarm_on_disconnect_in_idle_no_auto_rearm() {
    printf("[RUN]  test_disarm_on_disconnect_in_idle_no_auto_rearm\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    // Arm while in Idle (before any start)
    assert(controller.requestArmToggle());
    assert(controller.isArmed());
    assert(controller.getState() == FishingState::Idle);

    // Host disconnects / suspends while controller is in Idle
    hid.mounted = false;
    clock.advance(10);
    controller.update(false, false);

    // MUST immediately disarm!
    assert(!controller.isArmed());
    assert(controller.getLastStopReason() == StopReason::UsbUnmounted);

    // Host reconnects
    hid.mounted = true;
    clock.advance(100);
    controller.update(false, false);

    // MUST NOT auto-rearm upon reconnect!
    assert(!controller.isArmed());
    assert(controller.getState() == FishingState::Idle);

    printf("  [PASS] test_disarm_on_disconnect_in_idle_no_auto_rearm\n");
}

// 6. Review Point 5: Require Both Buttons Released After Arming Before Start
void test_require_both_buttons_released_after_arm() {
    printf("[RUN]  test_require_both_buttons_released_after_arm\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // User holds UP + DOWN for 1100ms to arm
    for (int i = 0; i < 110; ++i) {
        clock.advance(10);
        audio.packetsReceived += 1;
        controller.update(true, true);
    }
    assert(controller.isArmed());
    assert(controller.getState() == FishingState::Idle);

    // User releases DOWN, but KEEPS HOLDING UP for 200ms
    for (int i = 0; i < 20; ++i) {
        clock.advance(10);
        audio.packetsReceived += 1;
        controller.update(true, false);
    }

    // Controller MUST NOT start fishing! Both buttons must be observed released first.
    assert(controller.getState() == FishingState::Idle);
    assert(hid.pressCount == 0);

    // User releases UP (now both are released)
    clock.advance(50);
    controller.update(false, false);
    assert(controller.getState() == FishingState::Idle);

    // Now user performs a separate, deliberate press on Button UP
    for (int i = 0; i < 6; ++i) {
        clock.advance(10);
        audio.packetsReceived += 1;
        controller.update(true, false);
    }

    // NOW start triggers cleanly!
    assert(controller.getState() == FishingState::Casting);
    assert(hid.pressCount == 1);

    printf("  [PASS] test_require_both_buttons_released_after_arm\n");
}

// 7. Timeout at 30s From Cast Press Enters Timeout State, Then Cooldown, and Recasts
void test_timeout_at_30s_enters_timeout_then_cooldown_and_recasts() {
    printf("[RUN]  test_timeout_at_30s_enters_timeout_then_cooldown_and_recasts\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Cast key pressed and accepted at now = 10010ms
    assert(controller.requestStart());
    assert(controller.getState() == FishingState::Casting);
    assert(controller.getCastAcceptedTime() == 10010UL);

    // Complete cast key hold (80ms) and animation (1500ms)
    clock.advance(80);
    controller.update(false, false); // release cast key
    clock.advance(1500);
    audio.packetsReceived += 75;
    controller.update(false, false); // enters Waiting at now = 11590ms (elapsed from cast = 1580ms)
    assert(controller.getState() == FishingState::Waiting);

    // Remaining in Waiting until 30s cast deadline: 30000 - 1580 = 28420ms
    // 1. Advance 20s: MUST NOT time out!
    for (int i = 0; i < 20; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
        assert(controller.isArmed());
        assert(controller.getState() == FishingState::Waiting);
    }

    // 2. Advance another 8s (total 28s from cast, 26.42s in Waiting): MUST NOT time out!
    for (int i = 0; i < 8; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
        assert(controller.isArmed());
        assert(controller.getState() == FishingState::Waiting);
    }

    // 3. Advance to 29,999ms from accepted cast (419ms more): still 1ms remaining!
    clock.advance(419);
    audio.packetsReceived += 10;
    controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getWaitRemainingMs() == 1UL);

    // 4. Advance final 1ms (exactly 30,000ms from accepted cast): NOW transitions to Timeout state
    clock.advance(1);
    audio.packetsReceived += 10;
    controller.update(false, false);

    // Enters Timeout: remains ARMED, increments timeout counter exactly once
    assert(controller.isArmed());
    assert(controller.getState() == FishingState::Timeout);
    assert(controller.getTotalTimeouts() == 1);
    assert(controller.getWaitRemainingMs() == 0);
    // Crucial safety check: ZERO keys sent before deadline + cooldown!
    assert(hid.pressCount == 1); // Only the initial cast key was ever sent
    assert(hid.releaseCount == 1);

    // 5. Non-blocking timeout indication presentation delay (1500ms)
    clock.advance(1500);
    audio.packetsReceived += 75;
    controller.update(false, false);

    // Transitions to Cooldown: still armed, still NO new keys sent!
    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());
    assert(hid.pressCount == 1);

    // 6. Cooldown (2000ms >= 2s safety margin) -> Automatic recast without manual re-arming!
    clock.advance(2000);
    audio.packetsReceived += 100;
    controller.update(false, false);

    // Cast #2 dispatched!
    assert(controller.getState() == FishingState::Casting);
    assert(controller.isArmed());
    assert(hid.pressCount == 2);
    assert(controller.getTotalCasts() == 2);

    printf("  [PASS] test_timeout_at_30s_enters_timeout_then_cooldown_and_recasts\n");
}

// 8. Legitimate Bite at 29s From Cast Triggers Reel and Completes Cycle Cleanly
void test_late_bite_at_29s_in_waiting_triggers_reel_and_completes_cycle() {
    printf("[RUN]  test_late_bite_at_29s_in_waiting_triggers_reel_and_completes_cycle\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Start fishing (Cast spacebar #1 accepted at now = 10010ms)
    assert(controller.requestStart());
    assert(hid.pressCount == 1);
    assert(controller.getCastAcceptedTime() == 10010UL);

    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Advance to 29.0s from accepted cast (elapsed = 29,000ms < 30,000ms)
    // 10010 + 29000 = 39010ms. Current now is 11590ms. Delta = 27420ms.
    for (int i = 0; i < 27; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
        assert(controller.isArmed());
        assert(controller.getState() == FishingState::Waiting);
    }
    clock.advance(420);
    audio.packetsReceived += 20;
    controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getWaitRemainingMs() == 1000UL); // 1000ms remaining until 30s deadline

    // Legitimate bite arrives at 29s!
    audio.triggered = true;
    audio.packetsReceived += 5;
    clock.advance(10);
    controller.update(false, false);

    assert(controller.getState() == FishingState::BiteDetected);
    assert(controller.getTotalBites() == 1);

    // Humanized presentation pause (300ms) -> now is 39320ms (< 40010ms deadline!) -> Reeling
    clock.advance(300);
    audio.packetsReceived += 15;
    controller.update(false, false);
    assert(controller.getState() == FishingState::Reeling);
    assert(hid.pressCount == 2); // Space reel pressed!
    assert(controller.getTotalReels() == 1);

    // 80ms hold -> release reel key
    clock.advance(80);
    controller.update(false, false);
    assert(hid.releaseCount == 2);
    assert(controller.getKeyState() == KeyState::Idle);

    // 1200ms reel duration expires -> transitions to Cooldown in continuous mode
    clock.advance(1200);
    audio.packetsReceived += 60;
    controller.update(false, false);

    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());
    assert(controller.getWaitRemainingMs() == 0);
    assert(hid.pressCount == 2);
    assert(hid.releaseCount == 2);

    // After 2000ms cooldown, next cast (press #3) is dispatched automatically without manual rearm
    clock.advance(2000);
    audio.packetsReceived += 100;
    controller.update(false, false);

    assert(controller.getState() == FishingState::Casting);
    assert(controller.isArmed());
    assert(hid.pressCount == 3);
    assert(controller.getTotalCasts() == 2);

    printf("  [PASS] test_late_bite_at_29s_in_waiting_triggers_reel_and_completes_cycle\n");
}

// 9. Countdown and Time Remaining Reporting (Zero After State Stopped)
void test_countdown_and_time_remaining_reporting() {
    printf("[RUN]  test_countdown_and_time_remaining_reporting\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();

    // In Idle: must report 0
    assert(controller.getWaitRemainingMs() == 0);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Still Idle: must report 0
    assert(controller.getWaitRemainingMs() == 0);

    // Start fishing (in Casting): must report 0
    assert(controller.requestStart());
    assert(controller.getState() == FishingState::Casting);
    assert(controller.getWaitRemainingMs() == 0);

    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

    // In Waiting: reports remaining time from accepted cast press (30000 - 1580 = 28420ms)
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getWaitRemainingMs() == 28420UL);

    // Advance 10s -> 18420ms
    clock.advance(10000);
    audio.packetsReceived += 50;
    controller.update(false, false);
    assert(controller.getWaitRemainingMs() == 18420UL);

    // User stops early via physical DOWN: must drop to 0 immediately
    controller.requestStop(StopReason::UserStopped);
    assert(controller.getState() == FishingState::Idle);
    assert(controller.getWaitRemainingMs() == 0);

    printf("  [PASS] test_countdown_and_time_remaining_reporting\n");
}

// 10. Continuous Multi-Cycle Operation: 3 Consecutive Cycles Without Manual Rearm
void test_continuous_three_consecutive_cycles_without_rearm() {
    printf("[RUN]  test_continuous_three_consecutive_cycles_without_rearm\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    // Arm controller
    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);
    assert(controller.isArmed());

    // User short-presses UP to start continuous fishing
    assert(controller.requestStart());

    // Cycle 1:
    // Cast 1 (press #1)
    assert(controller.getState() == FishingState::Casting);
    assert(hid.pressCount == 1);
    assert(controller.getTotalCasts() == 1);
    clock.advance(80); controller.update(false, false); // release #1
    assert(hid.releaseCount == 1);
    clock.advance(1500); audio.packetsReceived += 70; controller.update(false, false); // -> Waiting
    assert(controller.getState() == FishingState::Waiting);

    // Bite 1
    audio.triggered = true;
    audio.packetsReceived += 5;
    clock.advance(10); controller.update(false, false); // -> BiteDetected
    assert(controller.getState() == FishingState::BiteDetected);
    assert(controller.getTotalBites() == 1);

    // Reel 1 (press #2)
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false); // -> Reeling
    assert(controller.getState() == FishingState::Reeling);
    assert(hid.pressCount == 2);
    assert(controller.getTotalReels() == 1);
    clock.advance(80); controller.update(false, false); // release #2
    assert(hid.releaseCount == 2);
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false); // -> Cooldown
    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());

    // Cooldown 2.0s -> Auto-recast without manual re-arm!
    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false); // -> Casting 2

    // Cycle 2:
    // Cast 2 (press #3)
    assert(controller.getState() == FishingState::Casting);
    assert(controller.isArmed());
    assert(hid.pressCount == 3);
    assert(controller.getTotalCasts() == 2);
    clock.advance(80); controller.update(false, false); // release #3
    assert(hid.releaseCount == 3);
    clock.advance(1500); audio.packetsReceived += 70; controller.update(false, false); // -> Waiting
    assert(controller.getState() == FishingState::Waiting);

    // Bite 2
    audio.triggered = true;
    audio.packetsReceived += 5;
    clock.advance(10); controller.update(false, false); // -> BiteDetected
    assert(controller.getState() == FishingState::BiteDetected);
    assert(controller.getTotalBites() == 2);

    // Reel 2 (press #4)
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false); // -> Reeling
    assert(controller.getState() == FishingState::Reeling);
    assert(hid.pressCount == 4);
    assert(controller.getTotalReels() == 2);
    clock.advance(80); controller.update(false, false); // release #4
    assert(hid.releaseCount == 4);
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false); // -> Cooldown
    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());

    // Cooldown 2.0s -> Auto-recast without manual re-arm!
    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false); // -> Casting 3

    // Cycle 3:
    // Cast 3 (press #5)
    assert(controller.getState() == FishingState::Casting);
    assert(controller.isArmed());
    assert(hid.pressCount == 5);
    assert(controller.getTotalCasts() == 3);
    clock.advance(80); controller.update(false, false); // release #5
    assert(hid.releaseCount == 5);
    clock.advance(1500); audio.packetsReceived += 70; controller.update(false, false); // -> Waiting
    assert(controller.getState() == FishingState::Waiting);

    // Bite 3
    audio.triggered = true;
    audio.packetsReceived += 5;
    clock.advance(10); controller.update(false, false); // -> BiteDetected
    assert(controller.getState() == FishingState::BiteDetected);
    assert(controller.getTotalBites() == 3);

    // Reel 3 (press #6)
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false); // -> Reeling
    assert(controller.getState() == FishingState::Reeling);
    assert(hid.pressCount == 6);
    assert(controller.getTotalReels() == 3);
    clock.advance(80); controller.update(false, false); // release #6
    assert(hid.releaseCount == 6);
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false); // -> Cooldown
    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());

    // All 3 cycles succeeded continuously without manual rearming
    assert(hid.pressCount == 6);
    assert(hid.releaseCount == 6);
    assert(controller.getTotalCasts() == 3);
    assert(controller.getTotalBites() == 3);
    assert(controller.getTotalReels() == 3);
    assert(controller.isArmed());

    printf("  [PASS] test_continuous_three_consecutive_cycles_without_rearm\n");
}

// 11. Continuous Busy Release on Reel Defers Cooldown Until Release Accepted
void test_continuous_busy_release_on_reel_defers_cooldown_until_released() {
    printf("[RUN]  test_continuous_busy_release_on_reel_defers_cooldown_until_released\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    controller.requestStart();
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    audio.triggered = true;
    audio.packetsReceived += 5;
    clock.advance(10);
    controller.update(false, false);
    clock.advance(300);
    audio.packetsReceived += 15;
    controller.update(false, false);
    assert(controller.getState() == FishingState::Reeling);
    assert(hid.pressCount == 2);

    // When reel key hold duration expires, simulate busy HID endpoint on release
    clock.advance(80);
    hid.ready = false;
    controller.update(false, false);
    // Key cannot complete release yet; MUST be in Releasing
    assert(controller.getKeyState() == KeyState::Releasing);
    assert(hid.releaseCount == 1); // Only cast was released, reel release pending

    // 1200ms reel duration expires while key release is still pending
    clock.advance(1200);
    audio.packetsReceived += 60;
    controller.update(false, false);

    // MUST NOT transition to Cooldown while key release is pending!
    assert(controller.getState() == FishingState::Reeling);
    assert(controller.isArmed());
    assert(controller.getKeyState() == KeyState::Releasing);

    // Tick multiple updates while still busy
    for (int i = 0; i < 5; ++i) {
        clock.advance(10);
        controller.update(false, false);
        assert(controller.getState() == FishingState::Reeling);
        assert(controller.isArmed());
        assert(controller.getKeyState() == KeyState::Releasing);
    }

    // Now HID endpoint becomes ready and accepts release
    hid.ready = true;
    clock.advance(10);
    controller.update(false, false);

    // Release confirmed! Now transitions to Cooldown
    assert(hid.releaseCount == 2);
    assert(controller.getKeyState() == KeyState::Idle);
    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());

    printf("  [PASS] test_continuous_busy_release_on_reel_defers_cooldown_until_released\n");
}

// 12. Stale Trigger Purge During Casting and Cooldown
void test_stale_trigger_purge_during_casting_and_cooldown() {
    printf("[RUN]  test_stale_trigger_purge_during_casting_and_cooldown\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Acoustic noise before start is purged upon start
    audio.triggered = true;
    assert(controller.requestStart());
    assert(!audio.triggered); // Purged by requestStart
    assert(controller.getState() == FishingState::Casting);

    clock.advance(80); controller.update(false, false);

    // Casting acoustic splash transients occur during Casting phase
    audio.triggered = true;
    clock.advance(1500); audio.packetsReceived += 50;
    controller.update(false, false); // -> enters Waiting

    // Purged at boundary! Must not trigger premature bite
    assert(!audio.triggered);
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getTotalBites() == 0);

    // Now genuine bite occurs in Waiting
    audio.triggered = true;
    clock.advance(10); controller.update(false, false);
    assert(controller.getState() == FishingState::BiteDetected);
    assert(controller.getTotalBites() == 1);

    // Reeling
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false);
    clock.advance(80); controller.update(false, false);
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);

    // Transient noise during Cooldown
    audio.triggered = true;
    clock.advance(2000); audio.packetsReceived += 100;
    controller.update(false, false); // -> enters Casting

    // Purged at CooldownComplete boundary!
    assert(!audio.triggered);
    assert(controller.getState() == FishingState::Casting);
    assert(controller.getTotalCasts() == 2);

    printf("  [PASS] test_stale_trigger_purge_during_casting_and_cooldown\n");
}

// 13. Cooldown Audio Dropout Fails Closed
void test_cooldown_dropout_fails_closed() {
    printf("[RUN]  test_cooldown_dropout_fails_closed\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    controller.requestStart();
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

    // Bite and Reel
    audio.triggered = true;
    clock.advance(10); controller.update(false, false);
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false);
    clock.advance(80); controller.update(false, false);
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);

    // Audio stream freezes during Cooldown (no new packets for >1s)
    clock.advance(1100);
    controller.update(false, false);

    // Must fail closed, disarm, and cancel recast
    assert(!controller.isArmed());
    assert(controller.getState() == FishingState::Idle);
    assert(controller.getLastStopReason() == StopReason::AudioDropout);
    assert(hid.pressCount == 2); // No 3rd press

    printf("  [PASS] test_cooldown_dropout_fails_closed\n");
}

// 14. Cooldown Interrupted by Physical Button DOWN Stops and Disarms
void test_cooldown_interrupted_by_user_stop() {
    printf("[RUN]  test_cooldown_interrupted_by_user_stop\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    controller.requestStart();
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

    // Bite and Reel
    audio.triggered = true;
    clock.advance(10); controller.update(false, false);
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false);
    clock.advance(80); controller.update(false, false);
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());

    // User presses DOWN during Cooldown
    controller.update(false, true);
    clock.advance(50);
    controller.update(false, true);

    assert(!controller.isArmed());
    assert(controller.getState() == FishingState::Idle);
    assert(controller.getLastStopReason() == StopReason::UserStopped);

    // Even if audio continues, controller does not recast
    clock.advance(3000);
    audio.packetsReceived += 100;
    controller.update(false, false);
    assert(controller.getState() == FishingState::Idle);
    assert(hid.pressCount == 2);

    printf("  [PASS] test_cooldown_interrupted_by_user_stop\n");
}

// 15. Bite Timeout in Continuous Mode Recasts After Cooldown
void test_timeout_in_continuous_mode_recasts_after_cooldown() {
    printf("[RUN]  test_timeout_in_continuous_mode_recasts_after_cooldown\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    controller.requestStart();
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Cycle 1 Bite and Reel
    audio.triggered = true;
    clock.advance(10); controller.update(false, false);
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false);
    clock.advance(80); controller.update(false, false);
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);

    // Auto-recast to Cycle 2
    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Casting);
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getTotalCasts() == 2);

    // In Cycle 2: No bite occurs for full 30000ms cast deadline
    for (int i = 0; i < 29; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
    }

    // Transitions to Timeout state, remains armed, NO keys sent
    assert(controller.isArmed());
    assert(controller.getState() == FishingState::Timeout);
    assert(controller.getTotalTimeouts() == 1);
    assert(controller.getWaitRemainingMs() == 0);
    assert(hid.pressCount == 3); // Cast 1, Reel 1, Cast 2 (no extra keys)

    // Timeout duration 1500ms -> Cooldown (2000ms) -> Cast 3
    clock.advance(1500); audio.packetsReceived += 75; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);
    assert(controller.isArmed());
    assert(hid.pressCount == 3);

    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Casting);
    assert(controller.isArmed());
    assert(hid.pressCount == 4); // Cast 3 sent
    assert(controller.getTotalCasts() == 3);

    printf("  [PASS] test_timeout_in_continuous_mode_recasts_after_cooldown\n");
}

// 16. Consecutive No-Bite Timeouts Recast Continuously
void test_consecutive_no_bite_timeouts_recast_continously() {
    printf("[RUN]  test_consecutive_no_bite_timeouts_recast_continously\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    assert(controller.requestStart());
    assert(controller.getTotalCasts() == 1);
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

    // Run 3 consecutive no-bite timeout cycles
    for (int cycle = 0; cycle < 3; ++cycle) {
        assert(controller.getState() == FishingState::Waiting);
        assert(controller.isArmed());

        // Wait past 30s cast deadline (e.g. 29s in Waiting crosses 30s from accepted cast)
        for (int i = 0; i < 29; ++i) {
            clock.advance(1000);
            audio.packetsReceived += 50;
            controller.update(false, false);
        }

        assert(controller.getState() == FishingState::Timeout);
        assert(controller.getTotalTimeouts() == static_cast<uint32_t>(cycle + 1));
        assert(controller.isArmed());

        // Timeout display delay 1500ms
        clock.advance(1500);
        audio.packetsReceived += 75;
        controller.update(false, false);
        assert(controller.getState() == FishingState::Cooldown);
        assert(controller.isArmed());

        // Cooldown 2000ms -> Recasts
        clock.advance(2000);
        audio.packetsReceived += 100;
        controller.update(false, false);
        assert(controller.getState() == FishingState::Casting);
        assert(controller.isArmed());
        assert(controller.getTotalCasts() == static_cast<uint32_t>(cycle + 2));

        // Advance past cast key hold and animation into Waiting for next cycle
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    }

    assert(controller.getTotalTimeouts() == 3);
    assert(controller.getTotalCasts() == 4);
    assert(controller.getTotalBites() == 0);
    assert(controller.getTotalReels() == 0);
    assert(hid.pressCount == 4);
    assert(hid.releaseCount == 4);
    assert(controller.isArmed());

    printf("  [PASS] test_consecutive_no_bite_timeouts_recast_continously\n");
}

// 17. Bite Just Before Deadline vs At or After Deadline
void test_bite_just_before_deadline_vs_at_or_after_deadline() {
    printf("[RUN]  test_bite_just_before_deadline_vs_at_or_after_deadline\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;

    // Part A: Legitimate bite at 29 seconds from cast (elapsed = 29000ms < 30000ms) -> triggers reel!
    {
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        // Cast accepted at 10010ms
        assert(controller.getCastAcceptedTime() == 10010UL);
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
        assert(controller.getState() == FishingState::Waiting);

        // Advance to 29.0s from accepted cast (now = 39010ms)
        for (int i = 0; i < 27; ++i) {
            clock.advance(1000);
            audio.packetsReceived += 50;
            controller.update(false, false);
        }
        clock.advance(420);
        audio.packetsReceived += 50;
        controller.update(false, false);
        assert(controller.getState() == FishingState::Waiting);

        // Acoustic bite trigger arrives at 29s
        audio.triggered = true;
        clock.advance(5);
        controller.update(false, false);

        // Valid bite detected before deadline!
        assert(controller.getState() == FishingState::BiteDetected);
        assert(controller.getTotalBites() == 1);
        assert(controller.getTotalTimeouts() == 0);

        // Reel key dispatched at 29.3s (< 30s deadline)
        clock.advance(300); audio.packetsReceived += 15; controller.update(false, false);
        assert(controller.getState() == FishingState::Reeling);
        assert(hid.pressCount == 2); // Reel key sent
    }

    // Part B: Trigger at or after 30s deadline (at 30,000ms from cast) -> Timeout takes strict priority, NO reel!
    {
        hid.reset();
        clock.reset();
        audio.reset();
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        // Cast accepted at 10010ms
        assert(controller.getCastAcceptedTime() == 10010UL);
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
        assert(controller.getState() == FishingState::Waiting);

        // Advance to 28,419ms in Waiting (now = 39999ms, elapsed from cast = 29999ms)
        for (int i = 0; i < 28; ++i) {
            clock.advance(1000);
            audio.packetsReceived += 50;
            controller.update(false, false);
        }
        clock.advance(419);
        audio.packetsReceived += 20;
        controller.update(false, false);
        assert(controller.getState() == FishingState::Waiting);

        // Advance 1ms to hit exactly 30,000ms from accepted cast, with audio trigger arriving simultaneously
        clock.advance(1);
        audio.packetsReceived += 20;
        audio.triggered = true;
        controller.update(false, false);

        // Timeout takes priority! Enters Timeout, NOT BiteDetected
        assert(controller.getState() == FishingState::Timeout);
        assert(controller.getTotalTimeouts() == 1);
        assert(controller.getTotalBites() == 0);
        // Stale trigger was cleared by priority deadline handler
        assert(!audio.isTriggered());
        // NO reel key was sent (pressCount remains 1 for initial cast)
        assert(hid.pressCount == 1);
    }

    printf("  [PASS] test_bite_just_before_deadline_vs_at_or_after_deadline\n");
}

// 18. No Stale Audio Trigger Carryover Across Timeout
void test_no_stale_audio_trigger_carryover_across_timeout() {
    printf("[RUN]  test_no_stale_audio_trigger_carryover_across_timeout\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);
    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);
    controller.requestStart();
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

    // Timeout occurs at 30s cast deadline
    for (int i = 0; i < 29; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
    }
    assert(controller.getState() == FishingState::Timeout);

    // Acoustic noise occurs during Timeout presentation
    audio.triggered = true;
    clock.advance(1500);
    audio.packetsReceived += 75;
    controller.update(false, false);

    // Purged at TimeoutComplete boundary!
    assert(!audio.isTriggered());
    assert(controller.getState() == FishingState::Cooldown);

    // Transient noise occurs during Cooldown
    audio.triggered = true;
    clock.advance(2000);
    audio.packetsReceived += 100;
    controller.update(false, false);

    // Purged at CooldownComplete boundary!
    assert(!audio.isTriggered());
    assert(controller.getState() == FishingState::Casting);

    // Transients during casting
    clock.advance(80); controller.update(false, false);
    audio.triggered = true;
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

    // Purged entering Waiting!
    assert(!audio.isTriggered());
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getTotalBites() == 0); // Zero premature bites

    printf("  [PASS] test_no_stale_audio_trigger_carryover_across_timeout\n");
}

// 19. User Stop in Timeout and Cooldown
void test_stop_in_timeout_and_cooldown() {
    printf("[RUN]  test_stop_in_timeout_and_cooldown\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;

    // Part A: Stop during Timeout
    {
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

        for (int i = 0; i < 29; ++i) {
            clock.advance(1000);
            audio.packetsReceived += 50;
            controller.update(false, false);
        }
        assert(controller.getState() == FishingState::Timeout);

        // User presses DOWN during Timeout
        controller.update(false, true);
        clock.advance(50);
        controller.update(false, true);

        assert(!controller.isArmed());
        assert(controller.getState() == FishingState::Idle);
        assert(controller.getLastStopReason() == StopReason::UserStopped);

        // Continued audio/time does not trigger recast
        clock.advance(5000);
        audio.packetsReceived += 200;
        controller.update(false, false);
        assert(controller.getState() == FishingState::Idle);
        assert(hid.pressCount == 1);
    }

    // Part B: Stop during Cooldown
    {
        hid.reset();
        clock.reset();
        audio.reset();
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

        for (int i = 0; i < 29; ++i) {
            clock.advance(1000);
            audio.packetsReceived += 50;
            controller.update(false, false);
        }
        clock.advance(1500); audio.packetsReceived += 75; controller.update(false, false);
        assert(controller.getState() == FishingState::Cooldown);

        // User presses DOWN during Cooldown
        controller.update(false, true);
        clock.advance(50);
        controller.update(false, true);

        assert(!controller.isArmed());
        assert(controller.getState() == FishingState::Idle);
        assert(controller.getLastStopReason() == StopReason::UserStopped);

        clock.advance(5000);
        audio.packetsReceived += 200;
        controller.update(false, false);
        assert(controller.getState() == FishingState::Idle);
        assert(hid.pressCount == 1);
    }

    printf("  [PASS] test_stop_in_timeout_and_cooldown\n");
}

// 20. Disconnect and Stream Stall in Timeout and Cooldown
void test_disconnect_and_stream_stall_in_timeout_and_cooldown() {
    printf("[RUN]  test_disconnect_and_stream_stall_in_timeout_and_cooldown\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;

    // Disconnect during Timeout
    {
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
        for (int i = 0; i < 29; ++i) {
            clock.advance(1000);
            audio.packetsReceived += 50;
            controller.update(false, false);
        }
        assert(controller.getState() == FishingState::Timeout);

        hid.mounted = false;
        controller.update(false, false);

        assert(!controller.isArmed());
        assert(controller.getState() == FishingState::Idle);
        assert(controller.getLastStopReason() == StopReason::UsbUnmounted);
    }

    // Audio stream stall during Timeout
    {
        hid.reset();
        clock.reset();
        audio.reset();
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
        for (int i = 0; i < 29; ++i) {
            clock.advance(1000);
            audio.packetsReceived += 50;
            controller.update(false, false);
        }
        assert(controller.getState() == FishingState::Timeout);

        // Stream freezes for >1000ms
        clock.advance(1100);
        controller.update(false, false);

        assert(!controller.isArmed());
        assert(controller.getState() == FishingState::Idle);
        assert(controller.getLastStopReason() == StopReason::AudioDropout);
    }

    printf("  [PASS] test_disconnect_and_stream_stall_in_timeout_and_cooldown\n");
}

// 21. Cooldown Release Busy Retry and No Key Duplication
void test_cooldown_release_busy_retry_no_key_duplication() {
    printf("[RUN]  test_cooldown_release_busy_retry_no_key_duplication\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);
    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);
    controller.requestStart();
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);

    // Bite and Reel
    audio.triggered = true;
    clock.advance(10); controller.update(false, false);
    clock.advance(300); audio.packetsReceived += 15; controller.update(false, false);
    assert(controller.getState() == FishingState::Reeling);
    assert(hid.pressCount == 2);

    // Hold expires, but release fails/is busy
    clock.advance(80);
    hid.ready = false;
    controller.update(false, false);
    assert(controller.getKeyState() == KeyState::Releasing);

    // Reel animation completes, but release still busy -> stays in Reeling
    clock.advance(1200); audio.packetsReceived += 60; controller.update(false, false);
    assert(controller.getState() == FishingState::Reeling);

    // Release completes
    hid.ready = true;
    clock.advance(10); controller.update(false, false);
    assert(controller.getKeyState() == KeyState::Idle);
    assert(controller.getState() == FishingState::Cooldown);

    // Advance 2000ms in Cooldown -> Recasts exactly once (no key duplication)
    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Casting);
    assert(hid.pressCount == 3); // Cast 1, Reel 1, Cast 2 (no duplicates!)

    printf("  [PASS] test_cooldown_release_busy_retry_no_key_duplication\n");
}

// 22. Wrap-Safe Arithmetic Across 32-Bit Overflow
void test_uint32_clock_wrap_safe_during_waiting_and_timeout() {
    printf("[RUN]  test_uint32_clock_wrap_safe_during_waiting_and_timeout\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;

    // Start clock 5000ms before 32-bit uint32 max (0xFFFFFFFF = 4294967295)
    clock.reset(0xFFFFF000); // 4294963200
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);
    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    assert(controller.requestStart());
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Clock wraps around 0xFFFFFFFF during Waiting!
    for (int i = 0; i < 29; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
    }

    // Wrap-safe unsigned arithmetic triggers Timeout cleanly
    assert(controller.getState() == FishingState::Timeout);
    assert(controller.getTotalTimeouts() == 1);
    assert(controller.isArmed());

    // Advance past Timeout (1500ms)
    clock.advance(1500); audio.packetsReceived += 75; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);

    // Advance past Cooldown (2000ms)
    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Casting);
    assert(controller.getTotalCasts() == 2);
    assert(controller.isArmed());

    printf("  [PASS] test_uint32_clock_wrap_safe_during_waiting_and_timeout\n");
}

// 23. Stop Priority Across All Phases
void test_stop_at_all_phases_and_priority() {
    printf("[RUN]  test_stop_at_all_phases_and_priority\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;

    // Test stop while in Casting with key held
    {
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        assert(controller.getState() == FishingState::Casting);
        assert(controller.getKeyState() == KeyState::Pressed);

        // Physical Stop via button DOWN
        controller.update(false, true);
        clock.advance(50);
        controller.update(false, true);

        assert(!controller.isArmed());
        assert(controller.getState() == FishingState::Idle);
        assert(controller.getKeyState() == KeyState::Idle);
        assert(hid.releaseCount == 1);
    }

    // Test stop while in Waiting
    {
        hid.reset();
        clock.reset();
        audio.reset();
        LiveFishingController controller(&hid, &clock, &audio);
        controller.init();
        controller.update(false, false);
        audio.packetsReceived += 10;
        controller.requestArmToggle();
        audio.packetsReceived += 10;
        clock.advance(10);
        controller.update(false, false);
        controller.requestStart();
        clock.advance(80); controller.update(false, false);
        clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
        assert(controller.getState() == FishingState::Waiting);

        controller.requestStop(StopReason::UserStopped);
        assert(controller.getState() == FishingState::Idle);
    }

    printf("  [PASS] test_stop_at_all_phases_and_priority\n");
}

// 24. Audio Stream Dropout & Queue Fail-Closed
void test_audio_dropout_and_queue_fail_closed() {
    printf("[RUN]  test_audio_dropout_and_queue_fail_closed\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);
    controller.requestStart();

    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Audio packets freeze
    clock.advance(1100);
    controller.update(false, false);

    assert(!controller.isArmed());
    assert(controller.getState() == FishingState::Idle);
    assert(controller.getLastStopReason() == StopReason::AudioDropout);

    printf("  [PASS] test_audio_dropout_and_queue_fail_closed\n");
}

// 25. Cast Accepted After Delayed Readiness Tracks Actual Monotonic Acceptance
void test_cast_accepted_after_delayed_readiness() {
    printf("[RUN]  test_cast_accepted_after_delayed_readiness\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Endpoint is busy when start is initiated
    hid.ready = false;
    assert(controller.requestStart());
    assert(controller.getState() == FishingState::Casting);
    assert(controller.getKeyState() == KeyState::PendingPress);
    // Acceptance timestamp not yet recorded while pending
    assert(controller.getCastAcceptedTime() == 0);

    // 250ms later, HID becomes ready
    clock.advance(250); // now = 10260ms
    hid.ready = true;
    controller.update(false, false);

    // Accepted! Monotonic timestamp tracked strictly at actual acceptance
    assert(controller.getKeyState() == KeyState::Pressed);
    assert(controller.getCastAcceptedTime() == 10260UL);
    assert(hid.pressCount == 1);

    // Release key
    clock.advance(80); controller.update(false, false);
    assert(controller.getKeyState() == KeyState::Idle);

    // Complete Casting presentation
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Deadline is strictly 30000ms from 10260ms = 40260ms
    // Current now is 10260 + 80 + 1500 = 11840ms. Remaining = 40260 - 11840 = 28420ms.
    assert(controller.getWaitRemainingMs() == 28420UL);

    // Advance 28419ms (1ms before deadline): still in Waiting!
    clock.advance(28419); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getWaitRemainingMs() == 1UL);

    // Advance 1ms (hits exactly 30000ms from cast press acceptance): transitions to Timeout!
    clock.advance(1); audio.packetsReceived += 10; controller.update(false, false);
    assert(controller.getState() == FishingState::Timeout);
    assert(controller.getTotalTimeouts() == 1);

    printf("  [PASS] test_cast_accepted_after_delayed_readiness\n");
}

// 26. No Recast Before >= 30000ms Plus Safety Margin
void test_no_recast_before_30000ms_plus_safety_margin() {
    printf("[RUN]  test_no_recast_before_30000ms_plus_safety_margin\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Cast accepted at now = 10010ms
    assert(controller.requestStart());
    assert(controller.getCastAcceptedTime() == 10010UL);
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Advance to 29999ms from accepted cast (now = 40009ms): still in Waiting, 1 press
    clock.advance(28419); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);
    assert(hid.pressCount == 1);

    // At 30000ms from cast (now = 40010ms): enters Timeout. Still 1 press
    clock.advance(1); audio.packetsReceived += 10; controller.update(false, false);
    assert(controller.getState() == FishingState::Timeout);
    assert(hid.pressCount == 1);

    // Advance 1499ms in Timeout (now = 41509ms): still in Timeout, still 1 press
    clock.advance(1499); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Timeout);
    assert(hid.pressCount == 1);

    // At 1500ms in Timeout (now = 41510ms): enters Cooldown. Still 1 press
    clock.advance(1); audio.packetsReceived += 10; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);
    assert(hid.pressCount == 1);

    // Advance 1999ms in Cooldown (now = 43509ms, 33499ms from cast): still in Cooldown, still 1 press!
    clock.advance(1999); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);
    assert(hid.pressCount == 1);

    // At 2000ms in Cooldown (now = 43510ms, >= 30000ms + 2000ms safety margin): recasts!
    clock.advance(1); audio.packetsReceived += 10; controller.update(false, false);
    assert(controller.getState() == FishingState::Casting);
    assert(hid.pressCount == 2);
    assert(controller.getTotalCasts() == 2);

    printf("  [PASS] test_no_recast_before_30000ms_plus_safety_margin\n");
}

// 27. Trigger at 29.9s Suppressed by 300ms Delay: No Late Reel Dispatched
void test_trigger_at_29_9s_with_300ms_delay_cancelled_at_deadline() {
    printf("[RUN]  test_trigger_at_29_9s_with_300ms_delay_cancelled_at_deadline\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Cast accepted at now = 10010ms
    assert(controller.requestStart());
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Advance to 29.9s from accepted cast (now = 39910ms, elapsed = 29900ms)
    // 29900 - 1580 = 28320ms in Waiting
    for (int i = 0; i < 28; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
    }
    clock.advance(320);
    audio.packetsReceived += 20;
    controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);
    assert(controller.getWaitRemainingMs() == 100UL); // 100ms remaining until 30s deadline

    // Acoustic trigger arrives at 29.9s
    audio.triggered = true;
    clock.advance(10); // now = 39920ms, elapsed = 29910ms
    controller.update(false, false);

    // Enters BiteDetected (requires 300ms presentation delay before reel)
    assert(controller.getState() == FishingState::BiteDetected);
    assert(controller.getTotalBites() == 1);
    assert(hid.pressCount == 1); // Only cast pressed so far

    // Advance 90ms -> hits now = 40010ms (exactly 30,000ms from cast, only 90ms of 300ms delay passed!)
    clock.advance(90);
    audio.packetsReceived += 10;
    controller.update(false, false);

    // Cast deadline reached while pending reel!
    // Controller MUST suppress/cancel pending reel, enter Timeout, and NEVER dispatch a late reel key!
    assert(controller.getState() == FishingState::Timeout);
    assert(controller.getTotalTimeouts() == 1);
    assert(controller.getTotalReels() == 0);
    assert(hid.pressCount == 1); // ZERO reel keys sent!

    // Advances to Cooldown -> recasts Cast #2 cleanly
    clock.advance(1500); audio.packetsReceived += 75; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);
    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Casting);
    assert(hid.pressCount == 2); // Recast #2
    assert(controller.getTotalCasts() == 2);

    printf("  [PASS] test_trigger_at_29_9s_with_300ms_delay_cancelled_at_deadline\n");
}

// 28. Pending Reel Held Busy Across Expiry Cancelled Without Press
void test_pending_reel_held_busy_across_expiry_cancelled_without_press() {
    printf("[RUN]  test_pending_reel_held_busy_across_expiry_cancelled_without_press\n");
    MockHidKeyboard hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    controller.update(false, false);

    audio.packetsReceived += 10;
    controller.requestArmToggle();
    audio.packetsReceived += 10;
    clock.advance(10);
    controller.update(false, false);

    // Cast accepted at now = 10010ms
    assert(controller.requestStart());
    clock.advance(80); controller.update(false, false);
    clock.advance(1500); audio.packetsReceived += 50; controller.update(false, false);
    assert(controller.getState() == FishingState::Waiting);

    // Bite detected at 28.0s from cast (now = 38010ms, elapsed = 28000ms)
    // 28000 - 1580 = 26420ms in Waiting
    for (int i = 0; i < 26; ++i) {
        clock.advance(1000);
        audio.packetsReceived += 50;
        controller.update(false, false);
    }
    clock.advance(420);
    audio.packetsReceived += 20;
    audio.triggered = true;
    controller.update(false, false);
    assert(controller.getState() == FishingState::BiteDetected);

    // 300ms presentation delay finishes at now = 38330ms (elapsed = 28320ms)
    clock.advance(300);
    // Simulate HID endpoint is busy when reel press is attempted!
    hid.ready = false;
    controller.update(false, false);

    // Controller transitions to Reeling, but keyState_ is PendingPress
    assert(controller.getState() == FishingState::Reeling);
    assert(controller.getKeyState() == KeyState::PendingPress);
    assert(hid.pressCount == 1); // Only cast was accepted

    // HID remains busy while time advances past 30,000ms cast expiry (e.g. to now = 40020ms)
    clock.advance(1710); // now = 40020ms, elapsed from cast = 30010ms >= 30000ms!
    audio.packetsReceived += 50;
    controller.update(false, false);

    // Deadline expired while reel was pending!
    // Controller cancels pending reel, resets key state to Idle, increments timeout, and enters Timeout
    assert(controller.getKeyState() == KeyState::Idle);
    assert(controller.getState() == FishingState::Timeout);
    assert(controller.getTotalTimeouts() == 1);
    assert(controller.getTotalReels() == 0);
    assert(hid.pressCount == 1); // NO reel key was ever pressed!

    // Endpoint now becomes ready after deadline
    hid.ready = true;
    controller.update(false, false);
    // Still zero reel keys sent!
    assert(hid.pressCount == 1);

    // Advances to Cooldown -> recasts Cast #2
    clock.advance(1500); audio.packetsReceived += 75; controller.update(false, false);
    assert(controller.getState() == FishingState::Cooldown);
    clock.advance(2000); audio.packetsReceived += 100; controller.update(false, false);
    assert(controller.getState() == FishingState::Casting);
    assert(hid.pressCount == 2);

    printf("  [PASS] test_pending_reel_held_busy_across_expiry_cancelled_without_press\n");
}

int main() {
    printf("=======================================================================\n");
    printf("PAXG Live Fishing Controller & Safe Spacebar HID Native Tests\n");
    printf("=======================================================================\n");

    test_default_boot_unarmed_passive();
    test_require_positive_packet_progress_and_freshness_before_start();
    test_release_retry_on_stop_disarm_and_rearm_lockout();
    test_press_failure_bounded_retry_fail_closed();
    test_disarm_on_disconnect_in_idle_no_auto_rearm();
    test_require_both_buttons_released_after_arm();
    test_timeout_at_30s_enters_timeout_then_cooldown_and_recasts();
    test_late_bite_at_29s_in_waiting_triggers_reel_and_completes_cycle();
    test_countdown_and_time_remaining_reporting();
    test_continuous_three_consecutive_cycles_without_rearm();
    test_continuous_busy_release_on_reel_defers_cooldown_until_released();
    test_stale_trigger_purge_during_casting_and_cooldown();
    test_cooldown_dropout_fails_closed();
    test_cooldown_interrupted_by_user_stop();
    test_timeout_in_continuous_mode_recasts_after_cooldown();
    test_consecutive_no_bite_timeouts_recast_continously();
    test_bite_just_before_deadline_vs_at_or_after_deadline();
    test_no_stale_audio_trigger_carryover_across_timeout();
    test_stop_in_timeout_and_cooldown();
    test_disconnect_and_stream_stall_in_timeout_and_cooldown();
    test_cooldown_release_busy_retry_no_key_duplication();
    test_uint32_clock_wrap_safe_during_waiting_and_timeout();
    test_cast_accepted_after_delayed_readiness();
    test_no_recast_before_30000ms_plus_safety_margin();
    test_trigger_at_29_9s_with_300ms_delay_cancelled_at_deadline();
    test_pending_reel_held_busy_across_expiry_cancelled_without_press();
    test_stop_at_all_phases_and_priority();
    test_audio_dropout_and_queue_fail_closed();

    printf("=======================================================================\n");
    printf("Results: 28/28 passed (0 failed)\n");
    printf("=======================================================================\n");
    return 0;
}
