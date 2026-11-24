#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>

#include "usb_audio_queue.h"
#include "usb_device_composite.h"
#include "uac_audio_bridge.h"
#include "live_fishing_controller.h"
#include "bag_full_detector.h"
#include "fishing_web_server.h"
#include "fishing_wifi_settings.h"
#include "paxg_async_quote.h"
#include "mode_navigator.h"

// Production TFT_eSPI display and double-buffered full-screen sprite
TFT_eSPI tft = TFT_eSPI();
static TFT_eSprite s_fishingSprite = TFT_eSprite(&tft);

// Retain compiler and linker symbol references for WiFi symbols
void dummy_wifi_symbol_retention_anchor() {
    WiFi.mode(WIFI_OFF);
    WiFi.begin((const char*)nullptr, (const char*)nullptr);
}
void (* volatile g_wifi_anchor_ref)() = &dummy_wifi_symbol_retention_anchor;

// Bounded allocation-free UAC PCM bridge feeding production PcmAudioDetector
static paxg::uac::UacAudioBridge g_uac_audio_bridge;

// Evidence-calibrated full-bag addon bell detector (3000ms +/- 350ms period, threshold > 3000)
static paxg::audio::BagFullDetector g_bag_full_detector;

// Audio output callback invoked from speaker FreeRTOS task (usb_spk_task)
static esp_err_t test_audio_output_cb(uint8_t *buf, size_t len, void *arg) {
    (void)arg;
    g_uac_audio_bridge.consumeUacChunk(buf, len);
    return ESP_OK;
}

// Audio discontinuity callback invoked from consumer task (usb_spk_task)
static void test_audio_discontinuity_cb(uint32_t reason, void *arg) {
    (void)reason;
    (void)arg;
    g_uac_audio_bridge.resetResidualOnStreamBreak();
}

// Audio mute callback strictly on host mute-control requests
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
    explicit UacBridgeAudioStream(paxg::uac::UacAudioBridge* bridge,
                                  paxg::audio::BagFullDetector* bagFull = nullptr)
        : bridge_(bridge), bagFull_(bagFull) {}

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
    bool isBagFull() const override {
        return bagFull_ ? bagFull_->isBagFull() : false;
    }
    bool isBellContinuing(uint32_t nowMs) const override {
        return bagFull_ ? bagFull_->isBellContinuing(nowMs) : false;
    }
    void clearBagFull() override {
        if (bagFull_) bagFull_->clearBagFull();
    }

private:
    paxg::uac::UacAudioBridge* bridge_;
    paxg::audio::BagFullDetector* bagFull_;
};

static CompositeHidKeyboard s_composite_hid;
static ArduinoClock s_arduino_clock;
static UacBridgeAudioStream s_bridge_stream(&g_uac_audio_bridge, &g_bag_full_detector);
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

// Retain linker symbols for PAXG and mode navigator
void dummy_paxg_symbol_retention_anchor() {
    paxg::market::getMarketQuoteSnapshot(0);
    paxg::market::startPaxgQuoteWorker();
    double p = 0, c = 0;
    paxg::market::parseBinanceTicker(nullptr, p, c);
    paxg::market::evaluateMarketSessions(0, 0, 0, false);
    paxg::ui::ModeNavigator nav;
    nav.update(false, false, 0, g_fishing_controller);
}
void (* volatile g_paxg_anchor_ref)() = &dummy_paxg_symbol_retention_anchor;

// Hardware button pins on LilyGo T-Display-S3 (active-low, internal pull-up)
constexpr int PIN_BUTTON_UP = 14;
constexpr int PIN_BUTTON_DOWN = 0;

// ----------------------------------------------------------------------------
// Real Fishing View Rendering (320x170 High-Contrast Layout with Local IP)
// ----------------------------------------------------------------------------
static void renderFishingScreen(uint32_t now, bool forceRedraw = false) {
    (void)now;
    const auto snap = g_uac_audio_bridge.getMetricsSnapshot();
    usb_composite_audio_stats_t usb_stats{};
    usb_composite_get_audio_stats(&usb_stats);
    const bool mounted = usb_composite_is_mounted();

    static uint64_t lastPacketsReceived = 0;
    const uint64_t deltaPackets = (snap.packetsReceived >= lastPacketsReceived)
                                      ? (snap.packetsReceived - lastPacketsReceived)
                                      : snap.packetsReceived;
    lastPacketsReceived = snap.packetsReceived;

    const auto fsmState = g_fishing_controller.getState();
    const auto lastStopReason = g_fishing_controller.getLastStopReason();
    const bool isArmed = g_fishing_controller.isArmed();
    const uint32_t casts = static_cast<uint32_t>(g_fishing_controller.getTotalCasts());
    const uint32_t bites = static_cast<uint32_t>(g_fishing_controller.getTotalBites());
    const uint32_t reels = static_cast<uint32_t>(g_fishing_controller.getTotalReels());

    // Calculate countdown remaining seconds
    unsigned long remSec = 0;
    if (fsmState == paxg::fishing::FishingState::Waiting) {
        const uint32_t remMs = g_fishing_controller.getWaitRemainingMs();
        remSec = (remMs + 999) / 1000;
    } else if (fsmState == paxg::fishing::FishingState::Cooldown) {
        const uint32_t duration = g_fishing_controller.getStateDurationMs();
        const uint32_t remMs = (duration < paxg::fishing::LiveFishingController::COOLDOWN_DURATION_MS)
                                   ? (paxg::fishing::LiveFishingController::COOLDOWN_DURATION_MS - duration)
                                   : 0;
        remSec = (remMs + 999) / 1000;
    }

    const uint8_t audioState = (!mounted) ? 0
                             : (deltaPackets == 0) ? 1
                             : (snap.detectorMetrics.rms == 0.0f) ? 2
                             : 3;

    // WiFi status and IP
    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    static bool s_lastWifiConnected = false;
    const bool wifiChanged = (wifiConnected != s_lastWifiConnected);
    s_lastWifiConnected = wifiConnected;

    const auto wifiStatus = paxg::wifi::getWifiSettingsManager().getStatus();
    static paxg::wifi::WifiState s_lastWifiState = paxg::wifi::WifiState::Uninitialized;
    const bool wifiStateChanged = (wifiStatus.state != s_lastWifiState);
    s_lastWifiState = wifiStatus.state;

    paxg::ui::FishingUiState currentUi{
        mounted,
        audioState,
        fsmState,
        lastStopReason,
        isArmed,
        remSec,
        casts,
        bites,
        reels
    };

    static paxg::ui::FishingUiState s_lastFishingUi;
    if (!forceRedraw && !wifiChanged && !wifiStateChanged && !currentUi.differsFrom(s_lastFishingUi)) {
        return; // Nothing changed, skip redraw completely
    }
    s_lastFishingUi = currentUi;

    // Header strings & colors
    const uint16_t usbColor = mounted ? TFT_GREEN : TFT_RED;
    const char* const audioStrs[] = { "NO USB  ", "NO AUDIO", "SILENCE ", "AUDIO OK" };
    const uint16_t audioColors[] = { TFT_RED, TFT_RED, TFT_YELLOW, TFT_GREEN };
    const char* audioStr = audioStrs[audioState];
    const uint16_t audioColor = audioColors[audioState];

    // FSM State Name and Color
    const char* stateStr = "IDLE    ";
    uint16_t stateColor = TFT_WHITE;
    switch (fsmState) {
        case paxg::fishing::FishingState::Idle:
            stateStr = "IDLE    ";
            stateColor = TFT_WHITE;
            break;
        case paxg::fishing::FishingState::Casting:
            stateStr = "CASTING ";
            stateColor = TFT_CYAN;
            break;
        case paxg::fishing::FishingState::Waiting:
            stateStr = "WAITING ";
            stateColor = TFT_WHITE;
            break;
        case paxg::fishing::FishingState::BiteDetected:
            stateStr = "BITE!   ";
            stateColor = TFT_RED;
            break;
        case paxg::fishing::FishingState::Reeling:
            stateStr = "REELING ";
            stateColor = TFT_GREEN;
            break;
        case paxg::fishing::FishingState::Cooldown:
            stateStr = "COOLDOWN";
            stateColor = TFT_DARKCYAN;
            break;
        case paxg::fishing::FishingState::Timeout:
            stateStr = "TIMEOUT ";
            stateColor = TFT_ORANGE;
            break;
        default:
            stateStr = "UNKNOWN ";
            stateColor = TFT_MAGENTA;
            break;
    }

    char countdownBuf[16];
    if (fsmState == paxg::fishing::FishingState::Waiting || fsmState == paxg::fishing::FishingState::Cooldown) {
        snprintf(countdownBuf, sizeof(countdownBuf), "%2lus", remSec);
    } else {
        snprintf(countdownBuf, sizeof(countdownBuf), " 0s");
    }

    char castBuf[12];
    snprintf(castBuf, sizeof(castBuf), "%lu", static_cast<unsigned long>(casts));
    char biteBuf[12];
    snprintf(biteBuf, sizeof(biteBuf), "%lu", static_cast<unsigned long>(bites));
    char reelBuf[12];
    snprintf(reelBuf, sizeof(reelBuf), "%lu", static_cast<unsigned long>(reels));

    char stopBuf[36];
    snprintf(stopBuf, sizeof(stopBuf), "STOP: %s", paxg::fishing::getStopReasonName(lastStopReason));
    const uint16_t stopColor = (lastStopReason == paxg::fishing::StopReason::None ||
                                lastStopReason == paxg::fishing::StopReason::CycleCompleted)
                                   ? TFT_LIGHTGREY
                                   : (lastStopReason == paxg::fishing::StopReason::BagFull)
                                         ? TFT_RED
                                         : (lastStopReason == paxg::fishing::StopReason::Timeout)
                                               ? TFT_ORANGE
                                               : TFT_RED;

    const char* footerStr = "HOLD UP+DN: ARM | UP: START | DOWN: STOP";
    const int footerX = 8;

    // Render into off-screen double-buffer sprite (Atomic full-frame update with zero flicker)
    if (s_fishingSprite.getPointer() != nullptr) {
        s_fishingSprite.fillSprite(TFT_BLACK);

        // Header (y=2..20)
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString("FISHING", 10, 2, 4);
        s_fishingSprite.setTextColor(usbColor, TFT_BLACK);
        s_fishingSprite.drawString(mounted ? "USB OK  " : "USB UNMT", 155, 4, 2);
        s_fishingSprite.setTextColor(audioColor, TFT_BLACK);
        s_fishingSprite.drawString(audioStr, 235, 4, 2);

        // Divider Line 1 (y=22)
        s_fishingSprite.drawFastHLine(0, 22, 320, TFT_CYAN);

        // State Name (y=32)
        s_fishingSprite.setTextColor(stateColor, TFT_BLACK);
        s_fishingSprite.drawString(stateStr, 12, 32, 4);

        // Arm Pill / Badge & Stop Reason (y=72..114)
        if (isArmed) {
            s_fishingSprite.fillRoundRect(12, 72, 90, 24, 5, TFT_GREEN);
            s_fishingSprite.setTextColor(TFT_BLACK, TFT_GREEN);
            s_fishingSprite.drawString("ARMED", 28, 76, 2);
        } else {
            s_fishingSprite.fillRoundRect(12, 72, 110, 24, 5, TFT_RED);
            s_fishingSprite.setTextColor(TFT_WHITE, TFT_RED);
            s_fishingSprite.drawString("DISARMED", 20, 76, 2);

            s_fishingSprite.setTextColor(stopColor, TFT_BLACK);
            s_fishingSprite.drawString(stopBuf, 12, 98, 2);
        }

        // Countdown Timer (Right Side, y=34, font 6)
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString(countdownBuf, 195, 34, 6);

        // LAN IP Display & Setup AP / PIN (Right of Arm pill, x=132)
        const char* tftPin = paxg::wifi::getWifiSettingsManager().getTftPin();
        if (wifiStatus.isSetupAp) {
            const char* apPw = paxg::wifi::getWifiSettingsManager().getTftApPassword();
            s_fishingSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
            s_fishingSprite.drawString("AP: " + String(wifiStatus.apSsid), 132, 60, 2);
            s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
            s_fishingSprite.drawString("PW: " + String(apPw), 132, 78, 2);
            s_fishingSprite.setTextColor(TFT_CYAN, TFT_BLACK);
            s_fishingSprite.drawString("192.168.4.1", 132, 96, 2);
            s_fishingSprite.setTextColor(TFT_ORANGE, TFT_BLACK);
            s_fishingSprite.drawString("PIN:" + String(tftPin), 245, 96, 2);
        } else if (wifiConnected) {
            String ipStr = WiFi.localIP().toString();
            s_fishingSprite.setTextColor(TFT_GREEN, TFT_BLACK);
            s_fishingSprite.drawString("IP: " + ipStr, 132, 74, 2);
            s_fishingSprite.setTextColor(TFT_CYAN, TFT_BLACK);
            s_fishingSprite.drawString("WEB:80 PIN:" + String(tftPin), 132, 96, 2);
        } else if (WiFi.getMode() != WIFI_OFF) {
            s_fishingSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
            s_fishingSprite.drawString("WiFi: CONNECTING...", 132, 74, 2);
        } else {
            s_fishingSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
            s_fishingSprite.drawString("WiFi: OFF", 132, 74, 2);
        }

        // Divider Line 2 (y=118)
        s_fishingSprite.drawFastHLine(0, 118, 320, TFT_CYAN);

        // Metrics Row (y=120..144)
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString("CAST", 8, 124, 2);
        s_fishingSprite.setTextColor(TFT_GREEN, TFT_BLACK);
        s_fishingSprite.drawString(castBuf, 56, 124, 2);

        s_fishingSprite.drawFastVLine(106, 120, 25, TFT_CYAN);
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString("BITE", 114, 124, 2);
        s_fishingSprite.setTextColor(TFT_GREEN, TFT_BLACK);
        s_fishingSprite.drawString(biteBuf, 162, 124, 2);

        s_fishingSprite.drawFastVLine(212, 120, 25, TFT_CYAN);
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString("REEL", 220, 124, 2);
        s_fishingSprite.setTextColor(TFT_GREEN, TFT_BLACK);
        s_fishingSprite.drawString(reelBuf, 268, 124, 2);

        // Divider Line 3 (y=146)
        s_fishingSprite.drawFastHLine(0, 146, 320, TFT_CYAN);

        // Footer (y=150)
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString(footerStr, footerX, 150, 2);

        // Atomic push of completed frame to ST7789 display
        s_fishingSprite.pushSprite(0, 0);
    }
}

// ----------------------------------------------------------------------------
// Setup & Loop
// ----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    // 1. Initialize physical buttons
    pinMode(PIN_BUTTON_UP, INPUT_PULLUP);
    pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);

    // 2. Initialize UAC 48kHz PCM Bridge and production PcmAudioDetector
    paxg::audio::AudioDetectorConfig audioCfg;
    audioCfg.sampleRate = 48000;
    audioCfg.frameSize = 384;      // 8.0 ms frame duration at 48 kHz
    audioCfg.debounceFrames = 2;   // 16.0 ms debounce verification
    audioCfg.cooldownFrames = 25;  // 200.0 ms lockout following trigger
    audioCfg.absoluteThreshold = 1500;
    audioCfg.thresholdRatio = 2.5f;
    audioCfg.noiseFloorAlpha = 0.05f;
    g_uac_audio_bridge.init(nullptr, &audioCfg);

    // 2b. Initialize evidence-calibrated BagFullDetector (3000ms +/- 350ms bell interval)
    paxg::audio::BagFullCalibration bagCal;
    bagCal.enabled = true;
    bagCal.bellIntervalMs = 3000;
    bagCal.intervalToleranceMs = 350;
    bagCal.minRmsThreshold = 3000.0f;
    bagCal.debounceWindowMs = 500;
    bagCal.requiredMatches = 2;
    g_bag_full_detector.updateCalibration(bagCal);

    // 3. Initialize live fishing controller (defaults to Disarmed & Idle)
    g_fishing_controller.init();

    // 4. TFT display initialization
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    // 5. Initialize full-screen double-buffer sprite for Real Fishing mode (106KB in RAM)
    s_fishingSprite.createSprite(320, 170);

    // 6. Wi-Fi initialization with dedicated NVS & recovery manager
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    paxg::wifi::getWifiSettingsManager().init(WIFI_SSID, WIFI_PASSWORD);
#else
    paxg::wifi::getWifiSettingsManager().init(nullptr, nullptr);
#endif

    // 7. Simultaneous USB Composite (UAC 2.0 Speaker + HID Keyboard) initialization
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

    // 0. Non-blocking Wi-Fi lifecycle & recovery state machine update
    paxg::wifi::getWifiSettingsManager().update(now);

    // 1. Drain & process pending web commands in the controller owner loop
    paxg::web::processPendingCommands(&g_fishing_controller, &g_bag_full_detector, now);

    // 2. Ingest audio RMS into BagFullDetector
    const auto audioSnap = g_uac_audio_bridge.getMetricsSnapshot();
    g_bag_full_detector.processRms(now, audioSnap.detectorMetrics.rms);

    // 3. Read physical buttons and update live fishing controller (prevent start during setup/apply)
    const bool rawUp = (digitalRead(PIN_BUTTON_UP) == LOW);
    const bool rawDown = (digitalRead(PIN_BUTTON_DOWN) == LOW);
    const bool allowStart = !paxg::wifi::getWifiSettingsManager().isSetupOrApplyActive();
    g_fishing_controller.update(rawUp && allowStart, rawDown);

    // 4. Publish synchronized status snapshot for web readers
    paxg::web::publishStatusSnapshot(&g_fishing_controller, &g_bag_full_detector, &g_uac_audio_bridge,
                                     WiFi.isConnected() ? WiFi.localIP().toString().c_str() : nullptr);

    // 5. Manage Fishing Web Server lifecycle (active for both STA and Setup AP)
    static bool s_webServerStarted = false;
    if (!s_webServerStarted) {
        if (paxg::web::startFishingWebServer(&g_fishing_controller, &g_bag_full_detector, &g_uac_audio_bridge, 80)) {
            s_webServerStarted = true;
            Serial.println("[WEB] Fishing Web Server started on port 80");
        }
    }

    // 4. Rate-limited UI display refresh (10 FPS tick)
    static uint32_t s_lastUiRefreshMs = 0;
    if (now - s_lastUiRefreshMs >= 100) {
        s_lastUiRefreshMs = now;
        renderFishingScreen(now);
    }

    // 5. Rate-limited 1-second serial telemetry diagnostics
    static uint32_t s_lastSerialLogMs = 0;
    if (now - s_lastSerialLogMs >= 1000) {
        s_lastSerialLogMs = now;
        const bool mounted = usb_composite_is_mounted();
        const auto bagSnap = g_bag_full_detector.getMetrics();

        Serial.printf("[FIRMWARE] arm=%d fsm=%s rem=%lums | usb=%d pkts=%llu rms=%.1f | bagFull=%d (dt=%lums, n=%lu) | stop=%s\n",
                      g_fishing_controller.isArmed() ? 1 : 0,
                      paxg::fishing::getFishingStateName(g_fishing_controller.getState()),
                      (unsigned long)g_fishing_controller.getWaitRemainingMs(),
                      mounted ? 1 : 0,
                      (unsigned long long)audioSnap.packetsReceived,
                      audioSnap.detectorMetrics.rms,
                      bagSnap.bagFullLatched ? 1 : 0,
                      (unsigned long)bagSnap.lastIntervalMs,
                      (unsigned long)bagSnap.totalPulsesDetected,
                      paxg::fishing::getStopReasonName(g_fishing_controller.getLastStopReason()));
    }

    (void)g_hid_demo_ref;
    (void)g_paxg_anchor_ref;
    delay(10);
}

extern "C" void app_main() {
    initArduino();
    setup();
    while (true) {
        loop();
    }
}
