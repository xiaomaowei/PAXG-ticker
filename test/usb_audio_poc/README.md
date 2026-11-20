# Isolated USB Audio Class (UAC) Feasibility Proof of Concept

This folder contains a minimal, isolated feasibility proof of concept for USB Audio Class (UAC) OUT reception using TinyUSB APIs under the LILYGO T-Display-S3 PlatformIO environment (`framework-arduinoespressif32 @ 3.20014.231204 / ESP-IDF v4.4.6`).

## Purpose & Scope

1. **Isolation**: Kept entirely separate from production firmware source sets (`src/core`, `src/hal`, `src/modes`). Cannot accidentally enter production builds.
2. **Coexistence Analysis**: Evaluates how UAC (Isochronous OUT endpoint) coexists alongside existing USB CDC (Virtual COM Port) and USB HID Keyboard (`USBHIDKeyboard`) interfaces.
3. **Empirical Verification**: Directly tests compilation and linking against the installed framework libraries to determine exact technical boundaries.

## Findings Summary

- **Compilation**: Header inclusion (`tusb.h`, `class/audio/audio_device.h`) compiles successfully when driver macros (`CFG_TUD_AUDIO`, `CFG_TUD_AUDIO_FUNC_1_DESC_LEN`, etc.) are supplied.
- **Link Stage**: Fails with undefined reference to `tud_audio_read` / `tud_audio_n_read`.
- **Harness Assertions**: In `run_validation.py`, the validation harness asserts that compilation succeeds AND that linking fails specifically with an undefined `tud_audio` symbol. If it links or fails for an unexpected reason, the harness fails. Generated temporary files (`compile_commands.json`, `.o`, `.elf`) are strictly cleaned up upon completion.
- **Root Cause**:
  1. The installed Arduino ESP32 framework (`framework-arduinoespressif32`) does not contain `audio_device.c` in its source tree.
  2. The precompiled static library `libarduino_tinyusb.a` contains zero `tud_audio*` symbols (verified via `xtensa-esp32s3-elf-nm`).
  3. `CONFIG_TINYUSB_AUDIO_ENABLED` is omitted from the precompiled ESP-IDF sdkconfig and `tusb_config.h`.
  4. TinyUSB core `usbd.c` inside `libarduino_tinyusb.a` was compiled with `CFG_TUD_AUDIO = 0`, meaning its internal class driver dispatch table has no audio driver entry point.
- **Feasibility Qualification**:
  This represents a link-time and packaging limitation of the precompiled Arduino ESP32 2.0.14 framework, not an architectural or hardware impossibility of the ESP32-S3. However, without framework migration or rebuilding the underlying IDF static libraries, UAC audio reception cannot link or function in this project.
  Therefore, Phase 4 is explicitly documented as **not implemented / unverified**, and UAC is NOT integrated into production firmware.
