#pragma once

#include <cstddef>
#include <cstdint>
#include "audio_ring_buffer.h"

namespace paxg {
namespace audio {

/**
 * @brief Configuration parameters for PCM audio detector.
 */
struct AudioDetectorConfig {
    uint32_t sampleRate = 16000;         // Audio sample rate in Hz (e.g. 16000, 44100, 48000)
    size_t frameSize = 128;              // Frame size in samples for analysis (128 samples = 8ms at 16kHz)
    float noiseFloorAlpha = 0.05f;       // Exponential moving average tracking factor for ambient noise floor (0.01 - 0.20)
    float thresholdRatio = 2.5f;         // Dynamic multiplier above estimated noise floor to consider a frame above threshold
    int32_t absoluteThreshold = 1500;    // Minimum absolute RMS value required to trigger regardless of quiet noise floor
    uint32_t debounceFrames = 2;         // Number of consecutive frames above threshold required to confirm trigger
    uint32_t cooldownFrames = 25;        // Number of frames of mandatory lockout following a trigger event (~200ms at 16kHz/128)
};

/**
 * @brief Telemetry and operational metrics emitted by the audio detector.
 */
struct AudioDetectorMetrics {
    float rms = 0.0f;                    // Root Mean Square amplitude of the latest processed frame
    uint32_t peak = 0;                   // Peak absolute sample value in latest processed frame [0..32768] (capable of representing INT16_MIN magnitude 32768)
    float noiseFloor = 100.0f;           // Estimated adaptive ambient noise floor RMS
    float effectiveThreshold = 1500.0f;  // Dynamic threshold = max(absoluteThreshold, noiseFloor * thresholdRatio)
    uint32_t triggerCount = 0;           // Total count of verified trigger events
    bool isAboveThreshold = false;       // True if current frame exceeded dynamic threshold
    bool triggered = false;              // Latched trigger event flag (cleared on read/clearTrigger)
    uint32_t consecutiveAbove = 0;       // Consecutive frames currently above threshold
    uint32_t cooldownRemaining = 0;      // Number of cooldown frames remaining before re-arming
    uint64_t totalFramesProcessed = 0;   // Cumulative frames analyzed
    uint64_t totalSamplesProcessed = 0;  // Cumulative raw samples ingested
};

/**
 * @brief Abstract interface for modular audio event detectors.
 *
 * Allows interchangeable DSP implementations (e.g., energy/RMS detector, zero-crossing,
 * or future spectral analysis) without coupling to hardware or game logic.
 */
class IAudioDetector {
public:
    virtual ~IAudioDetector() = default;

    /**
     * @brief Initialize or reconfigure detector with configuration parameters.
     */
    virtual void init(const AudioDetectorConfig& config) = 0;

    /**
     * @brief Reset internal states, counters, metrics, and ring buffer to defaults.
     */
    virtual void reset() = 0;

    /**
     * @brief Process incoming raw PCM16 mono audio samples.
     *
     * Non-blocking. Ingests samples into the bounded ring buffer and executes
     * frame-based analysis for all complete frames available.
     *
     * @param samples Pointer to PCM16 signed mono samples.
     * @param sampleCount Number of samples available in buffer.
     */
    virtual void processSamples(const int16_t* samples, size_t sampleCount) = 0;

    /**
     * @brief Check whether a bite/burst trigger event has latched.
     */
    virtual bool isTriggered() const = 0;

    /**
     * @brief Clear the latched trigger event.
     */
    virtual void clearTrigger() = 0;

    /**
     * @brief Retrieve snapshot of current audio metrics and telemetry.
     */
    virtual AudioDetectorMetrics getMetrics() const = 0;

    /**
     * @brief Retrieve current detector configuration.
     */
    virtual const AudioDetectorConfig& getConfig() const = 0;

    /**
     * @brief Update configuration on the fly.
     */
    virtual void updateConfig(const AudioDetectorConfig& config) = 0;
};

/**
 * @brief Default platform-independent PCM16 mono Audio Detector.
 *
 * Implements bounded ring buffer ingestion, Peak, RMS, adaptive noise floor tracking,
 * dynamic thresholding, debounce verification, and cooldown gating.
 */
class PcmAudioDetector : public IAudioDetector {
public:
    static constexpr size_t RING_BUFFER_CAPACITY = 2048;
    static constexpr size_t MAX_FRAME_SIZE = 512;

    PcmAudioDetector();
    explicit PcmAudioDetector(const AudioDetectorConfig& config);
    ~PcmAudioDetector() override = default;

    void init(const AudioDetectorConfig& config) override;
    void reset() override;
    void processSamples(const int16_t* samples, size_t sampleCount) override;
    bool isTriggered() const override;
    void clearTrigger() override;
    AudioDetectorMetrics getMetrics() const override;
    const AudioDetectorConfig& getConfig() const override;
    void updateConfig(const AudioDetectorConfig& config) override;

    // Buffer diagnostics
    size_t getBufferedSampleCount() const { return ringBuffer_.available(); }

private:
    void processSingleFrame(const int16_t* frame, size_t frameSize);

    AudioDetectorConfig config_;
    AudioDetectorMetrics metrics_;
    PcmRingBuffer<RING_BUFFER_CAPACITY> ringBuffer_;
    int16_t frameBuffer_[MAX_FRAME_SIZE];
    bool latchedTrigger_;
};

}  // namespace audio
}  // namespace paxg
