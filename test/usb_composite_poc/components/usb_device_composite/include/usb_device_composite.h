#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*uac_output_cb_t)(uint8_t *buf, size_t len, void *cb_ctx);
typedef void (*uac_set_mute_cb_t)(uint32_t mute, void *cb_ctx);
typedef void (*uac_set_volume_cb_t)(uint32_t volume, void *cb_ctx);

typedef struct {
    bool skip_tinyusb_init;
    uac_output_cb_t output_cb;
    uac_set_mute_cb_t set_mute_cb;
    uac_set_volume_cb_t set_volume_cb;
    void *cb_ctx;
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
 *
 * Distinguishes overall bus enumeration/configuration state from transient endpoint buffer readiness.
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
 * @brief Send a keyboard release event (all keys released).
 */
esp_err_t usb_composite_hid_keyboard_release(void);

#ifdef __cplusplus
}
#endif
