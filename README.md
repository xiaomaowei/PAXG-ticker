# PAXG-ticker

PAXG-ticker is an ESP32 firmware project for the LILYGO T-Display S3. The device presents PAX Gold (PAXG) market data and utility dashboards on the built-in display.

## Features

- Live PAXG price and 24h change display
- Animated PAXG mode with chart and session panel
- Digital clock mode
- Weather mode
- WoW Fishing mode with Phase 3 minimal standard USB HID Keyboard integration (stock Arduino `USBHIDKeyboard`, bounded nonblocking queue, thread-safe asynchronous disconnect signaling, explicit safety arming gates)
- Button-based mode switching with gesture affordances

## Market Data Source

Market data is retrieved from Binance public 24h ticker endpoint (`https://api.binance.com/api/v3/ticker/24hr?symbol=PAXGUSDT`) using symbol `PAXGUSDT`.

## Screenshots

### Quick Tour

The flow below is grouped by setup screens first, then display modes.

#### Setup Flow

| WiFi Choose | Keyboard |
| --- | --- |
| [![WiFi Choose](screenshots/wifi_choose.png)](screenshots/wifi_choose.png) | [![Keyboard](screenshots/keyboard.png)](screenshots/keyboard.png) |
| Network selection mock | Password input mock |

#### Modes

| Weather | Clock |
| --- | --- |
| [![Weather](screenshots/weather.png)](screenshots/weather.png) | [![Clock](screenshots/clock.png)](screenshots/clock.png) |
| Conditions and rain profile | Digital clock card |

#### Detail Panels

| Weather Icons | Sessions |
| --- | --- |
| [![Weather Icons](screenshots/weather_icons.png)](screenshots/weather_icons.png) | [![Sessions](screenshots/sessions.png)](screenshots/sessions.png) |
| Weather icon set | Market sessions panel |

Tip: Click any image to open the full-size version.

## Tech Stack

- PlatformIO
- Arduino framework (ESP32 2.0.14)
- TFT_eSPI
- ArduinoJson
- HTTPClient
- Stock USB / TinyUSB (`USBHIDKeyboard`)

## Repository Layout

- [src/core](src/core): app loop/controller, USB HID keyboard service, and shared runtime globals
- [src/modes](src/modes): mode implementations (paxg, clock, weather, fishing)
- [src/hal](src/hal): hardware-facing modules (display, wifi, brightness, ui utils)
- [src/audio](src/audio): modular PCM audio detector and synthetic waveform generator implementations
- [include/modules/audio](include/modules/audio): canonical audio headers (`audio_detector.h`, `audio_ring_buffer.h`, `synthetic_audio_samples.h`)
- [include/modules/modes](include/modules/modes): canonical mode headers
- [include/modules/hal](include/modules/hal): canonical HAL headers (`usb_hid_keyboard.h`, etc.)
- [include/modules/generated](include/modules/generated): auto-generated headers
- [include/modules](include/modules): shared non-domain headers
- [include/images](include/images): embedded graphics assets
- [test/usb_audio_poc](test/usb_audio_poc): Phase 4 isolated USB Audio (UAC) feasibility proof harness and test files
- [test/audio_detector](test/audio_detector): Phase 5 deterministic synthetic unit test suite
- [docs](docs): architectural and feasibility documentation ([docs/usb_feasibility.md](docs/usb_feasibility.md))
- [scripts/load_env.py](scripts/load_env.py): pre-build .env loader
- [platformio.ini](platformio.ini): PlatformIO build configuration

## WoW Fishing Mode (Phases 1–7 Software Integration)

The 4th display mode, **WoW Fishing**, provides a complete nonblocking finite state machine (FSM) with dark cyan/navy telemetry, safe standard USB HID Keyboard integration, a platform-independent modular PCM Audio Detector connected to a bounded deterministic synthetic PCM stream, three interactive 320x170 UI pages (Dashboard, Audio Monitor, Settings), and Arduino Preferences (NVS) persistence.

### Phases Overview

- **Phase 1 (Feasibility & Architecture)**:
  - Technical analysis of the ESP32-S3 USB subsystem under PlatformIO and Arduino ESP32 2.0.14.
  - Identified stock `USBHIDKeyboard` component and qualified USB Audio (UAC) status: precompiled sdkconfig omits `CONFIG_TINYUSB_AUDIO_ENABLED`, requiring a minimal TinyUSB/ESP-IDF proof of concept before concluding feasibility or considering framework migration.
  - Detailed findings documented in [docs/usb_feasibility.md](docs/usb_feasibility.md).
- **Phase 2 (Simulated Dashboard & State Machine)**:
  - Nonblocking state machine: `STOPPED` -> `CASTING` -> `WAITING BITE` -> `REELING`.
  - Synthetic audio test data simulating ambient water noise floor (~12-30%) with scheduled bite spike events (~82-97%) crossing the 75% threshold.
  - Low-flicker 320x170 rendering using `fishing_display` sprite.
  - In-mode button gestures preserving global mode navigation.
- **Phase 3 (Minimal Standard USB HID Keyboard & Safe Integration)**:
  - **Stock USBHIDKeyboard**: Uses stock Arduino `USBHIDKeyboard` standard reports; no custom VID/PID, hidden descriptors, evasion, or anticheat modifications.
  - **Bounded Nonblocking Command Queue**: Depth-bounded queue (`FISHING_KEY_QUEUE_CAPACITY = 8`) ensuring key press and release happen on separate service ticks (`FISHING_KEY_PRESS_HOLD_MS = 60ms`, `FISHING_KEY_RELEASE_GAP_MS = 60ms`).
  - **Thread-Safe Disconnect Signaling**: `usb_event_callback` does not mutate queue or call `releaseAll()` directly across FreeRTOS tasks; it sets a synchronized `std::atomic<bool> s_pendingDisconnect` signal.
  - **Loop Task Consumption & Auto-Disarm**: Pending disconnect/suspend signal is consumed in `usb_hid_keyboard_tick` on the Arduino main loop, immediately clearing the queue, calling `Keyboard.releaseAll()`, and forcing `fishingArmed = false` (auto-disarm). This prevents automated key transmission if the device is reconnected.
  - **Host-Mounted Gating**: Enqueue and sending strictly gate on actual host-mounted state (`_started && tinyusb_device_mounted` via `usb_hid_is_connected()`); USB initialization alone is not treated as a connected host. Unmounted state displays `USB: DISCONN` and `HID: UNAVAILABLE`.
  - **Re-arm Protection**: USB reconnect or resume events do not re-arm the device; explicit user dual-button action is required.
  - **Strict State Gating**: Keystrokes are gated behind `DisplayMode::Fishing` **AND** `usb_hid_is_connected()` **AND** an explicit user armed state (`fishingArmed`) **AND** an active running state (`Casting`, `Waiting`, `BiteDetected`, `Reeling`).
  - **Zero Leakage**: No keystrokes can ever be transmitted in `PAXG`, `Clock`, `Weather`, `Idle`, `Paused`, `Cooldown`, `Timeout`, or `Error` states.
  - **Configurable Safe Placeholders**: Cast key (`'1'`) and Interact key (`'2'`) configurable via Settings UI and persisted in NVS.
- **Phase 4 (Isolated USB Audio Feasibility Proof)**:
  - **Strict Isolation**: Implementation kept strictly inside `test/usb_audio_poc/**`; excluded from default firmware build (`pio run`).
  - **Validation Result**: Headers (`tusb.h`, `class/audio/audio_device.h`) compile cleanly when driver macros are configured; linking against `libarduino_tinyusb.a` fails with `undefined reference to tud_audio_n_read`.
  - **Asserted Expected Failure**: In `run_validation.py`, compilation success and specific link failure on undefined `tud_audio` symbols are asserted; all temporary files are unlinked.
  - **Missing Artifacts Evidence**: `libarduino_tinyusb.a` contains 0 `tud_audio` symbols; `audio_device.c` is not packaged in Arduino ESP32 2.0.14 framework sources; `CONFIG_TINYUSB_AUDIO_ENABLED` is omitted from precompiled sdkconfig.
  - **Status**: Framework packaging and static library configuration limitation, not platform-wide impossibility. Reported explicitly as **not implemented / unverified in production firmware**. No UAC code integrated into production app.
- **Phase 5 (Modular PCM Audio Detector & Synthetic Verification)**:
  - **Replaceable DSP Interface**: `IAudioDetector` interface with default `PcmAudioDetector` implementation, platform-independent and decoupled from transport layer.
  - **Bounded Buffer & Metrics**: Uses `PcmRingBuffer` (bounded zero-allocation FIFO), `uint32_t peak` calculation (computed via `int32_t` abs to represent `INT16_MIN` magnitude 32768 without narrowing or overflow), RMS calculation, adaptive exponential moving average (EMA) ambient noise floor tracking, and dynamic thresholding (`max(absoluteThreshold, noiseFloor * thresholdRatio)`).
  - **Debounce & Cooldown Guards**: Requires consecutive frames above threshold (`debounceFrames = 2`) before latching trigger; enforces mandatory lockout window (`cooldownFrames = 25`) preventing double-triggering.
  - **Synthetic Test Verification**: Exercised via deterministic synthetic waveforms ([test/audio_detector/test_audio_detector.cpp](test/audio_detector/test_audio_detector.cpp)) testing normal bursts, `INT16_MIN` boundary magnitude, noise spike debounce rejection, cooldown rejection, and re-arming.
- **Phase 6 (Synthetic PCM Loop, 9-State FSM & Multi-Page UI)**:
  - **Synthetic Stream Integration**: Connects `PcmAudioDetector` directly to bounded synthetic PCM16 mono frames (128 samples per frame) at a mathematically exact 8.0ms cadence (128 samples / 16000Hz = 8.0ms) matching the declared 16kHz PCM audio configuration. Clearly labeled as synthetic; makes no claim of actual audio capture or WoW bobber bite splash recognition accuracy.
  - **Deterministic Platform-Independent FSM (`FishingStateMachine`)**: Small, pure state machine (`include/modules/fishing_state_machine.h`) enforcing all state transition rules across 9 distinct named states: `IDLE`, `CASTING`, `WAITING`, `BITE_DETECTED`, `REELING`, `COOLDOWN`, `TIMEOUT`, `PAUSED`, `ERROR`. Timed purely via `millis()` with zero blocking delays.
  - **Reachable Deterministic Timeout**: Every 4th cast in simulation schedules bite burst beyond the configured `waitTimeoutMs` window, deterministically testing timeout transitions. Timeout can also be reached by lowering wait timeout in Settings.
  - **Safe Error State**: Any USB disconnection or subsystem failure transitions to `ERROR`, forces `fishingArmed = false`, clears the command queue, calls `releaseAll()`, and displays a clear error state.
  - **Three 320x170 UI Pages** (rendered via `fishing_display` sprite, zero LCD flicker):
    - **Page 0 (Dashboard)**: Simulated reel count (`SIM REEL CMDS`), timeouts, live keys sent, runtime uptime, wait/cast/cooldown progress bars, state pill, USB host mount status, HID safety indicator, PCM synthetic indicator (`Buf: SYNTH (No UAC)`).
    - **Page 1 (Audio Monitor)**: Full-width live RMS scope sparkline with horizontal threshold marker, numerical Peak/RMS/Noise readouts, dynamic threshold calculation, debounce/cooldown status, and ring buffer diagnostics.
    - **Page 2 (Settings)**: Interactive menu to select and adjust persistent parameters.
- **Phase 7 (Persistent Settings, Conflict-Free Controls & Final Local Acceptance)**:
  - **Arduino Preferences (NVS) Persistence**: Automatically stores and restores `absoluteThreshold` (500–5000, step 250), `waitTimeoutMs` (5000–30000ms, step 1000ms), `cooldownMs` (500–5000ms, step 500ms), `castKey` (whitelist: `'1'`, `'2'`, `'3'`, `'4'`, `'5'`, `'9'`, `'0'`, and space `0x20`), and `interactKey` (whitelist) in namespace `paxg_fishing`. Clamps all values on load and save. Default startup is safely disarmed (`fishingArmed = false`). No secrets stored.
    - **Key Selection & Display**: In Settings (Page 3), users can select `SPACE` independently for Cast Key (row 4) and Interact Key (row 5) by pressing short DOWN on the respective row to cycle through allowed keys. When ASCII space (`0x20`) is selected, the UI explicitly renders `SPACE (0x20)` instead of a blank character. Users utilizing one-key spacebar fishing addons can set both Cast and Interact keys to `SPACE`.
    - **Simulation and Safety Guarding**: Operation remains strictly synthetic and disarmed by default (`fishingArmed = false`). Keystroke output requires explicit user arming via dual-button hold, an active host-mounted USB connection, and an active running state. The audio detector operates against synthetic test streams and does not perform real in-game fishing recognition or bobber splash detection.
  - **Conflict-Free Button Handling**:
    - **PAXG / Clock / Weather Modes**: Button UP (GPIO 14) and Button DOWN (GPIO 0) releases strictly navigate modes forward and backward, regardless of whether pressed short or held long.
    - **Fishing Mode**:
      - **Short Press UP (< 800ms)**: Next Page (`Dashboard -> Audio -> Settings`) on Pages 0 & 1; Next Item on Page 2.
      - **Short Press DOWN (< 800ms)**: Adjusts Value on Page 2 (persisting to NVS); Reliable Exit to PAXG mode on Pages 0 & 1 (auto-disarms and releases keys).
      - **Long Press UP (Hold >= 800ms)**: Toggle Start / Pause / Resume on Pages 0 & 1; Quick Return to Dashboard on Page 2.
      - **Long Press DOWN (Hold >= 800ms)**: Stop (1st hold) / Reset counters (2nd hold) on Pages 0 & 1; Reset Settings to Defaults on Page 2.
      - **Dual-Button Long Press (Both UP + DOWN >= 800ms)**: Toggle ARM / DISARM (host-mounted gated).
      - **Mutual Exclusivity**: Long press actions execute at the 800ms threshold and set `handled = true`, preventing the short press from firing on release. Long and short events cannot both fire.
  - **Deterministic FSM Unit Tests**: Added unit tests 10 through 15 covering settings bounds clamping, normal bite flow, timeout transition, pause/resume/stop gating, error/disarm safety, and systematic invalid transition rejection in `test/audio_detector/test_audio_detector.cpp`, all calling the production `FishingStateMachine` directly.

### Button Controls & Gesture Navigation

The device provides two active-low pushbuttons (GPIO 14 UP, GPIO 0 DOWN):

| Gesture | Mode / Screen | Action |
| :--- | :--- | :--- |
| **Short Press UP (< 800ms)** | Outside Fishing (on release) | Cycle mode forward: `PAXG -> Clock -> Weather -> Fishing -> PAXG` |
| **Short Press DOWN (< 800ms)** | Outside Fishing (on release) | Cycle mode backward: `PAXG -> Fishing -> Weather -> Clock -> PAXG` |
| **Long Press UP (Hold >= 800ms)** | Outside Fishing (on release) | Preserves top-level mode navigation forward |
| **Long Press DOWN (Hold >= 800ms)** | Outside Fishing (on release) | Preserves top-level mode navigation backward |
| **Short Press UP (< 800ms)** | Fishing (Dashboard / Audio) | Cycle page forward: `Dashboard -> Audio Monitor -> Settings` |
| **Short Press UP (< 800ms)** | Fishing (Settings Page) | Navigate to next setting parameter (row 1 to 6) |
| **Short Press DOWN (< 800ms)** | Fishing (Dashboard / Audio) | **Reliable Exit** to PAXG mode (auto-disarms, flushes queue, releases keys) |
| **Short Press DOWN (< 800ms)** | Fishing (Settings Page) | **Adjust Value** of selected setting (persisted to NVS) |
| **Long Press UP (Hold >= 800ms)** | Fishing (Dashboard / Audio) | **Start / Pause / Resume** simulation |
| **Long Press UP (Hold >= 800ms)** | Fishing (Settings Page) | Quick Return to Dashboard |
| **Long Press DOWN (Hold >= 800ms)** | Fishing (Dashboard / Audio) | **Stop** simulation (1st hold) / **Reset** session telemetry (2nd hold) |
| **Long Press DOWN (Hold >= 800ms)** | Fishing (Settings Page) | **Reset Settings to Factory Defaults** |
| **Hold Both Buttons (>= 800ms)** | Fishing (Any page) | **Toggle Armed / Disarmed** (requires mounted USB host) |

#### Debounce, Safety, and Release Behavior
- **Debounce**: 50ms contact debounce filter applied to both button inputs to eliminate chatter.
- **Mutual Exclusivity**: Long-press actions execute on threshold (800ms) and set internal suppression flags. Releasing the button will not trigger a short-press action. Long and short events cannot both fire.
- **Auto-Disarm on Exit & Disconnect**: Leaving Fishing mode, exiting to PAXG, or unplugging/suspending USB immediately sets `fishingArmed = false`, flushes pending queued keys, and calls `Keyboard.releaseAll()`.
- **Reset Telemetry vs Stop**: In Fishing mode, the first long press on DOWN stops the active run (transitions to `IDLE`) and disarms. A second long press while already in `IDLE` resets session counters (`simReelCmds = 0`, `simTimeouts = 0`, `uptime = 0`, `liveKeySentCount = 0`).
- **Setup Isolation**: Wi-Fi selection and keyboard input routines in `src/hal/utils.cpp` execute inside `app_setup()` prior to `app_loop()` and do not interact with runtime mode switching.
  - **Wi-Fi Setup Controls**:
    - **Row Selection (Network / Boot Mode)**: Short press UP advances selection row; Short press DOWN confirms selection.
    - **Virtual Keyboard (Password Input)**:
      - **Short Press UP**: Advances cursor position through the 4x10 grid.
      - **Short Press DOWN (< 1000ms)**: Types selected character, toggles SHIFT, or deletes previous character on BACKSPACE.
      - **Long Press DOWN (Hold >= 1000ms)**: Submits the entered password and initiates connection (prompted on-screen by `Hold DOWN (1s) to Submit`).
      - **Timeout Safety**: If inactive for 180 seconds, input aborts without submitting partial credentials and returns to network selection without initiating a connection.

### USB Feasibility Status & Disclaimers

- **What Was Tested**:
  - Successful compilation and clean firmware linking with PlatformIO for target `lilygo-t-display-s3` (ESP32-S3), verifying stock `USBHIDKeyboard` integration, thread-safe disconnect consumption, bounded command queue state transitions, separate service tick press/release pairing, 9-state nonblocking FSM, 3 UI pages, and strict gating logic.
  - Phase 4 isolated UAC feasibility proof: asserted compilation success and asserted link failure specifically on missing `tud_audio` symbols due to absence of `audio_device.c` and symbols in `libarduino_tinyusb.a`.
  - Phases 5–7 modular PCM audio detector, synthetic stream loop, FSM transitions, and persistent settings clamping: verified via cross-compilation with strict flags (`-Wall -Wextra -Werror`).
  - Isolated Arduino + ESP-IDF 5.x PoC (`test/arduino_idf_integration_poc`): verified static build, 15/15 required symbols, 20/20 native unit tests passing, and byte-identical partition table matching production.

### Bounded Physical Hardware Acceptance Evidence (2026-11-20 Testing Session)

On 2026-11-20, bounded physical hardware acceptance testing was performed on a local ESP32-S3 (COM3) using the isolated PoC (`test/arduino_idf_integration_poc`):

- **Passive UAC Audio Ingestion (User-Observed Evidence)**:
  - ESP32-S3 on COM3 was temporarily flashed with the isolated PoC image.
  - Windows enumerated the USB composite device with VID `0x303A` PID `0x8002` and recognized the `usb uac` speaker endpoint.
  - The user routed World of Warcraft game audio directly to the `usb uac` speaker.
  - On-screen telemetry confirmed: USB connection reported mounted/active, packet counter steadily incremented, RMS audio energy fluctuated dynamically with game sound, and the Trigs counter incremented upon a single manually observed fish bite.
  - *Scope & Limitations*: This represents a single qualitative user observation during live manual play, not a statistically measured or calibrated true-positive (TP) or false-positive (FP) rate. No numeric packet logs or screenshots were captured during this session.
- **Authorized Isolated Controller Keystroke Test (User-Observed, Notepad Only)**:
  - The user explicitly authorized testing the controller HID key injection in a desktop text editor (Windows Notepad only) using separate audio input, and reported Notepad was focused during the test.
  - After arming and starting while audio was flowing, the user observed an initial space character (` ` / HID usage `0x2c`) typed into Notepad (corresponding to the start-of-run cast keypress; this does not constitute evidence of an audio-triggered reel keypress).
  - Pressing physical button DOWN immediately halted operation and returned the controller state to `DISARMED`.
  - *No In-Game Automatic Keystroke Test in That Run*: In that specific test run, keystrokes were directed to Windows Notepad only.
- **User-Supervised In-Game Observation & Later-Bite Timeout (User Report)**:
  - The user explicitly authorized a single-cycle supervised in-game test and reported observing one successful cast and reel cycle (one space cast, one space reel, followed by automatic disarm to DISARMED).
  - In subsequent tests involving later bites, the user observed the device stopping red in DISARMED ~20 seconds after casting.
  - *Timing Diagnosis & 30-Second Cast Window*: In source code, `WAIT_TIMEOUT_MS` was originally 20,000ms (20s) measured from entering `Waiting` (after 1,500ms `Casting`). The user reports a standard World of Warcraft cast lasts 30 seconds, and bites can happen at any time within it. This supports timeout as a plausible cause for the ~20s stop, though it does not eliminate the possibility of transient audio or USB interruptions.
  - *Updated Deadline & Display*: The waiting deadline was increased to 32,000ms (`WAIT_TIMEOUT_MS = 32000UL`, providing 33.5s total from cast start) to fully cover the 30-second window plus margin. The 320x170 LCD now displays an unambiguous countdown `(32s)` down to `( 1s)` during `Waiting` and explicitly renders the last stop reason (e.g. `ARM: [DISARMED - BITE_TIMEOUT]` vs `AUDIO_DROPOUT` or `USB_UNMOUNTED`) when stopped, without falsely showing positive time when stopped `( 0s)`.
  - *Limitations*: These observations are based solely on qualitative user reports without audio recordings, numeric packet dumps, or precision benchmarks. In-game automation success rates, acoustic true-positive rates, and false-positive rates under real game sounds/music remain unmeasured, and the audio threshold remains uncalibrated.
- **Production Firmware Restoration & Current Status**:
  - Following testing, the original production `app0` partition binary image was reflashed and verified by SHA-256 hash.
  - The user confirmed that the Home price screen, Wi-Fi connectivity, and standard game audio playback were completely restored.
  - *Current Production State*: The restored production firmware binary still operates with synthetic audio test loops; production firmware does NOT currently integrate UAC audio or the production detector pipeline. The source code UI label update for `SPACE` is in source code only, not in the currently running restored production binary.

- **What Remains Unverified**:
  - In-game automated keystrokes & reliability: Beyond the single supervised single-cycle observation reported by the user, continuous multi-cycle automation with both bite-triggered reel and safe 30s-cast-expiry timeout auto-recast is implemented in the isolated PoC with a 2.0s non-blocking cooldown, but remains unverified on live physical hardware.
  - Acoustic recognition calibration: The detector uses an uncalibrated energy threshold and does not claim verified discrimination between bobber splashes and background game sounds/music (false-alarm and true-positive rates remain unmeasured).
  - Production UAC integration: UAC 2.0 audio input and production detector pipeline integration remain in the isolated PoC and have not been merged into the production firmware build.
- **Simulation Notice**: In restored production firmware, reel counts remain labeled `SIM REEL CMDS` (simulated command counts, not confirmed catch telemetry). All production audio metrics are synthetic test patterns.

## Requirements

- VS Code with PlatformIO extension
- ESP32 board supported by the configured environment

## 3D Printed Cover

I designed this cover to make the project feel like a final device. Without it, this project is firmware only.

If you want to see the actual usage of this firmware, look here.

- [Cover for T-Display S3 on Printables](https://www.printables.com/model/1640957-cover-for-t-display-s3)

## Getting Started

1. Clone the repository.
2. Create a local .env file from the template.
3. Build and upload the firmware.

```powershell
copy .env.example .env
pio run
pio run -t upload
pio device monitor -b 115200
```

## Configuration via .env

Wi-Fi credentials are provided through a local .env file in the repository root.

```env
WIFI_SSID=your_wifi_name
WIFI_PASSWORD=your_wifi_password
# POSIX timezone string (e.g. CST-8 is Taiwan UTC+8 without DST)
APP_TIMEZONE=CST-8
WEATHER_CITY=Taipei
WEATHER_COUNTRY=TW
```

Build integration:

- [platformio.ini](platformio.ini) runs [scripts/load_env.py](scripts/load_env.py) as a pre-build step
- [scripts/load_env.py](scripts/load_env.py) reads .env values
- [scripts/load_env.py](scripts/load_env.py) resolves WEATHER_CITY to coordinates and generates [include/modules/generated/weather_location.h](include/modules/generated/weather_location.h)
- Runtime values are injected as compile-time defines
- [src/hal/wifi_connect.cpp](src/hal/wifi_connect.cpp) uses those defines in Home mode
- [src/modes/weather_mode.cpp](src/modes/weather_mode.cpp) uses generated coordinates for weather fetch

Canonical include policy:

- Domain headers live in subfolders only (`modules/modes`, `modules/hal`, `modules/generated`)
- Root-level duplicate wrappers should not be reintroduced

If credentials are not present, firmware falls back to manual Wi-Fi selection/input flow.

## Security

- .env is ignored by .gitignore
- .env.example is tracked for onboarding
- Credentials are not stored in source headers

## Build Environment

Primary build target is defined in [platformio.ini](platformio.ini):

- Environment: lilygo-t-display-s3
- Framework: arduino
- Dependencies: TFT_eSPI, WiFi, ArduinoJson, HTTPClient, USB

## Troubleshooting

- Build fails due to credentials:
  - Confirm .env exists in the project root
  - Confirm WIFI_SSID, WIFI_PASSWORD, APP_TIMEZONE, and WEATHER_CITY are non-empty
  - Confirm the build machine has internet access to resolve WEATHER_CITY during pre-build
- Device fails to connect in Home mode:
  - Verify SSID/password values
  - Use Standard mode to test manual connection

## Contributing

Issues and pull requests are welcome.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).

## Wi-Fi Connection Failure Diagnostics

When a Wi-Fi connection attempt fails in either **Home** or **Standard** startup mode, a dedicated diagnostic screen is presented on the 320x170 LCD for 6 seconds and logged to Serial:

- **Mode**: Indicates whether Home (fast connect) or Standard (manual entry) failed.
- **WiFi.status**: Numeric status code and enum name (e.g., `6 (WL_DISCONNECTED)`).
- **ESP reason**: Actual numeric ESP32 disconnect reason code and constant name (e.g., `202 (AUTH_FAIL)`). Missing reasons are reported as `unknown (none recorded)`.
- **Events**: Cross-task event telemetry including connection count, disconnection count, got-IP flag, and disconnection RSSI when available.
- **Stage & Diagnosis**: Distinguishes AP association, auth/handshake failure, or connected-to-AP waiting for DHCP IP assignment based on actual event evidence.
- **Action**: Indicates subsequent flow (fallback to manual setup for Home; return to network selection for Standard).
