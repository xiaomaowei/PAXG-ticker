# Isolated ESP-IDF TinyUSB USB Audio Class (UAC) Feasibility Proof

## 1. Overview and Objective

This isolated feasibility proof demonstrates receiving host PC audio over USB Audio Class (UAC 1.0 speaker receiver) on the ESP32-S3 using Espressif's official `esp_device_uac` component (`espressif/usb_device_uac` v1.3.2) and `espressif/tinyusb` (v0.19.0~3).

### Contrast with Production Firmware
- **Production Firmware**: Built on PlatformIO Arduino ESP32 `2.0.14` (ESP-IDF `v4.4.6`). As established in Phase 4 (`test/usb_audio_poc/`), the precompiled `libarduino_tinyusb.a` static archive was compiled with `CFG_TUD_AUDIO = 0`, omitting `tud_audio*` symbols and making in-tree UAC linking impossible without migrating the framework.
- **This Feasibility Proof**: Fully isolated PlatformIO project configured with `framework = espidf` (ESP-IDF `v5.1.2`), targeting the ESP32-S3 (`lilygo-t-display-s3`). It pulls the official Espressif `usb_device_uac` component and compiles TinyUSB from source with audio class support enabled.

---

## 2. Directory Structure & Strict Isolation

All proof-of-concept assets and build artifacts are quarantined strictly within `test/usb_audio_poc_idf/`:

```
test/usb_audio_poc_idf/
├── platformio.ini              # Isolated PlatformIO ESP-IDF project configuration
├── CMakeLists.txt              # Root CMake configuration for ESP-IDF
├── sdkconfig.defaults          # Target and UAC driver default configurations
├── main/
│   ├── CMakeLists.txt          # Component registration & dependency injection
│   └── main.c                 # Application entry point & PCM counting callback
├── components/                 # Pinned Espressif official components
│   ├── cmake_utilities/        # Build helper macros
│   ├── tinyusb/                # TinyUSB stack with UAC audio device driver
│   └── usb_device_uac/         # Espressif official USB Audio Class driver
├── run_validation.py           # Automated validation and symbol assertion script
└── README.md                   # This document
```

> [!IMPORTANT]
> The root `platformio.ini`, root `CMakeLists.txt`, and production application code in `src/`, `include/`, and `lib/` remain 100% untouched. No production files are modified.

---

## 3. Public API & No-Codec PCM Counting Architecture

The implementation uses the actual official public C API declared in `usb_device_uac.h`:

```c
typedef struct {
    bool skip_tinyusb_init;
    uac_output_cb_t output_cb;         // Host -> Device (Speaker playback)
    uac_input_cb_t input_cb;           // Device -> Host (Microphone capture)
    uac_set_mute_cb_t set_mute_cb;
    uac_set_volume_cb_t set_volume_cb;
    uac_set_volume_db_cb_t set_volume_db_cb;
    void *cb_ctx;
} uac_device_config_t;

esp_err_t uac_device_init(uac_device_config_t *config);
```

### No-Codec PCM Frame Reception Callback
To demonstrate feasibility on hardware lacking an audio DAC/codec, the callback receives raw isochronous PCM packets, increments packet and byte counters, and counts 16-bit PCM samples:

```c
static esp_err_t uac_speaker_output_cb(uint8_t *buf, size_t len, void *arg)
{
    uac_poc_stats_t *stats = (uac_poc_stats_t *)arg;
    if (buf != NULL && len > 0) {
        stats->packets_received++;
        stats->bytes_received += (uint32_t)len;
        stats->pcm_samples_counted += (uint32_t)(len / sizeof(int16_t));
    }
    return ESP_OK;
}
```

---

## 4. Environment & Resolved Versions

| Component | Resolved Version | Notes |
| :--- | :--- | :--- |
| **PlatformIO Core** | `6.1.19` | Local Python environment |
| **Platform** | `espressif32 @ 6.5.0` | Pin in `platformio.ini` |
| **Framework** | `framework-espidf @ 3.50102.240122` | ESP-IDF `v5.1.2` |
| **Toolchain** | `toolchain-xtensa-esp32s3 @ 12.2.0+20230208` | GCC 12.2.0 |
| **USB Component** | `espressif/usb_device_uac @ 1.3.2` | Espressif official UAC driver |
| **TinyUSB Stack** | `espressif/tinyusb @ 0.19.0~3` | Full source with `audio_device.c` |

---

## 5. Reproducible Build & Validation Commands

### Build the Isolated Project
```powershell
pio run -d test/usb_audio_poc_idf
```
- **Exit Code**: `0`
- **Output Artifacts**:
  - `test/usb_audio_poc_idf/.pio/build/esp32s3_uac_poc/firmware.elf`
  - `test/usb_audio_poc_idf/.pio/build/esp32s3_uac_poc/firmware.bin`
- **Memory Consumption**:
  - RAM: `15,968 bytes` (~4.9% of 320 KB)
  - Flash: `227,189 bytes` (~21.7% of 1 MB app partition)

### Run Automated Symbol & Build Verification
```powershell
python test/usb_audio_poc_idf/run_validation.py
```

### Verified Linker Symbols (`xtensa-esp32s3-elf-nm`)
All audio symbols missing in the Arduino framework are fully resolved:
- `42001b40 T uac_device_init`
- `420027d0 T tud_audio_n_read`
- `4200284c T tud_audio_n_write`
- `42001a38 T tud_audio_rx_done_isr`
- `420013cc t uac_speaker_output_cb`

---

## 6. Scope Boundaries & Disclaimers

1. **Compilation and Linking**: **VERIFIED**. The ESP-IDF stack, TinyUSB audio driver, and `esp_device_uac` compile and link cleanly for ESP32-S3 without missing symbol errors.
2. **Physical Hardware & Windows Host Enumeration**: **UNVERIFIED**. Testing was performed via toolchain cross-compilation and link inspection. Physical flashing to an ESP32-S3 board and Windows 11 host USB composite audio enumeration remain unverified.
3. **Production Isolation**: Production firmware remains strictly on PlatformIO Arduino ESP32 2.0.14. No production code imports or links this component.
