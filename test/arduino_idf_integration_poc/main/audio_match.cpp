#include "audio_match.h"
#include "bag_audio_reference.h"
#include "bag_full_detector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

namespace paxg::audio {
namespace {
using namespace reference;

constexpr unsigned ringSize = 4096;
constexpr size_t taskStackBytes = 4096;
constexpr float tau = 6.2831853071795864769f;
constexpr float minEnergy = 1e-8f;
const char* const tag = "fishing_audio";

alignas(4) int16_t ring[ringSize]{};
std::atomic<unsigned> written{0}, consumed{0}, dropped{0}, generation{0}, mode{0};
std::mutex stateMutex;
MatchSettings settings;
MatchMetrics metrics;
TaskHandle_t task = nullptr;
BagFullDetector* bagFull = nullptr;

struct Analysis {
    int16_t pcm[fft]{};
    float hann[fft]{};
    float real[fft]{}, imag[fft]{};
    float cosine[fft / 2]{}, sine[fft / 2]{};
    float history[frames][bands]{};
    float energy[frames]{};
    unsigned pcmIndex = 0, filled = 0, next = 0, count = 0, cooldown = 0, bagCooldown = 0;

    void init() {
        for (int i = 0; i < fft; ++i) hann[i] = 0.5f - 0.5f * std::cos(tau * i / (fft - 1));
        for (int i = 0; i < fft / 2; ++i) {
            cosine[i] = std::cos(-tau * i / fft);
            sine[i] = std::sin(-tau * i / fft);
        }
    }

    void reset() { pcmIndex = filled = next = count = cooldown = bagCooldown = 0; }

    void transform(MatchMetrics& out) {
        float sumSquares = 0, peak = 0;
        unsigned rev = 0;
        for (unsigned i = 0; i < fft; ++i) {
            const float sample = pcm[(pcmIndex + i) % fft] / 32768.0f;
            sumSquares += sample * sample;
            peak = std::max(peak, std::abs(sample));
            real[rev] = sample * hann[i];
            imag[rev] = 0;
            unsigned bit = fft / 2;
            while (bit && (rev & bit)) {
                rev ^= bit;
                bit /= 2;
            }
            rev ^= bit;
        }
        out.rms = std::sqrt(sumSquares / fft);
        out.peak = peak;
        energy[next] = sumSquares / fft;
        if (energy[next] <= minEnergy) {
            std::fill_n(real, fft, 0.0f);
            return;
        }
        for (int width = 2; width <= fft; width *= 2) {
            for (int start = 0; start < fft; start += width) {
                for (int i = 0; i < width / 2; ++i) {
                    const int even = start + i, odd = even + width / 2;
                    const int k = i * (fft / width);
                    const float r = cosine[k] * real[odd] - sine[k] * imag[odd];
                    const float im = cosine[k] * imag[odd] + sine[k] * real[odd];
                    real[odd] = real[even] - r;
                    imag[odd] = imag[even] - im;
                    real[even] += r;
                    imag[even] += im;
                }
            }
        }
    }

    bool append(int16_t sample) {
        pcm[pcmIndex] = sample;
        pcmIndex = (pcmIndex + 1) % fft;
        ++filled;
        if (filled != fft) return false;
        filled -= hop;
        return true;
    }

    void match(const MatchSettings& config, MatchMetrics& out) {
        float normSquared = 0, total = 0;
        for (int band = 0; band < bands; ++band) {
            float power = 0;
            for (int k = edges[band]; k < edges[band + 1]; ++k)
                power += real[k] * real[k] + imag[k] * imag[k];
            normSquared += power;
            history[next][band] = std::sqrt(power);
            total += history[next][band];
        }
        const float scale = normSquared > 0 ? 1 / std::sqrt(normSquared) : 0;
        for (float& value : history[next]) value = (value - total / bands) * scale;
        next = (next + 1) % frames;
        count = std::min(count + 1, static_cast<unsigned>(frames));
        if (count < frames) return;
        float norm = 0, maxEnergy = 0, bagDot = 0, dots[3]{};
        for (int f = 0; f < frames; ++f) {
            const unsigned index = (next + f) % frames;
            maxEnergy = std::max(maxEnergy, energy[index]);
            for (int band = 0; band < bands; ++band) {
                const float value = history[index][band];
                norm += value * value;
                bagDot += value * bagTemplate[f][band];
                if (config.spectral) {
                    for (int t = 0; t < 3; ++t) dots[t] += value * templates[t][f][band];
                }
            }
        }
        const float inverse = norm > 0 ? 1 / std::sqrt(norm) : 0;
        out.bagScore = std::clamp(bagDot * inverse, -1.0f, 1.0f);
        if (bagCooldown) --bagCooldown;
        if (!bagCooldown && maxEnergy > minEnergy && out.bagScore >= bagThreshold) {
            bagFull->registerPulse(static_cast<uint32_t>(esp_timer_get_time() / 1000), out.rms * 32768.0f);
            bagCooldown = 130; // 1.30 s, longer than the 1.26 s source clip.
        }
        if (!config.spectral) return;
        for (unsigned t = 0; t < 3; ++t) {
            out.scores[t] = std::clamp(dots[t] * inverse, -1.0f, 1.0f);
            if (!(config.templateMask & (1u << t))) continue;
            if (!out.best || out.scores[t] > out.scores[out.best - 1]) out.best = t + 1;
        }
        if (cooldown) --cooldown;
        if (!cooldown && out.best && maxEnergy > minEnergy &&
            out.scores[out.best - 1] >= config.thresholdPercent / 100.0f) {
            ++out.events;
            cooldown = refractory;
            ESP_LOGI(tag, "event=%u template=%u score=%.3f", out.events, out.best, out.scores[out.best - 1]);
        }
    }
};

Analysis analyzer;

void matchTask(void*) {
    analyzer.init();
    unsigned stream = generation.load();
    MatchMetrics out{};
    out.running = true;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        metrics = out;
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        while (true) {
            unsigned read = consumed.load(std::memory_order_relaxed);
            const unsigned write = written.load(std::memory_order_acquire);
            const unsigned epoch = generation.load(std::memory_order_acquire);
            if (stream != epoch) {
                stream = epoch;
                consumed.store(write, std::memory_order_release);
                analyzer.reset();
                const unsigned events = out.events, maxUs = out.maxDspUs;
                out = {};
                out.running = true;
                out.events = events;
                out.maxDspUs = maxUs;
                std::lock_guard<std::mutex> lock(stateMutex);
                metrics = out;
                continue;
            }
            if (write - read < hop) break;
            const int64_t begin = esp_timer_get_time();
            bool frame = false;
            for (unsigned end = read + hop; read != end; ++read)
                frame |= analyzer.append(ring[read % ringSize]);
            consumed.store(read, std::memory_order_release);
            if (!frame) continue;
            const auto config = matchSettings();
            out.best = 0;
            std::fill_n(out.scores, 3, 0.0f);
            analyzer.transform(out);
            analyzer.match(config, out);
            out.dspUs = esp_timer_get_time() - begin;
            out.maxDspUs = std::max(out.maxDspUs, out.dspUs);
            if (config.debug) out.stackFree = uxTaskGetStackHighWaterMark(nullptr);
            std::lock_guard<std::mutex> lock(stateMutex);
            metrics = out;
        }
    }
}
}  // namespace

void initAudioMatch(BagFullDetector& detector) {
    bagFull = &detector;
    nvs_handle_t handle;
    if (nvs_open("paxg_audio", NVS_READONLY, &handle) == ESP_OK) {
        MatchSettings saved;
        size_t size = sizeof(saved);
        if (nvs_get_blob(handle, "match_v1", &saved, &size) == ESP_OK && size == sizeof(saved))
            settings = saved;
        nvs_close(handle);
    }
    if (xTaskCreate(matchTask, "fishing_dsp", taskStackBytes, nullptr, 1, &task) != pdPASS)
        ESP_LOGE(tag, "could not allocate DSP task");
    mode.store(settings.spectral);
}

void pushMatchPcm(const int16_t* samples, size_t count) {
    if (!task) return;
    const unsigned write = written.load(std::memory_order_relaxed);
    const unsigned read = consumed.load(std::memory_order_acquire);
    if (count > ringSize - (write - read)) {
        dropped.fetch_add(count, std::memory_order_relaxed);
        resetMatchStream();
        return;
    }
    const size_t start = write % ringSize;
    const size_t first = std::min(count, ringSize - start);
    std::memcpy(ring + start, samples, first * sizeof(*samples));
    std::memcpy(ring, samples + first, (count - first) * sizeof(*samples));
    written.store(write + count, std::memory_order_release);
    if (write - read < hop && write + count - read >= hop) xTaskNotifyGive(task);
}

void resetMatchStream() {
    generation.fetch_add(1, std::memory_order_release);
    if (task) xTaskNotifyGive(task);
}

bool spectralMonitorSelected() { return mode.load(std::memory_order_acquire) != 0; }

MatchSettings matchSettings() {
    std::lock_guard<std::mutex> lock(stateMutex);
    return settings;
}

MatchMetrics matchMetrics() {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto result = metrics;
    const auto read = consumed.load(std::memory_order_acquire);
    result.queuedFrames = written.load(std::memory_order_acquire) - read;
    result.droppedFrames = dropped.load(std::memory_order_relaxed);
    return result;
}

esp_err_t saveMatchSettings(const MatchSettings& value) {
    if (value.spectral && !task) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("paxg_audio", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "match_v1", &value, sizeof(value));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        std::lock_guard<std::mutex> lock(stateMutex);
        settings = value;
        mode.store(value.spectral, std::memory_order_release);
        resetMatchStream();
    }
    return err;
}
}  // namespace paxg::audio
