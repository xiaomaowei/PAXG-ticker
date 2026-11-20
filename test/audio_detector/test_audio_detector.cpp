#include "modules/audio/audio_ring_buffer.h"
#include "modules/audio/audio_detector.h"
#include "modules/audio/synthetic_audio_samples.h"
#include "modules/fishing_types.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace paxg::audio;

// Simple test harness
static int g_testsRun = 0;
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            g_testsFailed++; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(testFunc) \
    do { \
        g_testsRun++; \
        std::printf("[RUN]  %s\n", #testFunc); \
        if (testFunc()) { \
            std::printf("  [PASS] %s\n", #testFunc); \
            g_testsPassed++; \
        } \
    } while (0)

/**
 * @brief Test 1: Bounded Ring Buffer Operations & Boundary Enforcement
 */
bool test_ring_buffer_basics_and_bounds() {
    PcmRingBuffer<8> rb;

    TEST_ASSERT(rb.isEmpty(), "Buffer should initially be empty");
    TEST_ASSERT(!rb.isFull(), "Buffer should not initially be full");
    TEST_ASSERT(rb.available() == 0, "Available samples should be 0");
    TEST_ASSERT(rb.capacity() == 8, "Capacity should be 8");
    TEST_ASSERT(rb.freeSpace() == 8, "Free space should be 8");

    // Push 4 samples
    int16_t samples1[4] = {10, 20, 30, 40};
    size_t pushed = rb.push(samples1, 4);
    TEST_ASSERT(pushed == 4, "Should push 4 samples");
    TEST_ASSERT(rb.available() == 4, "Available should be 4");
    TEST_ASSERT(rb.freeSpace() == 4, "Free space should be 4");

    // Peek 4 samples without removing
    int16_t peekBuf[4] = {0};
    size_t peeked = rb.peek(peekBuf, 4);
    TEST_ASSERT(peeked == 4, "Should peek 4 samples");
    TEST_ASSERT(peekBuf[0] == 10 && peekBuf[1] == 20 && peekBuf[2] == 30 && peekBuf[3] == 40, "Peeked contents match");
    TEST_ASSERT(rb.available() == 4, "Available unchanged after peek");

    // Pop 2 samples
    int16_t popBuf[4] = {0};
    size_t popped = rb.pop(popBuf, 2);
    TEST_ASSERT(popped == 2, "Should pop 2 samples");
    TEST_ASSERT(popBuf[0] == 10 && popBuf[1] == 20, "Popped values match");
    TEST_ASSERT(rb.available() == 2, "Available should be 2 after pop");

    // Fill to capacity
    int16_t samples2[6] = {50, 60, 70, 80, 90, 100};
    pushed = rb.push(samples2, 6);
    TEST_ASSERT(pushed == 6, "Should push remaining 6 to reach capacity 8");
    TEST_ASSERT(rb.isFull(), "Buffer should be full");
    TEST_ASSERT(rb.available() == 8, "Available should equal capacity");

    // Attempt pushing into full buffer with standard push (should drop / return 0)
    int16_t extra = 999;
    bool pushResult = rb.push(extra);
    TEST_ASSERT(!pushResult, "Push into full buffer should return false");
    TEST_ASSERT(rb.available() == 8, "Count should remain 8");

    return true;
}

/**
 * @brief Test 2: Bounded Ring Buffer Overwrite and Wraparound
 */
bool test_ring_buffer_overwrite_and_wraparound() {
    PcmRingBuffer<4> rb;

    // Push 4 items: 1, 2, 3, 4
    for (int16_t i = 1; i <= 4; ++i) {
        rb.pushOverwrite(i);
    }
    TEST_ASSERT(rb.isFull(), "Buffer should be full");

    // Push 2 more items: 5, 6 (should drop 1, 2; buffer now contains 3, 4, 5, 6)
    rb.pushOverwrite(5);
    rb.pushOverwrite(6);
    TEST_ASSERT(rb.available() == 4, "Count should remain clamped at 4");

    int16_t readOut[4] = {0};
    size_t popped = rb.pop(readOut, 4);
    TEST_ASSERT(popped == 4, "Should pop 4 samples");
    TEST_ASSERT(readOut[0] == 3 && readOut[1] == 4 && readOut[2] == 5 && readOut[3] == 6,
                "Oldest samples dropped; newest retained in order");

    TEST_ASSERT(rb.isEmpty(), "Buffer should now be empty");
    return true;
}

/**
 * @brief Test 3: Mathematical Ground Truth for RMS and Peak
 */
bool test_rms_and_peak_accuracy() {
    AudioDetectorConfig cfg;
    cfg.frameSize = 64;
    cfg.absoluteThreshold = 1000;
    PcmAudioDetector detector(cfg);

    // Case A: Silence
    int16_t silence[64] = {0};
    detector.processSamples(silence, 64);
    AudioDetectorMetrics mSilence = detector.getMetrics();
    TEST_ASSERT(mSilence.rms == 0.0f, "Silence RMS must be 0.0");
    TEST_ASSERT(mSilence.peak == 0, "Silence Peak must be 0");

    // Case B: Pure DC value = 1200
    int16_t dc[64];
    for (int i = 0; i < 64; ++i) dc[i] = 1200;
    detector.processSamples(dc, 64);
    AudioDetectorMetrics mDc = detector.getMetrics();
    TEST_ASSERT(std::fabs(mDc.rms - 1200.0f) < 0.1f, "DC RMS must equal sample magnitude (1200)");
    TEST_ASSERT(mDc.peak == 1200, "DC Peak must equal 1200");

    // Case C: Square wave alternating +3000, -3000
    int16_t square[64];
    for (int i = 0; i < 64; ++i) square[i] = (i % 2 == 0) ? 3000 : -3000;
    detector.processSamples(square, 64);
    AudioDetectorMetrics mSquare = detector.getMetrics();
    TEST_ASSERT(std::fabs(mSquare.rms - 3000.0f) < 0.1f, "Square wave RMS must equal amplitude (3000)");
    TEST_ASSERT(mSquare.peak == 3000, "Square wave Peak must equal 3000");

    // Case D: Single INT16_MIN (-32768) impulse in otherwise silent frame
    int16_t minImpulse[64] = {0};
    minImpulse[0] = -32768; // INT16_MIN
    detector.processSamples(minImpulse, 64);
    AudioDetectorMetrics mMinImpulse = detector.getMetrics();
    TEST_ASSERT(mMinImpulse.peak == 32768U, "Peak of INT16_MIN must be exactly 32768 without int16 narrowing");

    // Case E: Pure DC at INT16_MIN (-32768)
    int16_t dcMin[64];
    for (int i = 0; i < 64; ++i) dcMin[i] = -32768;
    detector.processSamples(dcMin, 64);
    AudioDetectorMetrics mDcMin = detector.getMetrics();
    TEST_ASSERT(mDcMin.peak == 32768U, "DC INT16_MIN peak must be 32768");
    TEST_ASSERT(std::fabs(mDcMin.rms - 32768.0f) < 0.1f, "DC INT16_MIN RMS must be 32768.0");

    // Case F: Pure DC at INT16_MAX (+32767)
    int16_t dcMax[64];
    for (int i = 0; i < 64; ++i) dcMax[i] = 32767;
    detector.processSamples(dcMax, 64);
    AudioDetectorMetrics mDcMax = detector.getMetrics();
    TEST_ASSERT(mDcMax.peak == 32767U, "DC INT16_MAX peak must be 32767");
    TEST_ASSERT(std::fabs(mDcMax.rms - 32767.0f) < 0.1f, "DC INT16_MAX RMS must be 32767.0");

    return true;
}

/**
 * @brief Test 4: Adaptive Noise Floor Tracking & Transient Immunity
 */
bool test_adaptive_noise_floor() {
    AudioDetectorConfig cfg;
    cfg.frameSize = 64;
    cfg.noiseFloorAlpha = 0.1f;
    cfg.absoluteThreshold = 2000;
    PcmAudioDetector detector(cfg);

    // Initial noise floor starts at 100.0
    TEST_ASSERT(detector.getMetrics().noiseFloor == 100.0f, "Initial noise floor is 100.0");

    // Feed quiet baseline noise with DC amplitude ~400
    int16_t ambient[64];
    for (int i = 0; i < 64; ++i) ambient[i] = 400;

    // Feed 10 frames of 400 RMS. The noise floor should adaptively climb toward 400.
    for (int f = 0; f < 10; ++f) {
        detector.processSamples(ambient, 64);
    }
    float adaptedNoiseFloor = detector.getMetrics().noiseFloor;
    TEST_ASSERT(adaptedNoiseFloor > 250.0f && adaptedNoiseFloor < 450.0f,
                "Noise floor should track upward toward ambient RMS (~400)");

    // Feed single frame of high-amplitude burst (e.g. 5000)
    int16_t burst[64];
    for (int i = 0; i < 64; ++i) burst[i] = 5000;
    detector.processSamples(burst, 64);

    // The single burst should NOT have corrupted the noise floor directly to 5000
    float noiseFloorAfterBurst = detector.getMetrics().noiseFloor;
    TEST_ASSERT(noiseFloorAfterBurst < 1000.0f,
                "Noise floor must not spike to burst level (immune to single burst corruption)");

    return true;
}

/**
 * @brief Test 5: Debounce Rejection of Transient Noise Glitches
 */
bool test_transient_noise_debounce_rejection() {
    AudioDetectorConfig cfg;
    cfg.frameSize = 64;
    cfg.debounceFrames = 2; // Require 2 consecutive frames above threshold
    cfg.cooldownFrames = 10;
    cfg.absoluteThreshold = 2000;
    PcmAudioDetector detector(cfg);

    int16_t quiet[64] = {0};
    int16_t loud[64];
    for (int i = 0; i < 64; ++i) loud[i] = 4000; // RMS = 4000 > 2000 threshold

    // Frame 1: Quiet
    detector.processSamples(quiet, 64);
    TEST_ASSERT(!detector.isTriggered(), "Quiet frame must not trigger");

    // Frame 2: 1-frame loud spike (transient glitch)
    detector.processSamples(loud, 64);
    AudioDetectorMetrics m1 = detector.getMetrics();
    TEST_ASSERT(m1.isAboveThreshold, "Frame 2 is above threshold");
    TEST_ASSERT(m1.consecutiveAbove == 1, "Consecutive count should be 1");
    TEST_ASSERT(!detector.isTriggered(), "1-frame spike must be rejected by debounce (debounceFrames=2)");
    TEST_ASSERT(m1.triggerCount == 0, "Trigger count must remain 0");

    // Frame 3: Returns to quiet
    detector.processSamples(quiet, 64);
    AudioDetectorMetrics m2 = detector.getMetrics();
    TEST_ASSERT(!m2.isAboveThreshold, "Frame 3 is below threshold");
    TEST_ASSERT(m2.consecutiveAbove == 0, "Consecutive count reset to 0");
    TEST_ASSERT(!detector.isTriggered(), "Still not triggered");
    TEST_ASSERT(m2.triggerCount == 0, "Trigger count remains 0");

    return true;
}

/**
 * @brief Test 6: Deterministic Valid Burst Trigger
 */
bool test_valid_burst_trigger() {
    AudioDetectorConfig cfg;
    cfg.frameSize = 64;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 10;
    cfg.absoluteThreshold = 2000;
    PcmAudioDetector detector(cfg);

    int16_t loud[64];
    for (int i = 0; i < 64; ++i) loud[i] = 4000;

    // Frame 1 of burst: consecutive = 1
    detector.processSamples(loud, 64);
    TEST_ASSERT(!detector.isTriggered(), "First frame should not trigger yet");
    TEST_ASSERT(detector.getMetrics().triggerCount == 0, "Trigger count 0");

    // Frame 2 of burst: consecutive = 2 >= debounceFrames -> TRIGGER!
    detector.processSamples(loud, 64);
    TEST_ASSERT(detector.isTriggered(), "Second consecutive frame must trigger event");
    AudioDetectorMetrics m = detector.getMetrics();
    TEST_ASSERT(m.triggerCount == 1, "Trigger count must be 1");
    TEST_ASSERT(m.cooldownRemaining == 10, "Cooldown initialized to 10 frames");

    return true;
}

/**
 * @brief Test 7: Cooldown Rejection of Immediate Secondary Bursts
 */
bool test_cooldown_rejection() {
    AudioDetectorConfig cfg;
    cfg.frameSize = 64;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 5;
    cfg.absoluteThreshold = 2000;
    PcmAudioDetector detector(cfg);

    int16_t loud[64];
    for (int i = 0; i < 64; ++i) loud[i] = 4000;

    // Trigger initial burst (2 frames)
    detector.processSamples(loud, 64);
    detector.processSamples(loud, 64);
    TEST_ASSERT(detector.getMetrics().triggerCount == 1, "Initial trigger fired");
    detector.clearTrigger();
    TEST_ASSERT(!detector.isTriggered(), "Latched trigger cleared");

    // Send 3 more consecutive loud frames while still in cooldown (cooldownFrames = 5)
    detector.processSamples(loud, 64); // cooldown decrements to 4
    detector.processSamples(loud, 64); // cooldown decrements to 3
    detector.processSamples(loud, 64); // cooldown decrements to 2

    AudioDetectorMetrics m = detector.getMetrics();
    TEST_ASSERT(!detector.isTriggered(), "Must not trigger while in cooldown");
    TEST_ASSERT(m.triggerCount == 1, "Trigger count must remain 1 during cooldown");
    TEST_ASSERT(m.cooldownRemaining == 2, "Cooldown counter should be at 2");

    return true;
}

/**
 * @brief Test 8: Cooldown Expiration and Re-arm
 */
bool test_cooldown_expiration_and_rearm() {
    AudioDetectorConfig cfg;
    cfg.frameSize = 64;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 3;
    cfg.absoluteThreshold = 2000;
    PcmAudioDetector detector(cfg);

    int16_t quiet[64] = {0};
    int16_t loud[64];
    for (int i = 0; i < 64; ++i) loud[i] = 4000;

    // Initial trigger
    detector.processSamples(loud, 64);
    detector.processSamples(loud, 64);
    TEST_ASSERT(detector.getMetrics().triggerCount == 1, "Trigger count 1");
    detector.clearTrigger();

    // Send 3 quiet frames to exhaust cooldown (3 frames)
    detector.processSamples(quiet, 64); // cooldown = 2
    detector.processSamples(quiet, 64); // cooldown = 1
    detector.processSamples(quiet, 64); // cooldown = 0 (re-armed!)
    TEST_ASSERT(detector.getMetrics().cooldownRemaining == 0, "Cooldown should be expired");

    // Send 2 loud frames -> should fire trigger #2!
    detector.processSamples(loud, 64);
    detector.processSamples(loud, 64);
    TEST_ASSERT(detector.isTriggered(), "Second burst after cooldown must trigger");
    TEST_ASSERT(detector.getMetrics().triggerCount == 2, "Trigger count must increment to 2");

    return true;
}

/**
 * @brief Test 9: End-to-End Synthetic Scenario Generator Verification
 */
bool test_synthetic_scenario_end_to_end() {
    AudioDetectorConfig cfg;
    cfg.sampleRate = 16000;
    cfg.frameSize = 128;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 20;
    cfg.absoluteThreshold = 1800;
    cfg.noiseFloorAlpha = 0.05f;
    PcmAudioDetector detector(cfg);

    // Total test buffer: 10 frames = 1280 samples
    // Frame 0..2: Ambient noise (amp 300)
    // Frame 3..5: Injected bite burst (amp 6000)
    // Frame 6..9: Ambient noise (amp 300)
    constexpr size_t TOTAL_SAMPLES = 1280;
    int16_t testStream[TOTAL_SAMPLES];

    SyntheticAudioGenerator::generateBurst(testStream, TOTAL_SAMPLES,
                                          300, 6000,
                                          384, 384, // start at sample 384 (frame 3), duration 384 (3 frames)
                                          999);

    detector.processSamples(testStream, TOTAL_SAMPLES);

    AudioDetectorMetrics m = detector.getMetrics();
    TEST_ASSERT(m.totalFramesProcessed == 10, "10 frames processed");
    TEST_ASSERT(m.totalSamplesProcessed == 1280, "1280 samples processed");
    TEST_ASSERT(detector.isTriggered(), "Bite burst triggered");
    TEST_ASSERT(m.triggerCount == 1, "Exactly 1 bite event registered");

    return true;
}

/**
 * @brief Test 10: Persistent Settings Sane Bounds Clamping
 */
bool test_fishing_settings_bounds_clamping() {
    using namespace paxg::fishing;

    FishingSettingsData s;
    s.clamp();
    TEST_ASSERT(s.absoluteThreshold == DEFAULT_THRESHOLD, "Default threshold is 1500");
    TEST_ASSERT(s.waitTimeoutMs == DEFAULT_WAIT_TIMEOUT_MS, "Default timeout is 20000ms");
    TEST_ASSERT(s.cooldownMs == DEFAULT_COOLDOWN_MS, "Default cooldown is 2000ms");
    TEST_ASSERT(s.castKey == DEFAULT_CAST_KEY, "Default cast key is '1'");
    TEST_ASSERT(s.interactKey == DEFAULT_INTERACT_KEY, "Default interact key is '2'");

    // Below minimum bounds
    s.absoluteThreshold = 100;
    s.waitTimeoutMs = 1000;
    s.cooldownMs = 200;
    s.castKey = 'x';       // invalid
    s.interactKey = 0xFF;  // invalid
    s.clamp();
    TEST_ASSERT(s.absoluteThreshold == MIN_THRESHOLD, "Clamped to MIN_THRESHOLD (500)");
    TEST_ASSERT(s.waitTimeoutMs == MIN_WAIT_TIMEOUT_MS, "Clamped to MIN_WAIT_TIMEOUT_MS (5000)");
    TEST_ASSERT(s.cooldownMs == MIN_COOLDOWN_MS, "Clamped to MIN_COOLDOWN_MS (500)");
    TEST_ASSERT(s.castKey == DEFAULT_CAST_KEY, "Invalid cast key restored to default '1'");
    TEST_ASSERT(s.interactKey == DEFAULT_INTERACT_KEY, "Invalid interact key restored to default '2'");

    // Above maximum bounds
    s.absoluteThreshold = 10000;
    s.waitTimeoutMs = 60000;
    s.cooldownMs = 10000;
    s.castKey = '3';      // valid allowed key
    s.interactKey = ' ';  // valid allowed key
    s.clamp();
    TEST_ASSERT(s.absoluteThreshold == MAX_THRESHOLD, "Clamped to MAX_THRESHOLD (5000)");
    TEST_ASSERT(s.waitTimeoutMs == MAX_WAIT_TIMEOUT_MS, "Clamped to MAX_WAIT_TIMEOUT_MS (30000)");
    TEST_ASSERT(s.cooldownMs == MAX_COOLDOWN_MS, "Clamped to MAX_COOLDOWN_MS (5000)");
    TEST_ASSERT(s.castKey == '3', "Allowed key '3' preserved");
    TEST_ASSERT(s.interactKey == ' ', "Allowed key ' ' preserved");

    return true;
}

/**
 * @brief Test 11: Production FSM Normal Bite Flow via FishingStateMachine
 */
bool test_fsm_normal_bite_flow() {
    using namespace paxg::fishing;

    AudioDetectorConfig cfg;
    cfg.sampleRate = 16000;
    cfg.frameSize = 128;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 20;
    cfg.absoluteThreshold = 1500;
    PcmAudioDetector detector(cfg);

    FishingStateMachine fsm(FishingState::Idle);
    TEST_ASSERT(fsm.getState() == FishingState::Idle, "Initial state must be Idle");

    // 1. User starts simulation: Idle -> Casting
    bool ok = fsm.handleEvent(FishingEvent::Start);
    TEST_ASSERT(ok, "Start event accepted in Idle");
    TEST_ASSERT(fsm.getState() == FishingState::Casting, "State transitions to Casting");

    // 2. Casting animation completes (1500ms): Casting -> Waiting
    ok = fsm.handleEvent(FishingEvent::CastComplete);
    TEST_ASSERT(ok, "CastComplete event accepted in Casting");
    TEST_ASSERT(fsm.getState() == FishingState::Waiting, "State transitions to Waiting");

    // Ingest ambient noise frames: detector must NOT trigger
    int16_t noiseFrame[128];
    SyntheticAudioGenerator::generateNoise(noiseFrame, 128, 250, 42);
    detector.processSamples(noiseFrame, 128);
    detector.processSamples(noiseFrame, 128);
    TEST_ASSERT(!detector.isTriggered(), "Detector not triggered by ambient noise");
    TEST_ASSERT(fsm.getState() == FishingState::Waiting, "FSM remains in Waiting");

    // Ingest bite splash burst (2 frames, RMS ~6000 > 1500)
    int16_t biteFrame[128];
    SyntheticAudioGenerator::generateBurst(biteFrame, 128, 250, 6000, 0, 128, 100);
    detector.processSamples(biteFrame, 128);
    detector.processSamples(biteFrame, 128);
    TEST_ASSERT(detector.isTriggered(), "Detector triggered by synthetic bite burst");
    detector.clearTrigger();

    // 3. Audio bite confirmed: Waiting -> BiteDetected
    ok = fsm.handleEvent(FishingEvent::BiteDetected);
    TEST_ASSERT(ok, "BiteDetected event accepted in Waiting");
    TEST_ASSERT(fsm.getState() == FishingState::BiteDetected, "State transitions to BiteDetected");

    // 4. BiteDetected presentation timer expires (300ms): BiteDetected -> Reeling
    ok = fsm.handleEvent(FishingEvent::ReelTrigger);
    TEST_ASSERT(ok, "ReelTrigger event accepted in BiteDetected");
    TEST_ASSERT(fsm.getState() == FishingState::Reeling, "State transitions to Reeling");

    // 5. Reeling duration completes (1200ms): Reeling -> Cooldown
    ok = fsm.handleEvent(FishingEvent::ReelComplete);
    TEST_ASSERT(ok, "ReelComplete event accepted in Reeling");
    TEST_ASSERT(fsm.getState() == FishingState::Cooldown, "State transitions to Cooldown");

    // 6. Cooldown duration completes: Cooldown -> Casting (next cycle)
    ok = fsm.handleEvent(FishingEvent::CooldownComplete);
    TEST_ASSERT(ok, "CooldownComplete event accepted in Cooldown");
    TEST_ASSERT(fsm.getState() == FishingState::Casting, "State transitions back to Casting");

    return true;
}

/**
 * @brief Test 12: Production FSM Timeout Flow via FishingStateMachine
 */
bool test_fsm_timeout_flow() {
    using namespace paxg::fishing;

    FishingStateMachine fsm(FishingState::Waiting);
    TEST_ASSERT(fsm.getState() == FishingState::Waiting, "Initial state is Waiting");

    // 1. Wait timeout elapsed: Waiting -> Timeout
    bool ok = fsm.handleEvent(FishingEvent::BiteTimeout);
    TEST_ASSERT(ok, "BiteTimeout event accepted in Waiting");
    TEST_ASSERT(fsm.getState() == FishingState::Timeout, "State transitions to Timeout");

    // 2. Timeout display duration expires (1500ms): Timeout -> Cooldown
    ok = fsm.handleEvent(FishingEvent::TimeoutComplete);
    TEST_ASSERT(ok, "TimeoutComplete event accepted in Timeout");
    TEST_ASSERT(fsm.getState() == FishingState::Cooldown, "State transitions to Cooldown");

    // 3. Cooldown completes: Cooldown -> Casting
    ok = fsm.handleEvent(FishingEvent::CooldownComplete);
    TEST_ASSERT(ok, "CooldownComplete event accepted in Cooldown");
    TEST_ASSERT(fsm.getState() == FishingState::Casting, "State transitions to Casting");

    return true;
}

/**
 * @brief Test 13: Production FSM Pause, Resume, and Stop Flow via FishingStateMachine
 */
bool test_fsm_pause_resume_flow() {
    using namespace paxg::fishing;

    FishingStateMachine fsm(FishingState::Waiting);

    // 1. Pause active fishing: Waiting -> Paused
    bool ok = fsm.handleEvent(FishingEvent::Pause);
    TEST_ASSERT(ok, "Pause event accepted in Waiting");
    TEST_ASSERT(fsm.getState() == FishingState::Paused, "State transitions to Paused");

    // 2. In Paused state, bite or cast events must be rejected
    TEST_ASSERT(!fsm.handleEvent(FishingEvent::BiteDetected), "Paused rejects BiteDetected");
    TEST_ASSERT(!fsm.handleEvent(FishingEvent::CastComplete), "Paused rejects CastComplete");
    TEST_ASSERT(fsm.getState() == FishingState::Paused, "State remains Paused");

    // 3. Resume fishing: Paused -> Casting
    ok = fsm.handleEvent(FishingEvent::Resume);
    TEST_ASSERT(ok, "Resume event accepted in Paused");
    TEST_ASSERT(fsm.getState() == FishingState::Casting, "State transitions to Casting on Resume");

    // 4. Stop from active state: Casting -> Idle
    ok = fsm.handleEvent(FishingEvent::Stop);
    TEST_ASSERT(ok, "Stop event accepted in Casting");
    TEST_ASSERT(fsm.getState() == FishingState::Idle, "State transitions to Idle on Stop");

    // 5. Stop while already in Idle: valid idempotent action
    ok = fsm.handleEvent(FishingEvent::Stop);
    TEST_ASSERT(ok, "Stop event accepted in Idle (idempotent)");
    TEST_ASSERT(fsm.getState() == FishingState::Idle, "State remains Idle");

    return true;
}

/**
 * @brief Test 14: Production FSM Error, Disarm, and Reset Flow via FishingStateMachine
 */
bool test_fsm_error_and_disarm_flow() {
    using namespace paxg::fishing;

    FishingStateMachine fsm(FishingState::Waiting);

    // 1. Simulate USB disconnect or subsystem fault: Waiting -> Error
    bool ok = fsm.handleEvent(FishingEvent::Error);
    TEST_ASSERT(ok, "Error event accepted in Waiting");
    TEST_ASSERT(fsm.getState() == FishingState::Error, "State transitions to Error");

    // 2. In Error state, Start, Resume, and Bite are strictly rejected
    TEST_ASSERT(!fsm.handleEvent(FishingEvent::Start), "Error state rejects Start");
    TEST_ASSERT(!fsm.handleEvent(FishingEvent::Resume), "Error state rejects Resume");
    TEST_ASSERT(!fsm.handleEvent(FishingEvent::BiteDetected), "Error state rejects BiteDetected");
    TEST_ASSERT(fsm.getState() == FishingState::Error, "State remains Error");

    // 3. User Stop/Reset clears Error: Error -> Idle
    ok = fsm.handleEvent(FishingEvent::Stop);
    TEST_ASSERT(ok, "Stop event clears Error state");
    TEST_ASSERT(fsm.getState() == FishingState::Idle, "State transitions to Idle on Stop");

    return true;
}

/**
 * @brief Test 15: Production FSM Invalid Transitions Systematic Rejection
 */
bool test_fsm_invalid_transitions_rejection() {
    using namespace paxg::fishing;

    // Test Idle: cannot advance without Start or Stop
    FishingStateMachine fsmIdle(FishingState::Idle);
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::CastComplete), "Idle rejects CastComplete");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::BiteDetected), "Idle rejects BiteDetected");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::BiteTimeout), "Idle rejects BiteTimeout");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::ReelTrigger), "Idle rejects ReelTrigger");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::ReelComplete), "Idle rejects ReelComplete");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::CooldownComplete), "Idle rejects CooldownComplete");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::TimeoutComplete), "Idle rejects TimeoutComplete");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::Resume), "Idle rejects Resume");
    TEST_ASSERT(!fsmIdle.handleEvent(FishingEvent::Pause), "Idle rejects Pause");
    TEST_ASSERT(fsmIdle.getState() == FishingState::Idle, "State unchanged after invalid events");

    // Test Casting: rejects invalid forward jumps
    FishingStateMachine fsmCast(FishingState::Casting);
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::Start), "Casting rejects Start");
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::BiteDetected), "Casting rejects BiteDetected");
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::BiteTimeout), "Casting rejects BiteTimeout");
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::ReelTrigger), "Casting rejects ReelTrigger");
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::ReelComplete), "Casting rejects ReelComplete");
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::CooldownComplete), "Casting rejects CooldownComplete");
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::TimeoutComplete), "Casting rejects TimeoutComplete");
    TEST_ASSERT(!fsmCast.handleEvent(FishingEvent::Resume), "Casting rejects Resume");
    TEST_ASSERT(fsmCast.getState() == FishingState::Casting, "State unchanged after invalid events");

    // Test Waiting: rejects premature reel completion
    FishingStateMachine fsmWait(FishingState::Waiting);
    TEST_ASSERT(!fsmWait.handleEvent(FishingEvent::Start), "Waiting rejects Start");
    TEST_ASSERT(!fsmWait.handleEvent(FishingEvent::CastComplete), "Waiting rejects CastComplete");
    TEST_ASSERT(!fsmWait.handleEvent(FishingEvent::ReelTrigger), "Waiting rejects ReelTrigger");
    TEST_ASSERT(!fsmWait.handleEvent(FishingEvent::ReelComplete), "Waiting rejects ReelComplete");
    TEST_ASSERT(!fsmWait.handleEvent(FishingEvent::CooldownComplete), "Waiting rejects CooldownComplete");
    TEST_ASSERT(!fsmWait.handleEvent(FishingEvent::TimeoutComplete), "Waiting rejects TimeoutComplete");
    TEST_ASSERT(!fsmWait.handleEvent(FishingEvent::Resume), "Waiting rejects Resume");
    TEST_ASSERT(fsmWait.getState() == FishingState::Waiting, "State unchanged after invalid events");

    // Test BiteDetected: rejects out-of-order events
    FishingStateMachine fsmBite(FishingState::BiteDetected);
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::Start), "BiteDetected rejects Start");
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::CastComplete), "BiteDetected rejects CastComplete");
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::BiteDetected), "BiteDetected rejects BiteDetected");
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::BiteTimeout), "BiteDetected rejects BiteTimeout");
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::ReelComplete), "BiteDetected rejects ReelComplete");
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::CooldownComplete), "BiteDetected rejects CooldownComplete");
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::TimeoutComplete), "BiteDetected rejects TimeoutComplete");
    TEST_ASSERT(!fsmBite.handleEvent(FishingEvent::Resume), "BiteDetected rejects Resume");
    TEST_ASSERT(fsmBite.getState() == FishingState::BiteDetected, "State unchanged after invalid events");

    // Test Reeling: rejects start or bite events
    FishingStateMachine fsmReel(FishingState::Reeling);
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::Start), "Reeling rejects Start");
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::CastComplete), "Reeling rejects CastComplete");
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::BiteDetected), "Reeling rejects BiteDetected");
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::BiteTimeout), "Reeling rejects BiteTimeout");
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::ReelTrigger), "Reeling rejects ReelTrigger");
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::CooldownComplete), "Reeling rejects CooldownComplete");
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::TimeoutComplete), "Reeling rejects TimeoutComplete");
    TEST_ASSERT(!fsmReel.handleEvent(FishingEvent::Resume), "Reeling rejects Resume");
    TEST_ASSERT(fsmReel.getState() == FishingState::Reeling, "State unchanged after invalid events");

    // Test Cooldown: rejects non-cooldown completion events
    FishingStateMachine fsmCd(FishingState::Cooldown);
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::Start), "Cooldown rejects Start");
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::CastComplete), "Cooldown rejects CastComplete");
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::BiteDetected), "Cooldown rejects BiteDetected");
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::BiteTimeout), "Cooldown rejects BiteTimeout");
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::ReelTrigger), "Cooldown rejects ReelTrigger");
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::ReelComplete), "Cooldown rejects ReelComplete");
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::TimeoutComplete), "Cooldown rejects TimeoutComplete");
    TEST_ASSERT(!fsmCd.handleEvent(FishingEvent::Resume), "Cooldown rejects Resume");
    TEST_ASSERT(fsmCd.getState() == FishingState::Cooldown, "State unchanged after invalid events");

    // Test Timeout: rejects start or bite events
    FishingStateMachine fsmTo(FishingState::Timeout);
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::Start), "Timeout rejects Start");
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::CastComplete), "Timeout rejects CastComplete");
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::BiteDetected), "Timeout rejects BiteDetected");
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::BiteTimeout), "Timeout rejects BiteTimeout");
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::ReelTrigger), "Timeout rejects ReelTrigger");
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::ReelComplete), "Timeout rejects ReelComplete");
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::CooldownComplete), "Timeout rejects CooldownComplete");
    TEST_ASSERT(!fsmTo.handleEvent(FishingEvent::Resume), "Timeout rejects Resume");
    TEST_ASSERT(fsmTo.getState() == FishingState::Timeout, "State unchanged after invalid events");

    return true;
}

/**
 * @brief Master Test Runner
 */
int run_all_audio_detector_tests() {
    std::printf("====================================================\n");
    std::printf("  PAXG-ticker Audio Detector Unit Tests (Phase 5-7) \n");
    std::printf("====================================================\n");

    g_testsRun = 0;
    g_testsPassed = 0;
    g_testsFailed = 0;

    RUN_TEST(test_ring_buffer_basics_and_bounds);
    RUN_TEST(test_ring_buffer_overwrite_and_wraparound);
    RUN_TEST(test_rms_and_peak_accuracy);
    RUN_TEST(test_adaptive_noise_floor);
    RUN_TEST(test_transient_noise_debounce_rejection);
    RUN_TEST(test_valid_burst_trigger);
    RUN_TEST(test_cooldown_rejection);
    RUN_TEST(test_cooldown_expiration_and_rearm);
    RUN_TEST(test_synthetic_scenario_end_to_end);
    RUN_TEST(test_fishing_settings_bounds_clamping);
    RUN_TEST(test_fsm_normal_bite_flow);
    RUN_TEST(test_fsm_timeout_flow);
    RUN_TEST(test_fsm_pause_resume_flow);
    RUN_TEST(test_fsm_error_and_disarm_flow);
    RUN_TEST(test_fsm_invalid_transitions_rejection);

    std::printf("----------------------------------------------------\n");
    std::printf("Results: %d/%d passed, %d failed\n", g_testsPassed, g_testsRun, g_testsFailed);
    std::printf("====================================================\n");

    return (g_testsFailed == 0) ? 0 : 1;
}

#if !defined(ARDUINO)
int main() {
    return run_all_audio_detector_tests();
}
#endif
