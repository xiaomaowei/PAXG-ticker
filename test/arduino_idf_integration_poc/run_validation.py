#!/usr/bin/env python3
"""
PAXG-ticker: Isolated Arduino-ESP32 + TFT_eSPI + WiFi + Composite USB (ESP-IDF 5.x)
             + Real UAC PCM to Production Audio Detector Pipeline Validation Runner.

This script independently validates:
1. Native C++ tests for UAC 48kHz stereo to production PcmAudioDetector pipeline
   (exercising split/unaligned packets, stereo extremes, silence vs signal, timing/cooldown,
    stream break residual flushing, and multi-threaded concurrency).
2. Strict cross-compilation with xtensa-esp32s3-elf-g++ (-Wall -Wextra -Werror).
3. Target PlatformIO ESP-IDF build and binary/symbol verification.
4. CLI compatibility with original options (--clean-only, --clean-after, --clean-deps, --check-production)
   plus --host-only to test bridge pipeline without triggering shared component builds.
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
TEST_DIR = POC_DIR / "test"
MAIN_DIR = POC_DIR / "main"

# Explicit list of generated artifacts owned strictly by this validation script.
# Wildcard deletion is forbidden to protect user-generated files.
OWNED_TEST_ARTIFACTS = [
    TEST_DIR / "test_uac_audio_bridge_native.exe",
    TEST_DIR / "test_uac_audio_bridge_native.pdb",
    TEST_DIR / "test_uac_audio_bridge_native.ilk",
    TEST_DIR / "test_live_fishing_controller_native.exe",
    TEST_DIR / "test_live_fishing_controller_native.pdb",
    TEST_DIR / "test_live_fishing_controller_native.ilk",
    TEST_DIR / "test_paxg_integration_native.exe",
    TEST_DIR / "test_paxg_integration_native.pdb",
    TEST_DIR / "test_paxg_integration_native.ilk",
    TEST_DIR / "test_bag_full_web_native.exe",
    TEST_DIR / "test_bag_full_web_native.pdb",
    TEST_DIR / "test_bag_full_web_native.ilk",
    TEST_DIR / "test_fishing_wifi_settings_native.exe",
    TEST_DIR / "test_fishing_wifi_settings_native.pdb",
    TEST_DIR / "test_fishing_wifi_settings_native.ilk",
    TEST_DIR / "uac_audio_bridge_cross.o",
    TEST_DIR / "test_uac_audio_bridge_cross.o",
    TEST_DIR / "pcm_audio_detector_cross.o",
    TEST_DIR / "live_fishing_controller_cross.o",
    TEST_DIR / "test_live_fishing_controller_cross.o",
    TEST_DIR / "mode_navigator_cross.o",
    TEST_DIR / "paxg_async_quote_cross.o",
    TEST_DIR / "test_paxg_integration_cross.o",
    TEST_DIR / "bag_full_detector_cross.o",
    TEST_DIR / "fishing_web_server_cross.o",
    TEST_DIR / "test_bag_full_web_cross.o",
    TEST_DIR / "fishing_wifi_settings_cross.o",
    TEST_DIR / "test_fishing_wifi_settings_cross.o",
]

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

def find_xtensa_gpp():
    """Locate xtensa-esp32s3-elf-g++ tool in PlatformIO toolchains or PATH."""
    home = Path.home()
    pio_toolchains = list(home.glob(".platformio/packages/toolchain-xtensa-esp32s3*/bin/xtensa-esp32s3-elf-g++*"))
    for tool in pio_toolchains:
        if tool.is_file() and os.access(tool, os.X_OK):
            return str(tool)
    which_gpp = shutil.which("xtensa-esp32s3-elf-g++")
    if which_gpp:
        return which_gpp
    return None

def find_host_cxx():
    """Detect available native C++ compiler on host."""
    for cand in ["g++", "clang++", "cl"]:
        which_cand = shutil.which(cand)
        if which_cand:
            return [which_cand]
    # Check uvx python-zig
    uv_path = shutil.which("uv") or shutil.which("uvx")
    if uv_path:
        res = subprocess.run(["uvx", "--from", "ziglang", "python-zig", "version"],
                             capture_output=True, text=True)
        if res.returncode == 0:
            return ["uvx", "--from", "ziglang", "python-zig", "c++"]
    return None

def run_cmd(cmd, cwd=None, check=True):
    print(f"[CMD] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(cmd, cwd=cwd or REPO_ROOT, shell=isinstance(cmd, str), capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"[ERROR] Command failed with code {res.returncode}:")
        if res.stdout:
            print(res.stdout)
        if res.stderr:
            print(res.stderr)
        sys.exit(res.returncode)
    return res

def clean_artifacts(clean_deps=False):
    print("[CLEAN] Cleaning generated build artifacts in PoC directory...")
    pio_dir = POC_DIR / ".pio"
    if pio_dir.exists():
        shutil.rmtree(pio_dir, ignore_errors=True)
        print(f"  Removed {pio_dir}")

    # Remove specific known generated sdkconfig files
    for sdk_name in ["sdkconfig.esp32s3_arduino_idf_poc", "sdkconfig.esp32s3_arduino_idf_poc.old"]:
        sdk = POC_DIR / sdk_name
        if sdk.is_file():
            sdk.unlink(missing_ok=True)
            print(f"  Removed {sdk}")

    if clean_deps:
        upstream_dir = TFT_COMP_DIR / "upstream"
        if upstream_dir.exists():
            shutil.rmtree(upstream_dir, ignore_errors=True)
            print(f"  Removed {upstream_dir}")

    # Remove only our explicitly owned test artifacts without wildcards
    for artifact in OWNED_TEST_ARTIFACTS:
        if artifact.is_file():
            artifact.unlink(missing_ok=True)
            print(f"  Removed {artifact.name}")

    print("[CLEAN] Done.")

def ensure_dependencies():
    """Ensure pinned TFT_eSPI dependency is acquired via bootstrap_tft.py."""
    bootstrap_script = TFT_COMP_DIR / "bootstrap_tft.py"
    upstream_dir = TFT_COMP_DIR / "upstream"
    if not (upstream_dir / "TFT_eSPI.cpp").is_file():
        print("[BOOTSTRAP] Acquiring pinned TFT_eSPI dependency...")
        run_cmd([sys.executable, str(bootstrap_script)])

def run_native_pipeline_tests():
    """Build and execute the native C++ unit test suite for the UAC PCM to detector pipeline."""
    print("\n--- Running UAC PCM to Production Audio Detector Native Pipeline Tests ---")
    cxx = find_host_cxx()
    if not cxx:
        print("[FAIL] Required host C++ compiler not found on system.")
        sys.exit(1)

    test_src = TEST_DIR / "test_uac_audio_bridge.cpp"
    bridge_src = MAIN_DIR / "uac_audio_bridge.cpp"
    detector_src = REPO_ROOT / "src" / "audio" / "pcm_audio_detector.cpp"
    queue_src = REPO_ROOT / "test" / "usb_composite_poc" / "components" / "usb_device_composite" / "usb_audio_queue.c"
    queue_inc = REPO_ROOT / "test" / "usb_composite_poc" / "components" / "usb_device_composite" / "include"
    test_exe = TEST_DIR / "test_uac_audio_bridge_native.exe"

    print(f"  Host C++ compiler: {' '.join(cxx)}")
    compile_cmd = cxx + [
        "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(REPO_ROOT / "include"),
        "-I" + str(MAIN_DIR),
        "-I" + str(queue_inc),
        str(test_src), str(bridge_src), str(detector_src),
        "-x", "c++", str(queue_src),
        "-o", str(test_exe)
    ]
    res_compile = subprocess.run(compile_cmd, capture_output=True, text=True)
    if res_compile.returncode != 0:
        print(f"[FAIL] Native test compilation failed:\n{res_compile.stderr}")
        sys.exit(res_compile.returncode)
    print("  [PASS] Native test suite compiled cleanly.")

    # Execute tests
    res_run = subprocess.run([str(test_exe)], capture_output=True, text=True)
    print(res_run.stdout)
    if res_run.returncode != 0:
        print(f"[FAIL] Native unit test assertions failed with exit code {res_run.returncode}")
        test_exe.unlink(missing_ok=True)
        sys.exit(res_run.returncode)
    print("  -> ALL 10 NATIVE PIPELINE UNIT TESTS EXECUTED AND PASSED.")

    # Clean up test executable immediately
    test_exe.unlink(missing_ok=True)
    pdb = TEST_DIR / "test_uac_audio_bridge_native.pdb"
    if pdb.is_file():
        pdb.unlink(missing_ok=True)

    # 2. Compile and run live fishing controller unit tests
    print("\n--- Running Live Fishing Controller & Safe Spacebar HID Native Tests ---")
    ctrl_test_src = TEST_DIR / "test_live_fishing_controller.cpp"
    ctrl_src = MAIN_DIR / "live_fishing_controller.cpp"
    ctrl_exe = TEST_DIR / "test_live_fishing_controller_native.exe"

    compile_ctrl_cmd = cxx + [
        "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(REPO_ROOT / "include"),
        "-I" + str(MAIN_DIR),
        str(ctrl_test_src), str(ctrl_src),
        "-o", str(ctrl_exe)
    ]
    res_ctrl_compile = subprocess.run(compile_ctrl_cmd, capture_output=True, text=True)
    if res_ctrl_compile.returncode != 0:
        print(f"[FAIL] Native controller test compilation failed:\n{res_ctrl_compile.stderr}")
        sys.exit(res_ctrl_compile.returncode)
    print("  [PASS] Native controller test suite compiled cleanly.")

    res_ctrl_run = subprocess.run([str(ctrl_exe)], capture_output=True, text=True)
    print(res_ctrl_run.stdout)
    if res_ctrl_run.returncode != 0:
        print(f"[FAIL] Native controller test assertions failed with exit code {res_ctrl_run.returncode}")
        ctrl_exe.unlink(missing_ok=True)
        sys.exit(res_ctrl_run.returncode)
    print("  -> ALL 28 NATIVE LIVE CONTROLLER TESTS EXECUTED AND PASSED.")

    ctrl_exe.unlink(missing_ok=True)
    ctrl_pdb = TEST_DIR / "test_live_fishing_controller_native.pdb"
    if ctrl_pdb.is_file():
        ctrl_pdb.unlink(missing_ok=True)

    # 3. Compile and run PAXG market & mode switching integration unit tests
    print("\n--- Running PAXG Market & Mode Switching Native Integration Tests ---")
    paxg_test_src = TEST_DIR / "test_paxg_integration.cpp"
    paxg_quote_src = MAIN_DIR / "paxg_async_quote.cpp"
    mode_nav_src = MAIN_DIR / "mode_navigator.cpp"
    paxg_exe = TEST_DIR / "test_paxg_integration_native.exe"

    compile_paxg_cmd = cxx + [
        "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(REPO_ROOT / "include"),
        "-I" + str(MAIN_DIR),
        str(paxg_test_src), str(paxg_quote_src), str(ctrl_src), str(mode_nav_src),
        "-o", str(paxg_exe)
    ]
    res_paxg_compile = subprocess.run(compile_paxg_cmd, capture_output=True, text=True)
    if res_paxg_compile.returncode != 0:
        print(f"[FAIL] Native PAXG integration test compilation failed:\n{res_paxg_compile.stderr}")
        sys.exit(res_paxg_compile.returncode)
    print("  [PASS] Native PAXG integration test suite compiled cleanly.")

    res_paxg_run = subprocess.run([str(paxg_exe)], capture_output=True, text=True)
    print(res_paxg_run.stdout)
    if res_paxg_run.returncode != 0:
        print(f"[FAIL] Native PAXG integration test assertions failed with exit code {res_paxg_run.returncode}")
        paxg_exe.unlink(missing_ok=True)
        sys.exit(res_paxg_run.returncode)
    print("  -> ALL 17 NATIVE PAXG INTEGRATION TESTS EXECUTED AND PASSED.")

    paxg_exe.unlink(missing_ok=True)
    paxg_pdb = TEST_DIR / "test_paxg_integration_native.pdb"
    if paxg_pdb.is_file():
        paxg_pdb.unlink(missing_ok=True)

    # 4. Compile and run Bag-Full Audio Stop & LAN Web Server unit tests
    print("\n--- Running Bag-Full Audio Stop & LAN Web Server Native Tests ---")
    bag_test_src = TEST_DIR / "test_bag_full_web.cpp"
    bag_detector_src = MAIN_DIR / "bag_full_detector.cpp"
    web_server_src = MAIN_DIR / "fishing_web_server.cpp"
    bag_exe = TEST_DIR / "test_bag_full_web_native.exe"

    compile_bag_cmd = cxx + [
        "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(REPO_ROOT / "include"),
        "-I" + str(MAIN_DIR),
        str(bag_test_src), str(bag_detector_src), str(web_server_src), str(MAIN_DIR / "fishing_wifi_settings.cpp"), str(ctrl_src), str(bridge_src), str(detector_src),
        "-o", str(bag_exe)
    ]
    res_bag_compile = subprocess.run(compile_bag_cmd, capture_output=True, text=True)
    if res_bag_compile.returncode != 0:
        print(f"[FAIL] Native Bag-Full & Web Server test compilation failed:\n{res_bag_compile.stderr}")
        sys.exit(res_bag_compile.returncode)
    print("  [PASS] Native Bag-Full & Web Server test suite compiled cleanly.")

    res_bag_run = subprocess.run([str(bag_exe)], capture_output=True, text=True)
    print(res_bag_run.stdout)
    if res_bag_run.returncode != 0:
        print(f"[FAIL] Native Bag-Full & Web Server test assertions failed with exit code {res_bag_run.returncode}")
        bag_exe.unlink(missing_ok=True)
        sys.exit(res_bag_run.returncode)
    print("  -> ALL 19 NATIVE BAG-FULL & WEB SERVER TESTS EXECUTED AND PASSED.")

    bag_exe.unlink(missing_ok=True)
    bag_pdb = TEST_DIR / "test_bag_full_web_native.pdb"
    if bag_pdb.is_file():
        bag_pdb.unlink(missing_ok=True)

    # 5. Compile and run Wi-Fi Settings & Recovery Manager unit tests
    print("\n--- Running Wi-Fi Settings & Recovery Manager Native Tests ---")
    wifi_test_src = TEST_DIR / "test_fishing_wifi_settings.cpp"
    wifi_settings_src = MAIN_DIR / "fishing_wifi_settings.cpp"
    wifi_exe = TEST_DIR / "test_fishing_wifi_settings_native.exe"

    compile_wifi_cmd = cxx + [
        "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(REPO_ROOT / "include"),
        "-I" + str(MAIN_DIR),
        str(wifi_test_src), str(wifi_settings_src), str(web_server_src), str(bag_detector_src), str(ctrl_src), str(bridge_src), str(detector_src),
        "-o", str(wifi_exe)
    ]
    res_wifi_compile = subprocess.run(compile_wifi_cmd, capture_output=True, text=True)
    if res_wifi_compile.returncode != 0:
        print(f"[FAIL] Native Wi-Fi Settings test compilation failed:\n{res_wifi_compile.stderr}")
        sys.exit(res_wifi_compile.returncode)
    print("  [PASS] Native Wi-Fi Settings test suite compiled cleanly.")

    res_wifi_run = subprocess.run([str(wifi_exe)], capture_output=True, text=True)
    print(res_wifi_run.stdout)
    if res_wifi_run.returncode != 0:
        print(f"[FAIL] Native Wi-Fi Settings test assertions failed with exit code {res_wifi_run.returncode}")
        wifi_exe.unlink(missing_ok=True)
        sys.exit(res_wifi_run.returncode)
    print("  -> ALL 11 NATIVE WI-FI SETTINGS & RECOVERY TESTS EXECUTED AND PASSED.")

    wifi_exe.unlink(missing_ok=True)
    wifi_pdb = TEST_DIR / "test_fishing_wifi_settings_native.pdb"
    if wifi_pdb.is_file():
        wifi_pdb.unlink(missing_ok=True)

    # Cross-compile check with xtensa-esp32s3-elf-g++
    xtensa_gpp = find_xtensa_gpp()
    if not xtensa_gpp:
        print("[FAIL] Required xtensa-esp32s3-elf-g++ not found on system.")
        sys.exit(1)

    print("\n  Cross-compiling bridge, controller, quote, bag-full, web server, wifi settings and test sources with xtensa-esp32s3-elf-g++...")
    for src in [bridge_src, test_src, ctrl_src, ctrl_test_src, detector_src, queue_src, paxg_quote_src, paxg_test_src, mode_nav_src, bag_detector_src, web_server_src, bag_test_src, wifi_settings_src, wifi_test_src]:
        obj = TEST_DIR / f"{src.stem}_cross.o"
        compiler = xtensa_gpp
        std_flag = "-std=gnu++11"
        extra_flags = []
        if src.suffix == ".c":
            # For C source, use xtensa gcc if available or pass -std=gnu11
            gcc_path = str(xtensa_gpp).replace("xtensa-esp32s3-elf-g++", "xtensa-esp32s3-elf-gcc")
            if os.path.isfile(gcc_path):
                compiler = gcc_path
            std_flag = "-std=gnu11"
        cross_cmd = [
            str(compiler), "-c", str(src),
            "-o", str(obj),
            std_flag, "-Wall", "-Wextra", "-Werror",
            "-I" + str(REPO_ROOT / "include"),
            "-I" + str(MAIN_DIR),
            "-I" + str(queue_inc)
        ]
        res_cross = subprocess.run(cross_cmd, capture_output=True, text=True)
        if res_cross.returncode != 0:
            print(f"[FAIL] Cross-compilation failed for {src}:\n{res_cross.stderr}")
            obj.unlink(missing_ok=True)
            sys.exit(res_cross.returncode)
        print(f"    [OK] Cross-compiled {src.name}")
        obj.unlink(missing_ok=True)
    print("  -> ALL SOURCES CROSS-COMPILED CLEANLY FOR ESP32-S3 (-Wall -Wextra -Werror).")

def build_isolated_poc():
    """Attempt PlatformIO ESP-IDF build and fail nonzero on failure."""
    print("\n--- Compiling Isolated Arduino+IDF PoC ---")
    pio_cmd = None
    temp_pio = Path(os.environ.get("TEMP", "")) / "paxg-pio-env" / "Scripts" / "pio.exe"
    if shutil.which("pio"):
        pio_cmd = ["pio", "run", "-d", str(POC_DIR)]
    elif temp_pio.is_file():
        pio_cmd = [str(temp_pio), "run", "-d", str(POC_DIR)]
    elif shutil.which("uv"):
        pio_cmd = ["uv", "run", "--python", "3.13", "--with", "setuptools<70", "--with", "platformio",
                   "pio", "run", "-d", str(POC_DIR)]

    if not pio_cmd:
        print("[FAIL] PlatformIO tool not available on system PATH.")
        sys.exit(1)

    res = run_cmd(pio_cmd, check=False)
    if res.returncode != 0:
        print(f"[FAIL] PoC build failed with exit code {res.returncode}.")
        sys.exit(res.returncode)
    print("[PASS] PoC build succeeded.")

def verify_elf_artifacts_and_symbols():
    """Verify generated firmware.elf and inspect required symbols."""
    print("\n--- Verifying Output Binaries and Symbols ---")
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

    nm_tool = find_nm_tool()
    if not nm_tool:
        print("[FAIL] Required xtensa-esp32s3-elf-nm tool not found on system.")
        sys.exit(1)

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
        ("paxg::uac::UacAudioBridge::consumeUacChunk", "UAC PCM Audio Bridge Consumer"),
        ("paxg::audio::PcmAudioDetector::processSamples", "Production Audio Detector Consumer"),
        ("usb_composite_get_audio_stats", "Composite Driver Audio Queue Telemetry"),
        ("paxg::fishing::LiveFishingController::update", "Live Fishing Controller Main Update Loop"),
        ("paxg::fishing::LiveFishingController::requestStart", "Live Fishing Controller Start Operation"),
        ("paxg::market::parseBinanceTicker", "Binance Ticker JSON Parser"),
        ("paxg::market::getMarketQuoteSnapshot", "Thread-Safe Market Quote Telemetry"),
        ("paxg::market::startPaxgQuoteWorker", "Bounded Async Network Worker Task"),
        ("paxg::market::evaluateMarketSessions", "Market Sessions Evaluator"),
        ("paxg::ui::ModeNavigator::update", "Mode Navigator Loop Update"),
        ("paxg::audio::BagFullDetector::processRms", "Evidence-Calibrated Bag-Full Audio Detector"),
        ("paxg::web::startFishingWebServer", "LAN Fishing Status and Web Server"),
        ("paxg::web::processPendingCommands", "Owner-Loop Thread-Safe Command Queue Processor"),
        ("paxg::wifi::getWifiSettingsManager", "Wi-Fi Settings & Recovery Manager"),
        ("paxg::wifi::WifiSettingsManager::update", "Wi-Fi Settings Manager Loop Update"),
    ]

    all_found = True
    for sym, desc in required_symbols:
        if sym in symbols:
            print(f"  [OK] Found symbol: {sym:<45} ({desc})")
        else:
            print(f"  [FAIL] Missing symbol: {sym:<45} ({desc})")
            all_found = False

    if not all_found:
        print("[FAIL] One or more critical symbols missing from firmware.elf.")
        sys.exit(1)
    print("[PASS] All required symbols present and verified.")

    # Verify partition table binary byte-identity with production
    poc_part_file = POC_DIR / ".pio" / "build" / "esp32s3_arduino_idf_poc" / "partitions.bin"
    prod_part_file = REPO_ROOT / ".pio" / "build" / "lilygo-t-display-s3" / "partitions.bin"

    if not poc_part_file.exists():
        print(f"[FAIL] Missing partitions.bin: {poc_part_file}")
        sys.exit(1)

    import hashlib
    with open(poc_part_file, "rb") as f:
        poc_part_hash = hashlib.sha256(f.read()).hexdigest()

    if prod_part_file.exists():
        with open(prod_part_file, "rb") as f:
            prod_part_hash = hashlib.sha256(f.read()).hexdigest()
        if poc_part_hash != prod_part_hash:
            print(f"[FAIL] Partition table mismatch!\n  PoC:  {poc_part_hash}\n  Prod: {prod_part_hash}")
            sys.exit(1)
        print(f"[PASS] partitions.bin is BYTE-IDENTICAL to production ({poc_part_hash}).")
    else:
        print(f"[INFO] Production partitions.bin not pre-built; PoC partitions.bin hash: {poc_part_hash}")

def main():
    parser = argparse.ArgumentParser(description="Validate Arduino-ESP32 + IDF 5.x PoC build and UAC Audio Bridge.")
    parser.add_argument("--clean-only", action="store_true", help="Clean build artifacts and exit.")
    parser.add_argument("--clean-after", action="store_true", help="Clean build artifacts after successful validation.")
    parser.add_argument("--clean-deps", action="store_true", help="Also clean downloaded dependency upstream directory.")
    parser.add_argument("--check-production", action="store_true", help="Also verify root production build.")
    parser.add_argument("--host-only", action="store_true",
                        help="Run native pipeline tests & cross-compilation only (avoids building shared composite component).")
    args = parser.parse_args()

    if args.clean_only:
        clean_artifacts(clean_deps=args.clean_deps)
        return

    print("=" * 70)
    print("PAXG-ticker: Arduino-as-IDF-Component & UAC Audio Bridge Validation")
    print(f"Target directory: {POC_DIR}")
    print("=" * 70)

    # 0. Ensure pinned dependencies
    print("\n[STEP 0/3] Checking pinned dependencies...")
    ensure_dependencies()

    # 1. Native pipeline unit tests & cross-compilation
    print("\n[STEP 1/3] Running native C++ unit tests & cross-compilation...")
    run_native_pipeline_tests()

    if args.host_only:
        print("\n[INFO] --host-only specified; skipping target PoC build.")
        if args.check_production:
            print("\n[OPTIONAL] Verifying root production project build...")
            run_cmd(["pio", "run"], cwd=REPO_ROOT)
            print("[PASS] Root production project builds cleanly.")
        if args.clean_after:
            clean_artifacts(clean_deps=args.clean_deps)
        print("\n" + "=" * 70)
        print("ALL HOST VALIDATION CHECKS PASSED SUCCESSFULLY.")
        print("=" * 70)
        return

    # 2. Compile isolated PoC (fails nonzero if build fails)
    print("\n[STEP 2/3] Compiling isolated PoC project...")
    build_isolated_poc()

    # 3. Check binary artifacts and verify symbols
    print("\n[STEP 3/3] Inspecting ELF symbols...")
    verify_elf_artifacts_and_symbols()

    if args.check_production:
        print("\n[OPTIONAL] Verifying root production project build...")
        run_cmd(["pio", "run"], cwd=REPO_ROOT)
        print("[PASS] Root production project builds cleanly.")

    if args.clean_after:
        clean_artifacts(clean_deps=args.clean_deps)

    print("\n" + "=" * 70)
    print("ALL VALIDATION CHECKS PASSED SUCCESSFULLY.")
    print("=" * 70)

if __name__ == "__main__":
    main()
