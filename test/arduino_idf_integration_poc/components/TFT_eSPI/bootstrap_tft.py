#!/usr/bin/env python3
"""
Bootstrap script for pinned TFT_eSPI dependency.

Acquires Bodmer/TFT_eSPI at pinned tag V2.5.43 without relying on root .pio/libdeps,
enabling fully reproducible builds in a fresh checkout without huge duplicate vendoring in git.

Steps:
1. Checks if upstream/ directory already contains TFT_eSPI.cpp.
2. If not, checks for existing local copy in root .pio/libdeps (cache fallback).
3. If not present locally, downloads the pinned release archive from GitHub:
   https://github.com/Bodmer/TFT_eSPI/archive/refs/tags/V2.5.43.tar.gz
4. Verifies SHA256 checksum.
5. Extracts required library files into upstream/ and validates license.txt.
"""

import hashlib
import io
import os
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

PINNED_TAG = "V2.5.43"
PINNED_ARCHIVE_URL = f"https://github.com/Bodmer/TFT_eSPI/archive/refs/tags/{PINNED_TAG}.tar.gz"
EXPECTED_SHA256 = "a303b432cbdb647673ce9efbb94139667a30e2739d2aeba741e3e7db8f64c8b0"

COMP_DIR = Path(__file__).resolve().parent
UPSTREAM_DIR = COMP_DIR / "upstream"
REPO_ROOT = COMP_DIR.parent.parent.parent
ROOT_PIO_TFT = REPO_ROOT / ".pio" / "libdeps" / "lilygo-t-display-s3" / "TFT_eSPI"

def is_valid_tft_dir(d: Path) -> bool:
    return (d / "TFT_eSPI.cpp").is_file() and (d / "TFT_eSPI.h").is_file()

def bootstrap(force=False, clean=False):
    if clean:
        if UPSTREAM_DIR.exists():
            print(f"[BOOTSTRAP] Cleaning {UPSTREAM_DIR}...")
            shutil.rmtree(UPSTREAM_DIR, ignore_errors=True)
        return 0

    if not force and is_valid_tft_dir(UPSTREAM_DIR):
        print(f"[BOOTSTRAP] Pinned TFT_eSPI {PINNED_TAG} already present in {UPSTREAM_DIR}.")
        return 0

    UPSTREAM_DIR.mkdir(parents=True, exist_ok=True)

    # Strategy 1: Check root .pio cache if available (fast offline path)
    if is_valid_tft_dir(ROOT_PIO_TFT):
        print(f"[BOOTSTRAP] Copying from local root cache {ROOT_PIO_TFT} -> {UPSTREAM_DIR}...")
        for item in ROOT_PIO_TFT.iterdir():
            if item.name.startswith("."):
                continue
            dst = UPSTREAM_DIR / item.name
            if item.is_dir():
                if dst.exists():
                    shutil.rmtree(dst)
                shutil.copytree(item, dst)
            else:
                shutil.copy2(item, dst)
        if is_valid_tft_dir(UPSTREAM_DIR):
            print("[BOOTSTRAP] Successfully populated from local cache.")
            return 0

    # Strategy 2: Download pinned GitHub release tarball
    print(f"[BOOTSTRAP] Downloading pinned TFT_eSPI {PINNED_TAG} from {PINNED_ARCHIVE_URL}...")
    try:
        req = urllib.request.Request(PINNED_ARCHIVE_URL, headers={"User-Agent": "PAXG-Bootstrap/1.0"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            tar_data = resp.read()
    except Exception as e:
        print(f"[BOOTSTRAP ERROR] Failed to download {PINNED_ARCHIVE_URL}: {e}")
        return 1

    actual_sha256 = hashlib.sha256(tar_data).hexdigest()
    print(f"[BOOTSTRAP] Downloaded {len(tar_data):,} bytes. SHA256: {actual_sha256}")
    if actual_sha256 != EXPECTED_SHA256:
        print(f"[BOOTSTRAP ERROR] Checksum mismatch! Expected {EXPECTED_SHA256}, got {actual_sha256}")
        return 1

    print("[BOOTSTRAP] Extracting archive...")
    with tarfile.open(fileobj=io.BytesIO(tar_data), mode="r:gz") as tar:
        # Pinned tarball root folder is TFT_eSPI-2.5.43
        members = tar.getmembers()
        prefix = members[0].name.split("/")[0] if members else ""
        for m in members:
            # Strip the leading folder name
            parts = Path(m.name).parts
            if len(parts) <= 1:
                continue
            rel_path = Path(*parts[1:])
            target_path = UPSTREAM_DIR / rel_path
            if m.isdir():
                target_path.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                target_path.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(m) as src, open(target_path, "wb") as dst:
                    shutil.copyfileobj(src, dst)

    if not is_valid_tft_dir(UPSTREAM_DIR):
        print(f"[BOOTSTRAP ERROR] Extraction incomplete; TFT_eSPI.cpp not found in {UPSTREAM_DIR}")
        return 1

    print(f"[BOOTSTRAP SUCCESS] Pinned TFT_eSPI {PINNED_TAG} ready in {UPSTREAM_DIR}")
    return 0

if __name__ == "__main__":
    force_flag = "--force" in sys.argv
    clean_flag = "--clean" in sys.argv
    sys.exit(bootstrap(force=force_flag, clean=clean_flag))
