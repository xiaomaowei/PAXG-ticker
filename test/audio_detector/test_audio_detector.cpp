#include "modules/audio/audio_ring_buffer.h"
#include "modules/audio/audio_detector.h"
#include "modules/audio/synthetic_audio_samples.h"

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
 * @brief Master Test Runner
 */
int run_all_audio_detector_tests() {
    std::printf("====================================================\n");
    std::printf("  PAXG-ticker Audio Detector Unit Tests (Phase 5)   \n");
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
