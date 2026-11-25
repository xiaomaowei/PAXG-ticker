#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include "audio_match_reference.h"

namespace paxg::audio {

struct MatchSettings {
    uint32_t thresholdPercent = static_cast<uint32_t>(reference::threshold * 100 + 0.5f);
    uint32_t templateMask = 7;
    uint32_t spectral = 0;
    uint32_t debug = 0;
};

struct MatchMetrics {
    float rms = 0;
    float peak = 0;
    float scores[3]{};
    unsigned best = 0;
    unsigned events = 0;
    unsigned queuedFrames = 0;
    unsigned droppedFrames = 0;
    unsigned dspUs = 0;
    unsigned maxDspUs = 0;
    unsigned stackFree = 0;
    bool running = false;
};

void initAudioMatch();
void pushMatchPcm(const int16_t* samples, size_t count);
void resetMatchStream();
bool spectralMonitorSelected();
MatchSettings matchSettings();
MatchMetrics matchMetrics();
esp_err_t saveMatchSettings(const MatchSettings& settings);

}  // namespace paxg::audio
