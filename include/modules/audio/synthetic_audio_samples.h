#pragma once

#include <cstddef>
#include <cstdint>

namespace paxg {
namespace audio {

/**
 * @brief Synthetic PCM16 audio pattern generator for deterministic offline and unit testing.
 *
 * Provides repeatable waveforms, ambient water noise, transient noise glitches, and
 * threshold-exceeding sound bursts without external audio hardware or host dependencies.
 */
class SyntheticAudioGenerator {
public:
    /**
     * @brief Fill buffer with digital silence (value 0).
     */
    static void generateSilence(int16_t* buffer, size_t count);

    /**
     * @brief Generate repeatable pseudo-random white/ambient noise using a deterministic LCG.
     * @param buffer Destination buffer.
     * @param count Number of samples.
     * @param maxAmplitude Peak amplitude range [-maxAmplitude, maxAmplitude].
     * @param seed Random seed for repeatable test vectors.
     */
    static void generateNoise(int16_t* buffer, size_t count, int16_t maxAmplitude, uint32_t seed = 42);

    /**
     * @brief Generate pure sine tone.
     * @param buffer Destination buffer.
     * @param count Number of samples.
     * @param frequencyHz Frequency in Hz.
     * @param sampleRate Sampling rate in Hz.
     * @param amplitude Peak amplitude.
     */
    static void generateTone(int16_t* buffer, size_t count, float frequencyHz, uint32_t sampleRate, int16_t amplitude);

    /**
     * @brief Generate baseline ambient noise with an injected high-amplitude burst.
     * @param buffer Destination buffer.
     * @param count Total samples.
     * @param baselineAmp Ambient noise amplitude.
     * @param burstAmp Burst amplitude.
     * @param burstStart Sample index where burst begins.
     * @param burstLength Duration of burst in samples.
     * @param seed Deterministic noise seed.
     */
    static void generateBurst(int16_t* buffer, size_t count,
                              int16_t baselineAmp, int16_t burstAmp,
                              size_t burstStart, size_t burstLength,
                              uint32_t seed = 100);
};

}  // namespace audio
}  // namespace paxg
