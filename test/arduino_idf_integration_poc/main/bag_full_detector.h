#pragma once

#include <cstdint>
#include <mutex>

namespace paxg {
namespace audio {

/**
 * @brief Empirical calibration parameters for addon full-bag bell detection.
 *
 * Empirical Evidence Baseline:
 * Live audio recordings from the user's running environment confirm:
 * - Repetition period: 2.952s - 3.040s (~3.00s nominal)
 * - Bell chime peak RMS: > 17,000 (ambient/game background: < 500)
 * - Bell chime duration/decay: ~1.2s
 *
 * Note on Evidence & Calibration Disclaimer:
 * Without frequency-domain spectral analysis (e.g. 4.1 kHz bell harmonic filter),
 * time-domain temporal periodicity alone cannot guarantee 100% false-alarm rejection
 * against arbitrary game sounds matching the period. Therefore, this detector
 * provides a configurable, evidence-calibrated mechanism and fail-closed stop,
 * but explicitly disclaims uncalibrated detector reliability.
 */
struct BagFullCalibration {
    bool enabled = true;
    uint32_t bellIntervalMs = 3000;       // Empirical baseline: ~3.0s addon bell interval
    uint32_t intervalToleranceMs = 350;   // Acceptance window [2650ms, 3350ms]
    float minRmsThreshold = 3000.0f;      // Bell onset measured > 17000; ambient background < 500
    uint32_t debounceWindowMs = 500;      // Suppress multiple triggers during 1.2s bell ring decay
    uint8_t requiredMatches = 2;          // Number of consecutive interval matches (2 matches = 3 pulses, ~6s)
};

/**
 * @brief Coherent thread-safe metrics snapshot for telemetry and web UI.
 */
struct BagFullMetricsSnapshot {
    bool bagFullLatched = false;
    bool bellContinuing = false;
    uint32_t totalPulsesDetected = 0;
    uint32_t lastPulseTimeMs = 0;
    uint32_t lastIntervalMs = 0;
    uint32_t consecutiveMatches = 0;
    uint32_t triggerCount = 0;
    float lastPulseRms = 0.0f;
    BagFullCalibration config{};
};

/**
 * @brief Evidence-calibrated detector for periodic addon bag-full bell signals.
 *
 * All state is strictly protected by internal std::mutex.
 * Zero dynamic memory allocations.
 */
class BagFullDetector {
public:
    explicit BagFullDetector(const BagFullCalibration& cal = BagFullCalibration{});

    /**
     * @brief Ingest audio frame RMS at timestamp nowMs.
     */
    void processRms(uint32_t nowMs, float rms);

    /**
     * @brief Direct pulse registration (for unit tests and calibration simulation).
     */
    void registerPulse(uint32_t nowMs, float rms);

    /**
     * @brief Check whether full-bag stop condition has been latched.
     */
    bool isBagFull() const;

    /**
     * @brief Check whether bell is currently continuing to ring (within nominal interval + tolerance).
     *        Allows start gating even on a single pulse before multiple interval matches have occurred.
     */
    bool isBellContinuing(uint32_t nowMs) const;

    /**
     * @brief Clear latched bag-full condition.
     */
    void clearBagFull();

    /**
     * @brief Get thread-safe metrics snapshot.
     */
    BagFullMetricsSnapshot getMetrics() const;

    /**
     * @brief Update calibration configuration.
     */
    void updateCalibration(const BagFullCalibration& cal);

    /**
     * @brief Reset all internal state and counters.
     */
    void reset();

private:
    void registerPulseLocked(uint32_t nowMs, float rms);

    mutable std::mutex mutex_;
    BagFullCalibration config_;
    bool bagFullLatched_ = false;
    uint32_t totalPulsesDetected_ = 0;
    uint32_t lastPulseTimeMs_ = 0;
    uint32_t lastIntervalMs_ = 0;
    uint32_t consecutiveMatches_ = 0;
    uint32_t triggerCount_ = 0;
    float lastPulseRms_ = 0.0f;
};

} // namespace audio
} // namespace paxg
