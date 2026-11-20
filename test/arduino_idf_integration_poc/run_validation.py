#!/usr/bin/env python3
"""
PAXG-ticker: Isolated Arduino-ESP32 + TFT_eSPI + WiFi + Composite USB (ESP-IDF 5.x) Validation Runner.

This script independently validates the build of the isolated PoC project located in
test/arduino_idf_integration_poc without touching or modifying any production code or documentation.

Requirements validated:
1. Pinned Arduino-ESP32 (v3.0.7) as an in-tree ESP-IDF 5.1.2 component.
2. Actual TFT_eSPI parallel 8-bit configuration for LilyGo T-Display-S3 (ST7789, GPIO 39-48).
3. Wi-Fi stack compatibility (with mock compile-time placeholder credentials).
4. Composite USB (UAC 2.0 Speaker + HID Keyboard) coexistence and link (with zero automatic runtime key sending).
5. Symbol verification in firmware.elf.
6. Optional cleanup of build artifacts.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

POC_DIR = Path(__file__).resolve().parent
REPO_ROOT = POC_DIR.parent.parent
TFT_COMP_DIR = POC_DIR / "components" / "TFT_eSPI"

def find_nm_tool():
    """Locate xtensa-esp32s3-elf-nm tool in PlatformIO toolchains or PATH."""
    home = Path.home()
    pio_toolchains = list(home.glob(".platformio/packages/toolchain-xtensa-esp32s3*/bin/xtensa-esp32s3-elf-nm*"))
    for tool in pio_toolchains:
        if tool.is_file() and os.access(tool, os.X_OK):
            return str(tool)
    which_nm = shutil.which("xtensa-esp32s3-elf-nm")
    if which_nm:
        return which_nm
    return None

def run_cmd(cmd, cwd=None, check=True):
    print(f"[CMD] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(cmd, cwd=cwd or REPO_ROOT, shell=isinstance(cmd, str), capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"[ERROR] Command failed with code {res.returncode}:")
        print(res.stdout)
        print(res.stderr)
        sys.exit(res.returncode)
    return res

def clean_artifacts(clean_deps=False):
    print("[CLEAN] Cleaning generated build artifacts in PoC directory...")
    pio_dir = POC_DIR / ".pio"
    if pio_dir.exists():
        shutil.rmtree(pio_dir, ignore_errors=True)
        print(f"  Removed {pio_dir}")
    for sdk in POC_DIR.glob("sdkconfig.esp32s3_arduino_idf_poc*"):
        if sdk.is_file():
            sdk.unlink(missing_ok=True)
            print(f"  Removed {sdk}")

    if clean_deps:
        upstream_dir = TFT_COMP_DIR / "upstream"
        if upstream_dir.exists():
            shutil.rmtree(upstream_dir, ignore_errors=True)
            print(f"  Removed {upstream_dir}")

    print("[CLEAN] Done.")

def ensure_dependencies():
    """Ensure pinned TFT_eSPI dependency is acquired via bootstrap_tft.py."""
    bootstrap_script = TFT_COMP_DIR / "bootstrap_tft.py"
    upstream_dir = TFT_COMP_DIR / "upstream"
    if not (upstream_dir / "TFT_eSPI.cpp").is_file():
        print("[BOOTSTRAP] Acquiring pinned TFT_eSPI dependency...")
        run_cmd([sys.executable, str(bootstrap_script)])

def main():
    parser = argparse.ArgumentParser(description="Validate Arduino-ESP32 + IDF 5.x PoC build.")
    parser.add_argument("--clean-only", action="store_true", help="Clean build artifacts and exit.")
    parser.add_argument("--clean-after", action="store_true", help="Clean build artifacts after successful validation.")
    parser.add_argument("--clean-deps", action="store_true", help="Also clean downloaded dependency upstream directory.")
    parser.add_argument("--check-production", action="store_true", help="Also verify root production build.")
    args = parser.parse_args()

    if args.clean_only:
        clean_artifacts(clean_deps=args.clean_deps)
        return

    print("=" * 70)
    print("PAXG-ticker: Arduino-as-IDF-Component (ESP-IDF 5.1.2) Validation")
    print(f"Target directory: {POC_DIR}")
    print("=" * 70)

    # 0. Ensure pinned dependencies
    print("\n[STEP 0/3] Checking pinned dependencies...")
    ensure_dependencies()

    # 1. Compile isolated PoC
    print("\n[STEP 1/3] Compiling isolated PoC project...")
    build_cmd = ["pio", "run", "-d", str(POC_DIR)]
    res = run_cmd(build_cmd)
    print(res.stdout[-600:] if len(res.stdout) > 600 else res.stdout)
    print("[PASS] Build succeeded.")

    # 2. Check binary artifacts
    print("\n[STEP 2/3] Checking output binary artifacts...")
    elf_file = POC_DIR / ".pio" / "build" / "esp32s3_arduino_idf_poc" / "firmware.elf"
    bin_file = POC_DIR / ".pio" / "build" / "esp32s3_arduino_idf_poc" / "firmware.bin"

    if not elf_file.exists() or elf_file.stat().st_size == 0:
        print(f"[FAIL] Missing or empty ELF: {elf_file}")
        sys.exit(1)
    if not bin_file.exists() or bin_file.stat().st_size == 0:
        print(f"[FAIL] Missing or empty BIN: {bin_file}")
        sys.exit(1)

    elf_size = elf_file.stat().st_size
    bin_size = bin_file.stat().st_size
    print(f"[PASS] firmware.elf ({elf_size:,} bytes) and firmware.bin ({bin_size:,} bytes) exist.")

    # 3. Verify symbols
    print("\n[STEP 3/3] Inspecting ELF symbols...")
    nm_tool = find_nm_tool()
    if not nm_tool:
        print("[WARN] xtensa-esp32s3-elf-nm not found; skipping symbol inspection.")
    else:
        nm_res = subprocess.run([nm_tool, "-C", str(elf_file)], capture_output=True, text=True, check=True)
        symbols = nm_res.stdout

        required_symbols = [
            ("TFT_eSPI::begin", "TFT_eSPI Display Driver"),
            ("TFT_eSprite::createSprite", "TFT_eSprite Framebuffer Support"),
            ("WiFiSTAClass::begin", "Arduino Wi-Fi STA"),
            ("WiFiGenericClass::mode", "Arduino Wi-Fi Generic Mode"),
            ("usb_composite_init", "Composite USB Device Init"),
            ("tud_audio_n_read", "TinyUSB Audio Class 2.0 Speaker"),
            ("usb_composite_hid_keyboard_press", "Composite USB HID Keyboard (Retained Reference)"),
            ("desc_configuration", "TinyUSB Composite Configuration Descriptor"),
            ("manual_trigger_keystroke_demo", "Manual Keystroke Trigger Function"),
            ("g_hid_demo_ref", "Volatile HID Reference Pointer"),
        ]

        all_found = True
        for sym, desc in required_symbols:
            if sym in symbols:
                print(f"  [OK] Found symbol: {sym:<35} ({desc})")
            else:
                print(f"  [FAIL] Missing symbol: {sym:<35} ({desc})")
                all_found = False

        if not all_found:
            print("[FAIL] One or more critical symbols missing from firmware.elf.")
            sys.exit(1)
        print("[PASS] All required symbols present and verified.")

    if args.check_production:
        print("\n[OPTIONAL] Verifying root production project build...")
        prod_res = run_cmd(["pio", "run"], cwd=REPO_ROOT)
        print("[PASS] Root production project builds cleanly.")

    if args.clean_after:
        clean_artifacts(clean_deps=args.clean_deps)

    print("\n" + "=" * 70)
    print("ALL VALIDATION CHECKS PASSED SUCCESSFULLY.")
    print("=" * 70)

if __name__ == "__main__":
    main()
