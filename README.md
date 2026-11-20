# PAXG-ticker

PAXG-ticker is an ESP32 firmware project for the LILYGO T-Display S3. The device presents PAX Gold (PAXG) market data and utility dashboards on the built-in display.

## Features

- Live PAXG price and 24h change display
- Animated PAXG mode with chart and session panel
- Digital clock mode
- Weather mode
- Simulated WoW Fishing mode (Phase 2 dashboard & nonblocking state machine)
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
- Arduino framework (ESP32)
- TFT_eSPI
- ArduinoJson
- HTTPClient

## Repository Layout

- [src/core](src/core): app loop/controller and shared runtime globals
- [src/modes](src/modes): mode implementations (paxg, clock, weather, fishing)
- [src/hal](src/hal): hardware-facing modules (display, wifi, brightness, ui utils)
- [include/modules/modes](include/modules/modes): canonical mode headers
- [include/modules/hal](include/modules/hal): canonical HAL headers
- [include/modules/generated](include/modules/generated): auto-generated headers
- [include/modules](include/modules): shared non-domain headers
- [include/images](include/images): embedded graphics assets
- [docs](docs): architectural and feasibility documentation ([docs/usb_feasibility.md](docs/usb_feasibility.md))
- [scripts/load_env.py](scripts/load_env.py): pre-build .env loader
- [platformio.ini](platformio.ini): PlatformIO build configuration

## WoW Fishing Mode (Phase 1 & Phase 2)

A 4th display mode, **WoW Fishing (Simulation)**, provides a nonblocking simulated dashboard with dark cyan/navy telemetry, synthetic audio detector test data, and dedicated button gestures.

### Phases Overview

- **Phase 1 (Feasibility & Architecture)**:
  - Technical analysis of the ESP32-S3 USB subsystem under PlatformIO and Arduino ESP32 2.0.14.
  - Verified USB HID feasibility (via TinyUSB) while qualifying USB Audio status: current Arduino 2.0.14 setup has no demonstrated ready-to-use UAC speaker capture path or configuration in this project, and the proposed route is unverified.
  - Noted that a minimal TinyUSB/ESP-IDF compatibility proof is required before concluding feasibility or considering migration (avoiding categorical platform-wide impossibility claims solely from missing configuration macros).
  - Detailed findings documented in [docs/usb_feasibility.md](docs/usb_feasibility.md).
- **Phase 2 (Simulated Dashboard & State Machine)**:
  - Nonblocking state machine: `STOPPED` -> `CASTING` -> `WAITING BITE` -> `REELING`.
  - Synthetic audio test data simulating ambient water noise floor (~15-25%) with scheduled bite spike events (~85-95%) crossing the 75% threshold.
  - Low-flicker 320x170 rendering using `fishing_display` sprite.
  - Compatible button gesture system preserving global mode navigation.
- **Phase 4 / Future Work**:
  - Real audio capture and USB keystroke automation remain **explicitly unimplemented and unverified**.

### Button Controls & Gesture Navigation

The device provides two active-low pushbuttons (GPIO 14 UP, GPIO 0 DOWN):

| Gesture | Active Modes | Action |
| :--- | :--- | :--- |
| **Short Press UP (< 800ms)** | All modes (on release) | Cycle mode forward: `PAXG -> Clock -> Weather -> Fishing -> PAXG` |
| **Short Press DOWN (< 800ms)** | All modes (on release) | Cycle mode backward: `PAXG -> Fishing -> Weather -> Clock -> PAXG` |
| **Long Press UP (Hold >= 800ms)** | Fishing Mode only | **Toggle Start / Pause / Resume** simulation |
| **Long Press DOWN (Hold >= 800ms)** | Fishing Mode only | **Stop** simulation (1st hold) / **Reset** counters (2nd hold) |

#### Debounce and Release Behavior
- **Debounce**: 50ms contact debounce filter applied to both button inputs to eliminate chatter.
- **Release-Triggered Navigation**: Short-press mode cycling triggers strictly on button release if the press duration was under 800ms and no long-press action fired.
- **Long-Press Protection**: In Fishing mode, holding for >= 800ms fires the start/pause or stop action immediately and sets an internal flag so that releasing the button does not trigger a mode cycle.
- **Setup Isolation**: Wi-Fi selection and keyboard input routines in `src/hal/utils.cpp` execute inside `app_setup()` prior to `app_loop()` and do not interact with runtime mode switching.

### USB Feasibility Status & Disclaimers

- **USB HID**: Feasible for Phase 3 via TinyUSB, but currently **unimplemented and disabled** in Phase 2.
- **USB Audio (UAC)**: Current Arduino 2.0.14 setup has no demonstrated ready-to-use UAC speaker capture path or configuration in this project; this route is unverified, and a minimal TinyUSB/ESP-IDF compatibility proof is required before concluding feasibility or migrating.
- **Phase 4 Status**: Real audio detection and hardware USB automation are explicitly unimplemented and unverified.
- **Simulation Notice**: All reel counts are clearly labeled `Sim Reel Cmds` (simulated command counts, not confirmed catch telemetry). All audio metrics are synthetic test patterns.
- **Known Limitations**: Firmware behavior has been verified via clean PlatformIO compilation; no live physical hardware or Windows host validation is claimed.

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
- Dependencies: TFT_eSPI, WiFi, ArduinoJson, HTTPClient

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
