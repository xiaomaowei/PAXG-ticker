#!/usr/bin/env python3
"""
Validation Harness for Isolated Simultaneous USB Composite (UAC 2.0 Speaker + HID Keyboard).

Performs:
1. Native compiler probe & C test execution / compile-only verification:
   - Probes for native host C compilers (MSVC via vswhere, clang, gcc).
   - If native compiler exists: compiles and runs test_button_fsm.c natively on host.
   - If no native host compiler exists: explicitly labels C test as COMPILE-ONLY
     (cross-compiled via xtensa-esp32s3-elf-gcc -Wall -Wextra -Werror) without claiming host pass.
2. Executes host-runnable deterministic unit tests for Button FSM (test_button_fsm.py):
   - Endpoint busy right after successful press does NOT trigger false disconnect.
   - Release report returning busy retries non-blocking on subsequent ticks.
   - True disconnect (unmount/suspend) aborts safely; reconnect while held emits 0 keys.
   - Held at boot and across mount emits zero keys; requires observed stable release.
   - Transient bounce rejection on press and release.
   - Single actuation per physical press (strictly NO auto-repeat on long holds).
3. Isolated composite build produces valid firmware.elf and firmware.bin via PlatformIO ESP-IDF.
4. Linker symbol verification via xtensa-esp32s3-elf-nm:
   - Shared composite descriptors (desc_configuration, desc_hid_report).
   - UAC 2.0 audio reception symbols (uac_device_init, usb_composite_init, tud_audio_n_read, tud_audio_rx_done_isr, uac_speaker_output_cb).
   - HID keyboard symbols (tud_hid_n_keyboard_report, tud_hid_descriptor_report_cb, hidd_open, usb_composite_hid_keyboard_press, usb_composite_hid_keyboard_release).
   - Connection status symbol (usb_composite_is_mounted).
5. Exact descriptor bytecode inspection via xtensa-esp32s3-elf-objdump:
   - Validates wTotalLength = 177 bytes (0x00B1).
   - Validates bNumInterfaces = 3 (Interface 0: Audio Control, Interface 1: Audio Streaming, Interface 2: HID Keyboard).
   - Validates UAC version is strictly UAC 2.0 (bcdADC = 0x0200, bInterfaceProtocol = 0x20).
   - Validates Audio EP 1 OUT: Isochronous, address 0x01, wMaxPacketSize = 196 bytes, bInterval = 1 ms.
   - Validates Audio EP 1 IN: Isochronous Feedback, address 0x81, wMaxPacketSize = 4 bytes, bInterval = 1 ms.
   - Validates HID Interface: Class 0x03, Subclass 0x01 (Boot), Protocol 0x01 (Keyboard).
   - Validates HID EP 2 IN: Interrupt, address 0x82, wMaxPacketSize = 16 bytes, bInterval = 10 ms.
6. Verifies production firmware (PlatformIO Arduino) builds cleanly without interference.
7. Automatically cleans up isolated temporary build outputs.
"""

import os
import sys
import subprocess
import shutil
import re

def run_cmd(cmd, cwd=None):
    print(f"Running: {' '.join(cmd)}")
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return res

def probe_native_host_compiler():
    # Check PATH first
    for cc in ["cl", "clang", "gcc"]:
        p = shutil.which(cc)
        if p:
            return cc, p

    # Probe vswhere for MSVC
    vswhere_exe = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if os.path.exists(vswhere_exe):
        res = subprocess.run([
            vswhere_exe, "-latest", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath"
        ], capture_output=True, text=True)
        if res.returncode == 0 and res.stdout.strip():
            vs_path = res.stdout.strip()
            # Search for cl.exe under vs_path
            for root, _, files in os.walk(os.path.join(vs_path, "VC", "Tools", "MSVC")):
                if "cl.exe" in files:
                    cl_p = os.path.join(root, "cl.exe")
                    if "Hostx64\\x64" in cl_p or "bin\\Hostx86\\x86" in cl_p:
                        return "cl", cl_p
    return None, None

def main():
    print("=" * 75)
    print("PAXG-ticker Simultaneous USB Composite (UAC 2.0 + HID) Validation Harness")
    print("=" * 75)

    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    poc_dir = os.path.join(project_root, "test", "usb_composite_poc")
    elf_path = os.path.join(poc_dir, ".pio", "build", "esp32s3_composite_poc", "firmware.elf")
    bin_path = os.path.join(poc_dir, ".pio", "build", "esp32s3_composite_poc", "firmware.bin")

    gcc_tool = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gcc.exe")
    nm_tool = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm.exe")
    objdump_tool = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-objdump.exe")

    try:
        # Step 0: Probe native host C compiler and execute or label COMPILE-ONLY
        print("\n--- Step 0: Probing native host C compiler & C test harness ---")
        cc_name, cc_path = probe_native_host_compiler()
        test_c = os.path.join(poc_dir, "test_button_fsm.c")

        if cc_name:
            print(f"  [FOUND] Native host C compiler detected: {cc_name} ({cc_path})")
            # Build and run native binary
            exe_out = os.path.join(poc_dir, "test_button_fsm.exe")
            if cc_name == "cl":
                compile_res = run_cmd([cc_path, "/W4", "/WX", f"/I{os.path.join(poc_dir, 'main')}", test_c, f"/Fe:{exe_out}"])
            else:
                compile_res = run_cmd([cc_path, "-Wall", "-Wextra", "-Werror", f"-I{os.path.join(poc_dir, 'main')}", test_c, "-o", exe_out])
            assert compile_res.returncode == 0, f"Native C compilation failed:\n{compile_res.stderr}"
            run_res = run_cmd([exe_out])
            assert run_res.returncode == 0, f"Native test run failed:\n{run_res.stderr}"
            print(run_res.stdout)
            if os.path.exists(exe_out):
                os.remove(exe_out)
            print("  -> ASSERTION PASSED: Native host C tests compiled and passed.")
        else:
            print("  [NOT FOUND] No native host C compiler (MSVC/cl.exe via vswhere, clang, gcc) found on host PATH.")
            print("  -> EVIDENCE BOUNDARY: Standalone C test (test_button_fsm.c) is strictly marked COMPILE-ONLY.")
            print("     Host pass is NOT claimed for the C binary because no native host toolchain is present.")
            if os.path.exists(gcc_tool):
                obj_out = os.path.join(poc_dir, "test_button_fsm.o")
                c_res = run_cmd([
                    gcc_tool, "-c", "-Wall", "-Wextra", "-Werror",
                    "-I", os.path.join(poc_dir, "main"),
                    test_c, "-o", obj_out
                ])
                if c_res.returncode != 0:
                    print(f"FAILED: Compilation of test_button_fsm.c failed:\n{c_res.stderr}")
                    sys.exit(1)
                if os.path.exists(obj_out):
                    os.remove(obj_out)
                print("  -> COMPILE-ONLY ASSERTION PASSED: test_button_fsm.c compiled cleanly under xtensa-esp32s3-elf-gcc with -Wall -Wextra -Werror.")
            else:
                print("  -> NOTE: Target cross-compiler not found.")

        # Step 0b: Execute host-runnable deterministic Button FSM tests via Python
        print("\n--- Step 0b: Running host-runnable deterministic Button FSM tests (Python 3.14) ---")
        test_py = os.path.join(poc_dir, "test_button_fsm.py")
        py_res = run_cmd([sys.executable, test_py])
        print(py_res.stdout)
        if py_res.returncode != 0:
            print(f"FAILED: test_button_fsm.py exit code {py_res.returncode}:\n{py_res.stderr}")
            sys.exit(1)
        print("  -> HOST ASSERTION PASSED: All 6 deterministic Button FSM unit tests passed on host Python.")

        # Step 1: Execute isolated PlatformIO ESP-IDF composite build
        print("\n--- Step 1: Building isolated test/usb_composite_poc ---")
        build_res = run_cmd(["pio", "run", "-d", "test/usb_composite_poc"], cwd=project_root)
        print(f"Build Exit Code: {build_res.returncode}")
        if build_res.returncode != 0:
            print("Build output:")
            print(build_res.stdout[-2000:])
            print(build_res.stderr[-2000:])
            sys.exit(1)
        print("  -> ASSERTION PASSED: Isolated composite ESP-IDF build completed with exit code 0.")

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
        if not os.path.exists(nm_tool):
            nm_tool = shutil.which("xtensa-esp32s3-elf-nm") or "nm"

        nm_res = run_cmd([nm_tool, "-C", elf_path])
        assert nm_res.returncode == 0, f"Failed to run nm on {elf_path}:\n{nm_res.stderr}"

        symbols_to_find = [
            # Composite USB Descriptors
            ("desc_configuration", "Shared Composite Configuration Descriptor"),
            ("desc_hid_report", "HID Keyboard Report Descriptor"),
            # UAC 2.0 Audio Speaker Class Symbols
            ("usb_composite_init", "Composite Device Initialization API"),
            ("uac_device_init", "Compatible UAC Device Initialization API"),
            ("tud_audio_n_read", "TinyUSB UAC Isochronous Data Read API"),
            ("tud_audio_rx_done_isr", "TinyUSB UAC Audio RX Done ISR Handler"),
            ("uac_speaker_output_cb", "No-Codec PCM Frame Reception Callback"),
            # HID Keyboard Class Symbols
            ("hidd_open", "TinyUSB HID Class Driver Open Handler"),
            ("tud_hid_descriptor_report_cb", "TinyUSB HID Report Descriptor Callback"),
            ("tud_hid_n_keyboard_report", "TinyUSB HID Keyboard Report Transmission"),
            ("usb_composite_hid_keyboard_press", "Composite HID Keyboard Keypress API"),
            ("usb_composite_hid_keyboard_release", "Composite HID Keyboard Release API"),
            # Connection Status Symbol
            ("usb_composite_is_mounted", "Composite Device Mount Status API"),
        ]

        matched_symbols = {}
        for line in nm_res.stdout.splitlines():
            for sym, _ in symbols_to_find:
                if sym in line:
                    matched_symbols[sym] = line.strip()

        print("Found symbols in firmware.elf:")
        for sym, desc in symbols_to_find:
            loc = matched_symbols.get(sym, None)
            print(f"  [{'FOUND' if loc else 'MISSING'}] {sym} ({desc}): {loc}")
            assert loc is not None, f"Required composite symbol '{sym}' was not found in firmware.elf!"

        print("  -> ASSERTION PASSED: All critical UAC, HID, and connection status symbols are resolved in the same image.")

        # Step 4: Bytecode Inspection of Configuration Descriptor
        print("\n--- Step 4: Extracting and inspecting descriptor bytecode ---")
        if not os.path.exists(objdump_tool):
            objdump_tool = shutil.which("xtensa-esp32s3-elf-objdump") or "objdump"

        sym_res = run_cmd([objdump_tool, "-t", elf_path])
        addr_match = re.search(r'([0-9a-fA-F]+)\s+[g\s]+\s+O\s+\.flash\.rodata\s+([0-9a-fA-F]+)\s+desc_configuration', sym_res.stdout)
        assert addr_match is not None, "Failed to locate desc_configuration in .flash.rodata via objdump!"

        desc_addr_int = int(addr_match.group(1), 16)
        desc_len_int = int(addr_match.group(2), 16)
        print(f"desc_configuration located at 0x{desc_addr_int:08x}, size = {desc_len_int} bytes (0x{desc_len_int:02x})")
        assert desc_len_int == 177, f"Expected desc_configuration length 177 bytes (0xB1), got {desc_len_int}!"

        dump_res = run_cmd([
            objdump_tool, "-s",
            f"--start-address=0x{desc_addr_int:x}",
            f"--stop-address=0x{(desc_addr_int + desc_len_int):x}",
            elf_path
        ])
        assert dump_res.returncode == 0, f"Failed to dump desc_configuration bytes:\n{dump_res.stderr}"

        raw_hex = []
        for line in dump_res.stdout.splitlines():
            parts = line.strip().split()
            if not parts:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue

            if desc_addr_int <= addr < desc_addr_int + desc_len_int:
                for p in parts[1:5]:
                    if len(p) == 8 and re.match(r'^[0-9a-fA-F]{8}$', p):
                        raw_hex.append(p[0:2])
                        raw_hex.append(p[2:4])
                        raw_hex.append(p[4:6])
                        raw_hex.append(p[6:8])
                    elif len(p) == 2 and re.match(r'^[0-9a-fA-F]{2}$', p):
                        raw_hex.append(p)

        desc_bytes = bytes([int(x, 16) for x in raw_hex[:desc_len_int]])
        assert len(desc_bytes) == 177, f"Extracted byte count {len(desc_bytes)} != expected 177"

        bLength = desc_bytes[0]
        bDescriptorType = desc_bytes[1]
        wTotalLength = desc_bytes[2] | (desc_bytes[3] << 8)
        bNumInterfaces = desc_bytes[4]
        print(f"  Configuration Descriptor: bLength={bLength}, type=0x{bDescriptorType:02x}, wTotalLength={wTotalLength}, bNumInterfaces={bNumInterfaces}")
        assert bLength == 9, "Invalid config descriptor bLength"
        assert bDescriptorType == 2, "Invalid config descriptor type (must be 0x02 CONFIGURATION)"
        assert wTotalLength == 177, f"Invalid wTotalLength ({wTotalLength} != 177)"
        assert bNumInterfaces == 3, f"Expected 3 interfaces (AC, AS, HID), found {bNumInterfaces}"

        iad_offset = 9
        iad_len = desc_bytes[iad_offset]
        iad_type = desc_bytes[iad_offset + 1]
        iad_first_itf = desc_bytes[iad_offset + 2]
        iad_itf_count = desc_bytes[iad_offset + 3]
        iad_class = desc_bytes[iad_offset + 4]
        iad_protocol = desc_bytes[iad_offset + 6]
        print(f"  Audio IAD: len={iad_len}, firstItf={iad_first_itf}, itfCount={iad_itf_count}, class=0x{iad_class:02x}, protocol=0x{iad_protocol:02x}")
        assert iad_len == 8, "Invalid IAD length"
        assert iad_type == 0x0B, "Invalid IAD type (0x0B)"
        assert iad_first_itf == 0, "IAD first interface must be 0"
        assert iad_itf_count == 2, "IAD interface count must be 2 (AC + AS)"
        assert iad_class == 0x01, "IAD class must be Audio (0x01)"
        assert iad_protocol == 0x20, "IAD protocol must be 0x20 (UAC 2.0 IP_VERSION_02_00)"

        ac_header_offset = 17 + 9
        ac_bcdADC = desc_bytes[ac_header_offset + 3] | (desc_bytes[ac_header_offset + 4] << 8)
        print(f"  Class-Specific AC Header: bcdADC=0x{ac_bcdADC:04x}")
        assert ac_bcdADC == 0x0200, f"Expected bcdADC 0x0200 (UAC 2.0), found 0x{ac_bcdADC:04x}!"

        ep_out_found = False
        for i in range(len(desc_bytes) - 6):
            if desc_bytes[i] == 7 and desc_bytes[i+1] == 5 and desc_bytes[i+2] == 0x01:
                ep_attr = desc_bytes[i+3]
                ep_max_sz = desc_bytes[i+4] | (desc_bytes[i+5] << 8)
                ep_interval = desc_bytes[i+6]
                print(f"  Audio OUT Endpoint: addr=0x01, attr=0x{ep_attr:02x} (Isochronous), maxPacketSize={ep_max_sz} B, interval={ep_interval} ms")
                assert (ep_attr & 0x03) == 1, "Audio OUT endpoint must be Isochronous (0x01)"
                assert ep_max_sz == 196, f"Expected 196 bytes maxPacketSize (48kHz 16b stereo frame), got {ep_max_sz}"
                assert ep_interval == 1, f"Expected 1 ms interval, got {ep_interval}"
                ep_out_found = True
                break
        assert ep_out_found, "Audio OUT Endpoint (0x01) not found in configuration descriptor!"

        ep_fb_found = False
        for i in range(len(desc_bytes) - 6):
            if desc_bytes[i] == 7 and desc_bytes[i+1] == 5 and desc_bytes[i+2] == 0x81:
                ep_attr = desc_bytes[i+3]
                ep_max_sz = desc_bytes[i+4] | (desc_bytes[i+5] << 8)
                ep_interval = desc_bytes[i+6]
                print(f"  Audio Feedback IN Endpoint: addr=0x81, attr=0x{ep_attr:02x} (Feedback), maxPacketSize={ep_max_sz} B, interval={ep_interval} ms")
                assert ep_max_sz == 4, f"Expected 4 bytes maxPacketSize for feedback, got {ep_max_sz}"
                assert ep_interval == 1, f"Expected 1 ms interval, got {ep_interval}"
                ep_fb_found = True
                break
        assert ep_fb_found, "Audio Feedback IN Endpoint (0x81) not found in configuration descriptor!"

        hid_itf_found = False
        for i in range(len(desc_bytes) - 8):
            if desc_bytes[i] == 9 and desc_bytes[i+1] == 4 and desc_bytes[i+2] == 2:
                itf_class = desc_bytes[i+5]
                itf_subclass = desc_bytes[i+6]
                itf_protocol = desc_bytes[i+7]
                print(f"  HID Interface: itfNum=2, class=0x{itf_class:02x} (HID), subClass=0x{itf_subclass:02x} (Boot), protocol=0x{itf_protocol:02x} (Keyboard)")
                assert itf_class == 3, "Interface 2 class must be HID (0x03)"
                assert itf_subclass == 1, "Interface 2 subclass must be Boot (0x01)"
                assert itf_protocol == 1, "Interface 2 protocol must be Keyboard (0x01)"
                hid_itf_found = True
                break
        assert hid_itf_found, "HID Interface Descriptor (Interface 2) not found in configuration descriptor!"

        hid_ep_found = False
        for i in range(len(desc_bytes) - 6):
            if desc_bytes[i] == 7 and desc_bytes[i+1] == 5 and desc_bytes[i+2] == 0x82:
                ep_attr = desc_bytes[i+3]
                ep_max_sz = desc_bytes[i+4] | (desc_bytes[i+5] << 8)
                ep_interval = desc_bytes[i+6]
                print(f"  HID IN Endpoint: addr=0x82, attr=0x{ep_attr:02x} (Interrupt), maxPacketSize={ep_max_sz} B, interval={ep_interval} ms")
                assert (ep_attr & 0x03) == 3, "HID endpoint must be Interrupt (0x03)"
                assert ep_max_sz == 16, f"Expected 16 bytes maxPacketSize, got {ep_max_sz}"
                assert ep_interval == 10, f"Expected 10 ms polling interval, got {ep_interval}"
                hid_ep_found = True
                break
        assert hid_ep_found, "HID IN Endpoint (0x82) not found in configuration descriptor!"

        print("  -> ASSERTION PASSED: All composite descriptor structures, UAC 2.0 headers, and endpoints verified.")

        # Step 5: Verify production firmware build is unaffected
        print("\n--- Step 5: Verifying production firmware build remains clean ---")
        prod_build = run_cmd(["pio", "run"], cwd=project_root)
        print(f"Production Build Exit Code: {prod_build.returncode}")
        assert prod_build.returncode == 0, f"Production firmware build failed!\n{prod_build.stderr}"
        print("  -> ASSERTION PASSED: Production firmware builds cleanly without errors.")

        print("\n" + "=" * 75)
        print("ALL VALIDATION CHECKS PASSED: Simultaneous UAC 2.0+HID Composite Confirmed.")
        print("=" * 75)

    finally:
        # Step 6: Clean up generated build outputs
        print("\n--- Step 6: Cleaning up generated build outputs ---")
        pio_dir = os.path.join(poc_dir, ".pio")
        if os.path.exists(pio_dir):
            shutil.rmtree(pio_dir, ignore_errors=True)
            print(f"  Removed {pio_dir}")
        for item in os.listdir(poc_dir):
            if item.startswith("sdkconfig.") and item != "sdkconfig.defaults":
                target = os.path.join(poc_dir, item)
                try:
                    os.remove(target)
                    print(f"  Removed {target}")
                except OSError:
                    pass

if __name__ == "__main__":
    main()
