#pragma once

#include <cstddef>
#include <cstdint>
#include "audio_match_reference.h"

namespace paxg::audio {

struct MatchMetrics {
    float rms = 0;
    float bagScore = 0;
};

class BagFullDetector;
void initAudioMatch(BagFullDetector& detector);
void pushMatchPcm(const int16_t* samples, size_t count);
void resetMatchStream();
MatchMetrics matchMetrics();

}  // namespace paxg::audio
