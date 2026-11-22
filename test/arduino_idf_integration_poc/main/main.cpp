#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "usb_audio_queue.h"
#include "usb_device_composite.h"
#include "uac_audio_bridge.h"
#include "live_fishing_controller.h"

// Production TFT_eSPI display and sprite objects
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite testSprite = TFT_eSprite(&tft);

// Retain compiler and linker symbol references for WiFi without invoking at runtime.
// Calling WiFi.begin() at runtime would persistently write placeholder credentials to NVS.
// This anchor retains WiFiSTAClass::begin and WiFiGenericClass::mode symbols for build/link
// verification without ever initializing the Wi-Fi radio or modifying flash/NVS.
void dummy_wifi_symbol_retention_anchor() {
    WiFi.mode(WIFI_OFF);
    WiFi.begin((const char*)nullptr, (const char*)nullptr);
}
void (* volatile g_wifi_anchor_ref)() = &dummy_wifi_symbol_retention_anchor;

// Bounded allocation-free UAC PCM bridge feeding production PcmAudioDetector
static paxg::uac::UacAudioBridge g_uac_audio_bridge;

// Audio output callback invoked from speaker FreeRTOS task (usb_spk_task)
static esp_err_t test_audio_output_cb(uint8_t *buf, size_t len, void *arg) {
    (void)arg;
    g_uac_audio_bridge.consumeUacChunk(buf, len);
    return ESP_OK;
}

// Audio discontinuity callback invoked from consumer task (usb_spk_task)
// BEFORE the first post-gap audio chunk is fed to the bridge.
static void test_audio_discontinuity_cb(uint32_t reason, void *arg) {
    (void)reason;
    (void)arg;
    g_uac_audio_bridge.resetResidualOnStreamBreak();
}

// Audio mute callback invoked strictly on host mute-control requests.
static void test_audio_set_mute_cb(uint32_t mute, void *arg) {
    (void)arg;
    (void)mute;
}

// Concrete hardware adapters for LiveFishingController
class CompositeHidKeyboard : public paxg::fishing::IHidKeyboard {
public:
    bool isMounted() override {
        return usb_composite_is_mounted();
    }
    bool isReady() override {
        return usb_composite_hid_ready();
    }
    bool press(uint8_t modifier, uint8_t keycode) override {
        return (usb_composite_hid_keyboard_press(modifier, keycode) == ESP_OK);
    }
    bool release() override {
        return (usb_composite_hid_keyboard_release() == ESP_OK);
    }
};

class ArduinoClock : public paxg::fishing::IClock {
public:
    uint32_t nowMs() override {
        return millis();
    }
};

class UacBridgeAudioStream : public paxg::fishing::IAudioStream {
public:
    explicit UacBridgeAudioStream(paxg::uac::UacAudioBridge* bridge) : bridge_(bridge) {}

    uint64_t getPacketsReceived() const override {
        return bridge_ ? bridge_->getMetricsSnapshot().packetsReceived : 0;
    }
    uint64_t getDiscontinuities() const override {
        if (!bridge_) return 0;
        const auto snap = bridge_->getMetricsSnapshot();
        return snap.streamDiscontinuityCount + snap.residualDiscontinuityCount;
    }
    uint32_t getQueueDrops() const override {
        usb_composite_audio_stats_t stats{};
        usb_composite_get_audio_stats(&stats);
        return stats.packets_dropped;
    }
    bool isTriggered() const override {
        return bridge_ ? bridge_->isTriggered() : false;
    }
    void clearTrigger() override {
        if (bridge_) bridge_->clearTrigger();
    }
    float getRms() const override {
        return bridge_ ? bridge_->getMetricsSnapshot().detectorMetrics.rms : 0.0f;
    }

private:
    paxg::uac::UacAudioBridge* bridge_;
};

static CompositeHidKeyboard s_composite_hid;
static ArduinoClock s_arduino_clock;
static UacBridgeAudioStream s_bridge_stream(&g_uac_audio_bridge);
static paxg::fishing::LiveFishingController g_fishing_controller(&s_composite_hid, &s_arduino_clock, &s_bridge_stream);

// Manual trigger helper for runtime keypress demonstration (retained for linker symbols)
void manual_trigger_keystroke_demo(uint8_t modifier, uint8_t keycode) {
    if (usb_composite_is_mounted() && usb_composite_hid_ready()) {
        usb_composite_hid_keyboard_press(modifier, keycode);
        delay(20);
        usb_composite_hid_keyboard_release();
    }
}
void (* volatile g_hid_demo_ref)(uint8_t, uint8_t) = &manual_trigger_keystroke_demo;

// Hardware button pins on LilyGo T-Display-S3 (active-low, internal pull-up)
constexpr int PIN_BUTTON_UP = 14;
constexpr int PIN_BUTTON_DOWN = 0;

void setup() {
    Serial.begin(115200);

    // Initialize physical buttons
    pinMode(PIN_BUTTON_UP, INPUT_PULLUP);
    pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);

    // 1. Initialize UAC 48kHz PCM Bridge and production PcmAudioDetector
    paxg::audio::AudioDetectorConfig audioCfg;
    audioCfg.sampleRate = 48000;
    audioCfg.frameSize = 384;      // 8.0 ms frame duration at 48 kHz (matches 128 samples at 16 kHz)
    audioCfg.debounceFrames = 2;   // 16.0 ms debounce verification
    audioCfg.cooldownFrames = 25;  // 200.0 ms lockout following trigger
    audioCfg.absoluteThreshold = 1500;
    audioCfg.thresholdRatio = 2.5f;
    audioCfg.noiseFloorAlpha = 0.05f;
    g_uac_audio_bridge.init(nullptr, &audioCfg);

    // 2. Initialize live fishing controller (defaults to Disarmed & Idle)
    g_fishing_controller.init();

    // 3. TFT_eSPI parallel 8-bit ST7789 display initialization
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("PAXG ticker IDF5", 10, 10);
    testSprite.createSprite(100, 50);
    testSprite.fillSprite(TFT_NAVY);
    testSprite.pushSprite(10, 30);

    // 4. Wi-Fi stack dormancy (NVS protection)
    (void)g_wifi_anchor_ref;

    // 5. Simultaneous USB Composite (UAC 2.0 Speaker + HID Keyboard) initialization
    usb_composite_config_t config = {
        .skip_tinyusb_init = false,
        .output_cb = test_audio_output_cb,
        .set_mute_cb = test_audio_set_mute_cb,
        .set_volume_cb = NULL,
        .cb_ctx = NULL,
        .discontinuity_cb = test_audio_discontinuity_cb,
    };
    usb_composite_init(&config);
}

void loop() {
    const uint32_t now = millis();

    // 1. Poll physical buttons and update live controller state machine
    const bool rawUpPressed = (digitalRead(PIN_BUTTON_UP) == LOW);
    const bool rawDownPressed = (digitalRead(PIN_BUTTON_DOWN) == LOW);
    g_fishing_controller.update(rawUpPressed, rawDownPressed);

    // 2. Rate-limited display and serial diagnostics (every 200ms for smooth state tracking)
    static uint32_t lastPrintMs = 0;
    if (now - lastPrintMs >= 200) {
        lastPrintMs = now;
        const auto snap = g_uac_audio_bridge.getMetricsSnapshot();
        usb_composite_audio_stats_t usb_stats{};
        usb_composite_get_audio_stats(&usb_stats);
        const bool mounted = usb_composite_is_mounted();

        static uint64_t lastPacketsReceived = 0;
        const uint64_t deltaPackets = (snap.packetsReceived >= lastPacketsReceived)
                                          ? (snap.packetsReceived - lastPacketsReceived)
                                          : snap.packetsReceived;
        lastPacketsReceived = snap.packetsReceived;

        // Periodic 1-second serial logging
        static uint32_t lastSerialLogMs = 0;
        if (now - lastSerialLogMs >= 1000) {
            lastSerialLogMs = now;
            const unsigned long remSec = (g_fishing_controller.getWaitRemainingMs() + 999) / 1000;
            Serial.printf("[FISHING_POC] arm=%d state=%s rem=%lus key=%d | usb=%d dpkts=%llu pkts=%llu | qdrop=%lu disc=%lu | rms=%.1f th=%.1f | casts=%lu bites=%lu reels=%lu | stop=%s\n",
                          g_fishing_controller.isArmed() ? 1 : 0,
                          paxg::fishing::getFishingStateName(g_fishing_controller.getState()),
                          remSec,
                          static_cast<int>(g_fishing_controller.getKeyState()),
                          mounted ? 1 : 0,
                          (unsigned long long)deltaPackets,
                          (unsigned long long)snap.packetsReceived,
                          (unsigned long)usb_stats.packets_dropped,
                          (unsigned long)(snap.streamDiscontinuityCount + snap.residualDiscontinuityCount),
                          snap.detectorMetrics.rms,
                          snap.detectorMetrics.effectiveThreshold,
                          static_cast<unsigned long>(g_fishing_controller.getTotalCasts()),
                          static_cast<unsigned long>(g_fishing_controller.getTotalBites()),
                          static_cast<unsigned long>(g_fishing_controller.getTotalReels()),
                          paxg::fishing::getStopReasonName(g_fishing_controller.getLastStopReason()));
        }

        // TFT LCD 320x170 High-Contrast Layout
        char buf[64];

        // Line 0 (y=2): Test Identification Banner
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("PAXG WoW Fishing PoC (UAC+HID)    ", 6, 2, 2);

        // Line 1 (y=20): Prominent Arm Status & Diagnostic Stop Indication
        const auto lastStopReason = g_fishing_controller.getLastStopReason();
        if (g_fishing_controller.isArmed()) {
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawString("ARM: [ARMED - SPACEBAR ACTIVE]    ", 6, 20, 2);
        } else if (g_fishing_controller.isCycleCompleted()) {
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawString("ARM: [DONE - CYCLE COMPLETED]     ", 6, 20, 2);
        } else if (lastStopReason == paxg::fishing::StopReason::Timeout) {
            tft.setTextColor(TFT_ORANGE, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - BITE_TIMEOUT]    ", 6, 20, 2);
        } else if (lastStopReason == paxg::fishing::StopReason::AudioDropout) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - AUDIO_DROPOUT]   ", 6, 20, 2);
        } else if (lastStopReason == paxg::fishing::StopReason::UsbUnmounted) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - USB_UNMOUNTED]   ", 6, 20, 2);
        } else if (lastStopReason == paxg::fishing::StopReason::QueueOverflowDrop) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - QUEUE_OVERFLOW]  ", 6, 20, 2);
        } else if (lastStopReason == paxg::fishing::StopReason::StreamDiscontinuity) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - STREAM_DISC]     ", 6, 20, 2);
        } else if (lastStopReason == paxg::fishing::StopReason::PressFailure) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - PRESS_FAILURE]   ", 6, 20, 2);
        } else if (lastStopReason == paxg::fishing::StopReason::UserStopped) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - USER_STOP]       ", 6, 20, 2);
        } else {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("ARM: [DISARMED - PASSIVE SAFE]    ", 6, 20, 2);
        }

        // Line 2 (y=38): FSM State, Countdown / Stop Reason, and Key Action
        const auto fsmState = g_fishing_controller.getState();
        const auto keyState = g_fishing_controller.getKeyState();
        const char* keyStateStr = (keyState == paxg::fishing::KeyState::Pressed)   ? "SPACE-DN"
                                : (keyState == paxg::fishing::KeyState::Releasing) ? "RELEASING"
                                                                                   : "IDLE";
        uint16_t stateColor = TFT_WHITE;
        switch (fsmState) {
            case paxg::fishing::FishingState::Idle: stateColor = TFT_LIGHTGREY; break;
            case paxg::fishing::FishingState::Casting: stateColor = TFT_CYAN; break;
            case paxg::fishing::FishingState::Waiting: stateColor = TFT_YELLOW; break;
            case paxg::fishing::FishingState::BiteDetected: stateColor = TFT_RED; break;
            case paxg::fishing::FishingState::Reeling: stateColor = TFT_GREEN; break;
            case paxg::fishing::FishingState::Cooldown: stateColor = TFT_DARKCYAN; break;
            case paxg::fishing::FishingState::Timeout: stateColor = TFT_ORANGE; break;
            default: stateColor = TFT_MAGENTA; break;
        }

        const uint32_t remMs = g_fishing_controller.getWaitRemainingMs();
        if (fsmState == paxg::fishing::FishingState::Waiting) {
            const unsigned long remSec = (remMs + 999) / 1000;
            snprintf(buf, sizeof(buf), "State: WAITING (%2lus) | Key: %-7s ",
                     remSec, keyStateStr);
        } else if (fsmState == paxg::fishing::FishingState::Idle && lastStopReason != paxg::fishing::StopReason::None) {
            snprintf(buf, sizeof(buf), "State: IDLE ( 0s) | Stop: %-10s ",
                     paxg::fishing::getStopReasonName(lastStopReason));
        } else {
            snprintf(buf, sizeof(buf), "State: %-10s | Key: %-9s ",
                     paxg::fishing::getFishingStateName(fsmState), keyStateStr);
        }
        tft.setTextColor(stateColor, TFT_BLACK);
        tft.drawString(buf, 6, 38, 2);

        // Line 3 (y=56): USB Mount & Audio Stream State
        const char* streamStr = (!mounted) ? "USB_UNMOUNT"
                              : (deltaPackets == 0) ? "NO_AUDIO"
                              : (snap.detectorMetrics.rms == 0.0f) ? "SILENCE"
                              : "STREAMING";
        snprintf(buf, sizeof(buf), "USB: %-7s | Strm: %-11s   ",
                 mounted ? "MOUNTED" : "UNMOUNT", streamStr);
        tft.setTextColor(mounted ? TFT_GREEN : TFT_RED, TFT_BLACK);
        tft.drawString(buf, 6, 56, 2);

        // Line 4 (y=74): Real Audio Levels (RMS, Peak, Effective Threshold)
        snprintf(buf, sizeof(buf), "RMS:%-5.1f Pk:%-5u Th:%-5.1f      ",
                 snap.detectorMetrics.rms,
                 (unsigned int)snap.detectorMetrics.peak,
                 snap.detectorMetrics.effectiveThreshold);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(buf, 6, 74, 2);

        // Line 5 (y=92): Session Progress Metrics
        snprintf(buf, sizeof(buf), "Cast:%-3lu Bite:%-3lu Reel:%-3lu Tout:%-2lu",
                 static_cast<unsigned long>(g_fishing_controller.getTotalCasts()),
                 static_cast<unsigned long>(g_fishing_controller.getTotalBites()),
                 static_cast<unsigned long>(g_fishing_controller.getTotalReels()),
                 static_cast<unsigned long>(g_fishing_controller.getTotalTimeouts()));
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(buf, 6, 92, 2);

        // Line 6 (y=110): Queue & Discontinuity Reliability
        snprintf(buf, sizeof(buf), "Pkts:%-6llu QDrop:%-2lu Disc:%-3llu  ",
                 (unsigned long long)snap.packetsReceived,
                 (unsigned long)usb_stats.packets_dropped,
                 (unsigned long long)(snap.streamDiscontinuityCount + snap.residualDiscontinuityCount));
        tft.setTextColor((usb_stats.packets_dropped > 0) ? TFT_MAGENTA : TFT_WHITE, TFT_BLACK);
        tft.drawString(buf, 6, 110, 2);

        // Line 7 (y=128): Energy Threshold Uncalibrated Warning Notice
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.drawString("WARN: Energy threshold uncalibrated!  ", 6, 128, 2);

        // Line 8 (y=146): Physical Controls Reference
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString("UP+DOWN (1s):ARM | UP:GO | DOWN:STOP  ", 6, 146, 2);
    }

    (void)g_hid_demo_ref;
    delay(10);
}

extern "C" void app_main() {
    initArduino();
    setup();
    while (true) {
        loop();
    }
}
