#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "bag_full_detector.h"
#include "fishing_web_server.h"
#include "live_fishing_controller.h"

using namespace paxg::audio;
using namespace paxg::fishing;
using namespace paxg::web;

// Test mocks
class MockHid : public IHidKeyboard {
public:
    bool mounted = true;
    bool ready = true;
    int pressCount = 0;
    int releaseCount = 0;
    uint8_t lastKeycode = 0;

    bool isMounted() override { return mounted; }
    bool isReady() override { return ready; }
    bool press(uint8_t, uint8_t keycode) override {
        pressCount++;
        lastKeycode = keycode;
        return true;
    }
    bool release() override {
        releaseCount++;
        return true;
    }
};

class MockClock : public IClock {
public:
    uint32_t currentMs = 1000;
    uint32_t nowMs() override { return currentMs; }
    void advance(uint32_t ms) { currentMs += ms; }
};

class MockAudioStreamWithBagFull : public IAudioStream {
public:
    uint64_t packets = 100;
    uint64_t discontinuities = 0;
    uint32_t queueDrops = 0;
    bool triggered = false;
    float rms = 100.0f;
    bool bagFull = false;
    bool bellContinuing = false;

    uint64_t getPacketsReceived() const override { return packets; }
    uint64_t getDiscontinuities() const override { return discontinuities; }
    uint32_t getQueueDrops() const override { return queueDrops; }
    bool isTriggered() const override { return triggered; }
    void clearTrigger() override { triggered = false; }
    float getRms() const override { return rms; }
    bool isBagFull() const override { return bagFull; }
    bool isBellContinuing(uint32_t nowMs = 0) const override {
        (void)nowMs;
        return bellContinuing;
    }
    void clearBagFull() override {
        bagFull = false;
        bellContinuing = false;
    }
};

// 1. BagFullDetector: periodic 3.0s pulses trigger detection after requiredMatches (2 matches = 3 pulses)
void test_bag_full_periodic_detection() {
    printf("[RUN]  test_bag_full_periodic_detection\n");
    fflush(stdout);
    BagFullDetector detector;

    // Pulse 1 at T=1000ms
    detector.processRms(1000, 15000.0f);
    assert(!detector.isBagFull());
    auto snap1 = detector.getMetrics();
    assert(snap1.totalPulsesDetected == 1);
    assert(snap1.consecutiveMatches == 0);

    // Pulse 2 at T=4000ms (first 3000ms interval match: 2 pulses, not yet latched)
    detector.processRms(4000, 16000.0f);
    assert(!detector.isBagFull());
    auto snap2 = detector.getMetrics();
    assert(snap2.totalPulsesDetected == 2);
    assert(snap2.consecutiveMatches == 1);
    assert(snap2.lastIntervalMs == 3000);
    assert(snap2.triggerCount == 0);

    // Pulse 3 at T=7000ms (second consecutive 3000ms interval match: 3 pulses, latches stop)
    detector.processRms(7000, 16000.0f);
    assert(detector.isBagFull());
    auto snap3 = detector.getMetrics();
    assert(snap3.totalPulsesDetected == 3);
    assert(snap3.consecutiveMatches == 2);
    assert(snap3.lastIntervalMs == 3000);
    assert(snap3.triggerCount == 1);

    printf("  [PASS] test_bag_full_periodic_detection\n");
    fflush(stdout);
}

// 2. BagFullDetector: single pulse and two pulses do not latch bagFull
void test_bag_full_single_pulse_no_trigger() {
    printf("[RUN]  test_bag_full_single_pulse_no_trigger\n");
    fflush(stdout);
    BagFullDetector detector;

    // 1 pulse
    detector.processRms(1000, 20000.0f);
    assert(!detector.isBagFull());
    assert(detector.getMetrics().consecutiveMatches == 0);

    // 2 pulses with valid interval (1 match < requiredMatches 2)
    detector.processRms(4000, 20000.0f);
    assert(!detector.isBagFull());
    assert(detector.getMetrics().consecutiveMatches == 1);

    printf("  [PASS] test_bag_full_single_pulse_no_trigger\n");
    fflush(stdout);
}

// 3. BagFullDetector: aperiodic pulses do not trigger, bad interval breaks streak
void test_bag_full_aperiodic_pulses_no_trigger() {
    printf("[RUN]  test_bag_full_aperiodic_pulses_no_trigger\n");
    fflush(stdout);
    BagFullDetector detector;

    // Pulse 1
    detector.processRms(1000, 15000.0f);
    // Interval 1000ms (far below 2650ms minimum)
    detector.processRms(2000, 15000.0f);
    assert(!detector.isBagFull());
    assert(detector.getMetrics().consecutiveMatches == 0);

    // First valid interval (T=2000 to T=5000 is 3000ms)
    detector.processRms(5000, 15000.0f);
    assert(!detector.isBagFull());
    assert(detector.getMetrics().consecutiveMatches == 1);

    // Interval 5000ms (far above 3350ms maximum) -> breaks streak back to 0
    detector.processRms(10000, 15000.0f);
    assert(!detector.isBagFull());
    assert(detector.getMetrics().consecutiveMatches == 0);

    printf("  [PASS] test_bag_full_aperiodic_pulses_no_trigger\n");
    fflush(stdout);
}

// 4. BagFullDetector: signals below threshold ignored
void test_bag_full_below_threshold_ignored() {
    printf("[RUN]  test_bag_full_below_threshold_ignored\n");
    fflush(stdout);
    BagFullDetector detector;

    // Normal game sounds (RMS 500..2500)
    detector.processRms(1000, 800.0f);
    detector.processRms(4000, 1500.0f);
    detector.processRms(7000, 2800.0f);

    assert(detector.getMetrics().totalPulsesDetected == 0);
    assert(!detector.isBagFull());

    printf("  [PASS] test_bag_full_below_threshold_ignored\n");
    fflush(stdout);
}

// 5. BagFullDetector: debounce window suppresses ringing decay re-triggering
void test_bag_full_debounce_window() {
    printf("[RUN]  test_bag_full_debounce_window\n");
    fflush(stdout);
    BagFullDetector detector;

    // Initial chime onset
    detector.processRms(1000, 18000.0f);
    // Residual ringing pulses within 500ms debounce
    detector.processRms(1100, 16000.0f);
    detector.processRms(1250, 14000.0f);
    detector.processRms(1450, 12000.0f);

    assert(detector.getMetrics().totalPulsesDetected == 1);

    printf("  [PASS] test_bag_full_debounce_window\n");
    fflush(stdout);
}

// 6. BagFullDetector: empirical calibration tolerance window [2650ms, 3350ms]
void test_bag_full_calibration_tolerance_window() {
    printf("[RUN]  test_bag_full_calibration_tolerance_window\n");
    fflush(stdout);
    BagFullDetector detector;

    // Test lower boundary: 2650ms (3000 - 350)
    detector.processRms(1000, 15000.0f);
    detector.processRms(3650, 15000.0f); // 1st interval match (2650ms)
    assert(!detector.isBagFull());
    detector.processRms(6300, 15000.0f); // 2nd consecutive interval match (2650ms)
    assert(detector.isBagFull());

    detector.reset();

    // Test upper boundary: 3350ms (3000 + 350)
    detector.processRms(1000, 15000.0f);
    detector.processRms(4350, 15000.0f); // 1st interval match (3350ms)
    assert(!detector.isBagFull());
    detector.processRms(7700, 15000.0f); // 2nd consecutive interval match (3350ms)
    assert(detector.isBagFull());

    printf("  [PASS] test_bag_full_calibration_tolerance_window\n");
    fflush(stdout);
}

// 7. BagFullDetector: clearBagFull resets latch
void test_bag_full_clear_latch_resets_state() {
    printf("[RUN]  test_bag_full_clear_latch_resets_state\n");
    fflush(stdout);
    BagFullDetector detector;

    detector.processRms(1000, 15000.0f);
    detector.processRms(4000, 15000.0f);
    detector.processRms(7000, 15000.0f);
    assert(detector.isBagFull());

    detector.clearBagFull();
    assert(!detector.isBagFull());
    assert(detector.getMetrics().consecutiveMatches == 0);

    printf("  [PASS] test_bag_full_clear_latch_resets_state\n");
    fflush(stdout);
}

// 8. Controller: Bag-full signal during Waiting halts, disarms, and prevents bite reeling
void test_controller_bag_full_during_waiting_halts_and_disarms() {
    printf("[RUN]  test_controller_bag_full_during_waiting_halts_and_disarms\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);

    ctrl.init();
    clock.advance(100);
    ctrl.update(false, false);

    // Arm controller
    ctrl.requestArmToggle();
    assert(ctrl.isArmed());
    stream.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);

    // Start fishing: enters Casting
    bool started = ctrl.requestStart();
    assert(started);
    assert(ctrl.getState() == FishingState::Casting);

    // Casting duration 1500ms; advance 80ms to release key, then 1500ms to complete cast presentation
    clock.advance(80);
    ctrl.update(false, false);
    clock.advance(1500);
    stream.packets += 50;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Waiting);

    // Simulate bag-full signal during Waiting (while simultaneously receiving audio bite noise)
    stream.bagFull = true;
    stream.triggered = true; // Bite sound should be strictly overridden by bag full
    ctrl.update(false, false);

    // Controller must halt immediately, set StopReason::BagFull, disarm, and not reel
    assert(ctrl.getState() == FishingState::Idle);
    assert(!ctrl.isArmed());
    assert(ctrl.getLastStopReason() == StopReason::BagFull);
    assert(ctrl.getTotalReels() == 0);

    printf("  [PASS] test_controller_bag_full_during_waiting_halts_and_disarms\n");
    fflush(stdout);
}

// 9. Controller: Bag-full signal during Cooldown prevents recast
void test_controller_bag_full_during_cooldown_prevents_recast() {
    printf("[RUN]  test_controller_bag_full_during_cooldown_prevents_recast\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);

    ctrl.init();
    clock.advance(100);
    ctrl.update(false, false);

    ctrl.requestArmToggle();
    stream.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);

    bool started = ctrl.requestStart();
    assert(started);

    // Fast-forward to Waiting
    clock.advance(80);
    ctrl.update(false, false);
    clock.advance(1500);
    stream.packets += 50;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Waiting);

    // Valid bite
    stream.triggered = true;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::BiteDetected);

    clock.advance(350);
    stream.packets += 10;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Reeling);

    clock.advance(80);
    ctrl.update(false, false);
    clock.advance(1200);
    stream.packets += 50;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Cooldown);

    // While in cooldown, bag-full alarm triggers
    stream.bagFull = true;
    clock.advance(100);
    ctrl.update(false, false);

    // Controller must immediately stop with BagFull, disarm, and never recast
    assert(ctrl.getState() == FishingState::Idle);
    assert(!ctrl.isArmed());
    assert(ctrl.getLastStopReason() == StopReason::BagFull);

    // Advance 5 seconds: must stay Idle and Disarmed
    clock.advance(5000);
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Idle);
    assert(!ctrl.isArmed());

    printf("  [PASS] test_controller_bag_full_during_cooldown_prevents_recast\n");
    fflush(stdout);
}

// 10. Controller: Bag-full signal during Idle disarms
void test_controller_bag_full_during_idle_disarms() {
    printf("[RUN]  test_controller_bag_full_during_idle_disarms\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);

    ctrl.init();
    clock.advance(100);
    ctrl.update(false, false);
    ctrl.requestArmToggle();
    assert(ctrl.isArmed());

    // Bag full detected while Idle
    stream.bagFull = true;
    ctrl.update(false, false);

    assert(!ctrl.isArmed());
    assert(ctrl.getLastStopReason() == StopReason::BagFull);

    printf("  [PASS] test_controller_bag_full_during_idle_disarms\n");
    fflush(stdout);
}

// 11. Controller: Latched bag-full condition refuses start
void test_controller_bag_full_refuses_start() {
    printf("[RUN]  test_controller_bag_full_refuses_start\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);

    ctrl.init();
    clock.advance(100);
    ctrl.update(false, false);
    ctrl.requestArmToggle();
    assert(ctrl.isArmed());

    stream.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);

    // Bag full is active
    stream.bagFull = true;

    bool started = ctrl.requestStart();
    assert(!started);
    assert(ctrl.getState() == FishingState::Idle);
    assert(!ctrl.isArmed());

    printf("  [PASS] test_controller_bag_full_refuses_start\n");
    fflush(stdout);
}

// 12. Web Server: JSON serialization format and fields
void test_web_server_status_json_serialization() {
    printf("[RUN]  test_web_server_status_json_serialization\n");
    fflush(stdout);
    WebServerStatus status;
    status.armed = true;
    status.fsmState = FishingState::Waiting;
    status.keyState = KeyState::Idle;
    status.stopReason = StopReason::None;
    status.waitRemainingMs = 24000;
    status.totalCasts = 5;
    status.totalBites = 4;
    status.totalReels = 4;
    status.totalTimeouts = 1;
    status.usbMounted = true;
    status.audioPackets = 48000;
    status.audioRms = 142.5f;
    status.bagFull = false;
    status.bellContinuing = true;
    status.bagIntervalMs = 2995;
    status.bagPulses = 3;
    status.bagMatches = 1;
    std::strncpy(status.localIp, "192.168.1.100", sizeof(status.localIp));

    char jsonBuf[512];
    size_t len = buildStatusJson(status, jsonBuf, sizeof(jsonBuf));
    assert(len > 0);

    // Verify key fields are in JSON
    assert(std::strstr(jsonBuf, "\"armed\":true") != nullptr);
    assert(std::strstr(jsonBuf, "\"fsm\":\"WAITING\"") != nullptr);
    assert(std::strstr(jsonBuf, "\"stop_reason\":\"NONE\"") != nullptr);
    assert(std::strstr(jsonBuf, "\"wait_remaining_ms\":24000") != nullptr);
    assert(std::strstr(jsonBuf, "\"casts\":5") != nullptr);
    assert(std::strstr(jsonBuf, "\"bites\":4") != nullptr);
    assert(std::strstr(jsonBuf, "\"usb_mounted\":true") != nullptr);
    assert(std::strstr(jsonBuf, "\"audio_packets\":48000") != nullptr);
    assert(std::strstr(jsonBuf, "\"bag_full\":false") != nullptr);
    assert(std::strstr(jsonBuf, "\"bell_continuing\":true") != nullptr);
    assert(std::strstr(jsonBuf, "\"bag_interval_ms\":2995") != nullptr);
    assert(std::strstr(jsonBuf, "\"ip\":\"192.168.1.100\"") != nullptr);

    printf("  [PASS] test_web_server_status_json_serialization\n");
    fflush(stdout);
}

// 13. Web Server: Status collection helper
void test_web_server_status_collection() {
    printf("[RUN]  test_web_server_status_collection\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);
    ctrl.init();

    BagFullDetector bagDetector;
    bagDetector.processRms(1000, 15000.0f);
    bagDetector.processRms(4000, 15000.0f);
    bagDetector.processRms(7000, 15000.0f);
    assert(bagDetector.isBagFull());

    WebServerStatus status = collectServerStatus(&ctrl, &bagDetector, nullptr, "10.0.0.42");
    assert(!status.armed);
    assert(status.fsmState == FishingState::Idle);
    assert(status.bagFull == true);
    assert(status.bagIntervalMs == 3000);
    assert(std::strcmp(status.localIp, "10.0.0.42") == 0);

    printf("  [PASS] test_web_server_status_collection\n");
    fflush(stdout);
}

// 14. BagFullDetector: isBellContinuing on single pulse
void test_bag_full_is_bell_continuing_single_pulse() {
    printf("[RUN]  test_bag_full_is_bell_continuing_single_pulse\n");
    fflush(stdout);
    BagFullDetector detector;

    // Before any pulse
    assert(!detector.isBellContinuing(1000));
    assert(!detector.isBagFull());

    // Single pulse at T=1000ms
    detector.processRms(1000, 15000.0f);
    assert(!detector.isBagFull()); // not yet latched by 2 pulses
    assert(detector.isBellContinuing(1500)); // within [1000, 4350ms] -> true!
    assert(detector.isBellContinuing(4350)); // exactly at upper boundary (1000 + 3350) -> true!
    assert(!detector.isBellContinuing(4351)); // beyond window -> false!

    printf("  [PASS] test_bag_full_is_bell_continuing_single_pulse\n");
    fflush(stdout);
}

// 15. Controller: Isolated pulse cannot block start, simulated isolated bite while Waiting produces reel
void test_controller_isolated_pulse_allows_start_and_bite_reels() {
    printf("[RUN]  test_controller_isolated_pulse_allows_start_and_bite_reels\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);

    ctrl.init();
    clock.advance(100);
    ctrl.update(false, false);
    ctrl.requestArmToggle();
    assert(ctrl.isArmed());

    stream.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);

    // 1. Isolated pulse / bellContinuing is present (bagFull=false), requestStart MUST succeed
    stream.bagFull = false;
    stream.bellContinuing = true;

    bool started = ctrl.requestStart();
    assert(started);
    assert(ctrl.getState() == FishingState::Casting);
    assert(ctrl.getLastStopReason() == StopReason::None);

    // Advance through Casting to Waiting
    clock.advance(80);
    ctrl.update(false, false);
    clock.advance(1500);
    stream.packets += 50;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Waiting);

    // 2. Simulated isolated bite pulse while Waiting produces reel instead of BAG_FULL
    stream.bagFull = false;
    stream.bellContinuing = true; // isolated RMS pulse / transient
    stream.triggered = true;      // bite trigger fired
    ctrl.update(false, false);

    // Must transition to BiteDetected, not halt with BagFull!
    assert(ctrl.getState() == FishingState::BiteDetected);
    assert(ctrl.getLastStopReason() != StopReason::BagFull);
    assert(ctrl.getTotalBites() == 1);

    // Advance through bite delay to Reeling
    clock.advance(350);
    stream.packets += 10;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Reeling);
    assert(ctrl.getTotalReels() == 1);

    printf("  [PASS] test_controller_isolated_pulse_allows_start_and_bite_reels\n");
    fflush(stdout);
}

// 15b. Controller & Web Queue: Confirmed full stops/disarms, blocks restart, and web label reflects BAG_FULL
void test_controller_confirmed_full_stops_disarms_blocks_restart() {
    printf("[RUN]  test_controller_confirmed_full_stops_disarms_blocks_restart\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);

    ctrl.init();
    clock.advance(100);
    ctrl.update(false, false);
    ctrl.requestArmToggle();
    assert(ctrl.isArmed());

    stream.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);

    bool started = ctrl.requestStart();
    assert(started);
    assert(ctrl.getState() == FishingState::Casting);

    // Advance to Waiting
    clock.advance(80);
    ctrl.update(false, false);
    clock.advance(1500);
    stream.packets += 50;
    ctrl.update(false, false);
    assert(ctrl.getState() == FishingState::Waiting);

    // Confirmed full (three periodic pulses with two consecutive intervals matched)
    stream.bagFull = true;
    ctrl.update(false, false);

    // Must halt, disarm, and set StopReason::BagFull
    assert(ctrl.getState() == FishingState::Idle);
    assert(!ctrl.isArmed());
    assert(ctrl.getLastStopReason() == StopReason::BagFull);

    // Confirmed full blocks restart even if arm toggle is attempted
    ctrl.requestArmToggle();
    assert(ctrl.isArmed()); // arm toggle itself flips armed flag
    stream.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);

    bool restartStarted = ctrl.requestStart();
    assert(!restartStarted);
    assert(ctrl.getState() == FishingState::Idle);
    assert(!ctrl.isArmed());
    assert(ctrl.getLastStopReason() == StopReason::BagFull);

    // Web command queue verification: Start command while confirmed bagFull returns BAG_FULL rejection
    BagFullDetector detector;
    detector.processRms(1000, 15000.0f);
    detector.processRms(4000, 15000.0f);
    detector.processRms(7000, 15000.0f);
    assert(detector.isBagFull());

    uint32_t startCmdId = 0;
    assert(enqueueCommand(CommandType::Start, &startCmdId));
    processPendingCommands(&ctrl, &detector, clock.nowMs());

    CommandOutcome startOutcome;
    assert(getCommandOutcome(startCmdId, &startOutcome));
    assert(!startOutcome.success);
    assert(std::strcmp(startOutcome.reason, "BAG_FULL") == 0);

    // When bagFull is cleared but isolated recent pulse exists (isBellContinuing only), web command does NOT reject with BAG_FULL
    detector.clearBagFull();
    assert(!detector.isBagFull());
    // Simulate isolated pulse on detector
    detector.processRms(5000, 15000.0f);
    assert(!detector.isBagFull());
    assert(detector.isBellContinuing(5500));

    // Re-arm controller for valid start test
    ctrl.requestArmToggle();
    assert(ctrl.isArmed());
    stream.bagFull = false;
    stream.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);

    uint32_t startCmdId2 = 0;
    assert(enqueueCommand(CommandType::Start, &startCmdId2));
    processPendingCommands(&ctrl, &detector, 5500);

    CommandOutcome startOutcome2;
    assert(getCommandOutcome(startCmdId2, &startOutcome2));
    assert(startOutcome2.success);
    assert(std::strcmp(startOutcome2.reason, "STARTED") == 0);

    printf("  [PASS] test_controller_confirmed_full_stops_disarms_blocks_restart\n");
    fflush(stdout);
}

// 16. Command Queue: Stop priority preempts and purges queued Start commands
void test_command_queue_stop_priority_and_outcome() {
    printf("[RUN]  test_command_queue_stop_priority_and_outcome\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);
    ctrl.init();

    // 1. Enqueue Start command
    uint32_t startCmdId = 0;
    bool enqStart = enqueueCommand(CommandType::Start, &startCmdId);
    assert(enqStart);

    // 2. Enqueue Stop command (must jump to priority slot and purge Start)
    uint32_t stopCmdId = 0;
    bool enqStop = enqueueCommand(CommandType::Stop, &stopCmdId);
    assert(enqStop);

    // 3. Process commands in controller owner loop
    processPendingCommands(&ctrl, nullptr, clock.nowMs());

    // 4. Verify outcome: Stop was executed
    CommandOutcome stopOutcome;
    bool gotStop = getCommandOutcome(stopCmdId, &stopOutcome);
    assert(gotStop);
    assert(stopOutcome.executed);
    assert(stopOutcome.success);
    assert(std::strcmp(stopOutcome.reason, "USER_STOP") == 0);
    assert(ctrl.getState() == FishingState::Idle);
    assert(!ctrl.isArmed());

    // 5. Subsequent processPendingCommands must find no pending Start (purged!)
    processPendingCommands(&ctrl, nullptr, clock.nowMs());
    CommandOutcome startOutcome;
    bool gotStart = getCommandOutcome(startCmdId, &startOutcome);
    assert(!gotStart); // Start was purged by Stop priority

    printf("  [PASS] test_command_queue_stop_priority_and_outcome\n");
    fflush(stdout);
}

// 17. Synchronized Status Snapshot: Isolated read from owner loop
void test_synchronized_status_snapshot_isolation() {
    printf("[RUN]  test_synchronized_status_snapshot_isolation\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);
    ctrl.init();
    ctrl.requestArmToggle();

    BagFullDetector bagDetector;
    bagDetector.processRms(1000, 15000.0f);

    // Publish snapshot from owner loop
    publishStatusSnapshot(&ctrl, &bagDetector, nullptr, "192.168.4.1");

    // Read snapshot from web task context
    WebServerStatus snap = getPublishedStatusSnapshot();
    assert(snap.armed == true);
    assert(snap.fsmState == FishingState::Idle);
    assert(snap.bagPulses == 1);
    assert(snap.bagFull == false);
    assert(std::strcmp(snap.localIp, "192.168.4.1") == 0);

    printf("  [PASS] test_synchronized_status_snapshot_isolation\n");
    fflush(stdout);
}

// 18. Explicit reset after clearing bags
void test_explicit_reset_after_clearing_bags() {
    printf("[RUN]  test_explicit_reset_after_clearing_bags\n");
    fflush(stdout);
    MockHid hid;
    MockClock clock;
    MockAudioStreamWithBagFull stream;
    LiveFishingController ctrl(&hid, &clock, &stream);
    ctrl.init();

    BagFullDetector bagDetector;
    bagDetector.processRms(1000, 15000.0f);
    bagDetector.processRms(4000, 15000.0f);
    bagDetector.processRms(7000, 15000.0f);
    assert(bagDetector.isBagFull());
    assert(bagDetector.isBellContinuing(7500));

    // Player clears bags in game (bell ceases), then explicitly clicks reset
    bagDetector.clearBagFull();

    // Both latch and continuing state are cleanly reset
    assert(!bagDetector.isBagFull());
    assert(!bagDetector.isBellContinuing(7500));
    assert(bagDetector.getMetrics().totalPulsesDetected == 3); // telemetry preserved
    assert(bagDetector.getMetrics().consecutiveMatches == 0); // match streak reset

    // Now starting is allowed
    ctrl.requestArmToggle();
    stream.packets += 50;
    clock.advance(10);
    ctrl.update(false, false);
    bool started = ctrl.requestStart();
    assert(started);
    assert(ctrl.getState() == FishingState::Casting);

    printf("  [PASS] test_explicit_reset_after_clearing_bags\n");
    fflush(stdout);
}

int main() {
    printf("=======================================================================\n");
    printf("PAXG Bag-Full Audio Stop & LAN Web Server Native Tests\n");
    printf("=======================================================================\n");
    fflush(stdout);

    test_bag_full_periodic_detection();
    test_bag_full_single_pulse_no_trigger();
    test_bag_full_aperiodic_pulses_no_trigger();
    test_bag_full_below_threshold_ignored();
    test_bag_full_debounce_window();
    test_bag_full_calibration_tolerance_window();
    test_bag_full_clear_latch_resets_state();
    test_controller_bag_full_during_waiting_halts_and_disarms();
    test_controller_bag_full_during_cooldown_prevents_recast();
    test_controller_bag_full_during_idle_disarms();
    test_controller_bag_full_refuses_start();
    test_web_server_status_json_serialization();
    test_web_server_status_collection();
    test_bag_full_is_bell_continuing_single_pulse();
    test_controller_isolated_pulse_allows_start_and_bite_reels();
    test_controller_confirmed_full_stops_disarms_blocks_restart();
    test_command_queue_stop_priority_and_outcome();
    test_synchronized_status_snapshot_isolation();
    test_explicit_reset_after_clearing_bags();

    printf("=======================================================================\n");
    printf("Results: 19/19 passed (0 failed)\n");
    printf("=======================================================================\n");
    fflush(stdout);
    return 0;
}
