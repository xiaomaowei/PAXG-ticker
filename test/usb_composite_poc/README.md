# Isolated Simultaneous USB Composite (UAC 2.0 Speaker + HID Keyboard) Feasibility Proof

## 1. Overview and Objective

This isolated feasibility proof demonstrates the simultaneous operation of **USB Audio Class 2.0 (UAC 2.0 speaker playback)** and **USB HID Keyboard** within a single composite USB device on the ESP32-S3 (`lilygo-t-display-s3`).

### Motivation: Towards Real One-Button Fishing
- **Production Firmware (Current)**: Built on PlatformIO Arduino ESP32 `2.0.14` (ESP-IDF `v4.4.6`). In that specific framework distribution, the precompiled static library `libarduino_tinyusb.a` omits `audio_device.c.obj` and was compiled with `CFG_TUD_AUDIO = 0`, preventing in-tree UAC linking. Keystrokes are sent via Arduino `USBHIDKeyboard` and audio is synthetic.
- **Isolated UAC Feasibility (`test/usb_audio_poc_idf/`)**: Confirmed that ESP-IDF `v5.1.2` compiles TinyUSB with `audio_device.c` and links UAC symbols.
- **This Proof (`test/usb_composite_poc/`)**: Merges both capabilities into a **single, unified composite USB image**. The ESP32-S3 registers both a UAC 2.0 speaker (Isochronous OUT + Sync Feedback IN) and a standard USB HID Keyboard (Interrupt IN) in the exact same USB configuration descriptor, linking all audio reception and keystroke injection APIs simultaneously.

---

## 2. Directory Structure & Strict Isolation

All proof-of-concept assets and build artifacts are strictly quarantined within `test/usb_composite_poc/`:

```
test/usb_composite_poc/
├── platformio.ini              # Isolated PlatformIO ESP-IDF project configuration
├── CMakeLists.txt              # Root CMake configuration (reusing tinyusb from IDF PoC)
├── sdkconfig.defaults          # Target and UAC driver default configurations
├── .gitignore                  # Quarantines build-generated sdkconfig and lockfiles
├── main/
│   ├── CMakeLists.txt          # Component registration & dependency injection
│   └── main.c                 # Application entry point, thread-safe atomics & GPIO 0 test button
├── components/
│   └── usb_device_composite/   # Unified composite UAC 2.0 + HID driver
│       ├── CMakeLists.txt      # Registers composite driver and descriptor files
│       ├── Kconfig             # Configuration menus
│       ├── Kconfig.uac         # UAC buffer and channel configuration
│       ├── include/
│       │   ├── usb_device_composite.h # Public API for composite UAC + HID keyboard
│       │   └── usb_device_uac.h       # Compatible alias header
│       ├── tusb/
│       │   ├── tusb_config.h          # Enables CFG_TUD_AUDIO=1 and CFG_TUD_HID=1
│       │   └── usb_descriptors.c      # Composite Device, Configuration, & String descriptors
│       ├── tusb_uac/                  # UAC 2.0 descriptor templates and macros
│       └── usb_device_composite.c     # Unified driver implementation & callbacks
├── run_validation.py           # Automated build, descriptor bytecode, and symbol assertion script
└── README.md                   # This document
```

> [!IMPORTANT]
> The root `platformio.ini`, root `CMakeLists.txt`, and production application code in `src/`, `include/`, and `lib/` remain 100% untouched.

---

## 3. Composite USB Architecture & Bytecode-Verified Endpoint Allocation

The configuration descriptor (`desc_configuration`, 177 bytes total) was extracted and verified byte-by-byte from `firmware.elf`:

| Interface Index | USB Class | Subclass / Protocol | Function | Endpoints Assigned | Max Packet Size / Interval |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **0** | `TUSB_CLASS_AUDIO` (0x01) | Audio Control (0x01) | UAC 2.0 (`bcdADC = 0x0200`) Clock source, feature unit, terminal control | `EP 0` (Default Control) | 64 bytes |
| **1** | `TUSB_CLASS_AUDIO` (0x01) | Audio Streaming (0x02) | UAC 2.0 48kHz 16-bit Stereo PCM OUT | `EP 1 OUT` (0x01, Isochronous)<br>`EP 1 IN` (0x81, Sync Feedback) | 196 bytes / 1 ms<br>4 bytes / 1 ms |
| **2** | `TUSB_CLASS_HID` (0x03) | Boot Keyboard (0x01) | Boot Keyboard keystroke injection | `EP 2 IN` (0x82, Interrupt) | 16 bytes / 10 ms |

### Endpoint Budget on ESP32-S3 USB OTG Peripheral
- **Hardware Limits**: ESP32-S3 OTG provides up to 5 IN endpoints and 5 OUT endpoints with 1024-byte FIFO RAM.
- **Resource Consumption**:
  - `OUT` endpoints: `EP 0 OUT` (Control, 64B), `EP 1 OUT` (Audio Streaming, 196B) -> 2 of 5 used.
  - `IN` endpoints: `EP 0 IN` (Control, 64B), `EP 1 IN` (Audio Feedback, 4B), `EP 2 IN` (HID Keyboard, 16B) -> 3 of 5 used.
  - **Verdict**: Endpoint allocation fits comfortably within the ESP32-S3 hardware limit.

---

## 4. Safety, Task Safety & Thread-Safety Design

1. **Separation of Connection State from Transient Endpoint Busy State**:
   - `usb_composite_is_mounted()` queries `tud_mounted() && !tud_suspended()` to determine genuine USB bus enumeration and active configuration by the host.
   - `tud_hid_ready()` reflects temporary endpoint buffer readiness. Immediately after transmitting a press report, `tud_hid_ready()` returns `false` while the report is in flight.
   - The FSM strictly gates bus disconnection on `!usb_composite_is_mounted()`. Temporary endpoint busy states **do NOT trigger disconnect aborts**, guaranteeing that held keys are never lost or left permanently pressed.
2. **Observed Stable Release Required on Startup & Mount**:
   - The state machine initializes strictly in `BTN_STATE_STARTUP_WAIT_RELEASE`.
   - If `GPIO 0` is held low across MCU boot or across USB mount, **zero keystrokes are emitted**.
   - An observed continuous stable physical release (>= `BUTTON_DEBOUNCE_MS = 50ms`) is mandatory before the button FSM arms to `BTN_STATE_IDLE`.
3. **Deterministic Single-Shot Actuation (Zero Auto-Repeat)**:
   - Physical press is debounced for >= 50ms.
   - Non-blocking hold: key is held active on host for 50ms without task-blocking delays (`vTaskDelay` removed).
   - Once triggered, the state machine enters `BTN_STATE_WAIT_RELEASE`. Holding the button indefinitely emits **strictly zero repeated keystrokes**.
4. **Release Report Verification & Non-Blocking Busy Retry**:
   - Return status of `usb_composite_hid_keyboard_release()` is checked: if busy (`ESP_ERR_NOT_FINISHED` / `BTN_OP_BUSY`), the FSM transitions to `BTN_STATE_RETRY_RELEASE` and retries non-blocking on subsequent ticks.
   - A key sequence is claimed successful (`s_poc_stats.hid_keys_sent` incremented) **only after the release report is confirmed accepted** by the host.
5. **Safe Reset Exclusively on Real Unmount/Suspend**:
   - If USB genuinely disconnects or suspends while holding or retrying release, the FSM aborts to `BTN_STATE_STARTUP_WAIT_RELEASE`.
   - Upon reconnect, the FSM requires a full observed release before re-arming (no auto-send on reconnect).
6. **Task Safety & Cross-Task Concurrency**:
   - **TinyUSB Device Task (`tusb_device_task`)**: Runs `tud_task()` handling standard control requests, bus events, and USB ISR notifications. Configured with `CFG_TUSB_OS = OPT_OS_FREERTOS`.
   - **Audio Forwarding Task (`usb_spk_task`)**: Woken by task notification from `tud_audio_rx_done_isr` to deliver PCM frames to `output_cb`.
   - **Diagnostic Monitor Task (`composite_monitor_task`)**: Polls `service_physical_button_test()` at 10ms intervals.
   - **Thread Safety**: HID keystroke transmission is called exclusively from the monitor task context; metrics telemetry uses lock-free C11 atomics (`stdatomic.h` / `atomic_uint_fast32_t`), eliminating cross-task data races.
7. **USB Audio Receive-Buffer Concurrency & Bounded Lock-Free Architecture**:
   - **Original Defect**: `tud_audio_rx_done_isr` executing in `tusb_device_task` context directly overwrote shared `spk_buf` and `spk_data_size` while `usb_spk_task` concurrently invoked `output_cb` and cleared `spk_data_size`. Coalesced task notifications in FreeRTOS caused packet overwrites, lost wakeups, and corrupted audio frames before delivery to the PCM bridge.
   - **Wait-Free Lock-Free SPSC Queue (`usb_audio_queue`)**: Replaced the shared buffer and blocking mutex (`portMAX_DELAY`) with a wait-free Single-Producer Single-Consumer (SPSC) ring queue using C11 atomic primitives (`stdatomic.h`). The TinyUSB producer executes in strictly bounded $O(1)$ time, never blocks, and never waits on a mutex.
   - **Exclusively Consumer-Owned Tail & Storage Protection**:
     - In strict accordance with SPSC ring buffer ownership, `tail` is exclusively read and written by the consumer (`usb_spk_task`). The producer and lifecycle tasks **never write `tail`**.
     - Previous designs that attempted to reset `tail = head` during stream reset violated storage ownership: if the consumer was mid-dequeue copying a slot, the producer could immediately overwrite that slot, resulting in data corruption and stale tail writes.
     - With strictly consumer-owned `tail`, the producer cannot reuse any slot until the consumer has finished copying and advanced `tail`.
   - **Lifecycle Epoch Synchronization & Stale Generation Retirement**:
     - Stream stop (`tud_audio_set_itf_close_EP_cb`, unmount, suspend) and start (`tud_audio_set_itf_cb`) advance an atomic counter: `stream_generation = usb_audio_queue_advance_generation(...)`.
     - Every queued audio slot is stamped with its `stream_generation` upon enqueue.
     - When dequeuing, the consumer checks whether the slot's generation matches `active_generation`. If stale, the consumer discards the payload, increments `stale_generation_drops`, and advances `tail`. This retires stale stream data in FIFO order without breaking storage ownership or inducing races.
   - **Explicit Drop Accounting & Non-Blocking USB Task**: If the consumer falls behind and the queue reaches capacity, the producer immediately drops the incoming frame without blocking and increments observable drop telemetry (`packets_dropped`, `bytes_dropped`).
   - **No Truncation on Dequeue**: Dequeue strictly validates caller buffer capacity (`max_len`). If undersized, it rejects the request without advancing `tail` or discarding data, reporting the required length and incrementing `buffer_undersized_rejects`.
   - **Precise Lifecycle Contract & Inherent TOCTOU Boundaries**:
     - In the TinyUSB task context, calling an execution barrier (e.g., blocking until `usb_spk_task` finishes `output_cb`) would stall USB event handling and violate USB bus timing constraints.
     - Consequently, stream close updates `stream_generation` and clears `spk_active`. The consumer checks these flags before invoking `output_cb`.
     - **Honest Limitation**: If a stream stop occurs while `output_cb` is *already executing*, that callback will run to completion. The lifecycle contract cannot guarantee zero in-flight callbacks after bus stop without synchronous blocking; the PCM bridge or audio sink must handle potential stream shutdown asynchronously.
   - **Ordered Discontinuity Signaling & Strict Consumer Serialization**:
     - `usb_composite_config_t` provides `uac_discontinuity_cb_t discontinuity_cb(uint32_t reason, void *cb_ctx)` supporting `UAC_DISCONTINUITY_REASON_STREAM_CHANGE` and `UAC_DISCONTINUITY_REASON_OVERFLOW_DROP`.
     - **Serialization Invariant**: Discontinuity callbacks are invoked EXCLUSIVELY in the consumer task (`usb_spk_task`) immediately BEFORE feeding the first post-gap audio chunk to `output_cb`.
     - The TinyUSB producer task NEVER executes bridge reset callbacks directly, completely eliminating concurrent interference or races against in-flight `output_cb` calls.
     - **All Gap Categories Handled**: Covers mid-stream queue overflow drops (via `USB_AUDIO_SLOT_FLAG_DISCONTINUITY` passed by `usb_audio_queue_dequeue_ext`), stream close/reopen while audio is queued (stale generation slots discarded prior to signaling), and idle gaps where no audio was queued until the subsequent stream begins.
   - **Observable Telemetry**: Public telemetry queries via `usb_composite_get_audio_stats()` and `usb_composite_reset_audio_stats()`.
8. **Host-Runnable Deterministic Tests & Evidence Boundaries**:
   - **Host Python Tests (`test_button_fsm.py`)**: **HOST EXECUTION VERIFIED** under Python 3.14 across 6 deterministic scenarios:
     1. Endpoint busy right after successful press does not trigger false disconnect.
     2. Release report busy retries non-blocking; completed only after acceptance.
     3. Real disconnect during hold/retry aborts safely; reconnect while held emits zero keys.
     4. Holding across boot and mount emits zero keys; requires observed stable release.
     5. Transient bounce rejection on press and release.
     6. Exactly one actuation per press (zero auto-repeat on 2s hold).
   - **Native Production C Audio Queue Tests (`test/usb_composite_poc/test/test_usb_audio_queue.c`)**: **HOST EXECUTION VERIFIED** via native compiler (`uvx --from ziglang python-zig cc` with Clang on Windows):
     - Empty dequeue rejection, FIFO ordering, burst coalescing.
     - Bounded capacity overflow and drop accounting.
     - Undersized buffer rejection without data loss.
     - **Deterministic Pause Mid-Dequeue Test**: Pauses consumer mid-copy holding slot 0 without advancing `tail`, triggers stream stop and restart with generation advance, produces new packets until queue is full, and verifies slot 0 is strictly protected from overwrite.
     - **True Multithreaded Producer-Consumer Race (20,000 packets)**: Verifies strictly monotonically increasing sequence IDs, zero payload corruption, and exact drop accounting under high concurrency.
     - **True Multithreaded Lifecycle Interleaving Race (50 cycles)**: Exercises concurrent producer, consumer, and rapid stream stop/start, verifying that stale generation frames are safely retired and state transitions never deadlock or corrupt memory.
   - **Native Test Runner (`test/usb_composite_poc/test/test_usb_audio_queue.py`)**: Executes native C build and test binary under `uvx --from ziglang python-zig cc`, asserting return code 0 (no duplicate Python model).
   - **Target Cross-Compilation**: `usb_audio_queue.c` verified **COMPILE-ONLY CLEAN** under `xtensa-esp32s3-elf-gcc` (`-Wall -Wextra -Werror`).
---

## 5. Build & Validation Commands

### Run Automated Build, Unit Tests, Bytecode, & Symbol Verification
```powershell
python test/usb_composite_poc/run_validation.py
```

### Verified Linker Symbols in `firmware.elf` (`xtensa-esp32s3-elf-nm`)
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

---

## 6. Scope Boundaries & Clear Disclaimers

1. **Compilation, Linking & Descriptor Structure**: **VERIFIED**. The ESP-IDF toolchain compiles TinyUSB from source with both `CFG_TUD_AUDIO=1` and `CFG_TUD_HID=1`. Generated ELF contains all required symbols and verified UAC 2.0 + HID descriptor bytecode.
2. **Code Presence vs. Runtime Reality**: Successful compilation and resolved symbols demonstrate **code presence only**. They do NOT prove Windows enumeration, simultaneous audio streaming and keystroke delivery, isochronous scheduling under Full-Speed (12Mbps) OTG, real audio packet arrival, or hardware timing validity.
3. **Physical Hardware Execution**: **UNVERIFIED**. The composite binary was compiled for symbol and descriptor verification and was **NOT flashed** to the physical board.
4. **Windows 11 Driver Binding**: **UNVERIFIED**. Simultaneous binding of Windows Audio Device (`usbaudio2.sys`) and HID Keyboard from this specific descriptor has not been executed on a real Windows host.
5. **Production Isolation**: Production firmware remains strictly on PlatformIO Arduino ESP32 2.0.14 without UAC code.
