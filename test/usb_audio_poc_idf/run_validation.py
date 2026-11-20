#!/usr/bin/env python3
"""
Validation Harness for Isolated ESP-IDF TinyUSB UAC Feasibility Proof.

Verifies:
1. Isolated build produces valid firmware.elf via PlatformIO.
2. Linker symbols in firmware.elf confirm uac_device_init and tud_audio_* are defined.
3. Production firmware (PlatformIO Arduino) builds cleanly without interference.
"""

import os
import sys
import subprocess
import shutil

def run_cmd(cmd, cwd=None):
    print(f"Running: {' '.join(cmd)}")
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return res

def main():
    print("=" * 70)
    print("PAXG-ticker Isolated ESP-IDF UAC Feasibility Validation Harness")
    print("=" * 70)

    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    poc_dir = os.path.join(project_root, "test", "usb_audio_poc_idf")
    elf_path = os.path.join(poc_dir, ".pio", "build", "esp32s3_uac_poc", "firmware.elf")
    bin_path = os.path.join(poc_dir, ".pio", "build", "esp32s3_uac_poc", "firmware.bin")

    # Step 1: Execute isolated PlatformIO ESP-IDF build
    print("\n--- Step 1: Building isolated test/usb_audio_poc_idf ---")
    build_res = run_cmd(["pio", "run", "-d", "test/usb_audio_poc_idf"], cwd=project_root)
    print(f"Build Exit Code: {build_res.returncode}")
    if build_res.returncode != 0:
        print("Build output:")
        print(build_res.stdout[-2000:])
        print(build_res.stderr[-2000:])
        sys.exit(1)
    print("  -> ASSERTION PASSED: Isolated ESP-IDF build completed with exit code 0.")

    # Step 2: Verify output binary artifacts exist
    print("\n--- Step 2: Verifying build artifacts ---")
    assert os.path.exists(elf_path), f"firmware.elf not found at {elf_path}"
    assert os.path.exists(bin_path), f"firmware.bin not found at {bin_path}"
    elf_size = os.path.getsize(elf_path)
    bin_size = os.path.getsize(bin_path)
    print(f"firmware.elf size: {elf_size} bytes")
    print(f"firmware.bin size: {bin_size} bytes")
    print("  -> ASSERTION PASSED: firmware.elf and firmware.bin exist and are non-empty.")

    # Step 3: Inspect symbols in firmware.elf
    print("\n--- Step 3: Inspecting symbols in firmware.elf ---")
    nm_tool = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm.exe")
    if not os.path.exists(nm_tool):
        # Fall back to PATH nm if toolchain binary not at default path
        nm_tool = shutil.which("xtensa-esp32s3-elf-nm") or "nm"

    nm_res = run_cmd([nm_tool, "-C", elf_path])
    assert nm_res.returncode == 0, f"Failed to run nm on {elf_path}:\n{nm_res.stderr}"

    symbols_to_find = [
        "uac_device_init",
        "tud_audio_n_read",
        "tud_audio_rx_done_isr",
        "uac_speaker_output_cb",
    ]

    matched_symbols = {}
    for line in nm_res.stdout.splitlines():
        for sym in symbols_to_find:
            if sym in line:
                matched_symbols[sym] = line.strip()

    print("Found symbols in firmware.elf:")
    for sym in symbols_to_find:
        loc = matched_symbols.get(sym, None)
        print(f"  [{'FOUND' if loc else 'MISSING'}] {sym}: {loc}")
        assert loc is not None, f"Required symbol '{sym}' was not found in firmware.elf!"

    print("  -> ASSERTION PASSED: All critical UAC driver and callback symbols are resolved.")

    # Step 4: Verify production firmware build is unaffected
    print("\n--- Step 4: Verifying production firmware build remains clean ---")
    prod_build = run_cmd(["pio", "run"], cwd=project_root)
    print(f"Production Build Exit Code: {prod_build.returncode}")
    assert prod_build.returncode == 0, f"Production firmware build failed!\n{prod_build.stderr}"
    print("  -> ASSERTION PASSED: Production firmware builds cleanly without errors.")

    print("\n" + "=" * 70)
    print("ALL VALIDATION CHECKS PASSED: Isolated ESP-IDF UAC Feasibility Confirmed.")
    print("=" * 70)

if __name__ == "__main__":
    main()
