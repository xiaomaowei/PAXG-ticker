#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "usb_device_composite.h"

// Production TFT_eSPI display and sprite objects
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite testSprite = TFT_eSprite(&tft);

// Static placeholder Wi-Fi credentials for compile/link symbol verification only.
// These are mock placeholders and MUST NOT be used for live network operation.
static const char *MOCK_WIFI_SSID = "placeholder_ssid";
static const char *MOCK_WIFI_PASS = "placeholder_password";

static esp_err_t test_audio_output_cb(uint8_t *buf, size_t len, void *arg) {
    (void)buf;
    (void)len;
    (void)arg;
    return ESP_OK;
}

// Manual trigger helper for runtime keypress demonstration (e.g. from physical button ISR).
// Automatic periodic key sending has been completely removed to avoid unintended host input.
void manual_trigger_keystroke_demo(uint8_t modifier, uint8_t keycode) {
    if (usb_composite_is_mounted() && usb_composite_hid_ready()) {
        usb_composite_hid_keyboard_press(modifier, keycode);
        delay(20);
        usb_composite_hid_keyboard_release();
    }
}

// Volatile reference to guarantee symbol retention by the linker without sending at runtime
void (* volatile g_hid_demo_ref)(uint8_t, uint8_t) = &manual_trigger_keystroke_demo;

void setup() {
    Serial.begin(115200);

    // 1. TFT_eSPI parallel 8-bit ST7789 display initialization
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("PAXG ticker IDF5", 10, 10);
    testSprite.createSprite(100, 50);
    testSprite.fillSprite(TFT_NAVY);
    testSprite.pushSprite(10, 30);

    // 2. Wi-Fi stack initialization (using mock credentials as compile-time placeholders)
    WiFi.mode(WIFI_STA);
    WiFi.begin(MOCK_WIFI_SSID, MOCK_WIFI_PASS);

    // 3. Simultaneous USB Composite (UAC 2.0 Speaker + HID Keyboard) initialization
    usb_composite_config_t config = {
        .skip_tinyusb_init = false,
        .output_cb = test_audio_output_cb,
        .set_mute_cb = NULL,
        .set_volume_cb = NULL,
        .cb_ctx = NULL,
    };
    usb_composite_init(&config);
}

void loop() {
    // Idle loop: purely passive.
    // No automatic HID keystrokes are transmitted at runtime.
    (void)g_hid_demo_ref;
    delay(1000);
}

extern "C" void app_main() {
    initArduino();
    setup();
    while (true) {
        loop();
    }
}
