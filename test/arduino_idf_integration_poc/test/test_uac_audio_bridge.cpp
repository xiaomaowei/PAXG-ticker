#include "uac_audio_bridge.h"
#include "usb_audio_queue.h"
#include "usb_device_composite.h"

#include "modules/audio/audio_detector.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// Simple deterministic test runner
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

namespace {

// Helper: pack a single 16-bit signed stereo frame into 4 bytes (little-endian)
inline void packStereoFrame(uint8_t* dest, int16_t left, int16_t right) {
    const uint16_t uLeft = static_cast<uint16_t>(left);
    const uint16_t uRight = static_cast<uint16_t>(right);
    dest[0] = static_cast<uint8_t>(uLeft & 0xFF);
    dest[1] = static_cast<uint8_t>((uLeft >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t>(uRight & 0xFF);
    dest[3] = static_cast<uint8_t>((uRight >> 8) & 0xFF);
}

// Helper: generate a vector of packed 16-bit stereo PCM bytes
std::vector<uint8_t> generateStereoStream(size_t frameCount, int16_t left, int16_t right) {
    std::vector<uint8_t> bytes(frameCount * 4);
    for (size_t i = 0; i < frameCount; ++i) {
        packStereoFrame(&bytes[i * 4], left, right);
    }
    return bytes;
}

}  // namespace

/**
 * @brief Test 1: Verification of basic initialization, empty/null ingestion, and state reset.
 */
bool test_bridge_init_and_empty_packet_handling() {
    paxg::uac::UacAudioBridge bridge;

    auto metrics = bridge.getMetricsSnapshot();
    TEST_ASSERT(metrics.packetsReceived == 0, "Initial packets received must be 0");
    TEST_ASSERT(metrics.bytesReceived == 0, "Initial bytes received must be 0");
    TEST_ASSERT(metrics.stereoFramesDecoded == 0, "Initial stereo frames must be 0");
    TEST_ASSERT(metrics.monoSamplesIngested == 0, "Initial mono samples must be 0");
    TEST_ASSERT(metrics.nullOrEmptyPackets == 0, "Initial null packet count must be 0");
    TEST_ASSERT(bridge.getResidualByteCount() == 0, "Initial residual bytes must be 0");

    // Null pointer call
    size_t decoded = bridge.consumeUacChunk(nullptr, 100);
    TEST_ASSERT(decoded == 0, "Null buffer must return 0 decoded frames");
    metrics = bridge.getMetricsSnapshot();
    TEST_ASSERT(metrics.nullOrEmptyPackets == 1, "nullOrEmptyPackets counter must increment");
    TEST_ASSERT(metrics.packetsReceived == 0, "packetsReceived must not increment for null");

    // Zero length call
    uint8_t dummy[4] = {0};
    decoded = bridge.consumeUacChunk(dummy, 0);
    TEST_ASSERT(decoded == 0, "Zero length buffer must return 0 decoded frames");
    metrics = bridge.getMetricsSnapshot();
    TEST_ASSERT(metrics.nullOrEmptyPackets == 2, "nullOrEmptyPackets counter must increment to 2");

    // Ingest some data and test deadlock-free reset and init
    auto packet = generateStereoStream(10, 1000, 1000);
    bridge.consumeUacChunk(packet.data(), packet.size());
    metrics = bridge.getMetricsSnapshot();
    TEST_ASSERT(metrics.packetsReceived == 1, "packetsReceived must be 1");
    TEST_ASSERT(metrics.stereoFramesDecoded == 10, "stereoFramesDecoded must be 10");

    bridge.reset();
    metrics = bridge.getMetricsSnapshot();
    TEST_ASSERT(metrics.packetsReceived == 0, "Reset must restore packetsReceived to 0");
    TEST_ASSERT(metrics.bytesReceived == 0, "Reset must restore bytesReceived to 0");
    TEST_ASSERT(metrics.stereoFramesDecoded == 0, "Reset must restore stereoFramesDecoded to 0");
    TEST_ASSERT(bridge.getResidualByteCount() == 0, "Reset must clear residual buffer");

    // Test re-init with custom config (verifies no recursive mutex deadlock)
    paxg::audio::AudioDetectorConfig customCfg;
    customCfg.sampleRate = 48000;
    customCfg.frameSize = 256;
    bridge.init(nullptr, &customCfg);
    TEST_ASSERT(bridge.getConfig().frameSize == 256, "Re-init should successfully apply new config");

    return true;
}

/**
 * @brief Test 2: Arbitrary chunk sizes and unaligned packet boundary residual handling.
 *
 * Verifies that split packets (e.g. 1 byte, 3 bytes, 5 bytes, 7 bytes) maintain
 * strict sample alignment and phase coherence across callback boundaries without data loss.
 */
bool test_arbitrary_chunk_sizes_and_residuals() {
    paxg::uac::UacAudioBridge bridge;

    // Generate 12 stereo frames = 48 bytes
    // Frame values: Left = 2000, Right = 4000 -> Mono = 3000
    std::vector<uint8_t> allBytes = generateStereoStream(12, 2000, 4000);
    TEST_ASSERT(allBytes.size() == 48, "Total bytes must be 48");

    const size_t chunkSizes[] = {1, 2, 1, 5, 3, 7, 6, 2, 11, 10};
    size_t totalChunkSum = 0;
    for (size_t sz : chunkSizes) {
        totalChunkSum += sz;
    }
    TEST_ASSERT(totalChunkSum == 48, "Chunk sizes must sum exactly to 48 bytes");

    size_t offset = 0;
    size_t totalDecoded = 0;
    for (size_t chunkSize : chunkSizes) {
        size_t decoded = bridge.consumeUacChunk(allBytes.data() + offset, chunkSize);
        totalDecoded += decoded;
        offset += chunkSize;
    }

    TEST_ASSERT(totalDecoded == 12, "Must decode exactly 12 complete frames across all chunks");
    TEST_ASSERT(bridge.getResidualByteCount() == 0, "Residual buffer must be empty at end");

    auto snap = bridge.getMetricsSnapshot();
    TEST_ASSERT(snap.packetsReceived == 10, "Must have received 10 chunks");
    TEST_ASSERT(snap.bytesReceived == 48, "Must have received 48 bytes total");
    TEST_ASSERT(snap.stereoFramesDecoded == 12, "Must have decoded 12 stereo frames");
    TEST_ASSERT(snap.monoSamplesIngested == 12, "Must have ingested 12 mono samples into detector");
    TEST_ASSERT(snap.unalignedChunkCount > 0, "Must have tracked unaligned chunk occurrences");

    return true;
}

/**
 * @brief Test 3: Stereo mix extreme cases.
 *
 * Verifies mathematical correctness and dynamic range across:
 * - Hard Left only (Right = 0)
 * - Hard Right only (Left = 0)
 * - Out-of-phase cancellation (+A, -A -> 0)
 * - Full-scale in-phase (+32767, +32767 -> +32767)
 * - Negative full scale (-32768, -32768 -> -32768, no signed overflow/UB)
 */
bool test_stereo_mix_extremes() {
    paxg::audio::PcmAudioDetector detector;
    paxg::audio::AudioDetectorConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameSize = 384;
    detector.init(cfg);

    paxg::uac::UacAudioBridge bridge(&detector);

    // 1. Hard Left only: Left = 4000, Right = 0 -> Mono = 2000
    auto streamLeftOnly = generateStereoStream(384, 4000, 0);
    bridge.consumeUacChunk(streamLeftOnly.data(), streamLeftOnly.size());
    auto m1 = bridge.getMetricsSnapshot();
    TEST_ASSERT(std::abs(m1.detectorMetrics.rms - 2000.0f) < 1.0f, "Left-only mix RMS must be 2000.0");
    TEST_ASSERT(m1.detectorMetrics.peak == 2000, "Left-only mix Peak must be 2000");

    bridge.reset();

    // 2. Hard Right only: Left = 0, Right = 6000 -> Mono = 3000
    auto streamRightOnly = generateStereoStream(384, 0, 6000);
    bridge.consumeUacChunk(streamRightOnly.data(), streamRightOnly.size());
    auto m2 = bridge.getMetricsSnapshot();
    TEST_ASSERT(std::abs(m2.detectorMetrics.rms - 3000.0f) < 1.0f, "Right-only mix RMS must be 3000.0");
    TEST_ASSERT(m2.detectorMetrics.peak == 3000, "Right-only mix Peak must be 3000");

    bridge.reset();

    // 3. Out-of-phase cancellation: Left = +5000, Right = -5000 -> Mono = 0
    auto streamOutOfPhase = generateStereoStream(384, 5000, -5000);
    bridge.consumeUacChunk(streamOutOfPhase.data(), streamOutOfPhase.size());
    auto m3 = bridge.getMetricsSnapshot();
    TEST_ASSERT(m3.detectorMetrics.rms == 0.0f, "Out-of-phase stereo mix must cancel to RMS 0.0");
    TEST_ASSERT(m3.detectorMetrics.peak == 0, "Out-of-phase stereo mix must cancel to Peak 0");

    bridge.reset();

    // 4. Positive full scale: Left = +32767, Right = +32767 -> Mono = 32767
    auto streamMaxPositive = generateStereoStream(384, 32767, 32767);
    bridge.consumeUacChunk(streamMaxPositive.data(), streamMaxPositive.size());
    auto m4 = bridge.getMetricsSnapshot();
    TEST_ASSERT(std::abs(m4.detectorMetrics.rms - 32767.0f) < 1.0f, "Positive full-scale RMS must be 32767.0");
    TEST_ASSERT(m4.detectorMetrics.peak == 32767, "Positive full-scale Peak must be 32767");

    bridge.reset();

    // 5. Negative full scale: Left = -32768, Right = -32768 -> Mono = -32768
    auto streamMinNegative = generateStereoStream(384, -32768, -32768);
    bridge.consumeUacChunk(streamMinNegative.data(), streamMinNegative.size());
    auto m5 = bridge.getMetricsSnapshot();
    TEST_ASSERT(std::abs(m5.detectorMetrics.rms - 32768.0f) < 1.0f, "Negative full-scale RMS must be 32768.0");
    TEST_ASSERT(m5.detectorMetrics.peak == 32768, "Negative full-scale Peak must be 32768 (stored in uint32_t)");

    return true;
}

/**
 * @brief Test 4: Silence vs. Signal & Runtime Telemetry Distinction.
 */
bool test_silence_vs_signal_distinction() {
    paxg::audio::PcmAudioDetector detector;
    paxg::audio::AudioDetectorConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameSize = 384;
    cfg.absoluteThreshold = 1500;
    cfg.thresholdRatio = 2.5f;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 25;
    detector.init(cfg);

    paxg::uac::UacAudioBridge bridge(&detector);

    // State A: No packets
    auto stateA = bridge.getMetricsSnapshot();
    TEST_ASSERT(stateA.packetsReceived == 0, "State A: packetsReceived must be 0");
    TEST_ASSERT(stateA.bytesReceived == 0, "State A: bytesReceived must be 0");
    TEST_ASSERT(stateA.detectorMetrics.totalFramesProcessed == 0, "State A: totalFramesProcessed must be 0");
    TEST_ASSERT(stateA.detectorMetrics.rms == 0.0f, "State A: RMS must be 0.0");
    TEST_ASSERT(!stateA.detectorMetrics.triggered, "State A: must not be triggered");

    // State B: Digital silence stream (10 packets of 48 frames each = 480 frames of 0)
    for (int p = 0; p < 10; ++p) {
        auto silencePacket = generateStereoStream(48, 0, 0);
        bridge.consumeUacChunk(silencePacket.data(), silencePacket.size());
    }

    auto stateB = bridge.getMetricsSnapshot();
    TEST_ASSERT(stateB.packetsReceived == 10, "State B: packetsReceived must be 10 (proves packets arrived!)");
    TEST_ASSERT(stateB.bytesReceived == 10 * 48 * 4, "State B: bytesReceived must match delivered payload");
    TEST_ASSERT(stateB.stereoFramesDecoded == 480, "State B: stereoFramesDecoded must be 480");
    TEST_ASSERT(stateB.detectorMetrics.totalFramesProcessed == 1, "State B: exactly 1 complete frame processed (480 >= 384)");
    TEST_ASSERT(stateB.detectorMetrics.rms == 0.0f, "State B: RMS must be 0.0 (confirms silence)");
    TEST_ASSERT(stateB.detectorMetrics.peak == 0, "State B: Peak must be 0");
    TEST_ASSERT(!stateB.detectorMetrics.triggered, "State B: silence must NOT trigger detector");

    // State C: Active sound signal exceeding threshold
    auto signalPacket1 = generateStereoStream(384, 4000, 4000);
    bridge.consumeUacChunk(signalPacket1.data(), signalPacket1.size());

    auto intermediateState = bridge.getMetricsSnapshot();
    TEST_ASSERT(intermediateState.detectorMetrics.isAboveThreshold, "Frame 1 must be above threshold");
    TEST_ASSERT(!intermediateState.detectorMetrics.triggered, "Frame 1 must not trigger yet (debounce = 2)");

    auto signalPacket2 = generateStereoStream(384, 4000, 4000);
    bridge.consumeUacChunk(signalPacket2.data(), signalPacket2.size());

    auto stateC = bridge.getMetricsSnapshot();
    TEST_ASSERT(stateC.detectorMetrics.triggered, "State C: Frame 2 must confirm debounce and latch trigger!");
    TEST_ASSERT(stateC.detectorMetrics.triggerCount == 1, "State C: triggerCount must be 1");
    TEST_ASSERT(bridge.isTriggered(), "Bridge isTriggered() helper must return true");

    // Test clearTrigger
    bridge.clearTrigger();
    auto stateCleared = bridge.getMetricsSnapshot();
    TEST_ASSERT(!stateCleared.detectorMetrics.triggered, "clearTrigger must unlatch trigger flag");
    TEST_ASSERT(!bridge.isTriggered(), "Bridge isTriggered() must return false after clear");
    TEST_ASSERT(stateCleared.detectorMetrics.triggerCount == 1, "triggerCount must be preserved after clearTrigger");

    return true;
}

/**
 * @brief Test 5: Timing, Cooldown Lockout, and Re-arming.
 */
bool test_timing_and_cooldown_behavior() {
    paxg::audio::PcmAudioDetector detector;
    paxg::audio::AudioDetectorConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameSize = 384;
    cfg.absoluteThreshold = 1500;
    cfg.thresholdRatio = 2.5f;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 25;
    detector.init(cfg);

    paxg::uac::UacAudioBridge bridge(&detector);

    // Initial quiet frames
    for (int i = 0; i < 5; ++i) {
        auto quiet = generateStereoStream(384, 200, 200);
        bridge.consumeUacChunk(quiet.data(), quiet.size());
    }

    // Trigger Burst 1 (2 frames of 3500)
    for (int i = 0; i < 2; ++i) {
        auto burst = generateStereoStream(384, 3500, 3500);
        bridge.consumeUacChunk(burst.data(), burst.size());
    }

    auto snap1 = bridge.getMetricsSnapshot();
    TEST_ASSERT(snap1.detectorMetrics.triggerCount == 1, "Burst 1 must trigger (count = 1)");
    TEST_ASSERT(snap1.detectorMetrics.cooldownRemaining == 25, "Cooldown must be armed to 25 frames");

    bridge.clearTrigger();

    // Loud burst during cooldown
    for (int i = 0; i < 5; ++i) {
        auto loudInCooldown = generateStereoStream(384, 5000, 5000);
        bridge.consumeUacChunk(loudInCooldown.data(), loudInCooldown.size());
    }

    auto snapCooldown = bridge.getMetricsSnapshot();
    TEST_ASSERT(snapCooldown.detectorMetrics.triggerCount == 1,
                "Loud burst during cooldown lockout must NOT increment triggerCount");

    // Expire remaining cooldown
    for (int i = 0; i < 20; ++i) {
        auto quiet = generateStereoStream(384, 200, 200);
        bridge.consumeUacChunk(quiet.data(), quiet.size());
    }

    auto snapExpired = bridge.getMetricsSnapshot();
    TEST_ASSERT(snapExpired.detectorMetrics.cooldownRemaining == 0,
                "Cooldown must be fully expired (0 remaining)");

    // Trigger Burst 2
    for (int i = 0; i < 2; ++i) {
        auto burst2 = generateStereoStream(384, 3500, 3500);
        bridge.consumeUacChunk(burst2.data(), burst2.size());
    }

    auto snapRearmed = bridge.getMetricsSnapshot();
    TEST_ASSERT(snapRearmed.detectorMetrics.triggerCount == 2,
                "After cooldown expiration, detector must rearm and increment triggerCount to 2!");

    return true;
}

/**
 * @brief Test 6: Reset residual state across USB stream breaks.
 *
 * Verifies that partial byte residuals (1-3 bytes) can be explicitly flushed upon
 * stream breaks (e.g. host audio mute/stop/unmount) via resetResidualOnStreamBreak(),
 * ensuring subsequent streams start clean without cross-session phase contamination.
 */
bool test_stream_break_residual_reset() {
    paxg::uac::UacAudioBridge bridge;

    // Send a 6-byte packet: contains 1 complete stereo frame (4 bytes) + 2 residual bytes
    uint8_t partialPacket[6] = {
        0x10, 0x00, 0x20, 0x00, // Frame 0: Left=16, Right=32
        0x30, 0x00             // Trailing 2 bytes (Left=48, incomplete Right)
    };

    size_t decoded = bridge.consumeUacChunk(partialPacket, 6);
    TEST_ASSERT(decoded == 1, "Must decode 1 full frame");
    TEST_ASSERT(bridge.getResidualByteCount() == 2, "Residual buffer must hold exactly 2 trailing bytes");

    auto snapBefore = bridge.getMetricsSnapshot();
    TEST_ASSERT(snapBefore.residualDiscontinuityCount == 0, "No stream break reset yet");

    // Simulate USB stream break (host halts audio stream)
    bridge.resetResidualOnStreamBreak();
    TEST_ASSERT(bridge.getResidualByteCount() == 0, "Residual buffer must be flushed after stream break reset");

    auto snapAfter = bridge.getMetricsSnapshot();
    TEST_ASSERT(snapAfter.residualDiscontinuityCount == 1,
                "residualDiscontinuityCount must increment to record flushed fractional bytes");

    // Subsequent stream starts with a fresh 4-byte frame
    auto cleanPacket = generateStereoStream(1, 1000, 1000);
    decoded = bridge.consumeUacChunk(cleanPacket.data(), cleanPacket.size());
    TEST_ASSERT(decoded == 1, "Subsequent stream must decode 1 complete frame cleanly");
    TEST_ASSERT(bridge.getResidualByteCount() == 0, "Residual buffer must remain clean (not corrupted by previous session)");

    return true;
}

/**
 * @brief Test 7: Multi-threaded Concurrency Stress Test on Production Bridge.
 *
 * Verifies that concurrent execution between:
 * - Thread 1: USB audio streaming ingestion (simulating usb_spk_task)
 * - Thread 2: Main loop polling telemetry snapshots and clearing triggers
 * - Thread 3: Asynchronous control operations (stream break resets and config queries)
 * runs completely free of deadlocks, data races, or state corruption under high contention.
 */
bool test_concurrent_audio_stream_and_control_access() {
    paxg::audio::PcmAudioDetector detector;
    paxg::audio::AudioDetectorConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameSize = 384;
    cfg.absoluteThreshold = 1500;
    cfg.debounceFrames = 2;
    cfg.cooldownFrames = 10;
    detector.init(cfg);

    paxg::uac::UacAudioBridge bridge(&detector);

    constexpr int NUM_STREAM_PACKETS = 800;
    std::atomic<bool> producerDone{false};
    std::atomic<size_t> totalFramesPushed{0};
    std::atomic<size_t> totalBytesPushed{0};

    // Thread 1: Audio ingestion worker (simulating usb_spk_task)
    std::thread producerThread([&]() {
        for (int p = 0; p < NUM_STREAM_PACKETS; ++p) {
            // Vary chunk lengths between 7 bytes (unaligned), 40 bytes, and 96 bytes
            const size_t framesInPacket = (p % 3 == 0) ? 24 : ((p % 3 == 1) ? 10 : 3);
            const int16_t amp = (p % 20 < 4) ? 4000 : 200; // Periodic bursts
            auto packet = generateStereoStream(framesInPacket, amp, amp);

            // Occasionally add 1 or 2 unaligned bytes to exercise residual lock under concurrency
            if (p % 5 == 0) {
                packet.push_back(0x42);
            }

            totalBytesPushed += packet.size();
            totalFramesPushed += framesInPacket;
            bridge.consumeUacChunk(packet.data(), packet.size());
            std::this_thread::yield();
        }
        producerDone.store(true);
    });

    // Thread 2: Telemetry reader & trigger consumer (simulating Arduino loop())
    std::atomic<uint64_t> readOperations{0};
    std::thread readerThread([&]() {
        while (!producerDone.load()) {
            auto snap = bridge.getMetricsSnapshot();
            (void)snap.packetsReceived;
            (void)snap.bytesReceived;
            (void)snap.detectorMetrics.rms;
            if (bridge.isTriggered()) {
                bridge.clearTrigger();
            }
            (void)bridge.getResidualByteCount();
            readOperations++;
            std::this_thread::yield();
        }
    });

    // Thread 3: Asynchronous control & config queries
    std::atomic<uint64_t> controlOperations{0};
    std::thread controlThread([&]() {
        while (!producerDone.load()) {
            auto c = bridge.getConfig();
            (void)c.frameSize;
            if (controlOperations % 50 == 0) {
                bridge.resetResidualOnStreamBreak();
            }
            controlOperations++;
            std::this_thread::yield();
        }
    });

    producerThread.join();
    readerThread.join();
    controlThread.join();

    auto finalSnap = bridge.getMetricsSnapshot();
    TEST_ASSERT(finalSnap.packetsReceived == NUM_STREAM_PACKETS,
                "Final packets received must match producer packet count");
    TEST_ASSERT(finalSnap.bytesReceived == totalBytesPushed.load(),
                "Final bytes received must match exact ground truth bytes delivered");
    TEST_ASSERT(readOperations > 50, "Reader thread must have executed multiple concurrent read passes");
    TEST_ASSERT(controlOperations > 50, "Control thread must have executed multiple concurrent control passes");

    return true;
}


/**
 * @brief Test 8: Deterministic Discontinuity & Gap Sample Isolation Test.
 *
 * Proves that:
 * 1. Partial unaligned bytes (residual) in the bridge are cleanly flushed on a discontinuity.
 * 2. Partial mono frame samples in the detector ring buffer are strictly purged.
 * 3. The production bridge CANNOT combine samples across a recorded gap/stream epoch.
 */
bool test_deterministic_discontinuity_sample_isolation() {
    paxg::audio::PcmAudioDetector detector;
    paxg::audio::AudioDetectorConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameSize = 384;      // 384 mono samples required for 1 complete frame
    cfg.absoluteThreshold = 1500;
    detector.init(cfg);

    paxg::uac::UacAudioBridge bridge(&detector);

    // Step 1: Ingest 100 mono samples (= 100 stereo frames = 400 bytes) + 2 residual bytes (402 bytes total)
    // 100 samples < 384 frame size: they sit in the detector ring buffer waiting for completion.
    auto packet1 = generateStereoStream(100, 2000, 2000);
    packet1.push_back(0x12);
    packet1.push_back(0x34); // 2 residual bytes

    size_t decoded = bridge.consumeUacChunk(packet1.data(), packet1.size());
    TEST_ASSERT(decoded == 100, "Must decode 100 stereo frames");
    TEST_ASSERT(bridge.getResidualByteCount() == 2, "Bridge must hold 2 residual bytes");

    auto snap1 = bridge.getMetricsSnapshot();
    TEST_ASSERT(snap1.detectorMetrics.totalFramesProcessed == 0,
                "0 frames processed yet because 100 < 384");
    TEST_ASSERT(snap1.residualDiscontinuityCount == 0, "No discontinuity yet");
    TEST_ASSERT(snap1.streamDiscontinuityCount == 0, "No stream discontinuity yet");

    // Step 2: Trigger stream break / discontinuity signal
    bridge.resetResidualOnStreamBreak();

    TEST_ASSERT(bridge.getResidualByteCount() == 0,
                "Residual bytes must be completely wiped on discontinuity");
    auto snap2 = bridge.getMetricsSnapshot();
    TEST_ASSERT(snap2.residualDiscontinuityCount == 1,
                "residualDiscontinuityCount must increment to record flushed fractional bytes");
    TEST_ASSERT(snap2.streamDiscontinuityCount == 1,
                "streamDiscontinuityCount must increment to record discontinuity event");
    TEST_ASSERT(snap2.detectorMetrics.totalFramesProcessed == 0,
                "Detector state was reset: 0 frames processed");

    // Step 3: Now ingest 334 mono samples (1336 bytes of clean 4-byte stereo frames)
    // CRITICAL PROOF:
    // If pre-gap samples were NOT purged: 100 (pre-gap) + 334 (post-gap) = 434 >= 384!
    // In that corrupted case, totalFramesProcessed would be 1 (combining pre-gap and post-gap!).
    // Because pre-gap samples WERE purged, exactly 334 samples exist in the detector, which is < 384!
    // Therefore, totalFramesProcessed MUST remain 0!
    auto postGapPacket1 = generateStereoStream(334, 2000, 2000);
    decoded = bridge.consumeUacChunk(postGapPacket1.data(), postGapPacket1.size());
    TEST_ASSERT(decoded == 334, "Must decode 334 stereo frames");

    auto snap3 = bridge.getMetricsSnapshot();
    TEST_ASSERT(snap3.detectorMetrics.totalFramesProcessed == 0,
                "CONFIRMED: Pre-gap samples were NOT combined across the stream break! Total frames is strictly 0!");

    // Step 4: Now feed the remaining 50 mono samples (200 bytes) -> total post-gap samples = 384
    auto postGapPacket2 = generateStereoStream(50, 2000, 2000);
    decoded = bridge.consumeUacChunk(postGapPacket2.data(), postGapPacket2.size());
    TEST_ASSERT(decoded == 50, "Must decode 50 stereo frames");

    auto snap4 = bridge.getMetricsSnapshot();
    TEST_ASSERT(snap4.detectorMetrics.totalFramesProcessed == 1,
                "Exactly 1 complete frame processed from pure post-gap audio!");

    return true;
}

/**
 * @brief Test 9: End-to-End Consumer Pipeline Ordering (Queue + Discontinuity Callback + Bridge).
 *
 * Simulates usb_spk_task consumer loop verifying that:
 * 1. Discontinuity callback is called BEFORE output_cb receives post-drop chunk.
 * 2. Stale generation packets in queue are discarded without calling output_cb.
 * 3. Gaps with no queued audio until next stream trigger discontinuity BEFORE first chunk.
 */
bool test_end_to_end_consumer_pipeline_ordering() {
    usb_audio_queue_t queue;
    usb_audio_queue_init(&queue, 4);

    paxg::uac::UacAudioBridge bridge;

    // Execution trace tracker
    std::vector<std::string> eventTrace;

    auto discCb = [&](uint32_t reason) {
        eventTrace.push_back("DISC_REASON_" + std::to_string(reason));
        bridge.resetResidualOnStreamBreak();
    };

    auto outputCb = [&](const uint8_t* buf, size_t len) {
        eventTrace.push_back("OUTPUT_LEN_" + std::to_string(len));
        bridge.consumeUacChunk(buf, len);
    };

    // Helper: Consumer drain step mimicking usb_spk_task
    auto runConsumerDrain = [&](uint32_t activeGen, bool active,
                                uint32_t& lastDeliveredGen, uint32_t& lastDiscGen,
                                bool& streamDiscPending) {
        if (!active) {
            streamDiscPending = true;
        }

        uint8_t buf[USB_AUDIO_QUEUE_SLOT_SIZE];
        size_t len = 0;
        uint32_t pktGen = 0;
        uint32_t pktFlags = 0;

        while (usb_audio_queue_dequeue_ext(&queue, buf, sizeof(buf), &len, &pktGen, &pktFlags)) {
            if (pktGen != activeGen || !active) {
                streamDiscPending = true;
                continue;
            }

            uint32_t discReason = UAC_DISCONTINUITY_REASON_UNKNOWN;
            if (streamDiscPending || (lastDeliveredGen != 0 && pktGen != lastDeliveredGen) || (lastDiscGen != pktGen)) {
                discReason = UAC_DISCONTINUITY_REASON_STREAM_CHANGE;
                lastDiscGen = pktGen;
                streamDiscPending = false;
            } else if (pktFlags & USB_AUDIO_SLOT_FLAG_DISCONTINUITY) {
                discReason = UAC_DISCONTINUITY_REASON_OVERFLOW_DROP;
            }

            if (discReason != UAC_DISCONTINUITY_REASON_UNKNOWN) {
                discCb(discReason);
            }

            lastDeliveredGen = pktGen;

            if (len > 0) {
                outputCb(buf, len);
            }
        }

        if (!active && streamDiscPending) {
            discCb(UAC_DISCONTINUITY_REASON_STREAM_CHANGE);
            lastDiscGen = activeGen;
            streamDiscPending = false;
        }
    };

    uint32_t lastDeliveredGen = 0;
    uint32_t lastDiscGen = 0;
    bool streamDiscPending = false;

    // --- Subcase A: Normal enqueue and dequeue of 2 packets in Gen 1 ---
    uint8_t dummy1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);

    runConsumerDrain(1, true, lastDeliveredGen, lastDiscGen, streamDiscPending);

    // Initial packets of stream 1 trigger initial stream disc, followed by output chunks
    TEST_ASSERT(eventTrace.size() == 3, "Trace must have 3 events (1 disc + 2 output)");
    TEST_ASSERT(eventTrace[0] == "DISC_REASON_1", "Event 0 must be stream change disc");
    TEST_ASSERT(eventTrace[1] == "OUTPUT_LEN_8", "Event 1 must be output");
    TEST_ASSERT(eventTrace[2] == "OUTPUT_LEN_8", "Event 2 must be output");
    eventTrace.clear();

    // --- Subcase B: Overflow drop ordering ---
    // Capacity is 4. Enqueue 4 packets.
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);

    // Attempt 5th packet -> DROPPED on overflow!
    bool dropped = !usb_audio_queue_enqueue(&queue, dummy1, 8, 1);
    TEST_ASSERT(dropped, "5th packet must be dropped on queue overflow");

    // Dequeue 1 packet to free space
    uint8_t tmpBuf[64];
    size_t tmpLen = 0;
    uint32_t tmpGen = 0;
    usb_audio_queue_dequeue(&queue, tmpBuf, sizeof(tmpBuf), &tmpLen, &tmpGen);

    // Now enqueue post-drop packet (packet 6) -> Must have DISCONTINUITY flag!
    bool enq6 = usb_audio_queue_enqueue(&queue, dummy1, 8, 1);
    TEST_ASSERT(enq6, "Packet 6 must be successfully enqueued");

    // Run consumer: Must drain remaining packets from before drop WITHOUT drop disc,
    // and then invoke DISC_REASON_2 (OVERFLOW_DROP) strictly BEFORE Packet 6 is output!
    runConsumerDrain(1, true, lastDeliveredGen, lastDiscGen, streamDiscPending);

    // Packets 2, 3, 4 have no drop. Then Packet 6 has drop disc before output.
    // Events: OUTPUT (pkt 2), OUTPUT (pkt 3), OUTPUT (pkt 4), DISC_REASON_2, OUTPUT (pkt 6)
    TEST_ASSERT(eventTrace.size() == 5, "Trace must contain 5 events for overflow case");
    TEST_ASSERT(eventTrace[0] == "OUTPUT_LEN_8", "Packet 2 output");
    TEST_ASSERT(eventTrace[1] == "OUTPUT_LEN_8", "Packet 3 output");
    TEST_ASSERT(eventTrace[2] == "OUTPUT_LEN_8", "Packet 4 output");
    TEST_ASSERT(eventTrace[3] == "DISC_REASON_2", "Discontinuity callback MUST precede packet 6!");
    TEST_ASSERT(eventTrace[4] == "OUTPUT_LEN_8", "Packet 6 output after discontinuity callback!");
    eventTrace.clear();

    // --- Subcase C: Lifecycle generation change while packets queued ---
    // Enqueue 2 packets in Gen 1
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);
    usb_audio_queue_enqueue(&queue, dummy1, 8, 1);

    // Host closes stream (Gen 2) and opens new stream (Gen 3)
    usb_audio_queue_advance_generation(&queue); // Gen 2 (closed)
    uint32_t gen3 = usb_audio_queue_advance_generation(&queue); // Gen 3 (reopened)
    TEST_ASSERT(gen3 == 3, "Gen must be 3");

    // Enqueue packet in Gen 3
    usb_audio_queue_enqueue(&queue, dummy1, 8, 3);

    // Run consumer in Gen 3:
    // Gen 1 packets must be discarded as stale.
    // DISC_REASON_1 must be called BEFORE Gen 3 packet is output!
    runConsumerDrain(3, true, lastDeliveredGen, lastDiscGen, streamDiscPending);

    TEST_ASSERT(eventTrace.size() == 2, "Trace must have exactly 2 events (disc + output of gen 3)");
    TEST_ASSERT(eventTrace[0] == "DISC_REASON_1", "Stream change discontinuity must precede gen 3 chunk!");
    TEST_ASSERT(eventTrace[1] == "OUTPUT_LEN_8", "Gen 3 chunk output after discontinuity!");
    eventTrace.clear();

    // --- Subcase D: Gap with no queued audio until next stream ---
    // Queue is empty. Stream stops (Gen 4).
    usb_audio_queue_advance_generation(&queue); // Gen 4
    runConsumerDrain(4, false, lastDeliveredGen, lastDiscGen, streamDiscPending);
    TEST_ASSERT(eventTrace.size() == 1, "Inactive empty queue drains and signals stream close");
    TEST_ASSERT(eventTrace[0] == "DISC_REASON_1", "Stream close disc signaled");
    eventTrace.clear();

    // Now stream 5 opens and enqueues a packet
    uint32_t gen5 = usb_audio_queue_advance_generation(&queue); // Gen 5
    usb_audio_queue_enqueue(&queue, dummy1, 8, gen5);

    runConsumerDrain(gen5, true, lastDeliveredGen, lastDiscGen, streamDiscPending);
    TEST_ASSERT(eventTrace.size() == 2, "Trace must have disc + output for gen 5");
    TEST_ASSERT(eventTrace[0] == "DISC_REASON_1", "Discontinuity signaled before first post-gap chunk of gen 5");
    TEST_ASSERT(eventTrace[1] == "OUTPUT_LEN_8", "Chunk delivered");

    return true;
}

/**
 * @brief Test 10: Multithreaded Race Stress Test with Concurrent Discontinuities & Audio Streaming.
 */
bool test_multithreaded_pipeline_race_stress() {
    usb_audio_queue_t queue;
    usb_audio_queue_init(&queue, 8);

    paxg::audio::PcmAudioDetector detector;
    paxg::audio::AudioDetectorConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameSize = 384;
    detector.init(cfg);

    paxg::uac::UacAudioBridge bridge(&detector);

    constexpr int NUM_ITERATIONS = 500;
    std::atomic<bool> done{false};
    std::atomic<size_t> totalDelivered{0};
    std::atomic<size_t> totalDisc{0};

    // Producer thread: enqueues audio and occasionally forces drops or advances generation
    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            uint32_t gen = (uint32_t)std::atomic_load_explicit(&queue.stream_generation, std::memory_order_acquire);
            auto packet = generateStereoStream(24, 500, 500);
            if (i % 7 == 0) {
                packet.push_back(0x55); // unaligned
            }
            usb_audio_queue_enqueue(&queue, packet.data(), packet.size(), gen);

            if (i % 50 == 0) {
                usb_audio_queue_advance_generation(&queue);
            }
            std::this_thread::yield();
        }
        done.store(true);
    });

    // Consumer thread: drains and feeds bridge
    std::thread consumer([&]() {
        uint32_t lastDeliveredGen = 0;
        uint32_t lastDiscGen = 0;
        bool streamDiscPending = false;
        uint8_t buf[USB_AUDIO_QUEUE_SLOT_SIZE];
        size_t len = 0;
        uint32_t pktGen = 0;
        uint32_t pktFlags = 0;

        while (!done.load() || !usb_audio_queue_is_empty(&queue)) {
            uint32_t activeGen = (uint32_t)std::atomic_load_explicit(&queue.stream_generation, std::memory_order_acquire);
            while (usb_audio_queue_dequeue_ext(&queue, buf, sizeof(buf), &len, &pktGen, &pktFlags)) {
                if (pktGen != activeGen) {
                    streamDiscPending = true;
                    continue;
                }

                uint32_t discReason = UAC_DISCONTINUITY_REASON_UNKNOWN;
                if (streamDiscPending || (lastDeliveredGen != 0 && pktGen != lastDeliveredGen) || (lastDiscGen != pktGen)) {
                    discReason = UAC_DISCONTINUITY_REASON_STREAM_CHANGE;
                    lastDiscGen = pktGen;
                    streamDiscPending = false;
                } else if (pktFlags & USB_AUDIO_SLOT_FLAG_DISCONTINUITY) {
                    discReason = UAC_DISCONTINUITY_REASON_OVERFLOW_DROP;
                }

                if (discReason != UAC_DISCONTINUITY_REASON_UNKNOWN) {
                    bridge.resetResidualOnStreamBreak();
                    totalDisc.fetch_add(1);
                }

                lastDeliveredGen = pktGen;
                bridge.consumeUacChunk(buf, len);
                totalDelivered.fetch_add(1);
            }
            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    TEST_ASSERT(totalDelivered.load() > 0, "Must have delivered audio chunks");
    TEST_ASSERT(totalDisc.load() > 0, "Must have processed discontinuity events");

    return true;
}

int main() {
    std::printf("=======================================================================\n");
    std::printf("PAXG UAC PCM to Production PcmAudioDetector Pipeline Native Tests\n");
    std::printf("=======================================================================\n");

    RUN_TEST(test_bridge_init_and_empty_packet_handling);
    RUN_TEST(test_arbitrary_chunk_sizes_and_residuals);
    RUN_TEST(test_stereo_mix_extremes);
    RUN_TEST(test_silence_vs_signal_distinction);
    RUN_TEST(test_timing_and_cooldown_behavior);
    RUN_TEST(test_stream_break_residual_reset);
    RUN_TEST(test_concurrent_audio_stream_and_control_access);
    RUN_TEST(test_deterministic_discontinuity_sample_isolation);
    RUN_TEST(test_end_to_end_consumer_pipeline_ordering);
    RUN_TEST(test_multithreaded_pipeline_race_stress);

    std::printf("=======================================================================\n");
    std::printf("Results: %d/%d passed (%d failed)\n", g_testsPassed, g_testsRun, g_testsFailed);
    std::printf("=======================================================================\n");

    return (g_testsFailed == 0) ? 0 : 1;
}
