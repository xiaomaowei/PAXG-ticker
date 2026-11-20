import json
import subprocess
import os
import re
import sys
import shutil

def main():
    print("==================================================================")
    print("PAXG-ticker Phase 4 & Phase 5 Feasibility & Validation Harness")
    print("==================================================================")

    gpp = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-g++.exe")
    nm = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm.exe")
    framework_dir = os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32")
    tinyusb_lib = os.path.join(framework_dir, "tools", "sdk", "esp32s3", "lib", "libarduino_tinyusb.a")

    temp_files = []
    temp_compiledb = False

    try:
        # -------------------------------------------------------------
        # Step 1: Symbol inspection in precompiled libarduino_tinyusb.a
        # -------------------------------------------------------------
        print("\n--- Step 1: Inspecting TinyUSB precompiled symbols ---")
        nm_res = subprocess.run([nm, "-g", tinyusb_lib], capture_output=True, text=True)
        audio_symbols = [line for line in nm_res.stdout.splitlines() if "audio" in line.lower() or "tud_audio" in line.lower()]
        print(f"libarduino_tinyusb.a path: {tinyusb_lib}")
        print(f"Total matching 'audio' or 'tud_audio' symbols: {len(audio_symbols)}")
        assert len(audio_symbols) == 0, f"Expected 0 audio symbols in stock libarduino_tinyusb.a, but found {len(audio_symbols)}"
        print("  -> CONFIRMED: libarduino_tinyusb.a contains ZERO audio symbols.")

        # -------------------------------------------------------------
        # Step 2: Compile isolated test/usb_audio_poc/usb_audio_poc.cpp
        # -------------------------------------------------------------
        print("\n--- Step 2: Compiling isolated test/usb_audio_poc/usb_audio_poc.cpp ---")
        if not os.path.exists("compile_commands.json"):
            print("  Generating temporary compile_commands.json via 'pio run -t compiledb'...")
            subprocess.run(["pio", "run", "-t", "compiledb"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            temp_compiledb = True

        with open("compile_commands.json", "r") as f:
            cc = json.load(f)

        if temp_compiledb and os.path.exists("compile_commands.json"):
            try:
                os.remove("compile_commands.json")
                temp_compiledb = False
            except Exception:
                pass

        # Use esp32-hal-tinyusb.c compilation flags as baseline
        item = next(x for x in cc if "esp32-hal-tinyusb.c" in x["file"])
        cmd_str = item["command"]

        filtered_flags = []
        pattern = re.compile(r'(-I[^\s"]+|-D[^\s"]+)')
        for match in pattern.finditer(cmd_str):
            flag = match.group(0).replace('\\"', '"').replace('\\\\', '/')
            if "ARDUINO_BOARD" in flag:
                continue
            filtered_flags.append(flag)

        filtered_flags.append('-DARDUINO_BOARD=LilyGo_T_Display_S3')

        obj_file = "test/usb_audio_poc/usb_audio_poc.o"
        temp_files.append(obj_file)
        compile_poc_cmd = [
            gpp, "-c", "test/usb_audio_poc/usb_audio_poc.cpp",
            "-o", obj_file,
            "-std=gnu++11",
            "-Itest/usb_audio_poc",
            "-Iinclude"
        ] + filtered_flags

        res_poc_compile = subprocess.run(compile_poc_cmd, capture_output=True, text=True)
        print(f"PoC Compilation return code: {res_poc_compile.returncode}")
        assert res_poc_compile.returncode == 0, f"PoC compilation must succeed, but failed with return code {res_poc_compile.returncode}:\n{res_poc_compile.stderr}"
        print("  -> ASSERTION PASSED: test/usb_audio_poc/usb_audio_poc.cpp compiled successfully.")

        # -------------------------------------------------------------
        # Step 3: Asserted Link Failure for isolated PoC against TinyUSB
        # -------------------------------------------------------------
        print("\n--- Step 3: Attempting link of isolated PoC against libarduino_tinyusb.a ---")
        elf_file = "test/usb_audio_poc/usb_audio_poc.elf"
        temp_files.append(elf_file)
        link_cmd = [
            gpp, obj_file,
            tinyusb_lib,
            "-o", elf_file,
            "-Wl,--unresolved-symbols=report-all"
        ]
        res_poc_link = subprocess.run(link_cmd, capture_output=True, text=True)
        print(f"PoC Link return code: {res_poc_link.returncode} (Expected failure: missing audio symbols)")

        # Assert link failure
        assert res_poc_link.returncode != 0, "PoC link unexpectedly succeeded! Expected link failure due to missing tud_audio symbols."

        # Assert link failed specifically with an undefined tud_audio symbol
        tud_audio_errors = [line.strip() for line in res_poc_link.stderr.splitlines() if "tud_audio" in line]
        print(f"Detected undefined audio references ({len(tud_audio_errors)} lines):")
        for line in tud_audio_errors:
            print(f"    {line}")

        assert len(tud_audio_errors) > 0, f"Link failed, but not specifically with an undefined tud_audio symbol! Stderr:\n{res_poc_link.stderr}"
        print("  -> ASSERTION PASSED: Link failed specifically with undefined reference to tud_audio symbol.")

        # -------------------------------------------------------------
        # Step 4: Phase 5 Audio Detector Tests Compilation & Execution
        # -------------------------------------------------------------
        print("\n--- Step 4: Phase 5 Audio Detector Tests ---")
        native_cc = None
        for cand in ["g++", "clang++", "cl"]:
            if shutil.which(cand):
                native_cc = cand
                break

        test_sources = [
            "test/audio_detector/test_audio_detector.cpp",
            "src/audio/pcm_audio_detector.cpp",
            "src/audio/synthetic_audio_samples.cpp"
        ]

        if native_cc:
            print(f"  Native host compiler detected: {native_cc}")
            native_exe = "test/audio_detector/test_audio_detector_native.exe"
            temp_files.append(native_exe)

            if native_cc in ("g++", "clang++"):
                native_build_cmd = [
                    native_cc, "-std=c++11", "-Wall", "-Wextra",
                    "-Itest/audio_detector", "-Iinclude"
                ] + test_sources + ["-o", native_exe]
            else: # cl.exe
                native_build_cmd = [
                    native_cc, "/EHsc", "/std:c++14",
                    "/Iinclude", "/Itest/audio_detector"
                ] + test_sources + [f"/Fe:{native_exe}"]

            res_native_build = subprocess.run(native_build_cmd, capture_output=True, text=True)
            print(f"  Native build return code: {res_native_build.returncode}")
            assert res_native_build.returncode == 0, f"Native build failed:\n{res_native_build.stderr}"

            # Execute the tests natively
            print("  Executing native unit tests...")
            res_test_run = subprocess.run([os.path.abspath(native_exe)], capture_output=True, text=True)
            print(f"  Test process exit code: {res_test_run.returncode}")
            for line in res_test_run.stdout.splitlines():
                print(f"    {line}")
            assert res_test_run.returncode == 0, f"Native unit test assertions failed with exit code {res_test_run.returncode}"
            print("  -> NATIVE UNIT TESTS EXECUTED AND PASSED.")
        else:
            print("  [NOTICE] Native host compiler (g++, clang++, cl) is NOT available on host PATH.")
            print("  [NOTICE] Unit test assertions were NOT executed on host.")
            print("  [NOTICE] Labeled as: Cross-compile compile-only verification (unit test execution NOT claimed as passed).")

            # Cross-compile all test and implementation sources with xtensa-esp32s3-elf-g++
            for src in test_sources:
                base_name = os.path.splitext(os.path.basename(src))[0]
                obj = f"test/audio_detector/{base_name}.o"
                temp_files.append(obj)
                cross_cmd = [
                    gpp, "-c", src,
                    "-o", obj,
                    "-std=gnu++11",
                    "-Wall", "-Wextra", "-Werror",
                    "-Itest/audio_detector", "-Iinclude"
                ]
                res_cross = subprocess.run(cross_cmd, capture_output=True, text=True)
                print(f"  Cross-compiling {src} -> return code: {res_cross.returncode}")
                assert res_cross.returncode == 0, f"Cross-compilation failed for {src}:\n{res_cross.stderr}"

            print("  -> ASSERTION PASSED: All test and implementation sources cross-compiled cleanly (-Wall -Wextra -Werror).")

    finally:
        # -------------------------------------------------------------
        # Step 5: Clean up all generated temporary files
        # -------------------------------------------------------------
        print("\n--- Step 5: Cleaning up all temporary files ---")
        if temp_compiledb and os.path.exists("compile_commands.json"):
            try:
                os.remove("compile_commands.json")
                print("  Removed compile_commands.json")
            except Exception as e:
                print(f"  Warning: failed to remove compile_commands.json: {e}")

        # Also check and clean any residual compile_commands.json if left by validation
        if os.path.exists("compile_commands.json"):
            try:
                os.remove("compile_commands.json")
                print("  Removed compile_commands.json")
            except Exception:
                pass

        for tf in temp_files:
            if os.path.exists(tf):
                try:
                    os.remove(tf)
                    print(f"  Removed temporary file: {tf}")
                except Exception as e:
                    print(f"  Warning: failed to remove {tf}: {e}")

    print("\n==================================================================")
    print("Feasibility & Validation Harness Completed Successfully.")
    print("==================================================================")
    return 0

if __name__ == "__main__":
    sys.exit(main())
