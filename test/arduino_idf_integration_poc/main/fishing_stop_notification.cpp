#include "fishing_stop_notification.h"

#include <WiFi.h>
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace paxg {
namespace notification {

namespace {

static const char* TAG = "FISHING_NOTIF";
static TaskHandle_t s_notificationTask = nullptr;

#if defined(MACRODROID_WEBHOOK_URL)
static void notificationTaskLoop(void* arg) {
    (void)arg;

    while (true) {
        // Wait for counting task notification from onActiveBagFullStop
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

        if (WiFi.status() != WL_CONNECTED) {
            ESP_LOGI(TAG, "Wi-Fi unavailable, skipping webhook notification");
            continue;
        }

        esp_http_client_config_t config = {};
        config.url = MACRODROID_WEBHOOK_URL;
        config.timeout_ms = 5000;
        config.disable_auto_redirect = true;
        config.max_redirection_count = 0;
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            ESP_LOGW(TAG, "Failed to init HTTP client");
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int statusCode = esp_http_client_get_status_code(client);
            if (statusCode >= 200 && statusCode <= 299) {
                ESP_LOGI(TAG, "Webhook dispatched successfully status=%d", statusCode);
            } else {
                ESP_LOGW(TAG, "Webhook returned non-2xx status=%d", statusCode);
            }
        } else {
            ESP_LOGW(TAG, "Webhook dispatch failed err=%d", err);
        }

        esp_http_client_cleanup(client);
    }
}
#endif

}  // namespace

void initStopNotification() {
#if defined(MACRODROID_WEBHOOK_URL)
    const char* urlLiteral = MACRODROID_WEBHOOK_URL;
    if (urlLiteral[0] == '\0') {
        ESP_LOGI(TAG, "MacroDroid webhook URL empty; feature dormant");
        return;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        notificationTaskLoop,
        "notif_task",
        4096,
        nullptr,
        1,
        &s_notificationTask,
        0
    );
    if (res == pdPASS) {
        ESP_LOGI(TAG, "MacroDroid stop notification worker initialized");
    } else {
        ESP_LOGE(TAG, "Failed to create notification task");
    }
#else
    ESP_LOGI(TAG, "MacroDroid webhook URL not defined; feature dormant");
#endif
}

void onActiveBagFullStop() {
    if (s_notificationTask != nullptr) {
        xTaskNotifyGive(s_notificationTask);
    }
}

}  // namespace notification
}  // namespace paxg
