#pragma once

#include <cstddef>
#include <cstdint>
#include "fishing_state_machine.h"

namespace paxg {
namespace fishing {

enum class FishingPage : int {
    Dashboard = 0,
    AudioMonitor = 1,
    Settings = 2,
};

// Sane bounds for persistent settings
constexpr int32_t MIN_THRESHOLD = 500;
constexpr int32_t MAX_THRESHOLD = 5000;
constexpr int32_t DEFAULT_THRESHOLD = 1500;
constexpr int32_t STEP_THRESHOLD = 250;

constexpr uint32_t MIN_WAIT_TIMEOUT_MS = 5000;
constexpr uint32_t MAX_WAIT_TIMEOUT_MS = 30000;
constexpr uint32_t DEFAULT_WAIT_TIMEOUT_MS = 20000;
constexpr uint32_t STEP_WAIT_TIMEOUT_MS = 1000;

constexpr uint32_t MIN_COOLDOWN_MS = 500;
constexpr uint32_t MAX_COOLDOWN_MS = 5000;
constexpr uint32_t DEFAULT_COOLDOWN_MS = 2000;
constexpr uint32_t STEP_COOLDOWN_MS = 500;

constexpr uint8_t DEFAULT_CAST_KEY = '1';
constexpr uint8_t DEFAULT_INTERACT_KEY = '2';

// Allowed key choices for cycle selection in Settings UI
constexpr uint8_t ALLOWED_KEYS[] = {'1', '2', '3', '4', '5', '9', '0', ' '};
constexpr size_t ALLOWED_KEYS_COUNT = sizeof(ALLOWED_KEYS) / sizeof(ALLOWED_KEYS[0]);

struct FishingSettingsData {
    int32_t absoluteThreshold = DEFAULT_THRESHOLD;
    uint32_t waitTimeoutMs = DEFAULT_WAIT_TIMEOUT_MS;
    uint32_t cooldownMs = DEFAULT_COOLDOWN_MS;
    uint8_t castKey = DEFAULT_CAST_KEY;
    uint8_t interactKey = DEFAULT_INTERACT_KEY;

    void clamp() {
        if (absoluteThreshold < MIN_THRESHOLD) absoluteThreshold = MIN_THRESHOLD;
        if (absoluteThreshold > MAX_THRESHOLD) absoluteThreshold = MAX_THRESHOLD;

        if (waitTimeoutMs < MIN_WAIT_TIMEOUT_MS) waitTimeoutMs = MIN_WAIT_TIMEOUT_MS;
        if (waitTimeoutMs > MAX_WAIT_TIMEOUT_MS) waitTimeoutMs = MAX_WAIT_TIMEOUT_MS;

        if (cooldownMs < MIN_COOLDOWN_MS) cooldownMs = MIN_COOLDOWN_MS;
        if (cooldownMs > MAX_COOLDOWN_MS) cooldownMs = MAX_COOLDOWN_MS;

        bool validCast = false;
        for (size_t i = 0; i < ALLOWED_KEYS_COUNT; ++i) {
            if (castKey == ALLOWED_KEYS[i]) {
                validCast = true;
                break;
            }
        }
        if (!validCast) castKey = DEFAULT_CAST_KEY;

        bool validInteract = false;
        for (size_t i = 0; i < ALLOWED_KEYS_COUNT; ++i) {
            if (interactKey == ALLOWED_KEYS[i]) {
                validInteract = true;
                break;
            }
        }
        if (!validInteract) interactKey = DEFAULT_INTERACT_KEY;
    }
};

}  // namespace fishing
}  // namespace paxg
