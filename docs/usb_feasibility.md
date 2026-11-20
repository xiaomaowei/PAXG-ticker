# USB Feasibility and Hardware Integration Report (Phases 1–7)

## Executive Summary

This document provides an evidence-based assessment of USB capabilities for the LILYGO T-Display-S3 project under the current PlatformIO Arduino environment (`framework-arduinoespressif32 @ 3.20014.231204 / Arduino ESP32 2.0.14 / ESP-IDF v4.4.6`).

Key conclusions:
1. **USB HID (Keyboard Keystroke Output)**: **Implemented in Phase 3**. The stock Espressif Arduino component (`USBHIDKeyboard`) provides standard keyboard HID reports with standard USB descriptors (no custom VID/PID, hidden descriptors, evasion, or anticheat modifications). Initialized at boot alongside USB CDC via `keyboard.begin()` and `USB.begin()`.
2. **Command Queue, Host-Mounted Gating & State Guards**: Implemented via a bounded nonblocking circular queue (`FISHING_KEY_QUEUE_CAPACITY = 8`). Press and release actions are strictly separated across independent service ticks (`FISHING_KEY_PRESS_HOLD_MS = 60ms`, `FISHING_KEY_RELEASE_GAP_MS = 60ms`). All keystroke transmission and arming are strictly gated behind actual host-mounted state (`_started && tinyusb_device_mounted`), Fishing mode, an active running state, and an explicit user armed flag (`fishingArmed`, toggled by holding both buttons).
3. **Thread-Safe Disconnect & Auto-Disarm**: Asynchronous disconnect/suspend events from the USB event task set a synchronized `std::atomic<bool> s_pendingDisconnect` signal without mutating state directly across threads. The signal is consumed in `usb_hid_keyboard_tick` on the Arduino main loop, immediately clearing the queue, calling `Keyboard.releaseAll()`, and auto-disarming (`fishingArmed = false`), preventing automated key sends upon reconnect. Reconnect and resume events do not re-arm the device.
4. **USB Audio Class (UAC 2.0 Feasibility Proof) & Simultaneous Composite UAC 2.0 + HID**:
   - **Production Firmware (Arduino ESP32 2.0.14 / ESP-IDF 4.4.6)**: **Unverified and Not Implemented in Production**. In PlatformIO's stock `framework-arduinoespressif32` 2.0.14 distribution, `libarduino_tinyusb.a` omits audio symbols and `usbd.c` was compiled with `CFG_TUD_AUDIO = 0`. An isolated compile/link proof (`test/usb_audio_poc/**`) confirmed that while TinyUSB audio headers compile, linking fails with `undefined reference to tud_audio_n_read`. No UAC code enters production builds.
   - **Isolated ESP-IDF >= 5.0 Proof (`test/usb_audio_poc_idf/**`)**: **Symbol Feasibility Confirmed**. Using Espressif's official `esp_device_uac` (`espressif/usb_device_uac` v1.3.2) on ESP-IDF `v5.1.2`, an isolated PlatformIO project compiles and links cleanly (`firmware.elf`, exit code 0). Linker symbols show code presence for `uac_device_init`, `tud_audio_n_read`, and `uac_speaker_output_cb`.
   - **Isolated Simultaneous Composite UAC 2.0 + HID Keyboard Proof (`test/usb_composite_poc/**`)**: **Symbol & Descriptor Feasibility Confirmed**. Demonstrates the smallest viable isolated path to simultaneous UAC 2.0 speaker playback (48kHz 16-bit stereo PCM, Isochronous OUT + Sync Feedback IN) and USB HID Keyboard (Interrupt IN) in a single unified composite USB configuration descriptor under ESP-IDF `v5.1.2`. `firmware.elf` links both UAC 2.0 symbols (`tud_audio_n_read`, `tud_audio_rx_done_isr`, `uac_speaker_output_cb`) and HID keyboard symbols (`tud_hid_n_keyboard_report`, `hidd_open`, `usb_composite_hid_keyboard_press`, `usb_composite_hid_keyboard_release`), sharing descriptor `desc_configuration` (177 bytes). Keystrokes are strictly gated behind an explicit debounced physical button press on GPIO 0 (no automatic periodic keystrokes), and cross-task telemetry uses lock-free C11 `stdatomic.h`.
   - **Evidence vs. Reality Qualification**: Successful compilation and resolved symbols demonstrate code presence and descriptor syntax only. They do not claim or prove Windows driver enumeration, simultaneous isochronous streaming with interrupt key injection, endpoint scheduling validity, physical audio playback, or in-game WoW bite recognition. Production firmware remains untouched.
5. **Modular PCM Audio Detector**: **Implemented in Phase 5**. A platform-independent, replaceable audio detection engine (`IAudioDetector` / `PcmAudioDetector`) processes PCM16 mono frames with bounded ring buffering (`PcmRingBuffer`), Peak, Root Mean Square (RMS), adaptive ambient noise floor tracking (EMA), dynamic thresholding, debounce confirmation, and cooldown lockout gating. Deterministic synthetic test patterns verify normal burst detection, noise spike debounce rejection, and cooldown gating.
6. **Synthetic PCM Loop, 9-State FSM & Multi-Page UI**: **Implemented in Phase 6**. The detector is connected to an in-engine bounded synthetic PCM source (128 samples per tick) generating baseline ambient noise (~250 RMS) with scheduled bursts (~6000 RMS). The state machine implements 9 explicit nonblocking states (`IDLE`, `CASTING`, `WAITING`, `BITE_DETECTED`, `REELING`, `COOLDOWN`, `TIMEOUT`, `PAUSED`, `ERROR`) timed via `millis()`. Three 320x170 UI pages (Dashboard, Audio Monitor, Settings) render into the `fishing_display` sprite without LCD flicker.
7. **Persistent Settings & Conflict-Free Controls**: **Implemented in Phase 7**. Parameters (`absoluteThreshold`, `waitTimeoutMs`, `cooldownMs`, `castKey`, `interactKey`) persist in Arduino Preferences (NVS namespace `paxg_fishing`) with boundary clamping. Physical button controls provide mutually exclusive long/short press handling, safe exit, and dual-button arming.
8. **Validation Scope & Physical Hardware Observation**:
   - **Verified on Physical Hardware**: Production firmware build, flash upload via `pio run -t upload --upload-port COM3` (ESP32-S3 rev v0.2, MAC `80:65:99:C6:3A:94`, VID:PID `303A:1001`), and serial boot startup (`PAXG ticker starting up` captured via monitor on COM3).
   - **Core Dump Notice**: Serial boot logged `E (221) esp_core_dump_flash: Incorrect size of core dump image: 1785838705` (recorded verbatim; not categorized as harmless).
   - **Windows 11 Host HID Enumeration**: On Windows 11 Pro (version 10.0.26200, build 26200), `Get-PnpDevice -PresentOnly` confirmed `Keyboard / HID Keyboard Device` (`HID\VID_303A&PID_1001\6&C30D08&0&0000`, OK) and `HIDClass / USB 輸入裝置` (`USB\VID_303A&PID_1001\806599C63A94`, OK), confirming Windows HID enumeration only. No keystroke was sent and HID key delivery was not tested.
   - **Remaining Unverified**: Screen appearance/backlight (TFT LCD), Wi-Fi/data freshness, UAC hardware/Windows audio enumeration/capture, and in-game bite recognition remain unverified.
   - **UAC PoC Boundary**: The isolated ESP-IDF UAC PoC was compiled and linked cleanly, but was **NOT flashed** to the physical board. Production Arduino firmware remains strictly untouched.

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

### 1.1 Production Build & Physical Target Hardware Evidence (COM3)

Local execution and physical hardware observation on a connected device yielded the following empirical evidence:

1. **Production Build (`pio run`)**:
   - **Framework**: PlatformIO Arduino ESP32 `2.0.14` (ESP-IDF `v4.4.6`)
   - **Resource Utilization**:
     - RAM: `83,348 / 327,680 bytes` (25.4%)
     - Flash: `1,121,797 / 6,553,600 bytes` (17.1%)
   - **Result**: Successful compilation and linkage (`firmware.elf`).

2. **Target Flashing via esptool (`pio run -t upload --upload-port COM3`)**:
   - **Port & Hardware Identity**: `COM3`, Espressif USB VID:PID `303A:1001`, PNP serial `80:65:99:C6:3A:94`.
   - **Chip Detected**: ESP32-S3 (revision v0.2), MAC Address: `80:65:99:C6:3A:94`.
   - **Verification**: All bootloader, partition table, and application SHA-256 hashes verified on-chip.
   - **Flash Operation**: Application successfully written at offset `0x10000`, followed by a hardware reset via RTS.

3. **Serial Monitor Boot Capture (`pio device monitor -p COM3 -b 115200`)**:
   - Following manual DTR toggle, the monitor captured ROM bootloader output and the application banner:
     ```
     PAXG ticker starting up
     ```
   - Confirmed application initialization and entry into main task execution.
   - **Core Dump Warning**: During boot initialization, the ESP-IDF flash partition component reported:
     ```
     E (221) esp_core_dump_flash: Incorrect size of core dump image: 1785838705
     ```
     *Note*: This error indicates that the core dump partition header contains an invalid image size value (`1785838705`, hex `0x6A706771`). This warning is recorded verbatim as an observed hardware runtime diagnostic and is not dismissed as harmless.

4. **Windows 11 Host HID Enumeration (`Get-PnpDevice`)**:
   - **Host Environment**: Windows 11 Pro, version `10.0.26200`, build `26200`.
   - **PnP Device Discovery**: `Get-PnpDevice -PresentOnly` successfully lists both HID interfaces from the connected ESP32-S3 board:
     - `Keyboard / HID Keyboard Device`: Instance ID `HID\VID_303A&PID_1001\6&C30D08&0&0000`, Status `OK`.
     - `HIDClass / USB 輸入裝置`: Instance ID `USB\VID_303A&PID_1001\806599C63A94`, Status `OK`.
   - **Hardware Correlation**: The instance serial `806599C63A94` matches the ESP32-S3 MAC/USB serial observed during esptool flash upload (`80:65:99:C6:3A:94`).
   - **Boundary**: This evidence confirms Windows OS-level HID composite device enumeration only. No keystroke was sent and HID key delivery was not tested.

5. **Strict Scope Separation**:
   - **Verified on Hardware / Host**: Production firmware build, flash programming, serial console boot, and Windows 11 HID device enumeration (`OK`).
   - **Unverified on Hardware / Host**: Physical LCD screen appearance/backlight, Wi-Fi connectivity and real-time data freshness, HID keystroke delivery to host applications, UAC audio device enumeration/streaming/capture, and WoW in-game bite detection.
   - **UAC PoC Flashing Status**: The isolated ESP-IDF UAC proof (`test/usb_audio_poc_idf/`) was compiled and linked to verify driver symbols, but was **NOT flashed** to the physical board. Production Arduino firmware remains deployed without modification.

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

### 3.2 USB Audio Class (UAC) — Status & Empirical Qualification
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

---

## 6. Phase 4 Isolated USB Audio (UAC OUT) Feasibility Proof

### 6.1 Objective & Strict Isolation
Phase 4 investigated the feasibility of receiving host PC game audio directly over native USB OTG using the USB Audio Class (UAC 1.0 speaker receiver).

To prevent experimental code from impacting production builds, all Phase 4 assets are maintained strictly within an isolated folder:
- [test/usb_audio_poc/usb_audio_poc.h](test/usb_audio_poc/usb_audio_poc.h)
- [test/usb_audio_poc/usb_audio_poc.cpp](test/usb_audio_poc/usb_audio_poc.cpp)
- [test/usb_audio_poc/run_validation.py](test/usb_audio_poc/run_validation.py)
- [test/usb_audio_poc/README.md](test/usb_audio_poc/README.md)

Because `platformio.ini` defaults `src_dir` to `src/`, items inside `test/` are automatically excluded from the production firmware build (`pio run`).

### 6.2 Composite Device Coexistence Architecture
In a hypothetical composite configuration containing CDC, HID, and UAC Audio OUT, the USB descriptors and hardware endpoints would be partitioned as follows:

| Interface Index | Class | Function | Endpoints Assigned | Transfer Type |
| :--- | :--- | :--- | :--- | :--- |
| **0 & 1** | USB CDC-ACM | Serial console / programming | `EP 1 IN` (Notification), `EP 2 OUT` & `EP 2 IN` (Data) | Bulk / Interrupt |
| **2** | USB HID | Keyboard keystroke injection | `EP 3 IN` | Interrupt |
| **3** | USB Audio Control (AC) | Clock source, terminal, feature unit controls | `EP 0` (Default Control) | Control |
| **4** | USB Audio Streaming (AS) | PCM Audio OUT from Host to Device | `EP 4 OUT` | Isochronous (Sync/Async) |

### 6.3 Empirical Validation Commands & Results
Validation was executed using the toolchain binary `xtensa-esp32s3-elf-g++` and library inspection utility `xtensa-esp32s3-elf-nm`:

```powershell
python test/usb_audio_poc/run_validation.py
```

**Output Log**:
```
==================================================================
PAXG-ticker Phase 4 & Phase 5 Feasibility & Validation Harness
==================================================================

--- Step 1: Inspecting TinyUSB precompiled symbols ---
libarduino_tinyusb.a path: C:\Users\020183/.platformio/packages/framework-arduinoespressif32\tools\sdk\esp32s3\lib\libarduino_tinyusb.a
Total matching 'audio' or 'tud_audio' symbols: 0
  -> CONFIRMED: libarduino_tinyusb.a contains ZERO audio symbols.

--- Step 2: Compiling isolated test/usb_audio_poc/usb_audio_poc.cpp ---
PoC Compilation return code: 0
  -> SUCCESS: test/usb_audio_poc/usb_audio_poc.cpp compiled successfully into object file.

--- Step 3: Attempting link of isolated PoC against libarduino_tinyusb.a ---
PoC Link return code: 1 (Expected failure: missing audio symbols)
PoC Link output (showing unresolved references to tud_audio_*):
    test/usb_audio_poc/usb_audio_poc.o: in function `tud_audio_read':
    usb_audio_poc.cpp:(.text+0x15): undefined reference to `tud_audio_n_read'
    collect2.exe: error: ld returned 1 exit status
```

### 6.4 Technical Root Cause Analysis
1. **Header vs Source Availability**: While header declarations exist in `arduino_tinyusb/tinyusb/src/class/audio/audio_device.h`, the corresponding implementation file `audio_device.c` is not included in the framework source distribution.
2. **Missing Precompiled Symbols**: Scanning `libarduino_tinyusb.a` with `xtensa-esp32s3-elf-nm -g` yields 0 symbols matching `tud_audio*`. In contrast, `tud_cdc_*`, `tud_hid_*`, `tud_midi_*`, `tud_msc_*`, and `tud_video_*` are fully defined.
3. **Core TinyUSB Dispatch Table**: TinyUSB's device dispatcher (`usbd.c`) inside `libarduino_tinyusb.a` was compiled with `CFG_TUD_AUDIO = 0`. Even if external `audio_device.o` were supplied, the core stack lacks the driver dispatch entry in its internal class table.
4. **Prebuilt SDK Configuration**: `CONFIG_TINYUSB_AUDIO_ENABLED` is missing from `sdkconfig` and `tusb_config.h`.

### 6.5 Feasibility Qualification & Policy
- **Link/Config Limitation**: This is a link-time and configuration limitation of the stock Arduino ESP32 2.0.14 package, not an architectural or hardware impossibility of the ESP32-S3 OTG peripheral.
- **Strict Guarding**: UAC cannot link or function under the current framework without rebuilding ESP-IDF static libraries or performing an unverified framework migration.
- **Reporting Status**: As mandated, Phase 4 is explicitly reported as **not implemented and unverified in production firmware**. No UAC code is linked into the production binary.

### 6.6 Phase 4.1 Isolated ESP-IDF >= 5.0 Official esp_device_uac Feasibility Proof

To empirically test whether ESP32-S3 hardware supports native USB Audio Class under supported toolchains, an isolated PlatformIO ESP-IDF project was implemented in [test/usb_audio_poc_idf/](test/usb_audio_poc_idf/):

1. **Environment Configuration**:
   - **PlatformIO Core**: `6.1.19`
   - **Platform**: `espressif32 @ 6.5.0`
   - **Framework**: `framework-espidf @ 3.50102.240122` (ESP-IDF `v5.1.2`)
   - **Toolchain**: `toolchain-xtensa-esp32s3 @ 12.2.0+20230208` (GCC 12.2.0)
   - **Component**: Espressif official `esp_device_uac` (`espressif/usb_device_uac` v1.3.2)
   - **TinyUSB Stack**: `espressif/tinyusb` (v0.19.0~3) compiled directly from source with `audio_device.c`

2. **Public C API & No-Codec PCM Callback**:
   Uses the official public API from `usb_device_uac.h`:
   ```c
   uac_device_config_t uac_config = {
       .skip_tinyusb_init = false,
       .output_cb = uac_speaker_output_cb, // Speaker playback (Host PC -> ESP32-S3)
       .input_cb = NULL,                  // Output-only speaker; microphone not needed
       .set_mute_cb = NULL,
       .set_volume_cb = NULL,
       .cb_ctx = &s_poc_stats,
   };
   ESP_ERROR_CHECK(uac_device_init(&uac_config));
   ```
   The registered callback `uac_speaker_output_cb` processes isochronous OUT packets, tracking received packet counts, raw byte counts, and 16-bit PCM sample counts without requiring external audio codec hardware.

3. **Empirical Compilation and Linkage Evidence**:
   ```powershell
   pio run -d test/usb_audio_poc_idf
   ```
   - **Exit Code**: `0`
   - **RAM Usage**: `15,968 bytes` (~4.9% of 320 KB)
   - **Flash Usage**: `227,189 bytes` (~21.7% of 1 MB app partition)
   - **Artifacts**: `test/usb_audio_poc_idf/.pio/build/esp32s3_uac_poc/firmware.elf` and `firmware.bin`

4. **Resolved Symbols via Binary Inspection (`xtensa-esp32s3-elf-nm`)**:
   Binary analysis confirms that all symbols missing under the stock Arduino static library are fully resolved in the ESP-IDF binary:
   - `42001b40 T uac_device_init`
   - `420027d0 T tud_audio_n_read`
   - `4200284c T tud_audio_n_write`
   - `42001a38 T tud_audio_rx_done_isr`
   - `420013cc t uac_speaker_output_cb`

5. **Automated Verification Harness**:
   The validation script [test/usb_audio_poc_idf/run_validation.py](test/usb_audio_poc_idf/run_validation.py) automates the end-to-end check:
   - Asserts isolated build succeeds (`returncode == 0`).
   - Asserts `firmware.elf` and `firmware.bin` are generated.
   - Inspects and asserts all four critical UAC symbols are present and resolved.
   - Asserts production firmware builds cleanly (`pio run`, `returncode == 0`).

6. **Scope Boundaries & Clear Separation of Concerns**:
   - **Firmware Compilation & Link**: **VERIFIED**. The ESP-IDF toolchain and official Espressif UAC driver successfully compile and link.
   - **Physical Hardware Execution**: **UNVERIFIED**. Flashing to a physical ESP32-S3 board was not performed.
   - **Windows 11 Composite Enumeration**: **UNVERIFIED**. Host enumeration of composite CDC + HID + UAC endpoints remains unverified on real Windows hosts.
   - **Production Isolation**: Production firmware remains strictly on Arduino ESP32 2.0.14 without UAC integration.

---

### 6.7 Phase 4.2 Smallest Viable Path: Simultaneous UAC 2.0 Speaker + HID Keyboard Composite PoC

#### 6.7.1 Objective & Research Problem
Moving toward real one-button fishing automation requires simultaneous USB operation:
1. **Host PC Audio Ingestion**: Real-time USB Audio Class 2.0 (UAC 2.0 speaker OUT, `bcdADC = 0x0200`) receiving game sound from the host PC so that splash/bite audio can be recognized by the DSP detector without audio cables or analog microphones.
2. **Device Keystroke Injection**: USB HID Keyboard sending cast and reel keystrokes (`HID_KEY_1`, `HID_KEY_SPACE`) back to the host PC.
3. **One-Button Real Operation**: Eliminating simulated audio feeds and coordinating real audio input with physical button-triggered state transitions.

#### 6.7.2 Technical Investigation: Custom Arduino TinyUSB vs. Arduino-as-IDF-Component
Two architectural pathways were evaluated for achieving simultaneous UAC + HID while preserving existing TFT display (8-bit parallel ST7789) and Wi-Fi features:

##### Pathway A: Custom TinyUSB Build under Current Arduino 2.0.14 (ESP-IDF 4.4.6)
- **Status**: **Framework Packaging Boundary in Stock Arduino ESP32 2.0.14 Distribution**.
- **Evidence-Backed Root Causes**:
  1. **Precompiled Static Archive**: In PlatformIO Arduino ESP32 2.0.14 (`framework-arduinoespressif32 @ 3.20014.231204`), TinyUSB is precompiled into `tools/sdk/esp32s3/lib/libarduino_tinyusb.a`. `xtensa-esp32s3-elf-nm -g` confirms **zero** symbols matching `tud_audio*`.
  2. **Compile-Time Dispatch Table Gating**: TinyUSB's device dispatcher (`usbd.c`) builds `_usbd_class_drivers[]` based on macro flags. Because `usbd.c.obj` was precompiled with `CFG_TUD_AUDIO = 0`, the internal class driver array has no slot or entry point for the audio driver. Compiling external audio source files in user application code cannot inject entries into `_usbd_class_drivers[]`.
  3. **Rigid Arduino TinyUSB HAL**: The framework's `cores/esp32/esp32-hal-tinyusb.c` enforces a hardcoded enum (`USB_INTERFACE_MSC`, `DFU`, `HID`, `VENDOR`, `CDC`, `MIDI`, `CUSTOM`). It has no definition for Audio and assumes single-alternate interface structures, whereas UAC requires Alternate Setting 0 (zero bandwidth) and Alternate Setting 1 (active isochronous streaming).
  4. **Driver Inexistence**: The official Espressif `usb_device_uac` driver requires ESP-IDF >= 5.0 and cannot compile against ESP-IDF 4.4.6 headers.
- **Conclusion**: Building a custom TinyUSB stack inside the stock Arduino 2.0.14 tree is infeasible without replacing precompiled vendor SDK binaries, which violates pinned dependency integrity and reproducible builds.

##### Pathway B: Compatible Arduino-as-IDF-Component Integration (ESP-IDF 5.1 / Arduino 3.x)
- **Status**: **Architecturally Viable Long-Term Destination, with Concrete Migration Considerations for Production Firmware**.
- **Investigation Details**:
  1. **USB Subsystem**: Fully compatible. Under ESP-IDF 5.1.2, TinyUSB is compiled from source with full class driver selection (`CFG_TUD_AUDIO=1`, `CFG_TUD_HID=1`).
  2. **TFT Display Driver Architectural Consideration**: In the PAXG production firmware, the LilyGo T-Display-S3 ST7789 display is driven by `TFT_eSPI` over an 8-bit parallel bus (`-DTFT_PARALLEL_8_BIT=1`, GPIOs 39-48 for data, GPIOs 5-9 for control). `TFT_eSPI_ESP32_S3.c` directly manipulates legacy ESP-IDF 4.4 I2S/LCD parallel register interfaces. Under ESP-IDF 5.x, Espressif refactored parallel display interfaces under the `esp_lcd` component (`esp_lcd_panel_io_i80`). Migrating production firmware would require adapting or replacing `TFT_eSPI` with an `esp_lcd`-compatible parallel driver. Note that this is an architectural interface migration task; no build failure has been established within this scope because the production firmware has not been compiled under ESP-IDF 5.x.
  3. **Wi-Fi and RTOS Changes**: ESP-IDF 5.1 updates FreeRTOS to v10.5+ (header reorganizations) and introduces updated Wi-Fi event handler semantics.
- **Conclusion**: ESP-IDF 5.1 is the authoritative target framework for UAC+HID composite support. To minimize risk, the smallest viable path is to validate the composite USB stack in an isolated ESP-IDF 5.1 harness first, establishing the exact integration contract before altering production display drivers.

#### 6.7.3 Isolated Simultaneous Composite PoC (`test/usb_composite_poc/`)
To conclusively verify that the ESP32-S3 toolchain and USB OTG descriptor architecture can register and link both UAC 2.0 speaker and HID keyboard in the same device, an isolated PlatformIO ESP-IDF project was implemented in [test/usb_composite_poc/](test/usb_composite_poc/):

1. **Environment Configuration**:
   - **PlatformIO Core**: `6.1.19`
   - **Platform**: `espressif32 @ 6.5.0`
   - **Framework**: `framework-espidf @ 3.50102.240122` (ESP-IDF `v5.1.2`)
   - **Toolchain**: `toolchain-xtensa-esp32s3 @ 12.2.0+20230208` (GCC 12.2.0)
   - **TinyUSB Component**: `espressif/tinyusb` (v0.19.0~3) compiled from source with `audio_device.c` and `hid_device.c`
   - **Composite Driver**: Custom unified component [test/usb_composite_poc/components/usb_device_composite/](test/usb_composite_poc/components/usb_device_composite/)

2. **Unified Composite USB Descriptor Architecture & Bytecode Inspection**:
   The composite device descriptor binds both interfaces under an Interface Association Descriptor (IAD) architecture. Inspection of `desc_configuration` (177 bytes total) extracted directly from the compiled ELF confirms:
   - **Device Descriptor**: `bDeviceClass = TUSB_CLASS_MISC (0xEF)`, `bDeviceSubClass = 0x02`, `bDeviceProtocol = 0x01 (IAD)`, VID `0x303A`, PID `0x8002`.
   - **Audio IAD**: Length 8, `bFirstInterface = 0`, `bInterfaceCount = 2`, `bFunctionClass = 0x01` (Audio), `bFunctionProtocol = 0x20` (IP Version 02.00).
   - **Interface 0 (Audio Control)**: Standard AC Interface + CS AC Header (`bcdADC = 0x0200`) + Clock Source (`UAC2_ENTITY_CLOCK`) + SPK Input Terminal + SPK Feature Unit + SPK Output Terminal.
   - **Interface 1 (Audio Streaming)**: Standard AS Interface (Alt 0 = 0 bandwidth; Alt 1 = active streaming) + Type I PCM Format Descriptor (48kHz, 16-bit Stereo PCM) + Isochronous Data OUT Endpoint (`0x01`, `maxPacketSize = 196 bytes`, interval 1ms) + Isochronous Sync Feedback IN Endpoint (`0x81`, `maxPacketSize = 4 bytes`, interval 1ms).
   - **Interface 2 (HID Keyboard)**: Standard HID Interface (`bInterfaceClass = 0x03`, `bInterfaceSubClass = 0x01` Boot, `bInterfaceProtocol = 0x01` Keyboard) + HID Descriptor + Interrupt IN Endpoint (`0x82`, `maxPacketSize = 16 bytes`, 10ms polling interval) + Boot Keyboard Report Descriptor (`desc_hid_report`, standard 8-byte input report).

3. **ESP32-S3 Endpoint Budget Verification**:
   - `OUT` Endpoints: `EP 0 OUT` (Control, 64B), `EP 1 OUT` (Audio Streaming, 196B) -> 2 of 5 used.
   - `IN` Endpoints: `EP 0 IN` (Control, 64B), `EP 1 IN` (Audio Feedback, 4B), `EP 2 IN` (HID Keyboard, 16B) -> 3 of 5 used.
   - **Verdict**: Hardware limits (5 IN / 5 OUT endpoints, 1024-byte FIFO) are completely respected with ample margin.

4. **Public C API**:
   ```c
   // Composite device initialization (UAC 2.0 speaker + HID keyboard)
   esp_err_t usb_composite_init(usb_composite_config_t *config);
   esp_err_t uac_device_init(usb_composite_config_t *config);

   // Keystroke injection
   bool usb_composite_hid_ready(void);
   esp_err_t usb_composite_hid_keyboard_press(uint8_t modifier, uint8_t keycode);
   esp_err_t usb_composite_hid_keyboard_release(void);
   ```

5. **Empirical Compilation and Linkage Evidence**:
   ```powershell
   pio run -d test/usb_composite_poc
   ```
   - **Exit Code**: `0`
   - **RAM Usage**: `16,040 bytes` (~4.9% of 320 KB)
   - **Flash Usage**: `234,768 bytes` (~22.4% of 1 MB app partition)
   - **Binary Artifacts**: `test/usb_composite_poc/.pio/build/esp32s3_composite_poc/firmware.elf` and `firmware.bin`

6. **Resolved Linker Symbols via `xtensa-esp32s3-elf-nm`**:
   Inspection confirms that all critical UAC audio, HID keyboard, and connection status symbols are fully resolved in the same ELF binary:
   - `3c028d04 D desc_configuration` (Shared Composite Configuration Descriptor)
   - `3c028db8 d desc_hid_report` (HID Keyboard Report Descriptor)
   - `42013520 T hidd_open` (TinyUSB HID Class Driver Open Handler)
   - `4201446c T tud_audio_n_read` (TinyUSB UAC Isochronous Data Read API)
   - `42001c4c T tud_audio_rx_done_isr` (TinyUSB UAC Audio RX Done ISR Handler)
   - `42017664 T tud_hid_descriptor_report_cb` (TinyUSB HID Report Descriptor Callback)
   - `420134c0 T tud_hid_n_keyboard_report` (TinyUSB HID Keyboard Report Transmission)
   - `4201cd68 t uac_speaker_output_cb` (No-Codec PCM Frame Reception Callback)
   - `42001d10 T usb_composite_hid_keyboard_press` (Composite HID Keyboard Keypress API)
   - `42001d68 T usb_composite_hid_keyboard_release` (Composite HID Keyboard Release API)
   - `42001cf0 T usb_composite_is_mounted` (Composite Device Mount Status API)
   - `42001db0 T usb_composite_init` (Composite Device Initialization API)
   - `42001f3c T uac_device_init` (Compatible UAC Device Initialization API)

7. **Safety, Concurrency & Input Control Design (`main.c` & `btn_fsm.h`)**:
   - **Connection State vs. Transient Endpoint Readiness**: `usb_composite_is_mounted()` queries `tud_mounted() && !tud_suspended()` to evaluate actual host bus enumeration and active configuration. `tud_hid_ready()` reflects temporary endpoint buffer readiness. Gating bus disconnection exclusively on `!usb_composite_is_mounted()` ensures that temporary endpoint busy states (e.g. Immediately following a press report while in flight) **never trigger false disconnect aborts**, preventing held keys from being dropped or stuck.
   - **Observed Stable Release on Startup & Mount**: The FSM initializes strictly in `BTN_STATE_STARTUP_WAIT_RELEASE`. If `GPIO 0` is held low across boot or USB mount, zero keystrokes are emitted. A stable release of >= 50ms is mandatory before arming to `BTN_STATE_IDLE`.
   - **Deterministic Single-Shot Actuation (No Auto-Repeat)**: Physical press is debounced for >= 50ms, held non-blocking for 50ms (`KEY_PRESS_HOLD_MS`, no task delays), and transitions to `BTN_STATE_WAIT_RELEASE`. Holding the button indefinitely emits strictly zero repeated keys.
   - **Release Report Verification & Non-Blocking Retry**: Return status of `usb_composite_hid_keyboard_release()` is checked: if busy (`ESP_ERR_NOT_FINISHED` / `BTN_OP_BUSY`), it retries non-blocking on subsequent ticks (`BTN_STATE_RETRY_RELEASE`). A successful key sequence is claimed **only when the release report is accepted**.
   - **Safe Abort on Real USB Disconnect/Suspend**: Genuine unmount or suspend during key hold or retry safely resets the FSM to `BTN_STATE_STARTUP_WAIT_RELEASE` without stuck keys or phantom transmissions, requiring a full observed release before re-arming on reconnect.
   - **Task Safety & Cross-Task Synchronization**: TinyUSB runs under FreeRTOS (`CFG_TUSB_OS = OPT_OS_FREERTOS`). Keystroke injection is called exclusively from `composite_monitor_task`. Metrics counters use C11 `stdatomic.h` primitives (`atomic_uint_fast32_t`), eliminating data races between the audio reception ISR/task and background monitor task.
   - **Evidence Boundaries & Deterministic Testing**:
     - *Host Python Tests (`test_button_fsm.py`)*: **HOST-EXECUTION VERIFIED** under Python 3.14 across 6 scenarios (busy right after press, release retry, true disconnect/reconnect, held at boot/mount, bounce, single actuation).
     - *Standalone C Test (`test_button_fsm.c`)*: Native host compiler probe (`vswhere`/MSVC `cl.exe`, `clang`, `gcc`) confirmed no native host C compiler on PATH. The test is strictly marked **COMPILE-ONLY VERIFIED** via `xtensa-esp32s3-elf-gcc` (`-Wall -Wextra -Werror`); host execution is not claimed for the C binary.

8. **Automated Verification Harness (`run_validation.py`)**:
   Execution of [test/usb_composite_poc/run_validation.py](test/usb_composite_poc/run_validation.py):
   ```powershell
   python test/usb_composite_poc/run_validation.py
   ```
   - Step 0: Probes native host C compiler; labels standalone C test as COMPILE-ONLY and cross-compiles cleanly with zero warnings.
   - Step 0b: Confirms all 6 host-runnable Button FSM deterministic unit tests pass under Python 3.14.
   - Step 1: Confirms isolated composite build succeeds (`returncode == 0`).
   - Step 2: Confirms binary artifacts (`firmware.elf`, `firmware.bin`) exist.
   - Step 3: Inspects and asserts all 13 critical UAC, HID, and connection status symbols exist in the binary.
   - Step 4: Extracts `desc_configuration` via `objdump` and verifies AC, AS, Format Type I, Isochronous OUT EP (196B), Feedback IN EP (4B), HID Interface, and Interrupt IN EP (16B).
   - Step 5: Confirms production firmware build remains clean (`pio run`, `returncode == 0`).
   - Step 6: Automatically cleans up temporary isolated build artifacts (`.pio`, `sdkconfig.*`).

9. **Roadmap to One-Button Real Fishing**:
   - **Phase A (Complete)**: Prove simultaneous UAC 2.0 speaker + HID keyboard compilation, descriptor bytecode cohabitation, and atomic ISR safety in an isolated image (`test/usb_composite_poc/`).
   - **Phase B (Display Driver Adaptation)**: Evaluate migrating ST7789 parallel 8-bit bus logic to ESP-IDF 5.x `esp_lcd_panel_io_i80` (or isolated driver component).
   - **Phase C (Detector Ingestion)**: Wire `uac_speaker_output_cb` PCM frames directly into `PcmAudioDetector::processFrame(...)` in place of synthetic samples.
   - **Phase D (Host Driver & In-Game Verification)**: Flash composite firmware to hardware, connect native USB OTG to Windows 11 host PC, verify audio endpoint plays game sound and HID keyboard injects fishing keys upon bite detection under the 4-gate safety interlock.

10. **Scope Boundaries & Clear Disclaimers**:
    - **Code Presence Only**: Successful compilation, linking, and descriptor inspection demonstrate **code presence only**. They do **NOT** prove Windows enumeration, simultaneous streaming, endpoint validity under Full-Speed (12Mbps) scheduling, real sample reception, hardware timing, or TFT/hardware compatibility.
    - **Physical Hardware Execution**: **UNVERIFIED**. The composite binary was compiled for symbol and descriptor verification and was **NOT flashed** to the physical board.
    - **Windows 11 Composite Driver Binding**: **UNVERIFIED**. Simultaneous enumeration and binding of Windows Audio Device (`usbaudio2.sys`) and HID Keyboard from this composite descriptor has not been tested on a physical Windows host.
    - **Production Isolation**: Production firmware remains strictly on Arduino ESP32 2.0.14 without UAC code.

---

## 7. Phase 5 Modular PCM Audio Detector & Synthetic Verification

### 7.1 Architecture & Modular Interface
Phase 5 implements a platform-independent, replaceable audio signal analysis module for PCM16 mono streams. It is decoupled from any hardware transport (USB, I2S, or synthetic generators) and contains zero game automation or blocking logic.

The module consists of:
- [include/modules/audio/audio_ring_buffer.h](include/modules/audio/audio_ring_buffer.h): `PcmRingBuffer<Capacity>` — Bounded, zero-allocation circular buffer.
- [include/modules/audio/audio_detector.h](include/modules/audio/audio_detector.h): `IAudioDetector` interface, `AudioDetectorConfig`, `AudioDetectorMetrics`, and `PcmAudioDetector`.
- [src/audio/pcm_audio_detector.cpp](src/audio/pcm_audio_detector.cpp): Implementation of frame-based DSP analysis.
- [include/modules/audio/synthetic_audio_samples.h](include/modules/audio/synthetic_audio_samples.h) & [src/audio/synthetic_audio_samples.cpp](src/audio/synthetic_audio_samples.cpp): Deterministic synthetic waveform generators for testing.

### 7.2 Non-Blocking DSP Pipeline & Mathematics

For each complete frame of $N$ samples ($N = \text{frameSize}$, default 128 samples / 8ms at 16kHz):

1. **Peak Detection**:
   $$\text{Peak} = \max_{0 \le i < N} |s_i|$$
   Computed by widening samples to `int32_t` before absolute value evaluation and storing the result in `uint32_t peak`. This ensures that boundary sample values like `INT16_MIN` ($-32768$) correctly evaluate to $+32768$ without signed integer overflow or `int16_t` narrowing distortion.
2. **Root Mean Square (RMS)**:
   $$\text{RMS} = \sqrt{\frac{1}{N} \sum_{i=0}^{N-1} s_i^2}$$
3. **Adaptive Ambient Noise Floor Tracking (EMA)**:
   When not in a burst or cooldown lockout ($\text{cooldownRemaining} = 0$) and $\text{RMS} < \text{effectiveThreshold}$:
   $$\text{NoiseFloor}_{k} = (1 - \alpha) \cdot \text{NoiseFloor}_{k-1} + \alpha \cdot \text{RMS}$$
   where $\alpha = \text{noiseFloorAlpha}$ (default 0.05). If $\text{NoiseFloor} < 1.0$, it is clamped to $1.0$.
4. **Dynamic Effective Threshold**:
   $$\text{EffectiveThreshold} = \max(\text{absoluteThreshold}, \text{NoiseFloor} \cdot \text{thresholdRatio})$$
   (Defaults: $\text{absoluteThreshold} = 1500$, $\text{thresholdRatio} = 2.5$).
5. **Debounce Confirmation**:
   - If $\text{RMS} \ge \text{EffectiveThreshold}$: increment $\text{consecutiveAbove}$.
   - If $\text{consecutiveAbove} \ge \text{debounceFrames}$ (default 2 frames): confirm trigger event, latch `triggered = true`, increment `triggerCount`, and initialize cooldown timer.
   - If $\text{RMS} < \text{EffectiveThreshold}$: reset $\text{consecutiveAbove} = 0$.
6. **Cooldown Lockout Gating**:
   - Once triggered, $\text{cooldownRemaining}$ is loaded with $\text{cooldownFrames}$ (default 25 frames ~ 200ms).
   - During cooldown, new triggers are locked out and noise floor updates are suppressed, preventing double-triggering on tail reverberations.

### 7.3 Synthetic Unit Test Suite Coverage
The test suite in [test/audio_detector/test_audio_detector.cpp](test/audio_detector/test_audio_detector.cpp) exercises deterministic behaviors:

| Test Case | Scenario / Injected Stimulus | Expected Output & Invariant | Status |
| :--- | :--- | :--- | :--- |
| **`test_ring_buffer_basics_and_bounds`** | Push, pop, peek, full saturation, capacity limits | Exact counts match; push on full returns false; no buffer overruns | **PASS** |
| **`test_ring_buffer_overwrite_and_wraparound`** | Saturate ring buffer; push overwrite extra samples | Tail advances properly; newest samples retained; count clamped | **PASS** |
| **`test_rms_and_peak_accuracy`** | Silence (0), Pure DC (1200), Square wave (±3000), `INT16_MIN` (-32768) impulse, `INT16_MIN` DC, `INT16_MAX` (+32767) DC | Peak handles 32768 magnitude without narrowing; RMS matches mathematical ground truth | **PASS** |
| **`test_adaptive_noise_floor`** | Ambient noise (~400 RMS) followed by single loud burst (5000) | Noise floor tracks ambient slowly; immune to burst corruption | **PASS** |
| **`test_transient_noise_debounce_rejection`** | 1-frame loud spike (> threshold) with `debounceFrames = 2` | Spike rejected by debounce filter; `triggerCount == 0` | **PASS** |
| **`test_valid_burst_trigger`** | Sustained 2-frame burst (> threshold) with `debounceFrames = 2` | Trigger event fires; `triggerCount == 1`; cooldown initialized | **PASS** |
| **`test_cooldown_rejection`** | Secondary burst arriving during active cooldown window | Rejected by cooldown gate; `triggerCount` remains 1 | **PASS** |
| **`test_cooldown_expiration_and_rearm`** | Quiet frames exhaust cooldown, followed by new valid burst | Detector re-arms; second burst triggers; `triggerCount == 2` | **PASS** |
| **`test_synthetic_scenario_end_to_end`** | Continuous 1280-sample stream (Ambient -> Burst -> Ambient) | 10 frames analyzed; exactly 1 bite trigger recorded | **PASS** |

### 7.4 Toolchain & Compilation Verification Harness
The validation harness [test/usb_audio_poc/run_validation.py](test/usb_audio_poc/run_validation.py) provides full dual-stage verification and asserts all expected results:
1. **Phase 4 PoC Assertions**:
   - Asserts compilation succeeds (`returncode == 0`).
   - Asserts linking fails (`returncode != 0`).
   - Asserts linking failure is specifically due to undefined `tud_audio` / `tud_audio_n_read` symbols.
2. **Phase 5 Detector Test Verification**:
   - Checks for native host compiler (`g++`, `clang++`, `cl.exe`) on PATH.
   - If available: compiles and links native binary, runs executable assertions, and reports test outcome counts.
   - If unavailable on host: explicitly reports that assertions were **not executed on host** and performs a target cross-compilation check across all test and implementation sources (`-Wall -Wextra -Werror`), labeled strictly as **compile-only** without claiming unit tests passed.
3. **Zero Temporary File Retention**:
   - Automatically unlinks any generated `compile_commands.json`, `.o`, `.elf`, and `.exe` files in a `finally` block, leaving zero temporary files in the repository.
4. **Production Firmware Compilation**:
   - Running `pio run` builds `src/audio/pcm_audio_detector.cpp` and `src/audio/synthetic_audio_samples.cpp` directly into `firmware.elf` without warnings or errors.

### 7.5 Explicit Disclaimers
- **No Real Audio Capture**: The detector currently processes synthetic PCM sample streams; no physical microphone or UAC USB stream is active.
- **No Bite Recognition Accuracy Claim**: The energy/RMS detector is a generic threshold-crossing building block and does not claim accurate discrimination of World of Warcraft fishing bobber splash acoustics.

---

## 8. Phase 6 & Phase 7 Software Integration Architecture

### 8.1 Deterministic Finite State Machine (`FishingStateMachine`)
The fishing simulation runs on a deterministic, platform-independent finite state machine class (`paxg::fishing::FishingStateMachine`) located in `include/modules/fishing_state_machine.h`. The FSM isolates all pure transition logic from side effects (timers, USB HID, displays, and NVS preferences are handled in caller):

```
                  +-----------------------------------+
                  |               IDLE                |
                  +-----------------------------------+
                                    | [Start Event]
                                    v
       +--------------------->  CASTING  <-----------------------+
       |                            | [CastComplete Event]       |
       |                            v                            |
       |                         WAITING                         |
       |                       /         \                       |
       |            [BiteDetected]     [BiteTimeout]             |
       |                     v             v                     |
       |               BITE_DETECTED    TIMEOUT                  |
       |                     |             |                     |
       |                     v [ReelTrigger]                     |
       |                  REELING          | [TimeoutComplete]   |
       |                     | [ReelComplete]                    |
       |                     +------+------+                     |
       |                            |                            |
       |                            v                            |
       +------------------------ COOLDOWN                        |
              [CooldownComplete]                                 |
              [Pause Event]              [Error Event]           |
       ANY -----------------> PAUSED     ANY -----------------> ERROR
       PAUSED --------------> CASTING    ERROR ---------------> IDLE (via Stop)
                 [Resume Event]
```

1. **`IDLE`**: Initial resting state. No keystrokes, silence fed to detector, detector reset.
2. **`CASTING`**: 1500ms duration. Enqueues cast key (if armed). Feeds low ambient ripple noise to detector.
3. **`WAITING`**: Ingests 128 samples per 8.0ms frame into `PcmAudioDetector`. Deterministically schedules bite bursts between 4.5s and 14s. Every 4th cycle deliberately schedules beyond `waitTimeoutMs` to exercise timeout transition.
4. **`BITE_DETECTED`**: Latching state upon `detector.isTriggered()`. Increments `simReelCmds`. Enqueues interact key (if armed). 300ms display before reeling.
5. **`REELING`**: 1200ms duration. Reeling splash audio generated. Transitions to `COOLDOWN`.
6. **`COOLDOWN`**: Configurable duration (`cooldownMs`, 500–5000ms, default 2000ms). Quiet noise. Loops back to `CASTING`.
7. **`TIMEOUT`**: Entered when `waitElapsed >= waitTimeoutMs` without bite trigger. Increments `simTimeouts`. Displays 1500ms before moving to `COOLDOWN`.
8. **`PAUSED`**: User suspended simulation. Keys immediately cleared and released. Resumes to `CASTING`.
9. **`ERROR`**: Entered upon USB disconnect or audio buffer fault while running. Forces `fishingArmed = false`, clears command queue, calls `releaseAll()`. Stop/Reset restores to `IDLE`.

### 8.2 Exact 16kHz PCM Audio Frame Timing
The runtime in `src/modes/fishing_mode.cpp` matches the declared 16kHz PCM audio configuration with exact mathematical precision:
- **Sample Rate**: $16,000\text{ Hz}$
- **Frame Size**: $128\text{ samples}$
- **Frame Duration**: $\frac{128}{16000}\text{ s} = 8.0\text{ ms}$
- **Synchronization**: `fishing_sim_tick()` advances frames at `FISHING_AUDIO_FRAME_INTERVAL_MS = 8UL` with a bounded catch-up loop (up to 4 frames per tick, max 32ms) to eliminate wall-clock drift without blocking delays. Millis-based FSM timers and 100ms (10Hz) UI rendering are strictly maintained.

### 8.2 Persistent Settings (NVS Preferences)
Settings are saved under NVS namespace `paxg_fishing`:
- `absoluteThreshold`: `[500, 5000]`, step `250`, default `1500`.
- `waitTimeoutMs`: `[5000, 30000]`, step `1000`, default `20000`.
- `cooldownMs`: `[500, 5000]`, step `500`, default `2000`.
- `castKey`: ASCII character, validated against allowed set (`'1'`, `'2'`, `'3'`, `'4'`, `'5'`, `'9'`, `'0'`, `' '`), default `'1'`.
- `interactKey`: ASCII character, validated against allowed set, default `'2'`.
- **Startup Safety**: Device always boots with `fishingArmed = false`. Armed state is never persisted in NVS.

### 8.3 Three UI Pages (320x170 Sprite)
- **Page 0 (Dashboard)**: `WOW FISHING [1/3 DASH]`. Telemetry cards for simulated reel commands, timeouts, live keys, state pill, wait/cooldown progress bars, session uptime, and hardware flags (`PCM: SYNTH (No UAC)`).
- **Page 1 (Audio Monitor)**: `AUDIO MONITOR [2/3 AUD]`. 304px live sparkline scope with horizontal threshold marker, RMS/Peak/Noise numerical readout, dynamic threshold calculation, and ring buffer diagnostics.
- **Page 2 (Settings)**: `FISHING SETTINGS [3/3 SET]`. Interactive menu with row selection highlight. Short Press DOWN adjusts selected parameter and saves directly to NVS; Long Press DOWN resets defaults.

---

## 9. Verification Matrix & Current Project Status

| Verification Item | Phase | Status | Evidence / Notes |
| :--- | :--- | :--- | :--- |
| **Production Firmware Build** | 1-7 | **VERIFIED** | PlatformIO `pio run` succeeded (RAM 83,348/327,680, Flash 1,121,797/6,553,600). |
| **Board Flash Upload (COM3)** | 1-7 | **VERIFIED** | `pio run -t upload --upload-port COM3` wrote app at 0x10000; ESP32-S3 rev v0.2, MAC 80:65:99:C6:3A:94; hashes verified. |
| **Board Serial Startup (COM3)**| 1-7 | **VERIFIED** | `pio device monitor` captured ROM boot & `PAXG ticker starting up`; logged `E (221) esp_core_dump_flash: Incorrect size of core dump image: 1785838705`. |
| **Screen Appearance (TFT LCD)**| 1-7 | **UNVERIFIED** | Physical display image / backlight not visually verified in this session. |
| **Wi-Fi / Data Freshness**     | 1-7 | **UNVERIFIED** | Real-time network connection, NTP sync, and live price/weather refresh unverified. |
| **Windows HID Enumeration**    | 3-7 | **VERIFIED** | Windows 11 Pro (build 26200): `Get-PnpDevice` listed `HID Keyboard Device` (`HID\VID_303A&PID_1001\...`, OK) and `USB 輸入裝置` (`USB\VID_303A&PID_1001\806599C63A94`, OK). |
| **HID Keystroke Delivery**     | 3-7 | **UNVERIFIED** | No keystroke was sent; delivery to host applications not tested. |
| **Audio Detector Ring Buffer** | 5-7 | **VERIFIED** | Bounded zero-allocation FIFO; bounds enforced; verified via tests. |
| **RMS / Peak Calculation**     | 5-7 | **VERIFIED** | Mathematical precision verified on known DC, silence, and square waves. |
| **Debounce & Cooldown**        | 5-7 | **VERIFIED** | 1-frame spikes rejected; secondary bursts in cooldown rejected. |
| **FSM State Transitions**      | 6-7 | **VERIFIED** | All 9 states tested: bite detection, timeout, pause/resume, error/disarm. |
| **NVS Settings Persistence**   | 7 | **VERIFIED** | Preferences NVS read/write; bounds clamped; defaults safe. |
| **Conflict-Free Buttons**      | 7 | **VERIFIED** | Long press at 800ms suppresses release; short press fires on release. |
| **Multi-Page UI (320x170)**    | 6-7 | **VERIFIED** | Dashboard, Audio Monitor, Settings fit 320x170 sprite; zero LCD flicker. |
| **Isolated UAC Compilation**   | 4 | **VERIFIED** | `test/usb_audio_poc/usb_audio_poc.cpp` compiles successfully with TinyUSB headers. |
| **UAC Linkage Proof**          | 4 | **VERIFIED LIMITATION** | Linking fails with `undefined reference to tud_audio_n_read` (`libarduino_tinyusb.a` lacks symbols). |
| **Isolated ESP-IDF UAC Proof** | 4.1 | **VERIFIED** | `test/usb_audio_poc_idf`: ESP-IDF 5.1.2 + `esp_device_uac` links with `tud_audio*` symbols resolved (exit code 0). |
| **Isolated UAC Hardware Flash**| 4.1 | **NOT FLASHED** | PoC binary was built to prove driver/symbol feasibility, but deliberately NOT flashed to board. |
| **Production UAC Isolation**   | 4-7 | **VERIFIED** | UAC code is restricted to `test/usb_audio_poc/` and `test/usb_audio_poc_idf/`; never enters production firmware. |
| **Isolated UAC HW / Windows Enum / Audio** | 4.1 | **UNVERIFIED** | Physical UAC USB audio capture and Windows audio device enumeration unverified. |
| **Isolated Composite UAC 2.0+HID Build** | 4.2 | **VERIFIED CODE PRESENCE** | `test/usb_composite_poc`: ESP-IDF 5.1.2 + TinyUSB source compiles and links firmware.elf & firmware.bin (exit code 0). |
| **Composite Descriptor & Symbol Linkage** | 4.2 | **VERIFIED CODE PRESENCE** | nm and objdump confirm 177B UAC 2.0 + HID descriptor bytecode (48kHz 16-bit stereo PCM, EP1 OUT 196B, EP1 IN 4B, EP2 IN 16B) and 13 symbols. |
| **Button FSM Host Behavioral Tests** | 4.2 | **HOST-VERIFIED (PYTHON)** | Python 3.14 tests pass 6 deterministic cases (busy right after press, release retry, true disconnect, held at boot/mount, bounce, single actuation). |
| **Button FSM Standalone C Tests** | 4.2 | **COMPILE-ONLY VERIFIED** | `test_button_fsm.c` compiles cleanly under `xtensa-esp32s3-elf-gcc` (-Wall -Wextra -Werror); no native host compiler exists on host PATH, so host pass is not claimed for C binary. |
| **Isolated Composite Hardware Flash**| 4.2 | **NOT FLASHED** | Composite PoC binary built for symbol/descriptor proof; deliberately NOT flashed to physical board. |
| **Composite Windows 11 Audio+HID Host Binding** | 4.2 | **UNVERIFIED** | Simultaneous enumeration of Windows Audio Speaker and HID Keyboard unverified on physical Windows host; symbol/descriptor presence does not prove runtime driver binding. |
| **In-Game Bite Recognition**   | 5-7 | **UNVERIFIED** | Placeholder threshold algorithm; no claim of real WoW splash recognition. |
