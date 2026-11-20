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
- [include/modules/modes](include/modules/modes): canonical mode headers
- [include/modules/hal](include/modules/hal): canonical HAL headers (`usb_hid_keyboard.h`, etc.)
- [include/modules/generated](include/modules/generated): auto-generated headers
- [include/modules](include/modules): shared non-domain headers
- [include/images](include/images): embedded graphics assets
- [docs](docs): architectural and feasibility documentation ([docs/usb_feasibility.md](docs/usb_feasibility.md))
- [scripts/load_env.py](scripts/load_env.py): pre-build .env loader
- [platformio.ini](platformio.ini): PlatformIO build configuration

## WoW Fishing Mode (Phases 1, 2 & 3)

The 4th display mode, **WoW Fishing**, provides a nonblocking state machine dashboard with dark cyan/navy telemetry, synthetic audio detector test patterns, and safe standard USB HID Keyboard integration.

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
  - **Strict State Gating**: Keystrokes are gated behind `DisplayMode::Fishing` **AND** `usb_hid_is_connected()` **AND** an explicit user armed state (`fishingArmed`) **AND** an active running state (`Casting`, `WaitingBite`, `Reeling`).
  - **Zero Leakage**: No keystrokes can ever be transmitted in `PAXG`, `Clock`, `Weather`, `Stopped`, or `Paused` modes.
  - **Configurable Safe Placeholders**: Cast key (`'1'`) and Interact key (`'2'`) constants configured in `constants.h`.
- **Phase 4 / Future Work**:
  - Real game automation, physical USB Audio (UAC) capture, and Windows composite host verification remain **explicitly unimplemented and unverified**.

### Button Controls & Gesture Navigation

The device provides two active-low pushbuttons (GPIO 14 UP, GPIO 0 DOWN):

| Gesture | Active Modes | Action |
| :--- | :--- | :--- |
| **Short Press UP (< 800ms)** | All modes (on release) | Cycle mode forward: `PAXG -> Clock -> Weather -> Fishing -> PAXG` |
| **Short Press DOWN (< 800ms)** | All modes (on release) | Cycle mode backward: `PAXG -> Fishing -> Weather -> Clock -> PAXG` |
| **Long Press UP (Hold >= 800ms)** | Fishing Mode only | **Toggle Start / Pause / Resume** simulation |
| **Long Press DOWN (Hold >= 800ms)** | Fishing Mode only | **Stop** simulation (1st hold) / **Reset** counters (2nd hold) |
| **Hold Both Buttons (>= 800ms)** | Fishing Mode only | **Toggle Armed / Disarmed** (Enables/disables live HID keystrokes, requires mounted host) |

#### Debounce, Safety, and Release Behavior
- **Debounce**: 50ms contact debounce filter applied to both button inputs to eliminate chatter.
- **Release-Triggered Navigation**: Short-press mode cycling triggers strictly on button release if the press duration was under 800ms and no long-press action fired.
- **Long-Press Protection**: In Fishing mode, holding for >= 800ms fires the start/pause, stop, or arm/disarm action immediately and sets an internal flag so that releasing the button does not trigger a mode cycle.
- **Auto-Disarm on Exit & Disconnect**: Leaving Fishing mode or unplugging/suspending USB immediately sets `fishingArmed = false`, flushes any pending queued keys, and calls `Keyboard.releaseAll()`.
- **Setup Isolation**: Wi-Fi selection and keyboard input routines in `src/hal/utils.cpp` execute inside `app_setup()` prior to `app_loop()` and do not interact with runtime mode switching.

### USB Feasibility Status & Disclaimers

- **What Was Tested**: Successful compilation and clean firmware linking with PlatformIO for target `lilygo-t-display-s3` (ESP32-S3), verifying stock `USBHIDKeyboard` integration, thread-safe disconnect consumption, bounded command queue state transitions, separate service tick press/release pairing, and strict gating logic.
- **What Remains Unverified**:
  - Physical execution on hardware: Local ESP32-S3 physical device validation was unavailable.
  - Windows 11 host USB composite driver enumeration and keystroke delivery across Windows USB hubs.
  - Game interaction: Keys `'1'` and `'2'` are safe placeholder constants and do not claim verified gameplay automation.
  - USB Audio Class (UAC): Remains completely unverified and unimplemented.
- **Simulation Notice**: Reel counts are labeled `SIM REEL CMDS` (simulated command counts, not confirmed catch telemetry). All audio metrics are synthetic test patterns.

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
