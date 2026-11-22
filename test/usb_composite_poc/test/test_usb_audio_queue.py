#!/usr/bin/env python3
"""
test_usb_audio_queue.py
Native test runner for the PAXG USB composite audio queue.

Compiles and executes the production C implementation (usb_audio_queue.c and test_usb_audio_queue.c)
directly on the native host via `uvx --from ziglang python-zig cc`.
Exercises true multithreaded races, burst coalescing, wait-free bounded producer handoff,
undersized buffer rejection, and lifecycle generation interleaving.
"""

import os
import sys
import subprocess
import shutil

def main():
    print("=" * 75)
    print("PAXG Native USB Audio Queue Verification Suite Runner")
    print("=" * 75)

    test_dir = os.path.dirname(os.path.abspath(__file__))
    poc_dir = os.path.abspath(os.path.join(test_dir, ".."))
    comp_dir = os.path.join(poc_dir, "components", "usb_device_composite")
    inc_dir = os.path.join(comp_dir, "include")
    src_queue = os.path.join(comp_dir, "usb_audio_queue.c")
    test_c = os.path.join(test_dir, "test_usb_audio_queue.c")
    exe_out = os.path.join(test_dir, "test_usb_audio_queue.exe")

    # Verify compiler availability
    uvx_path = shutil.which("uvx")
    if not uvx_path:
        print("ERROR: uvx is not found on PATH. Cannot invoke python-zig cc.")
        sys.exit(1)

    print(f"Using compiler: uvx --from ziglang python-zig cc")
    compile_cmd = [
        "uvx", "--from", "ziglang", "python-zig", "cc",
        "-DDEBUG", "-UNDEBUG",
        "-Wall", "-Wextra", "-Werror",
        "-I", inc_dir,
        src_queue,
        test_c,
        "-o", exe_out
    ]
    print("Running:", " ".join(compile_cmd))
    c_res = subprocess.run(compile_cmd, capture_output=True, text=True)
    if c_res.returncode != 0:
        print("COMPILATION FAILED:\n", c_res.stderr)
        sys.exit(1)

    print("Compilation SUCCESSFUL. Executing native test binary...")
    print("-" * 75)
    run_res = subprocess.run([exe_out], capture_output=True, text=True)
    print(run_res.stdout)
    if run_res.stderr:
        print(run_res.stderr)

    pdb_out = os.path.join(test_dir, "test_usb_audio_queue.pdb")
    for f in [exe_out, pdb_out]:
        if os.path.exists(f):
            try:
                os.remove(f)
            except Exception:
                pass

    if run_res.returncode != 0:
        print(f"FAILED: Native test exited with code {run_res.returncode}")
        sys.exit(run_res.returncode)

    print("-" * 75)
    print("NATIVE VERIFICATION COMPLETED WITH RETURN CODE 0.")

if __name__ == "__main__":
    main()
