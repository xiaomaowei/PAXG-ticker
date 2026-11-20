/**
 * @file main.c
 * @brief Isolated ESP-IDF Simultaneous USB Composite (UAC 2.0 Speaker + HID Keyboard) Feasibility Proof.
 *
 * Demonstrates:
 * 1. USB Audio Class 2.0 (UAC 2.0 speaker playback, Host -> ESP32-S3, 48kHz 16-bit stereo PCM).
 * 2. USB HID Keyboard (device keystroke injection, ESP32-S3 -> Host).
 *
 * Safety & Thread-Safety Design:
 * - NO automatic periodic keystrokes are sent on boot or USB mount/resume.
 * - HID keystroke testing requires an explicit physical button press on GPIO 0 (BOOT button).
 * - Debounce state machine guarantees exactly one press-and-release per physical actuation.
 * - Audio callback is minimal and non-blocking.
 * - Cross-task telemetry statistics use lock-free C11 atomic primitives (stdatomic.h)
 *   to eliminate data races between audio ISR/task and background monitor task.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "class/hid/hid.h"
#include "usb_device_composite.h"
#include "btn_fsm.h"

static const char *TAG = "usb_composite_poc";

#define TEST_BUTTON_GPIO         GPIO_NUM_0   // BOOT button on LilyGo T-Display-S3 (active LOW)
#define BUTTON_DEBOUNCE_MS       50
#define KEY_PRESS_HOLD_MS        50

/**
 * @brief Thread-safe telemetry statistics accessed across audio callback and monitor task.
 */
typedef struct {
    atomic_uint_fast32_t packets_received;
    atomic_uint_fast32_t bytes_received;
    atomic_uint_fast32_t pcm_samples_counted;
    atomic_uint_fast32_t hid_keys_sent;
} composite_poc_stats_t;

static composite_poc_stats_t s_poc_stats = {
    .packets_received = 0,
    .bytes_received = 0,
    .pcm_samples_counted = 0,
    .hid_keys_sent = 0,
};

/**
 * @brief Speaker output callback (Host PC -> ESP32-S3).
 *
 * Invoked by the TinyUSB UAC 2.0 driver whenever isochronous PCM audio data arrives.
 * Kept strictly minimal, non-blocking, and free of memory allocations or logging.
 */
static esp_err_t uac_speaker_output_cb(uint8_t *buf, size_t len, void *arg)
{
    composite_poc_stats_t *stats = (composite_poc_stats_t *)arg;
    if (buf != NULL && len > 0 && stats != NULL) {
        atomic_fetch_add_explicit(&stats->packets_received, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats->bytes_received, (uint32_t)len, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats->pcm_samples_counted, (uint32_t)(len / sizeof(int16_t)), memory_order_relaxed);
    }
    return ESP_OK;
}

static btn_fsm_t s_btn_fsm;

static btn_op_result_t poc_hid_press_cb(void *ctx)
{
    (void)ctx;
    esp_err_t err = usb_composite_hid_keyboard_press(0, HID_KEY_1);
    if (err == ESP_OK) {
        return BTN_OP_SUCCESS;
    } else if (err == ESP_ERR_NOT_FINISHED) {
        return BTN_OP_BUSY;
    } else {
        return BTN_OP_DISCONNECTED;
    }
}

static btn_op_result_t poc_hid_release_cb(void *ctx)
{
    (void)ctx;
    esp_err_t err = usb_composite_hid_keyboard_release();
    if (err == ESP_OK) {
        return BTN_OP_SUCCESS;
    } else if (err == ESP_ERR_NOT_FINISHED) {
        return BTN_OP_BUSY;
    } else {
        return BTN_OP_DISCONNECTED;
    }
}

/**
 * @brief Initialize GPIO for physical button input (active-low with pullup) and button FSM.
 */
static void init_test_button_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TEST_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    btn_fsm_init(&s_btn_fsm, BUTTON_DEBOUNCE_MS, KEY_PRESS_HOLD_MS);
}

/**
 * @brief Service debounced physical button action.
 *
 * Strictly single-shot and non-blocking:
 * - Separates true USB mount state from transient endpoint buffer busy state.
 * - Requires observed stable release after boot/mount before arming.
 * - Debounces for >= BUTTON_DEBOUNCE_MS on press and release.
 * - Non-blocking: holds key across ticks without blocking task delays.
 * - Endpoint busy retry: retries non-blocking while mounted; claims success
 *   ONLY when release report is accepted by host.
 * - Safe abort on real USB unmount/suspend without repeated or stuck keys.
 * - Requires full release before allowing another action (NO auto-repeat).
 */
static void service_physical_button_test(void)
{
    const bool is_pressed = (gpio_get_level(TEST_BUTTON_GPIO) == 0); // Active LOW
    const bool is_mounted = usb_composite_is_mounted(); // Real mount & not suspended
    const uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());

    btn_fsm_action_t action = btn_fsm_step(&s_btn_fsm,
                                           now_ms,
                                           is_pressed,
                                           is_mounted,
                                           poc_hid_press_cb,
                                           poc_hid_release_cb,
                                           NULL);

    switch (action) {
        case BTN_ACTION_SEND_PRESS:
            ESP_LOGI(TAG, "Physical button pressed (debounced): Sent HID_KEY_1 press report");
            break;

        case BTN_ACTION_RETRY_PRESS:
            ESP_LOGD(TAG, "Press report endpoint busy; retrying non-blocking on next tick");
            break;

        case BTN_ACTION_KEY_SEQUENCE_COMPLETED: {
            atomic_fetch_add_explicit(&s_poc_stats.hid_keys_sent, 1, memory_order_relaxed);
            uint32_t total = atomic_load_explicit(&s_poc_stats.hid_keys_sent, memory_order_relaxed);
            ESP_LOGI(TAG, "Release report accepted! Single key sequence completed (total sent=%" PRIu32 ")", total);
            break;
        }

        case BTN_ACTION_SEND_RELEASE:
            ESP_LOGD(TAG, "Release report busy or retrying; non-blocking retry on next tick");
            break;

        case BTN_ACTION_ABORT_DISCONNECT:
            ESP_LOGW(TAG, "USB disconnected/suspended during button action; cleared safely without sending key");
            break;

        case BTN_ACTION_DROP_UNREADY:
            ESP_LOGW(TAG, "Physical button pressed, but USB is not mounted; actuation dropped");
            break;

        case BTN_ACTION_NONE:
        default:
            break;
    }
}

/**
 * @brief Background task to periodically report telemetry and service physical button input.
 */
static void composite_monitor_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t last_reported_packets = 0;
    TickType_t last_log_tick = xTaskGetTickCount();

    while (1) {
        // Poll button with 10ms resolution for responsive debouncing
        vTaskDelay(pdMS_TO_TICKS(10));
        service_physical_button_test();

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_tick) >= pdMS_TO_TICKS(1000)) {
            last_log_tick = now;

            uint32_t cur_packets = atomic_load_explicit(&s_poc_stats.packets_received, memory_order_relaxed);
            if (cur_packets != last_reported_packets) {
                uint32_t cur_bytes = atomic_load_explicit(&s_poc_stats.bytes_received, memory_order_relaxed);
                uint32_t cur_samples = atomic_load_explicit(&s_poc_stats.pcm_samples_counted, memory_order_relaxed);
                ESP_LOGI(TAG, "UAC Status: packets=%" PRIu32 ", bytes=%" PRIu32 ", pcm_samples=%" PRIu32,
                         cur_packets, cur_bytes, cur_samples);
                last_reported_packets = cur_packets;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "Simultaneous USB Composite (UAC 2.0 Speaker + HID Keyboard) PoC");
    ESP_LOGI(TAG, "Target: ESP32-S3 (LilyGo T-Display-S3)");
    ESP_LOGI(TAG, "Interfaces: UAC 2.0 Speaker OUT (IAD) + HID Keyboard");
    ESP_LOGI(TAG, "Safety: NO automatic keystrokes. Press GPIO 0 to test HID.");
    ESP_LOGI(TAG, "==========================================================");

    init_test_button_gpio();

    usb_composite_config_t composite_cfg = {
        .skip_tinyusb_init = false,
        .output_cb = uac_speaker_output_cb,
        .set_mute_cb = NULL,
        .set_volume_cb = NULL,
        .cb_ctx = &s_poc_stats,
    };

    esp_err_t ret = uac_device_init(&composite_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize composite USB device: %s (0x%x)", esp_err_to_name(ret), ret);
        return;
    }

    ESP_LOGI(TAG, "Composite USB device initialized. Starting monitor task...");
    xTaskCreatePinnedToCore(composite_monitor_task, "composite_task", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Ready: Host PC should enumerate BOTH USB Audio Speaker AND HID Keyboard.");
}
