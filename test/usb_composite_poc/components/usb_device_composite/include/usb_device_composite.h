#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#if defined(__has_include)
  #if __has_include("esp_err.h")
    #include "esp_err.h"
  #else
    typedef int32_t esp_err_t;
    #ifndef ESP_OK
      #define ESP_OK 0
      #define ESP_FAIL -1
      #define ESP_ERR_INVALID_ARG 0x102
      #define ESP_ERR_INVALID_STATE 0x103
      #define ESP_ERR_NOT_FINISHED 0x108
    #endif
  #endif
#else
  #include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UAC_DISCONTINUITY_REASON_UNKNOWN = 0,
    UAC_DISCONTINUITY_REASON_STREAM_CHANGE = 1, /*!< Stream started, stopped, unmounted, or suspended */
    UAC_DISCONTINUITY_REASON_OVERFLOW_DROP = 2, /*!< Audio frame dropped due to queue overflow */
} uac_discontinuity_reason_t;

typedef esp_err_t (*uac_output_cb_t)(uint8_t *buf, size_t len, void *cb_ctx);
typedef void (*uac_set_mute_cb_t)(uint32_t mute, void *cb_ctx);
typedef void (*uac_set_volume_cb_t)(uint32_t volume, void *cb_ctx);
typedef void (*uac_discontinuity_cb_t)(uint32_t reason, void *cb_ctx);

typedef struct {
    bool skip_tinyusb_init;
    uac_output_cb_t output_cb;
    uac_set_mute_cb_t set_mute_cb;
    uac_set_volume_cb_t set_volume_cb;
    void *cb_ctx;
    uac_discontinuity_cb_t discontinuity_cb;
} usb_composite_config_t;

typedef usb_composite_config_t uac_device_config_t;

/**
 * @brief Initialize the composite USB device (UAC speaker + HID keyboard).
 */
esp_err_t usb_composite_init(usb_composite_config_t *config);

/**
 * @brief Compatible alias for uac_device_init.
 */
esp_err_t uac_device_init(usb_composite_config_t *config);

/**
 * @brief Check if USB composite device is currently mounted by host and not suspended.
 */
bool usb_composite_is_mounted(void);

/**
 * @brief Check if USB HID interface is ready for transmission (mounted and endpoint not busy).
 */
bool usb_composite_hid_ready(void);

/**
 * @brief Send a keyboard keypress event (modifier + keycode).
 */
esp_err_t usb_composite_hid_keyboard_press(uint8_t modifier, uint8_t keycode);

/**
 * @brief Telemetry statistics for USB composite audio reception.
 */
typedef struct {
    uint32_t packets_received;            /*!< Total packets offered to queue */
    uint32_t bytes_received;              /*!< Total bytes offered to queue */
    uint32_t packets_queued;              /*!< Packets successfully stored in queue */
    uint32_t bytes_queued;                /*!< Bytes successfully stored in queue */
    uint32_t packets_delivered;           /*!< Packets delivered to output_cb */
    uint32_t bytes_delivered;             /*!< Bytes delivered to output_cb */
    uint32_t packets_dropped;             /*!< Packets dropped on queue overflow */
    uint32_t bytes_dropped;               /*!< Bytes dropped on queue overflow */
    uint32_t buffer_undersized_rejects;   /*!< Dequeue attempts rejected due to insufficient buffer */
    uint32_t stale_generation_drops;      /*!< Packets discarded due to stream generation change */
    uint32_t queue_depth_current;         /*!< Instantaneous queue depth */
    uint32_t queue_depth_max;             /*!< High watermark queue depth */
} usb_composite_audio_stats_t;

/**
 * @brief Get audio reception and drop statistics.
 */
esp_err_t usb_composite_get_audio_stats(usb_composite_audio_stats_t *stats);

/**
 * @brief Reset cumulative audio reception and drop statistics.
 */
esp_err_t usb_composite_reset_audio_stats(void);

/**
 * @brief Send a keyboard release event (all keys released).
 */
esp_err_t usb_composite_hid_keyboard_release(void);

#ifdef __cplusplus
}
#endif
