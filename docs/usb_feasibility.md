# USB Feasibility and Hardware Integration Report (Phases 1, 2 & 3)

## Executive Summary

This document provides an evidence-based assessment of USB capabilities for the LILYGO T-Display-S3 project under the current PlatformIO Arduino environment (`framework-arduinoespressif32 @ 3.20014.231204 / Arduino ESP32 2.0.14`).

Key conclusions:
1. **USB HID (Keyboard Keystroke Output)**: **Implemented in Phase 3**. The stock Espressif Arduino component (`USBHIDKeyboard`) provides standard keyboard HID reports with standard USB descriptors (no custom VID/PID, hidden descriptors, evasion, or anticheat modifications). Initialized at boot alongside USB CDC via `keyboard.begin()` and `USB.begin()`.
2. **Command Queue, Host-Mounted Gating & State Guards**: Implemented via a bounded nonblocking circular queue (`FISHING_KEY_QUEUE_CAPACITY = 8`). Press and release actions are strictly separated across independent service ticks (`FISHING_KEY_PRESS_HOLD_MS = 60ms`, `FISHING_KEY_RELEASE_GAP_MS = 60ms`). All keystroke transmission and arming are strictly gated behind actual host-mounted state (`_started && tinyusb_device_mounted`), Fishing mode, an active running state, and an explicit user armed flag (`fishingArmed`, toggled by holding both buttons).
3. **Thread-Safe Disconnect & Auto-Disarm**: Asynchronous disconnect/suspend events from the USB event task set a synchronized `std::atomic<bool> s_pendingDisconnect` signal without mutating state directly across threads. The signal is consumed in `usb_hid_keyboard_tick` on the Arduino main loop, immediately clearing the queue, calling `Keyboard.releaseAll()`, and auto-disarming (`fishingArmed = false`), preventing automated key sends upon reconnect. Reconnect and resume events do not re-arm the device.
4. **USB Audio Class (UAC 1.0 / 2.0)**: **Unverified / No Demonstrated Ready-to-Use Path in Current Setup**. The current Arduino 2.0.14 setup has no demonstrated ready-to-use UAC speaker capture path or configuration in this project, and the proposed UAC route remains unverified. While low-level TinyUSB audio class headers exist in the framework (`arduino_tinyusb/tinyusb/src/class/audio/`), `CONFIG_TINYUSB_AUDIO_ENABLED` is not present in the default Arduino SDK configuration. A minimal TinyUSB/ESP-IDF compatibility proof of concept is required before concluding feasibility or considering framework migration. Platform-wide impossibility is not claimed solely from the missing configuration macro. Phase 4 remains explicitly unimplemented and unverified.
5. **Validation Scope**: Local verification consists of clean PlatformIO compilation and linking for `lilygo-t-display-s3` (ESP32-S3). Physical hardware validation on an ESP32-S3 board and Windows 11 host composite driver enumeration remain unavailable in this environment and are explicitly disclaimed.

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
| **USB Library** | `USB @ 2.0.0` | `~/.platformio/packages/framework-arduinoespressif32/libraries/USB` |

### PlatformIO Configuration Observations (`platformio.ini`)
- `board = lilygo-t-display-s3`
- `framework = arduino`
- `build_flags` define display pinout and drivers (`ST7789_DRIVER`, parallel 8-bit bus, dimensions `170x320`).
- Board default flags from `lilygo-t-display-s3.json` inject:
  - `-DARDUINO_USB_MODE=1`
  - `-DARDUINO_USB_CDC_ON_BOOT=1`
- Compiling `USBHIDKeyboard` and `USB.h` with the stock PlatformIO LDF automatically locates `framework-arduinoespressif32/libraries/USB` (`libUSB.a`), successfully building without requiring modifications to `platformio.ini`.

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

### 3.1 USB HID (Keyboard Emulation) — Phase 3 Implementation
Inspection of `framework-arduinoespressif32` shows:
- Header files available:
  - `cores/esp32/USB.h`
  - `libraries/USB/src/USBHID.h`
  - `libraries/USB/src/USBHIDKeyboard.h`
- In `sdkconfig`:
  - `CONFIG_TINYUSB_ENABLED=y`
  - `CONFIG_TINYUSB_HID_ENABLED=y`
- **Assessment**:
  The ESP32-S3 hardware and Arduino framework support USB HID Keyboard emulation. When initialized via `USB.begin()` and `USBHIDKeyboard.begin()`, the device registers standard HID reports (boot/report keyboard).

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

---

## 4. Phase 2 Simulated State & Dashboard Architecture

Phase 2 established the core simulation engine:
- Nonblocking state machine: `STOPPED` -> `CASTING` -> `WAITING BITE` -> `REELING`.
- Synthetic audio test data simulating ambient water noise floor (~12-30%) with scheduled bite spike events (~82-97%) crossing the 75% threshold.
- Low-flicker 320x170 rendering using `fishing_display` sprite.
- In-mode button gestures preserving global mode navigation.

---

## 5. Phase 3 USB HID Keyboard Implementation & Verification

### 5.1 Architecture & Command Queue

```
                              +---------------------------------------+
                              |         Application Main Loop         |
                              |              (app_loop)               |
                              +-------------------+-------------------+
                                                  |
                                                  v
                              +---------------------------------------+
                              |        usb_hid_keyboard_tick()        |
                              +-------------------+-------------------+
                                                  |
                                                  v
                              +---------------------------------------+
                              | 1. s_pendingDisconnect.exchange(...)  |
                              |    -> auto-disarm + releaseAll()      |
                              +-------------------+-------------------+
                                                  |
                                                  v
                              +---------------------------------------+
                              | 2. Safety Gates Check:                |
                              |    a. Host mounted (usb_is_connected) |
                              |    b. mode == DisplayMode::Fishing    |
                              |    c. fishingArmed == true            |
                              |    d. state == Casting/Wait/Reeling   |
                              +-------------------+-------------------+
                                                  |
                          +-----------------------+-----------------------+
                          | PASS                                          | FAIL (Unmounted/Disarmed/Stopped)
                          v                                               v
       +------------------------------------+          +------------------------------------+
       |   Bounded Queue (Capacity: 8)      |          | Purge Queue & keyboard.releaseAll()|
       +------------------+-----------------+          +------------------------------------+
                          |
              +-----------+-----------+
              |                       |
              v                       v
     [KEY PRESS TICK]        [KEY RELEASE TICK]
   keyboard.press(key)      keyboard.release(key)
    (Hold: >= 60ms)          (Gap: >= 60ms)
```

### 5.2 Safety and Non-Interference Guarantees
1. **Stock USB Stack**: Uses stock Arduino `USBHIDKeyboard` standard reports; no custom VID/PID, hidden descriptors, evasion, or anticheat behavior.
2. **Asynchronous Thread Safety**: `usb_event_callback` runs in the USB event task and never mutates queue state or calls `releaseAll()` directly. It sets a synchronized `std::atomic<bool> s_pendingDisconnect` signal that is consumed strictly in `usb_hid_keyboard_tick()` on the Arduino loop task.
3. **Host-Mounted State Gating**: Keystroke enqueue and transmission strictly check actual host-mounted state (`_started && tinyusb_device_mounted` via `usb_hid_is_connected()`); USB initialization alone is not treated as a connected host. Unmounted state displays `USB: DISCONN` and `HID: UNAVAILABLE`.
4. **Auto-Disarm on Disconnect**: Consuming a disconnect or suspend signal immediately clears the queue, calls `releaseAll()`, and forces `fishingArmed = false`. Reconnection or resume events do not re-arm the device, eliminating automatic key sends upon reconnect.
5. **Strict Quadruple-Gate Enforcement**: Keystrokes cannot be queued or sent unless:
   - USB device is mounted on host.
   - Device is in `DisplayMode::Fishing`.
   - User has explicitly toggled `fishingArmed = true` (via dual-button hold).
   - Simulation state machine is in active running mode (`Casting`, `WaitingBite`, `Reeling`).
6. **Zero Output Outside Armed State**: When in `PAXG`, `Clock`, `Weather`, `Stopped`, or `Paused`, the queue is cleared and `releaseAll()` is called immediately.
7. **Separate Service Ticks**: Every key press and release event is guaranteed to occur on separate service ticks. No press event can be stranded.
8. **Configurable Safe Placeholders**: Key values are defined as safe constants (`FISHING_KEY_CAST = '1'`, `FISHING_KEY_INTERACT = '2'`).
9. **Gesture Compatibility**:
   - Short press UP / DOWN (< 800ms): Mode forward / backward on release across all modes.
   - Long press UP (>= 800ms) in Fishing mode: Toggle Start / Pause.
   - Long press DOWN (>= 800ms) in Fishing mode: Stop simulation (1st hold) / Reset counters (2nd hold).
   - Both buttons held (>= 800ms) in Fishing mode: Toggle Armed / Disarmed safety state (rejected if host not mounted).
   - Mode switch away from Fishing mode automatically disarms and purges the queue.

### 5.3 Verification Matrix & Remaining Limitations

| Verification Item | Status in Phase 3 | Evidence / Notes |
| :--- | :--- | :--- |
| **Compilation & Link** | **VERIFIED** | PlatformIO `pio run` builds cleanly; firmware binary generated. |
| **LDF Auto-Discovery** | **VERIFIED** | Framework `libUSB.a` linked without requiring `platformio.ini` edits. |
| **Thread-Safe Callback** | **VERIFIED** | `std::atomic<bool>` signal avoids cross-task mutations. |
| **Host-Mounted Gating** | **VERIFIED** | Gated on `_started && tinyusb_device_mounted`. |
| **Auto-Disarm on Disconnect** | **VERIFIED** | Disconnect/suspend consumption forces `fishingArmed = false`. |
| **No Re-Arm on Resume** | **VERIFIED** | Resume/started events explicitly do not re-arm. |
| **Queue Boundedness** | **VERIFIED** | Buffer size fixed at 8 items; drops over-capacity requests. |
| **Press/Release Pairing**| **VERIFIED** | Tick state machine enforces `Idle` -> `Pressed` -> `Cooldown` -> `Idle`. |
| **Gating Verification** | **VERIFIED** | Code inspection confirms multiple layers of gating in enqueue and tick. |
| **Physical ESP32-S3** | **UNVERIFIED** | Physical board hardware execution not performed in this session. |
| **Windows 11 Composite** | **UNVERIFIED** | Host enumeration of CDC + HID composite endpoints unverified on Windows 11. |
| **Game Automation** | **UNVERIFIED** | Key outputs are placeholders; no game client integration tested. |
| **USB Audio (UAC)** | **UNVERIFIED** | Remained explicitly out of scope for Phase 3. |
