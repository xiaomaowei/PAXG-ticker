#include "fishing_stop_notification.h"

#include <atomic>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace paxg {
namespace notification {

namespace {

static const char* TAG = "FISHING_NOTIF";
static QueueHandle_t s_webhookQueue = nullptr;
static std::atomic<uint32_t> s_dispatchedQueueOverflowStop{0};

struct WebhookNotification {
    const char* url;
    uint32_t queueOverflowStopId;
};

#ifndef MACRODROID_WEBHOOK_URL
#define MACRODROID_WEBHOOK_URL ""
#endif
#ifndef MACRODROID_QUEUE_OVERFLOW_WEBHOOK_URL
#define MACRODROID_QUEUE_OVERFLOW_WEBHOOK_URL ""
#endif

static constexpr char kBagFullWebhook[] = MACRODROID_WEBHOOK_URL;
static constexpr char kQueueOverflowWebhook[] = MACRODROID_QUEUE_OVERFLOW_WEBHOOK_URL;

static void notificationTaskLoop(void* arg) {
    (void)arg;

    while (true) {
        WebhookNotification notification;
        xQueueReceive(s_webhookQueue, &notification, portMAX_DELAY);

        if (WiFi.status() != WL_CONNECTED) {
            ESP_LOGI(TAG, "Wi-Fi unavailable, skipping webhook notification");
            continue;
        }

        esp_http_client_config_t config = {};
        config.url = notification.url;
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
                if (notification.queueOverflowStopId != 0) {
                    s_dispatchedQueueOverflowStop.store(notification.queueOverflowStopId);
                }
            } else {
                ESP_LOGW(TAG, "Webhook returned non-2xx status=%d", statusCode);
            }
        } else {
            ESP_LOGW(TAG, "Webhook dispatch failed err=%d", err);
        }

        esp_http_client_cleanup(client);
    }
}

}  // namespace

void initStopNotification() {
    if (kBagFullWebhook[0] == '\0' && kQueueOverflowWebhook[0] == '\0') {
        ESP_LOGI(TAG, "MacroDroid webhooks are not configured; feature dormant");
        return;
    }
    s_webhookQueue = xQueueCreate(8, sizeof(WebhookNotification));
    if (s_webhookQueue == nullptr) {
        ESP_LOGE(TAG, "Failed to create webhook queue");
        return;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        notificationTaskLoop,
        "notif_task",
        4096,
        nullptr,
        1,
        nullptr,
        0
    );
    if (res == pdPASS) {
        ESP_LOGI(TAG, "MacroDroid stop notification worker initialized");
    } else {
        vQueueDelete(s_webhookQueue);
        s_webhookQueue = nullptr;
        ESP_LOGE(TAG, "Failed to create notification task");
    }
}

void notifyFishingStop(fishing::StopReason reason, uint32_t stopId) {
    const bool queueOverflow = reason == fishing::StopReason::QueueOverflowDrop;
    const WebhookNotification notification{
        queueOverflow ? kQueueOverflowWebhook : kBagFullWebhook,
        queueOverflow ? stopId : 0
    };
    if (s_webhookQueue != nullptr && notification.url[0] != '\0' &&
        xQueueSend(s_webhookQueue, &notification, 0) != pdPASS) {
        ESP_LOGW(TAG, "Webhook queue full");
    }
}

uint32_t takeDispatchedQueueOverflowStop() {
    return s_dispatchedQueueOverflowStop.exchange(0);
}

}  // namespace notification
}  // namespace paxg
