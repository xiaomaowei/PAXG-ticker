#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"
#include "uac_config.h"
#include "uac_descriptors.h"
#include "tusb_config_uac.h"

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+
#define CFG_TUSB_RHPORT0_MODE    OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED
#define CONFIG_USB_HS            0

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_FREERTOS
#endif

#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#if TU_CHECK_MCU(OPT_MCU_ESP32S2, OPT_MCU_ESP32S3, OPT_MCU_ESP32P4, OPT_MCU_ESP32S31, OPT_MCU_ESP32H4)
#define CFG_TUSB_OS_INC_PATH    freertos/
#endif

// Enable Device stack
#define CFG_TUD_ENABLED       1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//--------------------------------------------------------------------
// CLASS DRIVER CONFIGURATION: SIMULTANEOUS UAC + HID
//--------------------------------------------------------------------
// Audio Class is configured via tusb_config_uac.h (CFG_TUD_AUDIO 1)

// Enable HID Class Driver
#define CFG_TUD_HID               1
#define CFG_TUD_HID_EP_BUFSIZE    16

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
