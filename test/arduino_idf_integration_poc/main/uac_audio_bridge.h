#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include "modules/audio/audio_detector.h"

namespace paxg {
namespace uac {

/**
 * @brief Telemetry metrics emitted by the UAC PCM audio bridge.
 *
 * Provides non-secret runtime counters that distinguish between
 * a stream carrying digital silence vs. no USB audio packets at all,
 * as well as tracking unaligned chunks and stream discontinuity resets.
 */
struct UacBridgeMetrics {
    uint64_t packetsReceived = 0;              // Total UAC output callbacks invoked
    uint64_t bytesReceived = 0;                // Total raw bytes received across callbacks
    uint64_t nullOrEmptyPackets = 0;           // Number of invocations with null buffer or len == 0
    uint64_t stereoFramesDecoded = 0;          // Total 4-byte (16-bit stereo) frames successfully decoded
    uint64_t monoSamplesIngested = 0;          // Total mono samples fed to the detector
    uint64_t unalignedChunkCount = 0;          // Invocations that completed with unaligned residual bytes
    uint64_t residualDiscontinuityCount = 0;   // Invocations where residual bytes were flushed on stream break
    uint64_t streamDiscontinuityCount = 0;     // Total stream break and overflow discontinuity signals received
    paxg::audio::AudioDetectorMetrics detectorMetrics{}; // Coherent snapshot from production detector
};

/**
 * @brief Bounded, allocation-free bridge consuming UAC 2.0 48 kHz 16-bit stereo PCM
 *        into the production mono PcmAudioDetector.
 *
 * Concurrency & Thread-Safety Model:
 * - All mutable state (`detector_`, `residual_`, and all bridge runtime counters) is strictly
 *   synchronized under a single private `std::mutex`.
 * - Ingestion (`consumeUacChunk`) and control operations (`clearTrigger`, `init`, `reset`,
 *   `resetResidualOnStreamBreak`, `getResidualByteCount`, `getMetricsSnapshot`, `updateConfig`,
 *   `getConfig`) are fully thread-safe and mutually exclusive.
 * - Internal re-initialization is factored into `resetLocked()` to eliminate any deadlock risk
 *   between `init()` and `reset()`.
 * - Raw `IAudioDetector&` references are intentionally NOT exposed publicly to prevent outside callers
 *   from mutating the detector behind the bridge's back without holding the lock.
 * - Lifecycle Discontinuity & Stream Break API:
 *   Residual bytes (1-3 bytes) are preserved across consecutive callbacks during an active continuous stream.
 *   The method `resetResidualOnStreamBreak()` is provided as an explicit lifecycle API to flush pending fractional
 *   bytes and track discontinuities. Note: actual stream close, interface alternate setting changes, suspend,
 *   disconnect, or driver queue overflow signaling are not yet connected in the current composite driver callbacks,
 *   and require future lifecycle signaling hooks.
 */
class UacAudioBridge {
public:
    static constexpr uint32_t EXPECTED_SAMPLE_RATE = 48000;
    static constexpr size_t CHANNELS = 2;              // Stereo (Left, Right)
    static constexpr size_t BYTES_PER_SAMPLE = 2;      // 16-bit signed PCM
    static constexpr size_t BYTES_PER_FRAME = 4;       // 2 channels * 2 bytes = 4 bytes
    static constexpr size_t DEFAULT_FRAME_SIZE = 384;  // 8.0 ms at 48 kHz (matches 128 samples at 16 kHz)
    static constexpr size_t SCRATCH_BUFFER_CAPACITY = 128; // Bounded mono staging buffer

    using MonoSink = void (*)(const int16_t*, size_t);
    // For frame sizes at least SCRATCH_BUFFER_CAPACITY, receives every completed RMS frame.
    using RmsSink = void (*)(float);
    explicit UacAudioBridge(paxg::audio::IAudioDetector* externalDetector = nullptr,
                            MonoSink monoSink = nullptr, RmsSink rmsSink = nullptr);
    ~UacAudioBridge() = default;

    // Non-copyable, non-movable to ensure deterministic task ownership
    UacAudioBridge(const UacAudioBridge&) = delete;
    UacAudioBridge& operator=(const UacAudioBridge&) = delete;

    /**
     * @brief Thread-safe initialize or reconfigure bridge and detector.
     * @param externalDetector Optional pointer to external detector. If null, internal detector is used.
     * @param config Optional detector configuration (defaults to 48 kHz, 384 frame size, 25 cooldown).
     */
    void init(paxg::audio::IAudioDetector* externalDetector = nullptr,
              const paxg::audio::AudioDetectorConfig* config = nullptr);

    /**
     * @brief Thread-safe reset of all bridge counters, residual byte stage, and detector state.
     */
    void reset();

    /**
     * @brief Ingest an arbitrary-sized chunk of raw UAC PCM audio bytes.
     *
     * Fully synchronized under mutex: safe against concurrent clearTrigger/reset calls.
     * Guaranteed zero heap allocation, bounded execution time, and safe residual staging.
     *
     * @param buf Pointer to raw byte buffer.
     * @param len Buffer length in bytes.
     * @return Number of complete stereo frames decoded from this packet.
     */
    size_t consumeUacChunk(const uint8_t* buf, size_t len);

    /**
     * @brief Explicitly reset residual byte staging upon a USB stream interruption or interface reset.
     *
     * Prevents partial byte residuals of a stopped stream from corrupting the channel alignment
     * of a subsequent independent stream.
     */
    void resetResidualOnStreamBreak();

    /**
     * @brief Retrieve a thread-safe coherent snapshot of bridge and detector metrics.
     */
    UacBridgeMetrics getMetricsSnapshot() const;

    /**
     * @brief Thread-safe check whether detector has latched a trigger event.
     */
    bool isTriggered() const;

    /**
     * @brief Thread-safe clear of latched detector trigger event.
     */
    void clearTrigger();

    /**
     * @brief Thread-safe query of number of residual bytes currently held in staging (0 to 3).
     */
    size_t getResidualByteCount() const;

    /**
     * @brief Thread-safe retrieval of current detector configuration.
     */
    paxg::audio::AudioDetectorConfig getConfig() const;

    /**
     * @brief Thread-safe dynamic update of detector configuration.
     */
    void updateConfig(const paxg::audio::AudioDetectorConfig& config);

private:
    void feedMonoLocked(size_t count);
    void resetLocked();
    void updateSnapshotLocked();

    paxg::audio::IAudioDetector* detector_;
    const MonoSink monoSink_;
    const RmsSink rmsSink_;
    paxg::audio::PcmAudioDetector defaultDetector_;
    mutable std::mutex mutex_;

    // Residual byte buffer for packet fragmentation (max 3 leftover bytes)
    uint8_t residual_[4];
    size_t residualLen_;

    // Bounded mono staging buffer to batch feed detector::processSamples
    int16_t monoScratch_[SCRATCH_BUFFER_CAPACITY];

    // Bridge runtime counters
    uint64_t packetsReceived_;
    uint64_t bytesReceived_;
    uint64_t nullOrEmptyPackets_;
    uint64_t stereoFramesDecoded_;
    uint64_t monoSamplesIngested_;
    uint64_t unalignedChunkCount_;
    uint64_t residualDiscontinuityCount_;
    uint64_t streamDiscontinuityCount_;

    // Published snapshot protected by mutex_
    UacBridgeMetrics publishedSnapshot_;
};

}  // namespace uac
}  // namespace paxg
