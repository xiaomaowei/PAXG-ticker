#include "uac_audio_bridge.h"
#include <cstring>
#include <algorithm>

namespace paxg {
namespace uac {

UacAudioBridge::UacAudioBridge(paxg::audio::IAudioDetector* externalDetector, MonoSink monoSink, RmsSink rmsSink)
    : detector_(nullptr),
      monoSink_(monoSink),
      rmsSink_(rmsSink),
      residualLen_(0),
      packetsReceived_(0),
      bytesReceived_(0),
      nullOrEmptyPackets_(0),
      stereoFramesDecoded_(0),
      monoSamplesIngested_(0),
      unalignedChunkCount_(0),
      residualDiscontinuityCount_(0),
      streamDiscontinuityCount_(0) {
    std::memset(residual_, 0, sizeof(residual_));
    std::memset(monoScratch_, 0, sizeof(monoScratch_));
    init(externalDetector);
}

void UacAudioBridge::init(paxg::audio::IAudioDetector* externalDetector,
                          const paxg::audio::AudioDetectorConfig* config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (externalDetector != nullptr) {
        detector_ = externalDetector;
    } else {
        detector_ = &defaultDetector_;
    }

    paxg::audio::AudioDetectorConfig effCfg;
    if (config != nullptr) {
        effCfg = *config;
    } else {
        // Production 48 kHz stereo-to-mono configuration:
        // 384 samples frame size = 8.0 ms at 48 kHz (matches 128 samples at 16 kHz)
        // debounceFrames = 2 (16.0 ms)
        // cooldownFrames = 25 (200.0 ms)
        effCfg.sampleRate = EXPECTED_SAMPLE_RATE;
        effCfg.frameSize = DEFAULT_FRAME_SIZE;
        effCfg.debounceFrames = 2;
        effCfg.cooldownFrames = 25;
        effCfg.noiseFloorAlpha = 0.05f;
        effCfg.thresholdRatio = 2.5f;
        effCfg.absoluteThreshold = 1500;
    }

    detector_->init(effCfg);
    resetLocked();
}

void UacAudioBridge::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    resetLocked();
}

void UacAudioBridge::resetLocked() {
    residualLen_ = 0;
    std::memset(residual_, 0, sizeof(residual_));
    std::memset(monoScratch_, 0, sizeof(monoScratch_));

    packetsReceived_ = 0;
    bytesReceived_ = 0;
    nullOrEmptyPackets_ = 0;
    stereoFramesDecoded_ = 0;
    monoSamplesIngested_ = 0;
    unalignedChunkCount_ = 0;
    residualDiscontinuityCount_ = 0;
    streamDiscontinuityCount_ = 0;

    if (detector_ != nullptr) {
        detector_->reset();
    }
    updateSnapshotLocked();
}

void UacAudioBridge::feedMonoLocked(size_t count) {
    const auto previousFrames = rmsSink_ ? detector_->getMetrics().totalFramesProcessed : 0;
    detector_->processSamples(monoScratch_, count);
    if (rmsSink_) {
        const auto metrics = detector_->getMetrics();
        if (metrics.totalFramesProcessed != previousFrames) rmsSink_(metrics.rms);
    }
    if (monoSink_) monoSink_(monoScratch_, count);
}

size_t UacAudioBridge::consumeUacChunk(const uint8_t* buf, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (buf == nullptr || len == 0) {
        nullOrEmptyPackets_++;
        updateSnapshotLocked();
        return 0;
    }

    packetsReceived_++;
    bytesReceived_ += len;

    size_t inIdx = 0;
    size_t framesDecodedThisCall = 0;
    size_t scratchCount = 0;

    // 1. Stage residual bytes from previous unaligned chunks to complete a 4-byte stereo frame
    if (residualLen_ > 0) {
        while (residualLen_ < BYTES_PER_FRAME && inIdx < len) {
            residual_[residualLen_++] = buf[inIdx++];
        }

        if (residualLen_ == BYTES_PER_FRAME) {
            const int16_t left = static_cast<int16_t>(
                static_cast<uint16_t>(residual_[0]) | (static_cast<uint16_t>(residual_[1]) << 8));
            const int16_t right = static_cast<int16_t>(
                static_cast<uint16_t>(residual_[2]) | (static_cast<uint16_t>(residual_[3]) << 8));
            const int32_t mixed = (static_cast<int32_t>(left) + static_cast<int32_t>(right)) / 2;

            monoScratch_[scratchCount++] = static_cast<int16_t>(mixed);
            stereoFramesDecoded_++;
            monoSamplesIngested_++;
            framesDecodedThisCall++;
            residualLen_ = 0;
        }
    }

    // 2. Decode whole 4-byte stereo frames directly from the incoming buffer
    while (inIdx + BYTES_PER_FRAME <= len) {
        const int16_t left = static_cast<int16_t>(
            static_cast<uint16_t>(buf[inIdx]) | (static_cast<uint16_t>(buf[inIdx + 1]) << 8));
        const int16_t right = static_cast<int16_t>(
            static_cast<uint16_t>(buf[inIdx + 2]) | (static_cast<uint16_t>(buf[inIdx + 3]) << 8));
        const int32_t mixed = (static_cast<int32_t>(left) + static_cast<int32_t>(right)) / 2;

        monoScratch_[scratchCount++] = static_cast<int16_t>(mixed);
        stereoFramesDecoded_++;
        monoSamplesIngested_++;
        framesDecodedThisCall++;
        inIdx += BYTES_PER_FRAME;

        if (scratchCount == SCRATCH_BUFFER_CAPACITY) {
            feedMonoLocked(scratchCount);
            scratchCount = 0;
        }
    }

    // Flush any pending mono samples to the detector
    if (scratchCount > 0) {
        feedMonoLocked(scratchCount);
        scratchCount = 0;
    }

    // 3. Stash remaining trailing bytes (1 to 3 bytes) into the residual buffer
    if (inIdx < len) {
        unalignedChunkCount_++;
        while (inIdx < len && residualLen_ < BYTES_PER_FRAME) {
            residual_[residualLen_++] = buf[inIdx++];
        }
    }

    // 4. Update coherent published snapshot
    updateSnapshotLocked();

    return framesDecodedThisCall;
}

void UacAudioBridge::resetResidualOnStreamBreak() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (residualLen_ > 0) {
        residualDiscontinuityCount_++;
        residualLen_ = 0;
        std::memset(residual_, 0, sizeof(residual_));
    }
    streamDiscontinuityCount_++;
    // Clear partial detector frame and noise state
    if (detector_ != nullptr) {
        detector_->reset();
    }
    updateSnapshotLocked();
}

void UacAudioBridge::updateSnapshotLocked() {
    publishedSnapshot_.packetsReceived = packetsReceived_;
    publishedSnapshot_.bytesReceived = bytesReceived_;
    publishedSnapshot_.nullOrEmptyPackets = nullOrEmptyPackets_;
    publishedSnapshot_.stereoFramesDecoded = stereoFramesDecoded_;
    publishedSnapshot_.monoSamplesIngested = monoSamplesIngested_;
    publishedSnapshot_.unalignedChunkCount = unalignedChunkCount_;
    publishedSnapshot_.residualDiscontinuityCount = residualDiscontinuityCount_;
    publishedSnapshot_.streamDiscontinuityCount = streamDiscontinuityCount_;
    if (detector_ != nullptr) {
        publishedSnapshot_.detectorMetrics = detector_->getMetrics();
    }
}

UacBridgeMetrics UacAudioBridge::getMetricsSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishedSnapshot_;
}

bool UacAudioBridge::isTriggered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishedSnapshot_.detectorMetrics.triggered;
}

void UacAudioBridge::clearTrigger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (detector_ != nullptr) {
        detector_->clearTrigger();
        updateSnapshotLocked();
    }
}

size_t UacAudioBridge::getResidualByteCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return residualLen_;
}

paxg::audio::AudioDetectorConfig UacAudioBridge::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (detector_ != nullptr) {
        return detector_->getConfig();
    }
    return paxg::audio::AudioDetectorConfig{};
}

void UacAudioBridge::updateConfig(const paxg::audio::AudioDetectorConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (detector_ != nullptr) {
        detector_->updateConfig(config);
        updateSnapshotLocked();
    }
}

}  // namespace uac
}  // namespace paxg
