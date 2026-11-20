#include "modules/audio/synthetic_audio_samples.h"
#include <cmath>
#include <cstring>

namespace paxg {
namespace audio {

void SyntheticAudioGenerator::generateSilence(int16_t* buffer, size_t count) {
    if (buffer == nullptr || count == 0) {
        return;
    }
    std::memset(buffer, 0, count * sizeof(int16_t));
}

void SyntheticAudioGenerator::generateNoise(int16_t* buffer, size_t count, int16_t maxAmplitude, uint32_t seed) {
    if (buffer == nullptr || count == 0) {
        return;
    }

    uint32_t state = seed;
    for (size_t i = 0; i < count; ++i) {
        // Linear congruential generator (LCG)
        state = state * 1664525u + 1013904223u;
        const int32_t raw = static_cast<int32_t>(state >> 16) - 32768; // [-32768, 32767]
        const int32_t scaled = (raw * maxAmplitude) / 32768;
        buffer[i] = static_cast<int16_t>(scaled);
    }
}

void SyntheticAudioGenerator::generateTone(int16_t* buffer, size_t count, float frequencyHz, uint32_t sampleRate, int16_t amplitude) {
    if (buffer == nullptr || count == 0 || sampleRate == 0) {
        return;
    }

    constexpr float TWO_PI = 6.283185307179586f;
    const float step = (frequencyHz * TWO_PI) / static_cast<float>(sampleRate);

    for (size_t i = 0; i < count; ++i) {
        const float val = std::sin(step * static_cast<float>(i)) * static_cast<float>(amplitude);
        int32_t clamped = static_cast<int32_t>(val);
        if (clamped > 32767) clamped = 32767;
        if (clamped < -32768) clamped = -32768;
        buffer[i] = static_cast<int16_t>(clamped);
    }
}

void SyntheticAudioGenerator::generateBurst(int16_t* buffer, size_t count,
                                            int16_t baselineAmp, int16_t burstAmp,
                                            size_t burstStart, size_t burstLength,
                                            uint32_t seed) {
    if (buffer == nullptr || count == 0) {
        return;
    }

    uint32_t state = seed;
    const size_t burstEnd = burstStart + burstLength;

    for (size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        const int32_t raw = static_cast<int32_t>(state >> 16) - 32768;
        const int16_t currentMaxAmp = (i >= burstStart && i < burstEnd) ? burstAmp : baselineAmp;
        const int32_t scaled = (raw * currentMaxAmp) / 32768;
        buffer[i] = static_cast<int16_t>(scaled);
    }
}

}  // namespace audio
}  // namespace paxg
