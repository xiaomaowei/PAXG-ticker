#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file usb_audio_poc.h
 * @brief Phase 4 Isolated USB Audio (UAC OUT) Receive Feasibility Proof of Concept.
 *
 * This header declares a minimal proof-of-concept interface for USB Audio Class (UAC)
 * speaker/headphone receive (Audio OUT from host to device) using TinyUSB class APIs.
 *
 * It is maintained strictly in an isolated test folder (test/usb_audio_poc/**) to prevent
 * accidental inclusion in production firmware builds.
 *
 * Coexistence Architecture:
 * In a unified composite USB device, the interfaces and endpoints would be organized as:
 *   - Interface 0 & 1: USB CDC (Communication Device Class: Control & Data) -> EP 1 IN, EP 2 OUT/IN
 *   - Interface 2:     USB HID Keyboard (Standard Boot/Report Keyboard)     -> EP 3 IN
 *   - Interface 3:     USB Audio Control (AC Interface)                    -> EP 0 (Control)
 *   - Interface 4:     USB Audio Streaming (AS OUT Interface)              -> EP 4 OUT (Isochronous)
 */

namespace paxg {
namespace usb_audio_poc {

// Audio streaming parameters (PCM 16-bit, 16kHz or 48kHz mono/stereo)
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint8_t  AUDIO_CHANNELS = 1;
constexpr uint8_t  AUDIO_BITS_PER_SAMPLE = 16;
constexpr size_t   AUDIO_FRAME_SAMPLES = 128;
constexpr size_t   AUDIO_RX_BUFFER_BYTES = 512;

/**
 * @brief Status and diagnostic statistics for the USB Audio PoC.
 */
struct UsbAudioPocStats {
    uint32_t packetsReceived = 0;
    uint32_t bytesReceived = 0;
    uint32_t bufferOverflows = 0;
    bool isInterfaceOpen = false;
    uint8_t currentAltSetting = 0;
};

/**
 * @brief Initialize USB Audio receive buffers and state.
 */
void init();

/**
 * @brief Read available PCM16 audio samples received over USB Audio OUT endpoint.
 *
 * @param dest Destination buffer for PCM16 samples.
 * @param maxSamples Maximum number of int16_t samples dest can hold.
 * @param[out] samplesRead Actual number of samples written to dest.
 * @return true if samples were read successfully, false otherwise.
 */
bool readSamples(int16_t* dest, size_t maxSamples, size_t* samplesRead);

/**
 * @brief Retrieve current status of the audio receiver PoC.
 */
UsbAudioPocStats getStats();

}  // namespace usb_audio_poc
}  // namespace paxg
