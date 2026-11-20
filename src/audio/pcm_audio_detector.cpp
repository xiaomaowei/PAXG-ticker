#include "modules/audio/audio_detector.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace paxg {
namespace audio {

constexpr size_t PcmAudioDetector::RING_BUFFER_CAPACITY;
constexpr size_t PcmAudioDetector::MAX_FRAME_SIZE;

PcmAudioDetector::PcmAudioDetector()
    : latchedTrigger_(false) {
    std::memset(frameBuffer_, 0, sizeof(frameBuffer_));
    init(AudioDetectorConfig{});
}

PcmAudioDetector::PcmAudioDetector(const AudioDetectorConfig& config)
    : latchedTrigger_(false) {
    std::memset(frameBuffer_, 0, sizeof(frameBuffer_));
    init(config);
}

void PcmAudioDetector::init(const AudioDetectorConfig& config) {
    config_ = config;
    if (config_.frameSize == 0 || config_.frameSize > MAX_FRAME_SIZE) {
        config_.frameSize = 128;
    }
    if (config_.debounceFrames == 0) {
        config_.debounceFrames = 1;
    }
    reset();
}

void PcmAudioDetector::reset() {
    ringBuffer_.clear();
    latchedTrigger_ = false;

    metrics_.rms = 0.0f;
    metrics_.peak = 0;
    metrics_.noiseFloor = 100.0f;
    metrics_.effectiveThreshold = std::max(static_cast<float>(config_.absoluteThreshold),
                                           metrics_.noiseFloor * config_.thresholdRatio);
    metrics_.triggerCount = 0;
    metrics_.isAboveThreshold = false;
    metrics_.triggered = false;
    metrics_.consecutiveAbove = 0;
    metrics_.cooldownRemaining = 0;
    metrics_.totalFramesProcessed = 0;
    metrics_.totalSamplesProcessed = 0;
}

void PcmAudioDetector::processSamples(const int16_t* samples, size_t sampleCount) {
    if (samples == nullptr || sampleCount == 0) {
        return;
    }

    ringBuffer_.pushOverwrite(samples, sampleCount);
    metrics_.totalSamplesProcessed += sampleCount;

    const size_t effFrameSize = std::min(config_.frameSize, MAX_FRAME_SIZE);
    while (ringBuffer_.available() >= effFrameSize) {
        const size_t popped = ringBuffer_.pop(frameBuffer_, effFrameSize);
        if (popped == effFrameSize) {
            processSingleFrame(frameBuffer_, effFrameSize);
        }
    }
}

void PcmAudioDetector::processSingleFrame(const int16_t* frame, size_t frameSize) {
    metrics_.totalFramesProcessed++;

    uint32_t peak = 0;
    uint64_t sumSq = 0;

    for (size_t i = 0; i < frameSize; ++i) {
        const int32_t val = static_cast<int32_t>(frame[i]);
        const uint32_t absVal = static_cast<uint32_t>((val < 0) ? -val : val);
        if (absVal > peak) {
            peak = absVal;
        }
        sumSq += static_cast<uint64_t>(val * val);
    }

    const float rms = std::sqrt(static_cast<float>(sumSq) / static_cast<float>(frameSize));
    metrics_.rms = rms;
    metrics_.peak = peak;

    // Calculate effective threshold before decision
    const float dynThresh = metrics_.noiseFloor * config_.thresholdRatio;
    metrics_.effectiveThreshold = std::max(static_cast<float>(config_.absoluteThreshold), dynThresh);

    // Adaptive noise floor tracking (only adapt when quiet / outside burst & cooldown)
    if (metrics_.cooldownRemaining == 0 && rms < metrics_.effectiveThreshold) {
        metrics_.noiseFloor = (1.0f - config_.noiseFloorAlpha) * metrics_.noiseFloor + (config_.noiseFloorAlpha * rms);
        if (metrics_.noiseFloor < 1.0f) {
            metrics_.noiseFloor = 1.0f;
        }
    }

    // Cooldown, debounce, and trigger state machine
    if (metrics_.cooldownRemaining > 0) {
        metrics_.cooldownRemaining--;
        metrics_.consecutiveAbove = 0;
        metrics_.isAboveThreshold = (rms >= metrics_.effectiveThreshold);
    } else {
        if (rms >= metrics_.effectiveThreshold) {
            metrics_.isAboveThreshold = true;
            metrics_.consecutiveAbove++;
            if (metrics_.consecutiveAbove >= config_.debounceFrames) {
                latchedTrigger_ = true;
                metrics_.triggered = true;
                metrics_.triggerCount++;
                metrics_.cooldownRemaining = config_.cooldownFrames;
                metrics_.consecutiveAbove = 0;
            }
        } else {
            metrics_.isAboveThreshold = false;
            metrics_.consecutiveAbove = 0;
        }
    }
}

bool PcmAudioDetector::isTriggered() const {
    return latchedTrigger_;
}

void PcmAudioDetector::clearTrigger() {
    latchedTrigger_ = false;
    metrics_.triggered = false;
}

AudioDetectorMetrics PcmAudioDetector::getMetrics() const {
    AudioDetectorMetrics snap = metrics_;
    snap.triggered = latchedTrigger_;
    return snap;
}

const AudioDetectorConfig& PcmAudioDetector::getConfig() const {
    return config_;
}

void PcmAudioDetector::updateConfig(const AudioDetectorConfig& config) {
    config_ = config;
    if (config_.frameSize == 0 || config_.frameSize > MAX_FRAME_SIZE) {
        config_.frameSize = 128;
    }
    if (config_.debounceFrames == 0) {
        config_.debounceFrames = 1;
    }
}

}  // namespace audio
}  // namespace paxg
