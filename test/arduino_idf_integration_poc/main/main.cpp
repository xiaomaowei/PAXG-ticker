#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <atomic>
#include <mutex>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "usb_audio_queue.h"
#include "usb_device_composite.h"
#include "uac_audio_bridge.h"
#include "audio_match.h"
#include "live_fishing_controller.h"
#include "bag_full_detector.h"
#include "fishing_web_server.h"
#include "fishing_wifi_settings.h"
#include "paxg_async_quote.h"
#include "mode_navigator.h"
#include "ota_runtime.h"
#include "ota_server.h"
#include "fishing_stop_notification.h"

extern "C" bool tud_mounted(void);

// Production TFT_eSPI display and double-buffered full-screen sprite
TFT_eSPI tft = TFT_eSPI();
static TFT_eSprite s_fishingSprite = TFT_eSprite(&tft);

// Retain compiler and linker symbol references for WiFi symbols
void dummy_wifi_symbol_retention_anchor() {
    WiFi.mode(WIFI_OFF);
    WiFi.begin((const char*)nullptr, (const char*)nullptr);
}
void (* volatile g_wifi_anchor_ref)() = &dummy_wifi_symbol_retention_anchor;

// Evidence-calibrated full-bag addon bell detector (3000ms +/- 350ms period)
static paxg::audio::BagFullDetector g_bag_full_detector;

// Bounded allocation-free UAC PCM bridge feeding production PcmAudioDetector
static_assert(paxg::uac::UacAudioBridge::EXPECTED_SAMPLE_RATE == paxg::audio::reference::rate);
static paxg::uac::UacAudioBridge g_uac_audio_bridge(nullptr, paxg::audio::pushMatchPcm);
static std::atomic<bool> s_otaAudioPaused{false};
static std::atomic<uint32_t> s_audioDiscontinuities{0};

// Audio output callback invoked from speaker FreeRTOS task (usb_spk_task)
static esp_err_t test_audio_output_cb(uint8_t *buf, size_t len, void *arg) {
    (void)arg;
    if (!s_otaAudioPaused.load(std::memory_order_acquire)) {
        g_uac_audio_bridge.consumeUacChunk(buf, len);
    }
    return ESP_OK;
}

// Audio discontinuity callback invoked from consumer task (usb_spk_task)
static void test_audio_discontinuity_cb(uint32_t, void *arg) {
    (void)arg;
    g_uac_audio_bridge.resetResidualOnStreamBreak();
    paxg::audio::resetMatchStream();
    // Resume only after the dropped packet boundary has reached the USB consumer.
    s_audioDiscontinuities.fetch_add(1, std::memory_order_release);
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
        if (paxg::audio::spectralMonitorSelected()) return false;
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
        return s_audioDiscontinuities.load(std::memory_order_acquire);
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

// Production ModeNavigator coordinating PAXG Market and Real Fishing screens
static paxg::ui::ModeNavigator s_modeNavigator(paxg::ui::ScreenMode::PaxgMarket);

static std::mutex s_otaHealthMutex;
static paxg::ota::HealthSnapshot s_otaHealthSnapshot{};
static std::atomic<bool> s_otaInitialized{false};
static std::atomic<bool> s_otaHold{false};
static bool s_webServerStarted = false;
static bool s_usbInitSucceeded = false;
static bool s_displayReady = false;
static bool s_quoteWorkerStarted = false;
static uint32_t s_ownerLoopCount = 0;
static uint32_t s_ownerLoopLastMs = 0;
static std::atomic<uint32_t> s_otaFailureShownAt{0};
static std::atomic<bool> s_otaFailureTimerStarted{false};
static bool s_navigationReleaseRequired = false;
static bool s_credentialsVisible = false;
static bool s_credentialsRedrawNeeded = false;
static uint32_t s_credentialsShownAt = 0;
static uint32_t s_upHoldStartedAt = 0;
static bool s_upHoldTiming = false;
static bool s_upRevealConsumed = false;
static bool s_prevRawUp = false;
static bool s_prevRawDown = false;
static uint32_t s_pendingValidationStartedAt = 0;
static uint32_t s_healthySince = 0;
static bool s_rollbackAttempted = false;
static esp_timer_handle_t s_rollbackWatchdog = nullptr;

static void pendingValidationWatchdog(void*) {
    // Give the rollback-enabled bootloader one reboot opportunity if the owner loop stalls.
    esp_restart();
}

static void startPendingValidationWatchdog() {
    if (!paxg::ota::pendingValidation() || s_rollbackWatchdog) return;
    const esp_timer_create_args_t args = {
        .callback = pendingValidationWatchdog,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_validate_guard",
        .skip_unhandled_events = true
    };
    if (esp_timer_create(&args, &s_rollbackWatchdog) == ESP_OK) {
        esp_timer_start_once(s_rollbackWatchdog, 120000000);
    }
}

static void stopPendingValidationWatchdog() {
    if (s_rollbackWatchdog) esp_timer_stop(s_rollbackWatchdog);
}

static bool otaMaintenanceBegin(uint32_t timeoutMs) {
    auto& runtime = paxg::ota::getOtaRuntime();
    if (!s_otaInitialized) return false;
    s_otaHold = true;
    paxg::web::setMaintenanceActive(true);
    if (paxg::wifi::getWifiSettingsManager().isSetupOrApplyActive() ||
        !runtime.requestMaintenance("HTTP_OTA_REQUEST")) {
        s_otaHold = false;
        paxg::web::setMaintenanceActive(false);
        s_navigationReleaseRequired = true;
        return false;
    }
    paxg::market::setPaxgQuotePaused(true);
    s_otaAudioPaused.store(true, std::memory_order_release);
    paxg::audio::resetMatchStream();

    const uint32_t started = millis();
    while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
        const auto state = runtime.getState();
        if (state == paxg::ota::OtaState::MaintenanceActive) return true;
        if (state == paxg::ota::OtaState::Failed || state == paxg::ota::OtaState::Idle) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    runtime.cancelMaintenanceRequest();
    s_otaHold = false;
    paxg::web::setMaintenanceActive(false);
    paxg::market::setPaxgQuotePaused(false);
    s_otaAudioPaused.store(false, std::memory_order_release);
    g_uac_audio_bridge.resetResidualOnStreamBreak();
    s_navigationReleaseRequired = true;
    return false;
}

static void otaMaintenanceLeave(bool success) {
    paxg::ota::getOtaRuntime().leaveMaintenance(success, millis());
    if (!success) {
        s_otaFailureShownAt = millis();
        s_otaFailureTimerStarted = true;
    }
}

static void otaProgress(size_t written, size_t total) {
    paxg::ota::getOtaRuntime().updateProgress(written, total, millis());
}

static void otaRequestRestart() {
    paxg::ota::getOtaRuntime().requestRestart(millis());
}

static void otaHealthSnapshot(paxg::ota::HealthSnapshot* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(s_otaHealthMutex);
    *out = s_otaHealthSnapshot;
}

static void updateOtaHealthSnapshot(uint32_t now, bool ownerLoopResponsive) {
    paxg::ota::HealthSnapshot snapshot{};
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    snapshot.healthy = s_usbInitSucceeded && s_displayReady && s_quoteWorkerStarted &&
                       s_ownerLoopCount > 10 && ownerLoopResponsive && wifiConnected &&
                       paxg::ota::isRunning();
    snapshot.uptimeSec = now / 1000;
    snapshot.freeHeap = esp_get_free_heap_size();
    snapshot.minFreeHeap = esp_get_minimum_free_heap_size();
    snapshot.totalPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snapshot.freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snapshot.usbMounted = usb_composite_is_mounted();
    snapshot.usbHidReady = usb_composite_hid_ready();
    usb_composite_audio_stats_t audioStats{};
    static uint32_t lastPacketCount = 0;
    static uint32_t lastPacketAt = 0;
    if (usb_composite_get_audio_stats(&audioStats) == ESP_OK &&
        audioStats.packets_delivered != lastPacketCount) {
        lastPacketCount = audioStats.packets_delivered;
        lastPacketAt = now;
    }
    snapshot.usbAudioStreaming = snapshot.usbMounted && lastPacketAt != 0 &&
                                 static_cast<uint32_t>(now - lastPacketAt) < 5000;
    if (wifiConnected) {
        snapshot.rssi = static_cast<int8_t>(WiFi.RSSI());
        const String ip = WiFi.localIP().toString();
        std::snprintf(snapshot.localIp, sizeof(snapshot.localIp), "%s", ip.c_str());
    }
    std::snprintf(snapshot.wifiMode, sizeof(snapshot.wifiMode), "%s",
                  (WiFi.getMode() & WIFI_AP) ? ((WiFi.getMode() & WIFI_STA) ? "APSTA" : "AP") : "STA");
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) std::snprintf(snapshot.activeSlot, sizeof(snapshot.activeSlot), "%.7s", running->label);
    std::snprintf(snapshot.bootStateTxt, sizeof(snapshot.bootStateTxt), "%s",
                  paxg::ota::pendingValidation() ? "pending_validation" : "valid");
    std::snprintf(snapshot.operatingMode, sizeof(snapshot.operatingMode), "%s",
                  s_modeNavigator.getMode() == paxg::ui::ScreenMode::PaxgMarket ? "paxg" : "fishing");
    {
        std::lock_guard<std::mutex> lock(s_otaHealthMutex);
        s_otaHealthSnapshot = snapshot;
    }
}

static void initOtaOnceRadioIsActive() {
    if (!s_otaInitialized) {
        const paxg::ota::RuntimeHooks hooks{
            otaMaintenanceBegin, otaMaintenanceLeave, otaProgress, otaRequestRestart, otaHealthSnapshot
        };
        if (paxg::ota::init(hooks) == ESP_OK) s_otaInitialized = true;
    }
    if (s_otaInitialized && WiFi.status() == WL_CONNECTED) {
        esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) paxg::ota::start(sta);
    }
}

// Hardware button pins on LilyGo T-Display-S3 (active-low, internal pull-up)
constexpr int PIN_BUTTON_UP = 14;
constexpr int PIN_BUTTON_DOWN = 0;

// ----------------------------------------------------------------------------
// PAXG Market View Rendering (320x170 High-Contrast Layout with Local IP)
// ----------------------------------------------------------------------------
static void renderPaxgScreen(uint32_t now, bool forceRedraw = false) {
    const auto quoteSnap = paxg::market::getMarketQuoteSnapshot(now);
    const bool mounted = usb_composite_is_mounted();
    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    const auto wifiStatus = paxg::wifi::getWifiSettingsManager().getStatus();

    static double s_lastPrice = -1.0;
    static paxg::market::QuoteStatus s_lastStatus = paxg::market::QuoteStatus::Unavailable;
    static bool s_lastMounted = false;
    static bool s_lastWifi = false;

    const bool stateChanged = (quoteSnap.price != s_lastPrice) ||
                              (quoteSnap.status != s_lastStatus) ||
                              (mounted != s_lastMounted) ||
                              (wifiConnected != s_lastWifi);

    if (!forceRedraw && !stateChanged) {
        return;
    }
    s_lastPrice = quoteSnap.price;
    s_lastStatus = quoteSnap.status;
    s_lastMounted = mounted;
    s_lastWifi = wifiConnected;

    if (s_fishingSprite.getPointer() != nullptr) {
        s_fishingSprite.fillSprite(TFT_BLACK);

        // Header (y=2..20)
        s_fishingSprite.setTextColor(TFT_GOLD, TFT_BLACK);
        s_fishingSprite.drawString("PAXG / USD", 10, 2, 4);

        const uint16_t usbColor = mounted ? TFT_GREEN : TFT_RED;
        s_fishingSprite.setTextColor(usbColor, TFT_BLACK);
        s_fishingSprite.drawString(mounted ? "USB OK  " : "USB UNMT", 155, 4, 2);

        const char* statusStr = paxg::market::getQuoteStatusName(quoteSnap.status);
        const uint16_t statusColor = (quoteSnap.status == paxg::market::QuoteStatus::Fresh) ? TFT_GREEN
                                   : (quoteSnap.status == paxg::market::QuoteStatus::Stale) ? TFT_YELLOW
                                   : TFT_RED;
        s_fishingSprite.setTextColor(statusColor, TFT_BLACK);
        s_fishingSprite.drawString(statusStr, 245, 4, 2);

        // Divider Line 1 (y=22)
        s_fishingSprite.drawFastHLine(0, 22, 320, TFT_CYAN);

        // Price Display (y=30, Font 6 / 4)
        char priceBuf[32];
        if (quoteSnap.status != paxg::market::QuoteStatus::Unavailable && quoteSnap.price > 0.0) {
            snprintf(priceBuf, sizeof(priceBuf), "$%.2f", quoteSnap.price);
        } else {
            snprintf(priceBuf, sizeof(priceBuf), "$---.--");
        }
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString(priceBuf, 12, 30, 6);

        // 24h Change Percent (y=40, font 4)
        char changeBuf[32];
        if (quoteSnap.status != paxg::market::QuoteStatus::Unavailable) {
            snprintf(changeBuf, sizeof(changeBuf), "%+.2f%%", quoteSnap.percentChange);
            const uint16_t changeColor = (quoteSnap.percentChange >= 0.0) ? TFT_GREEN : TFT_RED;
            s_fishingSprite.setTextColor(changeColor, TFT_BLACK);
        } else {
            snprintf(changeBuf, sizeof(changeBuf), "--.---%%");
            s_fishingSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        }
        s_fishingSprite.drawString(changeBuf, 210, 42, 4);

        // Divider Line 2 (y=80)
        s_fishingSprite.drawFastHLine(0, 80, 320, TFT_DARKGREY);

        // Mini Price History Chart (x=12, y=85, w=120, h=28)
        s_fishingSprite.drawRect(12, 85, 120, 28, TFT_NAVY);
        if (quoteSnap.historyCount > 1) {
            double minP = quoteSnap.priceHistory[0];
            double maxP = quoteSnap.priceHistory[0];
            for (size_t i = 1; i < quoteSnap.historyCount && i < paxg::market::MAX_PRICE_HISTORY; ++i) {
                if (quoteSnap.priceHistory[i] < minP) minP = quoteSnap.priceHistory[i];
                if (quoteSnap.priceHistory[i] > maxP) maxP = quoteSnap.priceHistory[i];
            }
            double range = maxP - minP;
            if (range <= 0.001) range = 1.0;
            const int step = 110 / (quoteSnap.historyCount - 1);
            int prevX = 14;
            int prevY = 110 - static_cast<int>((quoteSnap.priceHistory[0] - minP) / range * 22.0);
            for (size_t i = 1; i < quoteSnap.historyCount && i < paxg::market::MAX_PRICE_HISTORY; ++i) {
                int curX = 14 + i * step;
                int curY = 110 - static_cast<int>((quoteSnap.priceHistory[i] - minP) / range * 22.0);
                s_fishingSprite.drawLine(prevX, prevY, curX, curY, TFT_GOLD);
                prevX = curX;
                prevY = curY;
            }
        } else {
            s_fishingSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
            s_fishingSprite.drawString("No History", 24, 92, 2);
        }

        // LAN IP Display & Setup AP / PIN (Right side, x=142, y=82)
        const char* tftPin = paxg::wifi::getWifiSettingsManager().getTftPin();
        if (wifiStatus.isSetupAp) {
            const char* apPw = paxg::wifi::getWifiSettingsManager().getTftApPassword();
            s_fishingSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
            s_fishingSprite.drawString("AP: " + String(wifiStatus.apSsid), 142, 82, 2);
            s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
            s_fishingSprite.drawString("PW:" + String(apPw) + " PIN:" + String(tftPin), 142, 98, 2);
        } else if (wifiConnected) {
            String ipStr = WiFi.localIP().toString();
            s_fishingSprite.setTextColor(TFT_GREEN, TFT_BLACK);
            s_fishingSprite.drawString("IP: " + ipStr, 142, 82, 2);
            s_fishingSprite.setTextColor(TFT_CYAN, TFT_BLACK);
            s_fishingSprite.drawString("WEB:80 PIN:" + String(tftPin), 142, 98, 2);
        } else if (WiFi.getMode() != WIFI_OFF) {
            s_fishingSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
            s_fishingSprite.drawString("WiFi: CONNECTING...", 142, 86, 2);
        } else {
            s_fishingSprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
            s_fishingSprite.drawString("WiFi: OFF", 142, 86, 2);
        }

        // Divider Line 3 (y=118)
        s_fishingSprite.drawFastHLine(0, 118, 320, TFT_CYAN);

        // Hardware & Memory Row (y=122..144)
        const auto mem = paxg::ota::getOtaRuntime().getMemoryMetrics();
        char memBuf[64];
        snprintf(memBuf, sizeof(memBuf), "PSRAM:%s (%uKB free) | Heap:%uKB",
                 mem.psramAvailable ? "8MB" : "NONE",
                 static_cast<unsigned int>(mem.psramFree / 1024),
                 static_cast<unsigned int>(mem.freeInternalHeap / 1024));
        s_fishingSprite.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        s_fishingSprite.drawString(memBuf, 10, 124, 2);

        // Divider Line 4 (y=146)
        s_fishingSprite.drawFastHLine(0, 146, 320, TFT_CYAN);

        // Footer (y=150)
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
        s_fishingSprite.drawString("DOWN: FISHING MODE | OTA TARGET ARMED", 10, 150, 2);

        // Push frame to ST7789
        s_fishingSprite.pushSprite(0, 0);
    }
}

// ----------------------------------------------------------------------------
// OTA Progress View Rendering (320x170 High-Contrast Layout)
// ----------------------------------------------------------------------------
static void renderOtaProgressScreen(uint32_t now) {
    (void)now;
    const auto prog = paxg::ota::getOtaRuntime().getProgress();

    if (s_fishingSprite.getPointer() != nullptr) {
        s_fishingSprite.fillSprite(TFT_BLACK);

        // Header (y=2..20)
        s_fishingSprite.setTextColor(TFT_CYAN, TFT_BLACK);
        s_fishingSprite.drawString("FIRMWARE OTA", 10, 2, 4);

        const char* stateName = paxg::ota::getOtaStateName(prog.state);
        const uint16_t stateColor = (prog.state == paxg::ota::OtaState::Failed) ? TFT_RED
                                  : (prog.state == paxg::ota::OtaState::PendingReboot) ? TFT_GREEN
                                  : TFT_YELLOW;
        s_fishingSprite.setTextColor(stateColor, TFT_BLACK);
        s_fishingSprite.drawString(stateName, 175, 4, 2);

        // Divider Line 1 (y=22)
        s_fishingSprite.drawFastHLine(0, 22, 320, TFT_CYAN);

        // Status or Error (y=32..55)
        if (prog.state == paxg::ota::OtaState::Failed) {
            s_fishingSprite.setTextColor(TFT_RED, TFT_BLACK);
            s_fishingSprite.drawString("FAILED: " + String(prog.failureReason), 12, 32, 2);
            s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
            s_fishingSprite.drawString("REVERTED TO SAFE DISARMED MODE", 12, 50, 2);
        } else {
            s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
            s_fishingSprite.drawString(prog.statusMessage[0] ? prog.statusMessage : "UPDATING FIRMWARE...", 12, 32, 4);
        }

        // Progress Bar (y=70..94, w=296, h=24)
        s_fishingSprite.drawRect(12, 70, 296, 24, TFT_WHITE);
        const uint16_t barWidth = static_cast<uint16_t>((292 * std::min<uint8_t>(100, prog.percent)) / 100);
        if (barWidth > 0) {
            const uint16_t barColor = (prog.state == paxg::ota::OtaState::Failed) ? TFT_RED : TFT_GREEN;
            s_fishingSprite.fillRect(14, 72, barWidth, 20, barColor);
        }

        // Progress Byte Count (y=100..114)
        char byteBuf[64];
        if (prog.totalBytes > 0) {
            snprintf(byteBuf, sizeof(byteBuf), "%u%% (%u KB / %u KB)",
                     prog.percent,
                     static_cast<unsigned int>(prog.bytesWritten / 1024),
                     static_cast<unsigned int>(prog.totalBytes / 1024));
        } else {
            snprintf(byteBuf, sizeof(byteBuf), "%u%% (%u bytes written)",
                     prog.percent,
                     static_cast<unsigned int>(prog.bytesWritten));
        }
        s_fishingSprite.setTextColor(TFT_CYAN, TFT_BLACK);
        s_fishingSprite.drawString(byteBuf, 14, 100, 2);

        // Divider Line 2 (y=118)
        s_fishingSprite.drawFastHLine(0, 118, 320, TFT_CYAN);

        // Safety Status Row (y=124..144)
        s_fishingSprite.fillRoundRect(12, 122, 110, 22, 4, TFT_RED);
        s_fishingSprite.setTextColor(TFT_WHITE, TFT_RED);
        s_fishingSprite.drawString("DISARMED", 20, 125, 2);

        s_fishingSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
        s_fishingSprite.drawString("USB LIVE | ANALYSIS PAUSED", 132, 125, 2);

        // Divider Line 3 (y=146)
        s_fishingSprite.drawFastHLine(0, 146, 320, TFT_CYAN);

        // Footer (y=150)
        s_fishingSprite.setTextColor(TFT_ORANGE, TFT_BLACK);
        s_fishingSprite.drawString("DO NOT POWER OFF DEVICE DURING WRITE", 10, 150, 2);

        // Push frame to ST7789
        s_fishingSprite.pushSprite(0, 0);
    }
}

static void renderOtaCredentialsScreen() {
    const char* token = paxg::ota::getToken();
    uint8_t mac[6] = {};
    char deviceId[24] = "STA MAC: unavailable";
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        std::snprintf(deviceId, sizeof(deviceId), "STA MAC: %02X%02X%02X%02X%02X%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    if (!s_fishingSprite.getPointer()) return;
    s_fishingSprite.fillSprite(TFT_BLACK);
    s_fishingSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    s_fishingSprite.drawString("OTA TOKEN (LCD ONLY)", 8, 4, 2);
    s_fishingSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    for (size_t row = 0; row < 4; ++row) {
        char tokenLine[17] = {};
        if (token) std::snprintf(tokenLine, sizeof(tokenLine), "%.16s", token + row * 16);
        s_fishingSprite.drawString(tokenLine, 8, 26 + static_cast<int>(row) * 20, 2);
    }
    s_fishingSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_fishingSprite.drawString(deviceId, 8, 112, 2);
    s_fishingSprite.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    s_fishingSprite.drawString("AUTO HIDE 30s | PRESS BUTTON TO HIDE", 8, 142, 2);
    s_fishingSprite.pushSprite(0, 0);
}

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
    audioCfg.frameSize = paxg::uac::UacAudioBridge::DEFAULT_FRAME_SIZE; // 8 ms at 48 kHz
    static_assert(paxg::uac::UacAudioBridge::DEFAULT_FRAME_SIZE >=
                  paxg::uac::UacAudioBridge::SCRATCH_BUFFER_CAPACITY);
    audioCfg.debounceFrames = 4;   // Require 32 ms above threshold for a bite
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
    paxg::audio::initAudioMatch(g_bag_full_detector);

    // 3. Initialize live fishing controller (defaults to Disarmed & Idle)
    g_fishing_controller.init();
    paxg::notification::initStopNotification();
    g_fishing_controller.setBagFullStopCallback(paxg::notification::onActiveBagFullStop);

    // 4. TFT display initialization
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    // 5. Initialize full-screen double-buffer sprite for Real Fishing mode (106KB in RAM)
    s_fishingSprite.createSprite(320, 170);
    s_displayReady = s_fishingSprite.getPointer() != nullptr;

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
    s_usbInitSucceeded = usb_composite_init(&config) == ESP_OK;

    // 8. Start PAXG market quote async background worker (Core 0, Priority 1)
    paxg::market::startPaxgQuoteWorker();
    s_quoteWorkerStarted = paxg::market::isPaxgQuoteWorkerRunning();
    startPendingValidationWatchdog();

    // 9. Instrument task initialization and memory metrics (internal SRAM + 8MB Octal PSRAM)
    const auto mem = paxg::ota::getOtaRuntime().getMemoryMetrics();
    Serial.printf("[INIT] Task initialization complete\n");
    Serial.printf("[INIT] USB composite mounted=%d\n", usb_composite_is_mounted() ? 1 : 0);
    Serial.printf("[INIT] PSRAM avail=%d total=%u free=%u | Heap free=%u minFree=%u\n",
                  mem.psramAvailable ? 1 : 0,
                  static_cast<unsigned int>(mem.psramTotal),
                  static_cast<unsigned int>(mem.psramFree),
                  static_cast<unsigned int>(mem.freeInternalHeap),
                  static_cast<unsigned int>(mem.minFreeInternalHeap));
}

void loop() {
    const uint32_t now = millis();

    auto& wifiManager = paxg::wifi::getWifiSettingsManager();
    auto& otaRuntime = paxg::ota::getOtaRuntime();
    if (!s_otaHold) wifiManager.update(now);

    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (wifiConnected) initOtaOnceRadioIsActive(); // Uses the actual WIFI_STA_DEF netif; no stop on link loss.

    const bool ownerLoopResponsive = !s_ownerLoopLastMs ||
                                     static_cast<uint32_t>(now - s_ownerLoopLastMs) < 1000;
    ++s_ownerLoopCount;
    const bool usbActuallyMounted = tud_mounted(); // Suspended remains mounted and must wait for HID readiness.
    const bool wasFailure = otaRuntime.hasFailed();
    otaRuntime.pollOwnerLoop(now, g_fishing_controller, s_composite_hid, usbActuallyMounted);
    const bool nowFailed = otaRuntime.hasFailed();
    if (nowFailed && !s_otaFailureTimerStarted) {
        s_otaFailureShownAt = now;
        s_otaFailureTimerStarted = true;
    }
    if (wasFailure && !nowFailed) s_otaFailureTimerStarted = false;

    // A bounded error screen is shown before returning disarmed to the prior mode.
    if (nowFailed && s_otaFailureTimerStarted && static_cast<uint32_t>(now - s_otaFailureShownAt) >= 5000) {
        otaRuntime.exitMaintenance(now);
        s_otaHold = false;
        s_otaFailureTimerStarted = false;
        paxg::web::setMaintenanceActive(false);
        paxg::market::setPaxgQuotePaused(false);
        s_otaAudioPaused.store(false, std::memory_order_release);
        g_uac_audio_bridge.resetResidualOnStreamBreak();
    }

    const bool spectralMonitor = paxg::audio::spectralMonitorSelected();
    if (spectralMonitor && (g_fishing_controller.isArmed() ||
        g_fishing_controller.getState() != paxg::fishing::FishingState::Idle)) {
        g_fishing_controller.requestStop(paxg::fishing::StopReason::UserStopped);
        g_fishing_controller.disarm();
    }

    // Drain only the emergency stop while maintenance is held; start/arm queues were purged.
    paxg::web::processPendingCommands(&g_fishing_controller, &g_bag_full_detector, now);

    const bool rawUp = (digitalRead(PIN_BUTTON_UP) == LOW);
    const bool rawDown = (digitalRead(PIN_BUTTON_DOWN) == LOW);
    const bool upPressEdge = rawUp && !s_prevRawUp;
    const bool downPressEdge = rawDown && !s_prevRawDown;
    const bool upReleaseEdge = !rawUp && s_prevRawUp;

    // LCD-only token reveal: hold UP for 3s in PAXG mode while OTA is ready.
    if (s_credentialsVisible && (downPressEdge || (upPressEdge && !s_upHoldTiming))) {
        s_credentialsVisible = false;
        s_navigationReleaseRequired = true;
        s_credentialsRedrawNeeded = true;
    }
    if (s_credentialsVisible && (!paxg::ota::isReady() || s_otaHold || wifiManager.isSetupOrApplyActive() ||
                                 s_modeNavigator.getMode() != paxg::ui::ScreenMode::PaxgMarket)) {
        s_credentialsVisible = false;
        s_navigationReleaseRequired = true;
        s_credentialsRedrawNeeded = true;
    }
    if (s_credentialsVisible && static_cast<uint32_t>(now - s_credentialsShownAt) >= 30000) {
        s_credentialsVisible = false;
        s_navigationReleaseRequired = true;
        s_credentialsRedrawNeeded = true;
    }
    const bool paxgMode = s_modeNavigator.getMode() == paxg::ui::ScreenMode::PaxgMarket;
    if (paxgMode && paxg::ota::isReady() && !s_otaHold && !wifiManager.isSetupOrApplyActive() && rawUp && !rawDown) {
        if (!s_upHoldTiming) {
            s_upHoldTiming = true;
            s_upHoldStartedAt = now;
        } else if (static_cast<uint32_t>(now - s_upHoldStartedAt) >= 3000 && !s_credentialsVisible && !s_upRevealConsumed) {
            s_credentialsVisible = true;
            s_credentialsShownAt = now;
            s_upRevealConsumed = true;
        }
    } else {
        s_upHoldTiming = false;
    }
    if (upReleaseEdge) {
        s_upHoldTiming = false;
        s_upRevealConsumed = false;
    }

    const bool navPaused = s_otaHold || otaRuntime.isMaintenanceActive() || otaRuntime.hasFailed();
    if (navPaused) {
        s_navigationReleaseRequired = true;
    } else if (s_navigationReleaseRequired) {
        if (!rawUp && !rawDown) {
            s_modeNavigator = paxg::ui::ModeNavigator(s_modeNavigator.getMode());
            s_navigationReleaseRequired = false;
        }
    } else if (!s_credentialsVisible) {
        const bool allowInput = !wifiManager.isSetupOrApplyActive();
        s_modeNavigator.update(rawUp && allowInput && !spectralMonitor, rawDown, now, g_fishing_controller);
    }
    s_prevRawUp = rawUp;
    s_prevRawDown = rawDown;

    // 6. Publish synchronized status snapshot for web readers
    paxg::web::publishStatusSnapshot(&g_fishing_controller, &g_bag_full_detector, &g_uac_audio_bridge,
                                     WiFi.isConnected() ? WiFi.localIP().toString().c_str() : nullptr);

    // 7. Manage Fishing Web Server lifecycle (active for both STA and Setup AP)
    if (!s_webServerStarted) {
        if (paxg::web::startFishingWebServer(&g_fishing_controller, &g_bag_full_detector, &g_uac_audio_bridge, 80)) {
            s_webServerStarted = true;
            Serial.println("[WEB] Fishing Web Server started on port 80");
        }
    }

    if (s_otaInitialized) paxg::ota::tick();
    updateOtaHealthSnapshot(now, ownerLoopResponsive);
    s_ownerLoopLastMs = now;

    // Validate only after sustained local health; on failure request the official rollback once.
    if (paxg::ota::pendingValidation()) {
        if (!s_pendingValidationStartedAt) s_pendingValidationStartedAt = now;
        const bool healthy = s_otaHealthSnapshot.healthy;
        if (healthy) {
            if (!s_healthySince) s_healthySince = now;
            if (static_cast<uint32_t>(now - s_healthySince) >= 15000) {
                const esp_err_t valid = esp_ota_mark_app_valid_cancel_rollback();
                if (valid == ESP_OK) {
                    s_pendingValidationStartedAt = 0;
                    s_healthySince = 0;
                    stopPendingValidationWatchdog();
                    paxg::ota::logEvent("boot health stable; image marked valid");
                } else {
                    paxg::ota::logEvent("mark valid failed; retaining recovery mode");
                }
            }
        } else {
            s_healthySince = 0;
        }
        if (s_pendingValidationStartedAt && !s_rollbackAttempted &&
            static_cast<uint32_t>(now - s_pendingValidationStartedAt) >= 90000) {
            s_rollbackAttempted = true;
            const esp_err_t rollback = esp_ota_mark_app_invalid_rollback_and_reboot();
            if (rollback != ESP_OK) {
                paxg::ota::logEvent("rollback request failed; staying available for recovery");
            }
        }
    }

    if (otaRuntime.restartDue(now)) esp_restart();

    // 8. Rate-limited UI display refresh (10 FPS tick)
    static uint32_t s_lastUiRefreshMs = 0;
    if (now - s_lastUiRefreshMs >= 100) {
        s_lastUiRefreshMs = now;
        const bool modeChanged = s_modeNavigator.isModeChanged();

        if (s_otaHold || otaRuntime.isMaintenanceActive() || otaRuntime.hasFailed()) {
            renderOtaProgressScreen(now);
        } else if (s_credentialsVisible) {
            renderOtaCredentialsScreen();
        } else if (s_modeNavigator.getMode() == paxg::ui::ScreenMode::PaxgMarket) {
            renderPaxgScreen(now, modeChanged || s_credentialsRedrawNeeded);
            s_credentialsRedrawNeeded = false;
        } else {
            renderFishingScreen(now, modeChanged || s_credentialsRedrawNeeded);
            s_credentialsRedrawNeeded = false;
        }

        if (modeChanged) {
            s_modeNavigator.clearModeChanged();
        }
    }

    // 9. Rate-limited 1-second serial telemetry diagnostics
    static uint32_t s_lastSerialLogMs = 0;
    if (now - s_lastSerialLogMs >= 1000) {
        s_lastSerialLogMs = now;
        const bool mounted = usb_composite_is_mounted();
        const auto bagSnap = g_bag_full_detector.getMetrics();
        const auto audioSnap = g_uac_audio_bridge.getMetricsSnapshot();
        const auto otaProg = paxg::ota::getOtaRuntime().getProgress();

        Serial.printf("[FIRMWARE] mode=%s arm=%d fsm=%s rem=%lums | usb=%d pkts=%llu rms=%.1f | bagFull=%d (dt=%lums, n=%lu) | stop=%s | ota=%s(%u%%)\n",
                      (s_modeNavigator.getMode() == paxg::ui::ScreenMode::PaxgMarket) ? "PAXG" : "FISHING",
                      g_fishing_controller.isArmed() ? 1 : 0,
                      paxg::fishing::getFishingStateName(g_fishing_controller.getState()),
                      (unsigned long)g_fishing_controller.getWaitRemainingMs(),
                      mounted ? 1 : 0,
                      (unsigned long long)audioSnap.packetsReceived,
                      audioSnap.detectorMetrics.rms,
                      bagSnap.bagFullLatched ? 1 : 0,
                      (unsigned long)bagSnap.lastIntervalMs,
                      (unsigned long)bagSnap.totalPulsesDetected,
                      paxg::fishing::getStopReasonName(g_fishing_controller.getLastStopReason()),
                      paxg::ota::getOtaStateName(otaProg.state),
                      (unsigned int)otaProg.percent);
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
