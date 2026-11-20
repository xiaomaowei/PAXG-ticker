/**
 * @file main.c
 * @brief Isolated ESP-IDF TinyUSB USB Audio Class (UAC) Feasibility Proof for ESP32-S3.
 *
 * Demonstrates receiving host PC USB audio (UAC Speaker OUT) using Espressif's
 * official `usb_device_uac` (espressif/usb_device_uac) component on ESP-IDF >= 5.0.
 *
 * Implements a non-blocking output callback that counts received packets, bytes,
 * and 16-bit PCM samples without requiring an external audio codec (no-codec setup).
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_device_uac.h"

static const char *TAG = "usb_audio_poc_idf";

/**
 * @brief Statistics tracked by the isolated UAC callback.
 */
typedef struct {
    uint32_t packets_received;
    uint32_t bytes_received;
    uint32_t pcm_samples_counted;
    uint32_t last_report_packets;
} uac_poc_stats_t;

static uac_poc_stats_t s_poc_stats = {0};

/**
 * @brief Speaker output callback (Host PC -> ESP32-S3).
 *
 * Invoked by the TinyUSB / usb_device_uac driver whenever the host streams
 * isochronous PCM audio data to the device.
 *
 * @param buf Pointer to raw audio PCM bytes received from USB host.
 * @param len Length in bytes of the received audio frame.
 * @param arg User context pointer registered via cb_ctx.
 * @return ESP_OK on success.
 */
static esp_err_t uac_speaker_output_cb(uint8_t *buf, size_t len, void *arg)
{
    uac_poc_stats_t *stats = (uac_poc_stats_t *)arg;
    if (stats == NULL) {
        stats = &s_poc_stats;
    }

    if (buf != NULL && len > 0) {
        stats->packets_received++;
        stats->bytes_received += (uint32_t)len;
        // Count 16-bit PCM samples (2 bytes per sample per channel)
        stats->pcm_samples_counted += (uint32_t)(len / sizeof(int16_t));
    }

    return ESP_OK;
}

/**
 * @brief Background task to periodically report received audio telemetry.
 */
static void uac_monitor_task(void *pvParameters)
{
    (void)pvParameters;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_poc_stats.packets_received != s_poc_stats.last_report_packets) {
            ESP_LOGI(TAG, "UAC Status: packets=%" PRIu32 ", bytes=%" PRIu32 ", pcm_samples=%" PRIu32,
                     s_poc_stats.packets_received,
                     s_poc_stats.bytes_received,
                     s_poc_stats.pcm_samples_counted);
            s_poc_stats.last_report_packets = s_poc_stats.packets_received;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "ESP-IDF TinyUSB UAC Feasibility Proof (ESP32-S3 / esp_device_uac)");
    ESP_LOGI(TAG, "Framework: ESP-IDF >= 5.0 (PlatformIO isolated env)");
    ESP_LOGI(TAG, "Mode: UAC Speaker OUT (Host -> ESP32-S3, No-Codec Counting)");
    ESP_LOGI(TAG, "==========================================================");

    // Configure UAC device with speaker output callback
    uac_device_config_t uac_config = {
        .skip_tinyusb_init = false,
        .output_cb = uac_speaker_output_cb,
        .input_cb = NULL,           // Output-only speaker mode; microphone not needed
        .set_mute_cb = NULL,
        .set_volume_cb = NULL,
        .cb_ctx = &s_poc_stats,
    };

    esp_err_t ret = uac_device_init(&uac_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UAC device: %s (0x%x)", esp_err_to_name(ret), ret);
        return;
    }

    ESP_LOGI(TAG, "UAC device initialized successfully. Starting monitor task...");

    xTaskCreatePinnedToCore(uac_monitor_task, "uac_monitor", 2048, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Ready: Connect ESP32-S3 native USB to host PC.");
    ESP_LOGI(TAG, "PC should enumerate device as USB Audio Output device.");
}
