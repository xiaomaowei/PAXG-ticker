#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "uac_config.h"
#include "usb_device_composite.h"
#include "uac_descriptors.h"

static const char *TAG = "usbd_composite";

const uint32_t sample_rates[] = {DEFAULT_SAMPLE_RATE};
#define N_SAMPLE_RATES  TU_ARRAY_SIZE(sample_rates)

enum {
    VOLUME_CTRL_0_DB = 0,
    VOLUME_CTRL_10_DB = 2560,
    VOLUME_CTRL_20_DB = 5120,
    VOLUME_CTRL_30_DB = 7680,
    VOLUME_CTRL_40_DB = 10240,
    VOLUME_CTRL_50_DB = 12800,
    VOLUME_CTRL_60_DB = 15360,
    VOLUME_CTRL_70_DB = 17920,
    VOLUME_CTRL_80_DB = 20480,
    VOLUME_CTRL_90_DB = 23040,
    VOLUME_CTRL_100_DB = 25600,
    VOLUME_CTRL_SILENCE = 0x8000,
};

const uint8_t spk_resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX};

typedef struct {
    usb_phy_handle_t phy_hdl;
    usb_composite_config_t user_cfg;
    int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];
    int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];
    uint8_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ];
    int spk_data_size;
    int spk_itf_num;
    uint8_t spk_resolution;
    uint32_t current_sample_rate;
    TaskHandle_t spk_task_handle;
    size_t spk_bytes_per_ms;
    bool spk_active;
    bool usb_mounted;
} usb_composite_device_t;

static usb_composite_device_t *s_composite_dev = NULL;

static void usb_phy_init(void)
{
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
    };
    usb_new_phy(&phy_conf, &s_composite_dev->phy_hdl);
}

static void tusb_device_task(void *arg)
{
    (void)arg;
    while (1) {
        tud_task();
    }
}

// Invoked when device is mounted
void tud_mount_cb(void)
{
    if (s_composite_dev) {
        s_composite_dev->spk_active = false;
        s_composite_dev->usb_mounted = true;
    }
    ESP_LOGI(TAG, "USB Composite device mounted by host");
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    if (s_composite_dev) {
        s_composite_dev->spk_active = false;
        s_composite_dev->usb_mounted = false;
    }
    ESP_LOGI(TAG, "USB Composite device unmounted");
}

// Invoked when usb bus is suspended
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    if (s_composite_dev) {
        s_composite_dev->spk_active = false;
        s_composite_dev->usb_mounted = false;
    }
    ESP_LOGI(TAG, "USB Composite device suspended");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    if (s_composite_dev) {
        s_composite_dev->usb_mounted = true;
    }
    ESP_LOGI(TAG, "USB Composite device resumed");
}

// Audio Clock GET request
static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t curf = { (int32_t) tu_htole32(s_composite_dev->current_sample_rate) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
        } else if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) rangef = {
                .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
            };
            for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                rangef.subrange[i].bMin = (int32_t) sample_rates[i];
                rangef.subrange[i].bMax = (int32_t) sample_rates[i];
                rangef.subrange[i].bRes = 0;
            }
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
        }
    } else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = { .bCur = 1 };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
    }
    return false;
}

// Audio Clock SET request
static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));
        uint32_t target_sample_rate = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;
        if (target_sample_rate != s_composite_dev->current_sample_rate) {
            return false;
        }
        return true;
    }
    return false;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = s_composite_dev->current_sample_rate;
}

static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute1 = { .bCur = s_composite_dev->mute[request->bChannelNumber] };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range_vol = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256) }
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
        } else if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(s_composite_dev->volume[request->bChannelNumber]) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
        }
    }
    return false;
}

static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
        s_composite_dev->mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;
        if (s_composite_dev->user_cfg.set_mute_cb) {
            s_composite_dev->user_cfg.set_mute_cb(s_composite_dev->mute[request->bChannelNumber], s_composite_dev->user_cfg.cb_ctx);
        }
        return true;
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
        s_composite_dev->volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
        int volume_db = s_composite_dev->volume[request->bChannelNumber] / 256;
        int volume = (volume_db + 50) * 2;
        if (s_composite_dev->user_cfg.set_volume_cb) {
            s_composite_dev->user_cfg.set_volume_cb(volume, s_composite_dev->user_cfg.cb_ctx);
        }
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;
    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return tud_audio_clock_get_request(rhport, request);
    }
    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
        return tud_audio_feature_unit_get_request(rhport, request);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;
    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
        return tud_audio_feature_unit_set_request(rhport, request, buf);
    }
    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return tud_audio_clock_set_request(rhport, request, buf);
    }
    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (s_composite_dev->spk_itf_num == itf && alt == 0) {
        s_composite_dev->spk_data_size = 0;
        s_composite_dev->spk_active = false;
    }
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (s_composite_dev->spk_itf_num == itf && alt != 0) {
        s_composite_dev->spk_data_size = 0;
        s_composite_dev->spk_resolution = spk_resolutions_per_format[alt - 1];
        s_composite_dev->spk_active = true;
        s_composite_dev->spk_bytes_per_ms = s_composite_dev->current_sample_rate / 1000 * SPEAK_CHANNEL_NUM * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX;
        xTaskNotifyGive(s_composite_dev->spk_task_handle);
    }
    return true;
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)ep_out;
    (void)cur_alt_setting;
    (void)n_bytes_received;

    static bool new_play = false;
    static int64_t last_time = 0;
    int64_t now = esp_timer_get_time();

    if (now - last_time > 100 * CONFIG_UAC_SPK_NEW_PLAY_INTERVAL) {
        new_play = true;
        tud_audio_n_clear_ep_out_ff(func_id);
    }
    last_time = now;

    int bytes_remained = tud_audio_n_available(func_id);
    size_t bytes_require = s_composite_dev->spk_bytes_per_ms;

    if (new_play) {
        bytes_require = SPK_INTERVAL_MS * s_composite_dev->spk_bytes_per_ms / 2;
        if (bytes_remained < bytes_require) {
            return true;
        }
        new_play = false;
    }

    s_composite_dev->spk_data_size = tud_audio_n_read(func_id, s_composite_dev->spk_buf, bytes_require);
    xTaskNotifyGive(s_composite_dev->spk_task_handle);
    return true;
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_sent;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    return true;
}

static void usb_spk_task(void *pvParam)
{
    (void)pvParam;
    while (1) {
        if (!s_composite_dev->spk_active) {
            ulTaskNotifyTake(pdFAIL, portMAX_DELAY);
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_composite_dev->spk_data_size == 0) {
            continue;
        }
        if (s_composite_dev->user_cfg.output_cb) {
            s_composite_dev->user_cfg.output_cb(s_composite_dev->spk_buf, s_composite_dev->spk_data_size, s_composite_dev->user_cfg.cb_ctx);
        }
        s_composite_dev->spk_data_size = 0;
    }
}

//--------------------------------------------------------------------+
// HID Keyboard Implementation
//--------------------------------------------------------------------+

bool usb_composite_is_mounted(void)
{
    return tud_mounted() && !tud_suspended();
}

bool usb_composite_hid_ready(void)
{
    return tud_mounted() && !tud_suspended() && tud_hid_ready();
}

esp_err_t usb_composite_hid_keyboard_press(uint8_t modifier, uint8_t keycode)
{
    if (!tud_mounted() || tud_suspended()) {
        return ESP_ERR_INVALID_STATE; // Device is not mounted or is suspended
    }
    if (!tud_hid_ready()) {
        return ESP_ERR_NOT_FINISHED; // Endpoint is temporarily busy; caller should retry
    }
    uint8_t keys[6] = { keycode, 0, 0, 0, 0, 0 };
    bool ok = tud_hid_keyboard_report(0, modifier, keys);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t usb_composite_hid_keyboard_release(void)
{
    if (!tud_mounted() || tud_suspended()) {
        return ESP_ERR_INVALID_STATE; // Device is not mounted or is suspended
    }
    if (!tud_hid_ready()) {
        return ESP_ERR_NOT_FINISHED; // Endpoint is temporarily busy; caller should retry
    }
    uint8_t keys[6] = { 0 };
    bool ok = tud_hid_keyboard_report(0, 0, keys);
    return ok ? ESP_OK : ESP_FAIL;
}

//--------------------------------------------------------------------+
// Composite Initialization
//--------------------------------------------------------------------+

esp_err_t usb_composite_init(usb_composite_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    if (s_composite_dev != NULL) {
        ESP_LOGW(TAG, "Composite device already initialized");
        return ESP_OK;
    }

    s_composite_dev = calloc(1, sizeof(usb_composite_device_t));
    ESP_RETURN_ON_FALSE(s_composite_dev != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate composite dev");

    s_composite_dev->user_cfg = *config;
    s_composite_dev->current_sample_rate = DEFAULT_SAMPLE_RATE;
    s_composite_dev->spk_itf_num = ITF_NUM_AUDIO_STREAMING_SPK;

    BaseType_t ret_val;
    if (!config->skip_tinyusb_init) {
        usb_phy_init();
        bool usb_init = tusb_init();
        if (!usb_init) {
            ESP_LOGE(TAG, "TinyUSB Init Fail");
            return ESP_FAIL;
        }
        ret_val = xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL, CONFIG_UAC_TINYUSB_TASK_PRIORITY,
                                          NULL, CONFIG_UAC_TINYUSB_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_TINYUSB_TASK_CORE);
        ESP_RETURN_ON_FALSE(ret_val == pdPASS, ESP_FAIL, TAG, "Failed to create TinyUSB task");
    }

    ret_val = xTaskCreatePinnedToCore(usb_spk_task, "usb_spk_task", 4096, NULL, CONFIG_UAC_SPK_TASK_PRIORITY,
                                      &s_composite_dev->spk_task_handle, CONFIG_UAC_SPK_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_SPK_TASK_CORE);
    ESP_RETURN_ON_FALSE(ret_val == pdPASS, ESP_FAIL, TAG, "Failed to create usb_spk task");

    ESP_LOGI(TAG, "Simultaneous UAC Speaker + HID Keyboard Composite Device Initialized");
    return ESP_OK;
}

esp_err_t uac_device_init(usb_composite_config_t *config)
{
    return usb_composite_init(config);
}
