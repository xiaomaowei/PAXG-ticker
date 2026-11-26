# PAXG-ticker

**Documentation:** [English](README.md) · [繁體中文](README.zh-TW.md) · [OTA 更新維護手冊 (docs/OTA.md)](docs/OTA.md)

PAXG-ticker is an ESP32-S3 firmware project for the LILYGO T-Display S3. The device delivers a dedicated firmware integrating the real **WoW Fishing Screen** powered by real USB UAC 2.0 audio capture, USB HID composite keyboard control, an evidence-based **Bag-Full Audio Stop** detector, and a local **LAN Fishing Status & Control Web UI**.

> [!TIP]
> **OTA 無線更新快速導覽 (OTA Quick Overview)**
>
> 整合目標韌體（`esp32s3_arduino_idf_poc`）現已支援局域網 HTTP OTA 無線部署與雙槽回退機制。詳細架構、初次 USB 完整重刷步驟與 Cloudflare WARP /32 遠端通道配置請參閱完整手冊：
> 📖 **[docs/OTA.md (OTA 建置與維護手冊)](docs/OTA.md)**
>
> 常用部署指令（Windows PowerShell 7，上傳必須指定 `--device-id` 鎖定硬體身分）：
> ```powershell
> # 1. 離線本地二進位檔檢查
> python scripts/deploy_ota.py --dry-run
>
> # 2. 設定目標裝置連線變數（以下範例值請替換為實際環境數值）
> $otaHost = '192.168.1.50'
> $otaDeviceId = '0123456789ab'
>
> # 3. 安全遮蔽輸入 64 字元十六進位 Token，傳遞後立即自記憶體清除環境變數
> $env:PAXG_OTA_TOKEN = Read-Host "請輸入裝置 LCD 顯示之 64 字元 OTA Token" -MaskInput
> python scripts/deploy_ota.py --host $otaHost --device-id $otaDeviceId --build
> Remove-Item Env:PAXG_OTA_TOKEN
> ```

## Features

- **Dedicated Real Fishing Display with Local IP**:
  - Direct 48 kHz stereo USB UAC 2.0 audio ingestion into the production `PcmAudioDetector` pipeline.
  - Clean 320x170 high-contrast telemetry layout with flicker-free double-buffered sprite rendering.
  - Local device IP address and port prominently displayed on the TFT screen (`IP: 192.168.x.x | WEB: PORT 80`).
  - Single-key (Spacebar = HID 0x2c) cast/reel coordination with 30s cast duration coverage (32s waiting window) and continuous auto-recast on bite or timeout.
  - Strict fail-closed safety guards: positive audio packet progress gating, idle disconnect auto-disarm, non-blocking release retry, and deliberate arming.
- **Evidence-Based Bag-Full Audio Stop**:
  - **Context & Empirical Evidence**: User currently has full bags and an in-game addon bell repeating every 3 seconds. Passive WASAPI loopback audio sampling from the live `usb uac` device empirically measured the periodic bell: onset intervals at `2.952s` and `3.040s` (~3.00s nominal interval), peak RMS > 17,000, and dominant spectral component at 4.1 kHz.
  - **Periodic Pulse Identification**: Bounded time-domain interval detector (`BagFullDetector`) tracks high-energy pulses (RMS >= 3000) within a calibrated acceptance window (`3000ms +/- 350ms`, `[2650ms, 3350ms]`) with 500ms intra-chime debounce. Default requires 2 consecutive matching intervals (3 pulses across ~6s) to latch confirmed full-bag stop, eliminating false stops from isolated transient spikes or dual-pulse coincidences.
  - **Strict Precedence & Fail-Closed Disarm**: When a full-bag condition is identified, the controller halts immediately, sets `StopReason::BagFull`, disarms (`armed_ = false`), clears bite triggers, safely releases any pressed keys, and prevents automatic recast or restart.
  - **Calibration Mechanism & Disclaimer**: Uncalibrated detector reliability is explicitly disclaimed. The firmware provides a safe calibration mechanism via `/api/calibrate` and `BagFullCalibration` to adjust interval and threshold parameters.
- **LAN Web UI & Local Control API**:
  - Built on native ESP-IDF `esp_http_server` running on Core 0 at priority 1 (below TinyUSB audio task priority 5) to minimize thread contention with isochronous audio streaming.
  - Local responsive web dashboard at `http://<device-ip>:80` displaying live armed state, FSM state, stop reason, wait countdown, cast/bite/reel counters, USB audio RMS, and bag-full metrics.
  - Deliberate control buttons: **[ DELIBERATE ARM ]**, **[ START FISHING ]**, **[ EMERGENCY STOP ]**, and **[ RESET BAG-FULL ]**.
  - **Editable Wi-Fi Settings & Setup AP Recovery**:
    - Accessible via `/wifi` and linked directly from the home web dashboard.
    - Displays current SSID and status (never exposes passwords).
    - Requires device-local short PIN displayed on the S3 TFT screen to authorize changes; validates POST with custom header (`X-Requested-With` / `X-Setup-Auth`) and bounded input parsing.
    - **Time-Bounded WPA2 Setup AP Recovery**: If STA is unavailable or unconfigured, starts a time-bounded (5 min) WPA2 setup AP (`PAXG-Setup-XXXX`) with device-generated password visible on TFT (never open AP).
    - **Setup AP Security Lockout**: Setup AP serves settings at `http://192.168.4.1/wifi` but cannot issue HID commands (`/api/start` and `/api/arm` return 403 Forbidden).
    - **Dedicated NVS Storage**: Successful new STA connection with DHCP is saved to dedicated NVS namespace `paxg_wifi`; last working config is preserved on failure/reboot and restored on timeout; Setup AP remains reachable during connection attempts; `.env` is never mutated and passwords are never logged.
  - **Network Safety Invariant**: Web callbacks cannot bypass USB or audio safety guards. `/api/start` delegates strictly to `LiveFishingController::requestStart()`, rejecting start if disarmed, unmounted, audio stale, or bag-full latched.
- **Dedicated Port 8080 OTA Management**:
  - Built on native ESP-IDF `esp_http_server` listening on wildcard socket, with request handlers strictly restricted to requests on private STA interface / local IP.
  - Root `/` serves an embedded public static dashboard shell without exposing live telemetry unauthenticated.
  - REST API endpoints (`/api/version`, `/api/health`, `/api/logs`, `/api/update`, `/api/reboot`, `/api/rollback`) strictly protected by independent 256-bit Bearer Token.
  - Dual-slot partition layout (`app0` / `app1`) with ESP-IDF Bootloader automatic rollback protection.
- **釣魚音訊判定**：從拋竿按鍵送出起忽略 2.2 秒的咬鉤訊號，之後音量須連續 32 ms 超過門檻才會收竿。USB 音訊串流切換或佇列丟包時會暫停送出新按鍵，丟棄尚未送出的收竿指令；收到新封包且至少 250 ms 未再發現中斷或丟包後才繼續。音訊恢復逾時或 USB 拔除仍會停止。
- **MacroDroid Webhook 停止通知**: 於根目錄 `.env` 設定 `MACRODROID_WEBHOOK_URL`（未設定則休眠）。僅在實際活躍釣魚會話因背包滿（`BAG_FULL`）停止並回到 Idle 時，透過 FreeRTOS 計數任務通知喚醒低優先級背景 Worker 執行單次 HTTPS GET（5 秒逾時、不重試、停用重定向、附加 CRT bundle、不記錄 URL，僅 2xx 視為成功）。手動停止、故障、閒置時偵測或正常收拋竿均不觸發。

- **Safe Boot Default**:
  - Boots strictly into `DISARMED` and `IDLE`.
  - Operates with `WiFi.persistent(false)`, preserving user Wi-Fi credentials in Flash NVS without automatic overwrites.

## Firmware Targets

1. **Original Multi-Mode Firmware** (`src/`, build target `lilygo-t-display-s3` in [platformio.ini](platformio.ini)):
   - Framework: Arduino-ESP32 2.0.14
   - Features: Interactive Wi-Fi AP selector, 4x10 on-screen virtual keyboard password input, clock, weather, and synthetic fishing simulation.
   - Workflow: Retains standard USB flash workflow unchanged.
2. **Integrated Target Firmware** ([`test/arduino_idf_integration_poc`](test/arduino_idf_integration_poc), build target `esp32s3_arduino_idf_poc`):
   - Framework: ESP-IDF 5.1.2 with Arduino-ESP32 3.0.7 as component (16MB Flash, 8MB OPI PSRAM)
   - Features: Dedicated Real Fishing screen with device IP display, LAN status & control Web UI (Port 80), evidence-based bag-full stop, real USB UAC 2.0 / HID keyboard composite, and dedicated Port 8080 OTA management with dual-slot rollback.
   - Startup: Uses `.env` compile-time definitions (`WIFI_SSID`, `WIFI_PASSWORD`) with `WiFi.persistent(false)`. Market screen accessible via button navigation.
   - OTA Reference: See [docs/OTA.md](docs/OTA.md) for complete initial USB flashing and wireless deployment instructions.

## Market Data Source

Market data is retrieved from Binance public 24h ticker endpoint (`https://api.binance.com/api/v3/ticker/24hr?symbol=PAXGUSDT`) using symbol `PAXGUSDT` over authenticated TLS.

## Screenshots

### Setup Flow & Screens (Original Multi-Mode Firmware Only)

> [!NOTE]
> The setup flow below is present in the **Original Multi-Mode Firmware** (`src/`). The **Unified Target Firmware** boots directly to the PAXG Market screen using preconfigured `.env` credentials.

| WiFi Choose | Keyboard |
| --- | --- |
| [![WiFi Choose](screenshots/wifi_choose.png)](screenshots/wifi_choose.png) | [![Keyboard](screenshots/keyboard.png)](screenshots/keyboard.png) |
| Network selection | Password input |

### Display Modes (Unified Target Firmware)

| PAXG Market Screen | Real Fishing 320x170 Screen |
| --- | --- |
| [![PAXG Market](screenshots/sessions.png)](screenshots/sessions.png) | `FISHING` \| `USB OK` \| `AUDIO OK`<br>`WAITING 18s`<br>`[ARMED]`<br>`CAST 128 \| BITE 124 \| REEL 124`<br>`UP: START  DOWN: STOP` |
| Live price, 24h change, sessions & chart | High-contrast FSM countdown & real metrics |

Tip: Click any image to open the full-size version.

## Tech Stack

- **Target Build Framework**: ESP-IDF 5.1.2 with Arduino-ESP32 3.0.7 as component
- **Display Driver**: TFT_eSPI (Parallel 8-bit ST7789, 320x170 landscape)
- **USB Composite Stack**: TinyUSB UAC 2.0 Speaker (Isochronous OUT 48 kHz) + HID Keyboard
- **Networking**: Arduino WiFi + NetworkClientSecure + HTTPClient (bounded async FreeRTOS worker)
- **DSP Audio Pipeline**: Platform-independent `PcmAudioDetector` with EMA noise floor tracking

## Repository Layout

- [test/arduino_idf_integration_poc](test/arduino_idf_integration_poc): Target firmware build and validation harness (ESP-IDF 5.x + Arduino + UAC 2.0 + HID + PAXG Market View)
- [src/core](src/core): shared core runtime globals and controllers
- [src/modes](src/modes): reference mode implementations
- [src/hal](src/hal): hardware-facing modules (display, wifi, brightness, ui utils)
- [src/audio](src/audio): production PCM audio detector implementation (`pcm_audio_detector.cpp`)
- [include/modules/audio](include/modules/audio): audio headers (`audio_detector.h`, `audio_ring_buffer.h`)
- [include/modules/modes](include/modules/modes): canonical mode headers
- [include/images](include/images): embedded graphics assets (badges, flags, arrows)
- [docs](docs): architectural and feasibility documentation ([docs/usb_feasibility.md](docs/usb_feasibility.md), [docs/OTA.md](docs/OTA.md))
- [platformio.ini](platformio.ini): root PlatformIO configuration

## Button Controls & Safe Navigation

The device provides two active-low pushbuttons (GPIO 14 UP, GPIO 0 DOWN):

| Gesture | Mode / Screen | Action |
| :--- | :--- | :--- |
| **Short Press DOWN (< 800ms)** | PAXG Market Screen (on release) | **Switch to Real Fishing Screen** (guaranteed DISARMED and IDLE) |
| **Short Press UP (< 800ms)** | Real Fishing Screen (when ARMED & IDLE) | **START Fishing** (initiates casting sequence with Spacebar) |
| **Any Press DOWN** | Real Fishing Screen (when ARMED or RUNNING) | **STOP Fishing** (highest priority: stops FSM, releases keys, DISARMS) |
| **Short Press DOWN (< 800ms)** | Real Fishing Screen (when DISARMED & IDLE) | **Switch to PAXG Market Screen** (auto-disarms and releases keys) |
| **Hold Both Buttons (>= 1000ms)** | Real Fishing Screen (any state) | **Toggle ARMED / DISARMED** (requires mounted USB host) |
| **Hold UP 3 Seconds (>= 3000ms)** | PAXG Market Screen | **Display OTA Token & Device ID** (intended operation: 64-character hex token in 4 rows, 12-hex STA MAC Device ID, time-limited overlay; physical verification pending) |

### Debounce, Safety, and Release Behavior
- **Debounce**: 50ms contact debounce filter applied to both button inputs to eliminate contact chatter. Pulses < 50ms are discarded.
- **Stop-Then-Exit Semantics**: In Real Fishing mode, if the controller is running or armed, pressing DOWN immediately triggers an emergency stop and disarms. Releasing that DOWN press does **not** exit the mode. Only a subsequent, separate DOWN press while already idle and disarmed exits back to the PAXG Market screen.
- **Mutual Exclusivity**: Long-press arming (1000ms) executes on threshold and suppresses short-press firing upon release.
- **Zero Keystroke Leakage on Mode Switch**:
  - Switching from Real Fishing to PAXG Market calls `requestStop(StopReason::UserStopped)` and forces `armed = false`.
  - While in PAXG Market screen, the fishing controller runs with zero simulated buttons (`update(false, false)`). It continues servicing background USB and audio telemetry, but can never send a keystroke.
  - Switching from PAXG Market to Real Fishing initializes the screen strictly in `DISARMED` and `IDLE`. Keystrokes cannot be emitted until the user deliberately holds both buttons for 1.0s to arm, releases both buttons, and presses UP.
- **Fail-Closed Safety**: Any USB host disconnect, audio stream stall (> 1.0s), or driver queue overflow immediately disarms the controller and transitions to Idle. Rearming upon reconnect is strictly prohibited.
- **Setup Isolation**: Wi-Fi configuration via `.env` is loaded safely using `WiFi.persistent(false)` to prevent writing temporary credentials to Flash NVS.

### Target Firmware Build & Symbol Verification

The unified firmware binary targeting the LilyGo T-Display S3 is built from `test/arduino_idf_integration_poc`:

```powershell
# Run validation (55 native unit tests, cross-compilation, and symbol checks)
python test/arduino_idf_integration_poc/run_validation.py

# Build target firmware binary
pio run -d test/arduino_idf_integration_poc -e esp32s3_arduino_idf_poc
```

- **Framework**: ESP-IDF 5.1.2 with Arduino-ESP32 3.0.7 as component
- **Verification Status**:
  - **85/85 Native Unit Tests Passing**: 10 UAC audio bridge tests, 28 live fishing controller tests, 17 PAXG market tests, 19 Bag-Full audio stop & LAN web server tests, and 11 Wi-Fi settings & recovery manager tests.
  - **Full-Bag Chime Detection & State Gating**: User-confirmed designated bell signal (3.0s interval $\pm 350\text{ms}$, RMS $\ge 3000.0f$, 4.1 kHz chime harmonic). Verified periodic pulses latch confirmed `StopReason::BagFull`, halting the controller, disarming, and locking state until explicitly reset via `/api/bagfull/reset`. Isolated acoustic pulses (such as bite splashes) do not falsely equate to confirmed full and allow normal fishing and reel progression.
  - **Bounded Thread-Safe Command Routing**: HTTP server task never accesses `LiveFishingController` directly. Web requests enter a bounded command queue; emergency Stop has priority over Start and purges queued casts; controller owner loop executes commands and synchronizes actual outcomes via condition variables.
  - **Synchronized Status Snapshot**: Owner loop periodically publishes complete telemetry snapshots under mutex protection, allowing lock-free reads by `/api/status` without thread contention or audio starvation.
  - **LAN Web Dashboard & Wi-Fi Settings**: Clean responsive web dashboard served on Core 0 priority 1, rendering real-time FSM state, stop reasons, countdown, audio RMS, deliberate start/stop controls, and dedicated `/wifi` settings page.
  - **25/25 ELF Symbols Verified**: Includes `paxg::wifi::getWifiSettingsManager`, `paxg::wifi::WifiSettingsManager::update`, `BagFullDetector::processRms`, `startFishingWebServer`, `processPendingCommands`, `parseBinanceTicker`, `getMarketQuoteSnapshot`, `usb_composite_init`, and all driver symbols.
  - **Partition Table Parity**: Byte-identical to production 16MB layout (SHA-256 `bd0f7954aca2ef7d925ee21aaa1f3dc8822d1d6ce5cbbd26a135e5886bfff6ce`).

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
  - In-game automated keystrokes & reliability: Beyond the single supervised single-cycle observation reported by the user, continuous multi-cycle automation with both bite-triggered reel and safe 30s-cast-expiry timeout auto-recast is implemented in the unified target firmware with a 2.0s non-blocking cooldown, but has not undergone live in-game automated validation.
  - Acoustic recognition calibration: The detector uses an uncalibrated energy threshold and does not claim verified discrimination between bobber splashes and background game sounds/music (false-alarm and true-positive rates remain unmeasured).
  - Physical device flashing: In accordance with project safety constraints, physical hardware flashing was not executed during this task. Binary symbols, static linking, and unit test suites have been fully verified.

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
3. Build the unified target firmware (PAXG Market + Real Fishing):

```powershell
copy .env.example .env

# Validate native test suites & cross-compilation
python test/arduino_idf_integration_poc/run_validation.py

# Build target firmware (ESP-IDF 5.x + Arduino + UAC 2.0 + HID + PAXG Market)
pio run -d test/arduino_idf_integration_poc -e esp32s3_arduino_idf_poc
```

### Initial USB Upload & Subsequent OTA

- **Initial USB Flash**: The first upgrade to the integrated target firmware MUST be a full USB upload (bootloader, partition table, `app0`, and reinitializing `otadata` to prevent old `app1` states from persisting). The PlatformIO 6.5.0 ESP-IDF builder automatically generates `ota_data_initial.bin` and writes it to `0xe000`. Do **not** use `erase_flash` in order to preserve existing NVS credentials.
  ```powershell
  # Full USB upload command (after user permission)
  pio run -d test/arduino_idf_integration_poc -e esp32s3_arduino_idf_poc -t upload --upload-port COMx
  ```
- **Subsequent OTA Updates**: Future updates can be deployed wirelessly over Port 8080 using `scripts/deploy_ota.py`. See [docs/OTA.md](docs/OTA.md) for full details.

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
