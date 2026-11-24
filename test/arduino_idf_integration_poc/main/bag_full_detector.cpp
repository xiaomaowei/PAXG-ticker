#include "bag_full_detector.h"

namespace paxg {
namespace audio {

BagFullDetector::BagFullDetector(const BagFullCalibration& cal)
    : config_(cal) {
}

void BagFullDetector::processRms(uint32_t nowMs, float rms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) {
        return;
    }

    if (rms < config_.minRmsThreshold) {
        return;
    }

    registerPulseLocked(nowMs, rms);
}

void BagFullDetector::registerPulse(uint32_t nowMs, float rms) {
    std::lock_guard<std::mutex> lock(mutex_);
    registerPulseLocked(nowMs, rms);
}

void BagFullDetector::registerPulseLocked(uint32_t nowMs, float rms) {
    // 1. Debounce within the same ringing bell envelope
    if (lastPulseTimeMs_ > 0 && (nowMs - lastPulseTimeMs_) < config_.debounceWindowMs) {
        return;
    }

    totalPulsesDetected_++;
    lastPulseRms_ = rms;

    if (lastPulseTimeMs_ > 0) {
        const uint32_t interval = nowMs - lastPulseTimeMs_;
        lastIntervalMs_ = interval;

        const uint32_t minValid = (config_.bellIntervalMs >= config_.intervalToleranceMs)
                                      ? (config_.bellIntervalMs - config_.intervalToleranceMs)
                                      : 0;
        const uint32_t maxValid = config_.bellIntervalMs + config_.intervalToleranceMs;

        if (interval >= minValid && interval <= maxValid) {
            consecutiveMatches_++;
            if (consecutiveMatches_ >= config_.requiredMatches) {
                bagFullLatched_ = true;
                triggerCount_++;
            }
        } else {
            // Interval was out of the calibrated window: reset match streak
            consecutiveMatches_ = 0;
        }
    }

    lastPulseTimeMs_ = nowMs;
}

bool BagFullDetector::isBagFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bagFullLatched_;
}

bool BagFullDetector::isBellContinuing(uint32_t nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled || lastPulseTimeMs_ == 0) {
        return false;
    }
    const uint32_t maxInterval = config_.bellIntervalMs + config_.intervalToleranceMs;
    return (nowMs >= lastPulseTimeMs_) && ((nowMs - lastPulseTimeMs_) <= maxInterval);
}

void BagFullDetector::clearBagFull() {
    std::lock_guard<std::mutex> lock(mutex_);
    bagFullLatched_ = false;
    lastPulseTimeMs_ = 0;
    lastIntervalMs_ = 0;
    consecutiveMatches_ = 0;
}

BagFullMetricsSnapshot BagFullDetector::getMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BagFullMetricsSnapshot snap;
    snap.bagFullLatched = bagFullLatched_;
    snap.bellContinuing = false;
    snap.totalPulsesDetected = totalPulsesDetected_;
    snap.lastPulseTimeMs = lastPulseTimeMs_;
    snap.lastIntervalMs = lastIntervalMs_;
    snap.consecutiveMatches = consecutiveMatches_;
    snap.triggerCount = triggerCount_;
    snap.lastPulseRms = lastPulseRms_;
    snap.config = config_;
    return snap;
}

void BagFullDetector::updateCalibration(const BagFullCalibration& cal) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cal;
}

void BagFullDetector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    bagFullLatched_ = false;
    totalPulsesDetected_ = 0;
    lastPulseTimeMs_ = 0;
    lastIntervalMs_ = 0;
    consecutiveMatches_ = 0;
    triggerCount_ = 0;
    lastPulseRms_ = 0.0f;
}

} // namespace audio
} // namespace paxg
