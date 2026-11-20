# USB Feasibility and Hardware Integration Report (Phase 1)

## Executive Summary

This document provides an evidence-based assessment of USB capabilities for the LILYGO T-Display-S3 project under the current PlatformIO Arduino environment (`framework-arduinoespressif32 @ 3.20014.231204 / Arduino ESP32 2.0.14`).

Key conclusions:
1. **USB HID (Keyboard Keystroke Output)**: **Feasible for future Phase 3**. The underlying Espressif TinyUSB SDK components support USB HID Keyboard emulation. However, it requires enabling specific Arduino USB compile flags (`-DARDUINO_USB_MODE=1` with TinyUSB device initialization) and is intentionally **unimplemented** in Phase 2.
2. **USB Audio Class (UAC 1.0 / 2.0)**: **Unverified / No Demonstrated Ready-to-Use Path in Current Setup**. The current Arduino 2.0.14 setup has no demonstrated ready-to-use UAC speaker capture path or configuration in this project, and the proposed UAC route remains unverified. While low-level TinyUSB audio class headers exist in the framework (`arduino_tinyusb/tinyusb/src/class/audio/`), `CONFIG_TINYUSB_AUDIO_ENABLED` is not present in the default Arduino SDK configuration. A minimal TinyUSB/ESP-IDF compatibility proof of concept is required before concluding feasibility or considering framework migration. Platform-wide impossibility is not claimed solely from the missing configuration macro. Phase 4 remains explicitly unimplemented and unverified.
3. **Phase 2 Implementation Scope**: The WoW Fishing mode operates strictly in **Simulation Mode**. All metrics, reel triggers, and audio levels are synthetic. No USB keystrokes are transmitted, and USB status is explicitly displayed as **UNAVAILABLE / DISCONNECTED**.

---

## 1. Toolchain and Framework Inspection

Inspection of the local PlatformIO environment and resolved package versions reveals:

| Component | Resolved Version | Location / Source |
| :--- | :--- | :--- |
| **PlatformIO Core** | `6.1.19` | Local Python environment |
| **Platform** | `Espressif 32 (6.5.0)` | `~/.platformio/platforms/espressif32` |
| **Board Definition** | `lilygo-t-display-s3` | `~/.platformio/platforms/espressif32/boards/lilygo-t-display-s3.json` |
| **Framework** | `framework-arduinoespressif32 @ 3.20014.231204 (2.0.14)` | Based on ESP-IDF `v4.4.6` |
| **Toolchain** | `xtensa-esp32s3-elf @ 8.4.0+2021r2-patch5` | Precompiled toolchain |
| **Display Library** | `TFT_eSPI @ 2.5.43` | `.pio/libdeps/lilygo-t-display-s3/TFT_eSPI` |

### PlatformIO Configuration Observations (`platformio.ini`)
- `board = lilygo-t-display-s3`
- `framework = arduino`
- Current `build_flags` define display pinout and drivers (`ST7789_DRIVER`, parallel 8-bit bus, dimensions `170x320`).
- No USB flags (`ARDUINO_USB_MODE`, `ARDUINO_USB_CDC_ON_BOOT`, or TinyUSB) are currently declared in `platformio.ini`.
- Board default flags from `lilygo-t-display-s3.json` inject:
  - `-DARDUINO_USB_MODE=1`
  - `-DARDUINO_USB_CDC_ON_BOOT=1`
  This configuration binds the ESP32-S3 native USB port to a CDC Serial bridge for flashing and debug logs (`Serial.print`), leaving native composite HID unconfigured in application code.

---

## 2. Hardware Architecture & Pin Allocation

The LILYGO T-Display-S3 features an ESP32-S3 dual-core Xtensa LX7 MCU with built-in Full-Speed USB OTG (12 Mbps):

| Resource | Pins / Hardware | Status / Allocation in Project |
| :--- | :--- | :--- |
| **Native USB OTG** | `GPIO 19` (D-) / `GPIO 20` (D+) | Routed directly to USB Type-C connector |
| **Display Bus** | `GPIO 39-48` (D0-D7), `GPIO 5-9` (Ctrl) | Dedicated 8-bit parallel bus for ST7789 |
| **Display Backlight** | `GPIO 38` | Controlled via analogWrite (LEDC PWM) |
| **Buttons** | `GPIO 0` (Down/Boot), `GPIO 14` (Up) | Active-low with internal pull-ups |
| **Free / Auxiliary Pins** | Header breakout pins | Available for future I2S peripheral (e.g., INMP441) |

---

## 3. Evidence-Based USB Subsystem Feasibility

### 3.1 USB HID (Keyboard Emulation) — Feasible for Phase 3
Inspection of `framework-arduinoespressif32` shows:
- Header files available:
  - `cores/esp32/USB.h`
  - `libraries/USB/src/USBHID.h`
  - `libraries/USB/src/USBHIDKeyboard.h`
- In `sdkconfig`:
  - `CONFIG_TINYUSB_ENABLED=y`
  - `CONFIG_TINYUSB_HID_ENABLED=y`
- **Assessment**:
  The ESP32-S3 hardware and Arduino framework support USB HID Keyboard emulation. When initialized via `USB.begin()` and `USBHIDKeyboard.begin()`, the device can register as a Human Interface Device to send simulated keystrokes (e.g., casting or reeling keys).
- **Prerequisites for Implementation**:
  1. Configure `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=0` (or composite CDC+HID).
  2. Implement safe connection guards to prevent USB lockouts or host enumeration loops.
  3. Keep disabled in Phase 2 in strict accordance with project requirements.

### 3.2 USB Audio Class (UAC) — Current Setup Status & Feasibility Qualification
Inspection of `tools/sdk/esp32s3/include/arduino_tinyusb/include/tusb_config.h` and `sdkconfig` reveals:
- `CONFIG_TINYUSB_CDC_ENABLED=y`
- `CONFIG_TINYUSB_MSC_ENABLED=y`
- `CONFIG_TINYUSB_HID_ENABLED=y`
- `CONFIG_TINYUSB_MIDI_ENABLED=y`
- `CONFIG_TINYUSB_VIDEO_ENABLED=y`
- `CONFIG_TINYUSB_DFU_ENABLED=y`
- `CONFIG_TINYUSB_VENDOR_ENABLED=y`
- **`CONFIG_TINYUSB_AUDIO_ENABLED` is omitted from the precompiled Arduino SDK configuration**.
- Low-level TinyUSB audio class headers are present on disk under `arduino_tinyusb/tinyusb/src/class/audio/` (`audio.h`, `audio_device.h`).

**Evidence-Bounded Assessment**:
- The current Arduino 2.0.14 setup has no demonstrated ready-to-use UAC speaker capture path or configuration in this project, and the proposed UAC route is unverified.
- Platform-wide impossibility should not be asserted solely from the absence of the `CONFIG_TINYUSB_AUDIO_ENABLED` macro in the prebuilt sdkconfig. However, successfully operating a UAC speaker capture device (receiving PC game audio over USB) requires valid isochronous OUT endpoint descriptors, host audio stream synchronization, and circular audio DMA buffering, none of which currently exist in this project or standard Arduino ESP32 wrappers.
- A minimal TinyUSB/ESP-IDF compatibility proof of concept is required before concluding definitive feasibility or deciding whether framework migration is warranted.
- Phase 4 (real audio capture / hardware keystroke injection) remains **explicitly unimplemented and unverified** in this project.

**Alternative Capture Routes (Unverified Options for Future Consideration)**:
If physical audio detection is explored in future phases after validation:
- Dedicated hardware I2S microphone (e.g., INMP441 / ICS-43434) connected to auxiliary GPIO breakout pins.
- Or an analog line-in to the ESP32-S3 ADC with an external operational amplifier.
- Both routes represent unverified alternatives that would decouple audio intake from the USB bus.

---

## 4. Phase 2 Simulated State & Dashboard Architecture

To satisfy all Phase 2 criteria cleanly without external dependencies:

```
                      +-----------------------------+
                      |       Button Handler        |
                      | (Debounce & Release Logic)  |
                      +--------------+--------------+
                                     |
               +---------------------+---------------------+
               |                     |                     |
     Short Press (Release)   Long Press Up (800ms)  Long Press Down (800ms)
               |                     |                     |
      Cycle Display Mode       Toggle Start/Pause      Stop / Reset Simulation
      (PAXG/Clock/Weather/     (Running <-> Paused)    (Idle / Clear Counters)
       Fishing)                      |                     |
                                     v                     v
                      +-----------------------------+
                      |  Nonblocking State Machine  |
                      |   (src/modes/fishing_mode)  |
                      +--------------+--------------+
                                     |
               +---------------------+---------------------+
               |                     |                     |
               v                     v                     v
         [CASTING]            [WAITING BITE]          [REELING]
       (1.5s timer)        (Synthetic audio gen)   (Sim Reel Cmd++)
                                     |
                          Audio Spike >= 0.75 Thresh
                                     |
                                     v
                      +-----------------------------+
                      |   TFT_eSprite (320x170)     |
                      |   Dark Cyan/Navy Dashboard  |
                      |    Zero-Flicker Rendering   |
                      +-----------------------------+
```

### Safety and Non-Interference Guarantees
1. **Existing Modes Preserved**: PAXG, Clock, Weather, Wi-Fi configuration, and brightness routines remain completely functional.
2. **Setup Isolation**: Wi-Fi selection and keyboard input flows in `src/hal/utils.cpp` execute in `app_setup()` prior to the main loop and are unaffected by runtime mode switching.
3. **Explicit Labeling**: Reel metrics are explicitly labeled `Sim Reel Cmds (Simulated - no catch data)` to avoid misrepresenting simulation as real automation.
4. **USB Status Notice**: Display explicitly renders `USB HID: UNAVAILABLE` and `USB UAC: NOT CONNECTED`.
