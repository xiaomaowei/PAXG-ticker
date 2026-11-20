/**
 * @file usb_audio_poc.cpp
 * @brief Phase 4 Isolated USB Audio (UAC 1.0 OUT) Receive Feasibility Proof.
 *
 * This file demonstrates the exact TinyUSB configuration and API calls required to
 * implement a USB Audio Class speaker receiver (Audio OUT from host) on ESP32-S3.
 *
 * NOTE: This file is intentionally kept isolated within test/usb_audio_poc/ and is NOT
 * compiled into production firmware.
 */

#include "usb_audio_poc.h"

// -----------------------------------------------------------------------------
// TinyUSB Audio Class Driver Configuration Macros
//
// In TinyUSB, class/audio/audio_device.h requires specific macro definitions
// before inclusion to configure endpoint sizes, descriptor lengths, and buffers.
// -----------------------------------------------------------------------------
#ifndef CFG_TUD_AUDIO
#define CFG_TUD_AUDIO 1
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN 64
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_N_AS_INT
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 1
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64
#endif

#ifndef CFG_TUD_AUDIO_ENABLE_EP_OUT
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX 256
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ 256
#endif

#ifndef CFG_TUD_AUDIO_EP_SZ_MAX
#define CFG_TUD_AUDIO_EP_SZ_MAX 256
#endif

// Include TinyUSB core and audio class headers
#include "tusb.h"
#include "class/audio/audio_device.h"

#include <cstring>
#include <algorithm>

namespace paxg {
namespace usb_audio_poc {

namespace {

static uint8_t s_rxRawBuffer[AUDIO_RX_BUFFER_BYTES];
static int16_t s_pcmRingBuffer[AUDIO_RX_BUFFER_BYTES / sizeof(int16_t)];
static size_t  s_pcmCount = 0;
static UsbAudioPocStats s_stats;

}  // namespace

void init() {
    std::memset(s_rxRawBuffer, 0, sizeof(s_rxRawBuffer));
    std::memset(s_pcmRingBuffer, 0, sizeof(s_pcmRingBuffer));
    s_pcmCount = 0;
    s_stats = UsbAudioPocStats{};
}

bool readSamples(int16_t* dest, size_t maxSamples, size_t* samplesRead) {
    if (dest == nullptr || samplesRead == nullptr || maxSamples == 0) {
        return false;
    }

    if (s_pcmCount == 0) {
        *samplesRead = 0;
        return true;
    }

    const size_t toCopy = std::min(maxSamples, s_pcmCount);
    std::memcpy(dest, s_pcmRingBuffer, toCopy * sizeof(int16_t));

    // Shift remaining samples
    if (toCopy < s_pcmCount) {
        std::memmove(s_pcmRingBuffer, &s_pcmRingBuffer[toCopy], (s_pcmCount - toCopy) * sizeof(int16_t));
    }
    s_pcmCount -= toCopy;
    *samplesRead = toCopy;
    return true;
}

UsbAudioPocStats getStats() {
    return s_stats;
}

}  // namespace usb_audio_poc
}  // namespace paxg

// -----------------------------------------------------------------------------
// TinyUSB Audio Class Driver Callbacks
//
// These callbacks are required by TinyUSB's audiod class driver when
// CFG_TUD_AUDIO is enabled.
// -----------------------------------------------------------------------------

#if CFG_TUD_AUDIO

// Invoked when audio data is received on the OUT endpoint from host
bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    paxg::usb_audio_poc::s_stats.packetsReceived++;
    paxg::usb_audio_poc::s_stats.bytesReceived += n_bytes_received;

    // Call TinyUSB audio read API to drain the incoming OUT endpoint FIFO
    // In TinyUSB audio driver: tud_audio_read(buffer, n_bytes)
    uint8_t tempBuf[256];
    const uint16_t bytesToRead = std::min(n_bytes_received, static_cast<uint16_t>(sizeof(tempBuf)));
    const uint16_t bytesRead = tud_audio_read(tempBuf, bytesToRead);

    // Convert raw PCM bytes to int16_t samples
    const size_t newSamples = bytesRead / sizeof(int16_t);
    const size_t maxCapacity = sizeof(paxg::usb_audio_poc::s_pcmRingBuffer) / sizeof(int16_t);

    if (paxg::usb_audio_poc::s_pcmCount + newSamples <= maxCapacity) {
        std::memcpy(&paxg::usb_audio_poc::s_pcmRingBuffer[paxg::usb_audio_poc::s_pcmCount],
                    tempBuf, newSamples * sizeof(int16_t));
        paxg::usb_audio_poc::s_pcmCount += newSamples;
    } else {
        paxg::usb_audio_poc::s_stats.bufferOverflows++;
    }

    return true;
}

// Invoked when host changes alternate setting (e.g., 0 = Idle/Mute, 1 = Active Stream)
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request) {
    (void)rhport;
    paxg::usb_audio_poc::s_stats.currentAltSetting = static_cast<uint8_t>(p_request->wValue);
    paxg::usb_audio_poc::s_stats.isInterfaceOpen = (paxg::usb_audio_poc::s_stats.currentAltSetting > 0);
    return true;
}

// Invoked when host issues an Audio Control request (e.g. Set Volume, Mute)
bool tud_audio_set_req_channel_cb(uint8_t rhport, tusb_control_request_t const * p_request, uint8_t *p_buff) {
    (void)rhport;
    (void)p_request;
    (void)p_buff;
    return true; // Acknowledge standard control requests
}

// Invoked when host queries current channel volume/mute setting
bool tud_audio_get_req_channel_cb(uint8_t rhport, tusb_control_request_t const * p_request, uint8_t *p_buff) {
    (void)rhport;
    (void)p_request;
    if (p_buff != nullptr) {
        p_buff[0] = 0x00; // Default flat volume / unmuted
    }
    return true;
}

#endif  // CFG_TUD_AUDIO
