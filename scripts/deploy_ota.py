#!/usr/bin/env python3
"""Windows-compatible Python 3 OTA deploy client for ESP32-S3.

Integrated PlatformIO project: test/arduino_idf_integration_poc
Standard library only (no external dependencies).
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time
from typing import Any, Callable, Dict, Optional, Tuple

# Constants
DEFAULT_PORT = 8080
DEFAULT_ENV = "esp32s3_arduino_idf_poc"
DEFAULT_TIMEOUT = 60.0
POLL_INTERVAL = 2.0

ESP_IMAGE_MAGIC = 0xE9
ESP32_S3_CHIP_ID = 9
ESP_APP_DESC_MAGIC = 0xABCD5432
EXPECTED_PROJECT_NAME = "esp32s3_arduino_idf_poc"
MAX_FIRMWARE_SIZE = 0x640000  # 6,553,600 bytes (inactive app partition capacity)
REQUIRED_VERSION_PREFIX = "ota1-"

# esp_app_desc_t offsets within binary:
# Binary offset 0: ESP image header (24 bytes)
# Binary offset 24: Segment 0 header (8 bytes)
# Binary offset 32: esp_app_desc_t (256 bytes)
OFFSET_APP_DESC = 32
OFFSET_MAGIC = 0
OFFSET_CHIP_ID = 12
OFFSET_APP_MAGIC = OFFSET_APP_DESC + 0
OFFSET_SECURE_VERSION = OFFSET_APP_DESC + 4
OFFSET_VERSION = OFFSET_APP_DESC + 16       # offset 48
OFFSET_PROJECT_NAME = OFFSET_APP_DESC + 48   # offset 80
OFFSET_TIME = OFFSET_APP_DESC + 80           # offset 112
OFFSET_DATE = OFFSET_APP_DESC + 96           # offset 128
OFFSET_IDF_VER = OFFSET_APP_DESC + 112       # offset 144
OFFSET_APP_ELF_SHA256 = OFFSET_APP_DESC + 144 # offset 176 (32 bytes)


class FirmwareInfo:
    """Holds parsed and validated ESP32-S3 firmware metadata."""

    def __init__(
        self,
        path: Path,
        size: int,
        sha256: str,
        chip_id: int,
        version: str,
        project_name: str,
        elf_sha256: str,
        compile_time: str,
        compile_date: str,
        idf_ver: str,
    ) -> None:
        self.path = path
        self.size = size
        self.sha256 = sha256
        self.chip_id = chip_id
        self.version = version
        self.project_name = project_name
        self.elf_sha256 = elf_sha256
        self.compile_time = compile_time
        self.compile_date = compile_date
        self.idf_ver = idf_ver

    def __str__(self) -> str:
        return (
            f"Firmware: {self.path.name} ({self.size:,} bytes)\n"
            f"  SHA-256:      {self.sha256}\n"
            f"  Chip ID:      {self.chip_id} (ESP32-S3)\n"
            f"  Project Name: {self.project_name}\n"
            f"  Version:      {self.version}\n"
            f"  ELF SHA-256:  {self.elf_sha256}\n"
            f"  IDF Version:  {self.idf_ver}\n"
            f"  Built:        {self.compile_date} {self.compile_time}"
        )


def parse_and_validate_firmware(
    path: Path, expected_project: str = EXPECTED_PROJECT_NAME
) -> FirmwareInfo:
    """Read and validate ESP32-S3 firmware image headers and esp_app_desc_t."""
    if not path.is_file():
        raise FileNotFoundError(f"Firmware file not found: {path}")

    size = path.stat().st_size
    if size < 256:
        raise ValueError(f"Firmware file too small ({size} bytes, minimum 256 required)")
    if size > MAX_FIRMWARE_SIZE:
        raise ValueError(
            f"Firmware file too large ({size:,} bytes, exceeds 0x{MAX_FIRMWARE_SIZE:x} / {MAX_FIRMWARE_SIZE:,} bytes)"
        )

    # Read only prefix header into memory; hash file in streamed chunks
    hasher = hashlib.sha256()
    with open(path, "rb") as f:
        header = f.read(256)
        if len(header) < 256:
            raise ValueError(f"Failed to read 256-byte header from {path}")
        hasher.update(header)
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            hasher.update(chunk)

    # Offset 0: ESP image magic
    magic = header[OFFSET_MAGIC]
    if magic != ESP_IMAGE_MAGIC:
        raise ValueError(
            f"Invalid ESP image magic: 0x{magic:02x} (expected 0x{ESP_IMAGE_MAGIC:02x})"
        )

    # Offset 12: Chip ID (uint16 little-endian)
    chip_id = struct.unpack("<H", header[OFFSET_CHIP_ID : OFFSET_CHIP_ID + 2])[0]
    if chip_id != ESP32_S3_CHIP_ID:
        raise ValueError(
            f"Invalid chip ID: {chip_id} (expected {ESP32_S3_CHIP_ID} for ESP32-S3)"
        )

    # Offset 32: esp_app_desc magic word (0xABCD5432 uint32 little-endian)
    app_magic = struct.unpack("<I", header[OFFSET_APP_MAGIC : OFFSET_APP_MAGIC + 4])[0]
    if app_magic != ESP_APP_DESC_MAGIC:
        raise ValueError(
            f"Invalid esp_app_desc magic: 0x{app_magic:08x} "
            f"(expected 0x{ESP_APP_DESC_MAGIC:08x})"
        )

    # Offset 48..79: version char[32]
    raw_version = header[OFFSET_VERSION : OFFSET_VERSION + 32]
    version = raw_version.split(b"\x00")[0].decode("utf-8", errors="replace").strip()
    if not version.startswith(REQUIRED_VERSION_PREFIX):
        raise ValueError(
            f"Legacy non-OTA image rejected: version '{version}' does not start with '{REQUIRED_VERSION_PREFIX}'"
        )

    # Offset 80..111: project_name char[32]
    raw_project = header[OFFSET_PROJECT_NAME : OFFSET_PROJECT_NAME + 32]
    project_name = raw_project.split(b"\x00")[0].decode("utf-8", errors="replace").strip()
    if expected_project and project_name != expected_project:
        raise ValueError(
            f"Project name mismatch: '{project_name}' (expected '{expected_project}')"
        )

    # Offset 112..127: time char[16]
    compile_time = (
        header[OFFSET_TIME : OFFSET_TIME + 16]
        .split(b"\x00")[0]
        .decode("utf-8", errors="replace")
        .strip()
    )

    # Offset 128..143: date char[16]
    compile_date = (
        header[OFFSET_DATE : OFFSET_DATE + 16]
        .split(b"\x00")[0]
        .decode("utf-8", errors="replace")
        .strip()
    )

    # Offset 144..175: idf_ver char[32]
    idf_ver = (
        header[OFFSET_IDF_VER : OFFSET_IDF_VER + 32]
        .split(b"\x00")[0]
        .decode("utf-8", errors="replace")
        .strip()
    )

    # Offset 176..207: app_elf_sha256 uint8_t[32]
    elf_sha256_bytes = header[OFFSET_APP_ELF_SHA256 : OFFSET_APP_ELF_SHA256 + 32]
    elf_sha256 = elf_sha256_bytes.hex().lower()

    sha256 = hasher.hexdigest().lower()

    return FirmwareInfo(
        path=path,
        size=size,
        sha256=sha256,
        chip_id=chip_id,
        version=version,
        project_name=project_name,
        elf_sha256=elf_sha256,
        compile_time=compile_time,
        compile_date=compile_date,
        idf_ver=idf_ver,
    )


def validate_and_resolve_host(host: str) -> str:
    """Validate target host as private/loopback/link-local IPv4. Disallow mDNS and public IPs."""
    host = host.strip()
    if host.startswith("http://") or host.startswith("https://"):
        # Strip scheme if user accidentally passed full URL
        host = host.split("://", 1)[1]
    if "/" in host:
        host = host.split("/", 1)[0]
    if ":" in host:
        # Host contains port, strip it
        host = host.split(":", 1)[0]

    if not host:
        raise ValueError("Host cannot be empty")

    # Reject mDNS discovery
    if host.lower().endswith(".local"):
        raise ValueError(
            f"mDNS hostnames (*.local) are not permitted: '{host}'. "
            f"Specify a direct private IPv4 address."
        )

    # Parse IP or resolve hostname
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        try:
            # Resolve hostname to IPv4
            resolved_ip = socket.gethostbyname(host)
            ip = ipaddress.ip_address(resolved_ip)
        except Exception as e:
            raise ValueError(f"Cannot resolve host '{host}' to IPv4 address: {e}")

    if ip.version != 4:
        raise ValueError(f"Only IPv4 addresses are supported: {ip}")

    # Enforce private/internal/loopback/link-local IPv4 only
    if not (ip.is_private or ip.is_loopback or ip.is_link_local):
        raise ValueError(
            f"Host '{host}' resolves to public IP '{ip}'. "
            f"Deployment is strictly restricted to private/internal IPv4 addresses."
        )

    return str(ip)


def get_repo_and_project_dirs() -> Tuple[Path, Path]:
    """Find repository root and integrated PlatformIO project dir."""
    script_path = Path(__file__).resolve()
    repo_root = script_path.parent.parent
    project_dir = repo_root / "test" / "arduino_idf_integration_poc"
    if not project_dir.is_dir():
        # Fallback to current working directory checking
        cwd = Path.cwd().resolve()
        candidate = cwd / "test" / "arduino_idf_integration_poc"
        if candidate.is_dir():
            project_dir = candidate
            repo_root = cwd
    return repo_root, project_dir


def load_config_defaults(repo_root: Path, project_dir: Path) -> Dict[str, Any]:
    """Read optional ota-deploy.local.ini and platformio.ini for defaults."""
    config: Dict[str, Any] = {
        "host": None,
        "port": DEFAULT_PORT,
        "env": DEFAULT_ENV,
        "expected_device_id": None,
        "timeout": DEFAULT_TIMEOUT,
        "firmware": None,
    }

    # 1. Read test project platformio.ini or root platformio.ini custom options
    for pio_ini in [project_dir / "platformio.ini", repo_root / "platformio.ini"]:
        if pio_ini.is_file():
            cp = configparser.ConfigParser()
            try:
                cp.read(str(pio_ini), encoding="utf-8")
                for section in cp.sections():
                    if cp.has_option(section, "custom_ota_host"):
                        config["host"] = cp.get(section, "custom_ota_host")
                    if cp.has_option(section, "custom_ota_port"):
                        config["port"] = cp.getint(section, "custom_ota_port")
                    if cp.has_option(section, "custom_ota_env"):
                        config["env"] = cp.get(section, "custom_ota_env")
                    if cp.has_option(section, "custom_ota_device_id"):
                        config["expected_device_id"] = cp.get(section, "custom_ota_device_id")
                    if cp.has_option(section, "custom_expected_device_id"):
                        config["expected_device_id"] = cp.get(section, "custom_expected_device_id")
            except Exception:
                pass

    # 2. Read optional root ota-deploy.local.ini [ota]
    local_ini = repo_root / "ota-deploy.local.ini"
    if local_ini.is_file():
        cp = configparser.ConfigParser()
        try:
            cp.read(str(local_ini), encoding="utf-8")
            if cp.has_section("ota"):
                if cp.has_option("ota", "host"):
                    config["host"] = cp.get("ota", "host")
                if cp.has_option("ota", "port"):
                    config["port"] = cp.getint("ota", "port")
                if cp.has_option("ota", "env"):
                    config["env"] = cp.get("ota", "env")
                if cp.has_option("ota", "expected_device_id"):
                    config["expected_device_id"] = cp.get("ota", "expected_device_id")
                elif cp.has_option("ota", "device_id"):
                    config["expected_device_id"] = cp.get("ota", "device_id")
                if cp.has_option("ota", "timeout"):
                    config["timeout"] = cp.getfloat("ota", "timeout")
                if cp.has_option("ota", "firmware"):
                    config["firmware"] = cp.get("ota", "firmware")
        except Exception:
            pass

    # 3. Environment variables (higher precedence than ini files)
    if os.environ.get("PAXG_OTA_HOST"):
        config["host"] = os.environ.get("PAXG_OTA_HOST")
    if os.environ.get("PAXG_OTA_PORT"):
        try:
            config["port"] = int(os.environ.get("PAXG_OTA_PORT", ""))
        except ValueError:
            pass
    if os.environ.get("PAXG_OTA_ENV"):
        config["env"] = os.environ.get("PAXG_OTA_ENV")
    if os.environ.get("PAXG_OTA_DEVICE_ID"):
        config["expected_device_id"] = os.environ.get("PAXG_OTA_DEVICE_ID")
    if os.environ.get("PAXG_OTA_TIMEOUT"):
        try:
            config["timeout"] = float(os.environ.get("PAXG_OTA_TIMEOUT", ""))
        except ValueError:
            pass

    return config


class OTAClient:
    """Handles HTTP wire contract with the ESP32-S3 OTA web server."""

    def __init__(self, host: str, port: int, token: str, timeout: float = 10.0) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout

    def _make_connection(self, req_timeout: Optional[float] = None) -> http.client.HTTPConnection:
        t = req_timeout if req_timeout is not None else self.timeout
        return http.client.HTTPConnection(self.host, self.port, timeout=t)

    def get_json(self, path: str, req_timeout: Optional[float] = None) -> Tuple[int, Dict[str, Any]]:
        """Perform authenticated GET request and parse JSON response. Disallows redirects."""
        conn = self._make_connection(req_timeout)
        headers = {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/json",
            "Connection": "close",
            "User-Agent": "PAXG-OTA-Deploy/1.0",
        }
        try:
            conn.request("GET", path, headers=headers)
            res = conn.getresponse()
            if res.status in (301, 302, 303, 307, 308):
                raise RuntimeError(
                    f"HTTP redirect ({res.status}) received for GET {path}. Redirects are strictly disallowed."
                )
            # Bounded read to avoid memory exhaustion or malformed floods
            raw_chunk = res.read(8192)
            body = raw_chunk.decode("utf-8", errors="replace")
            try:
                data = json.loads(body) if body else {}
                if not isinstance(data, dict):
                    data = {"response": str(data)[:200]}
            except Exception:
                # Sanitized bounded summary, never echoing verbatim bodies
                data = {"message": f"HTTP {res.status} response received"}
            return res.status, data
        finally:
            conn.close()

    def get_version(self, req_timeout: Optional[float] = None) -> Tuple[int, Dict[str, Any]]:
        return self.get_json("/api/version", req_timeout=req_timeout)

    def get_health(self, req_timeout: Optional[float] = None) -> Tuple[int, Dict[str, Any]]:
        return self.get_json("/api/health", req_timeout=req_timeout)

    def post_update(
        self,
        firmware_path: Path,
        sha256_hex: str,
        file_size: int,
        progress_cb: Optional[Callable[[int, int], None]] = None,
        req_timeout: float = 60.0,
    ) -> Tuple[int, Dict[str, Any]]:
        """Stream raw binary upload via POST /api/update with Content-Length and X-Firmware-SHA256."""
        conn = self._make_connection(req_timeout)
        try:
            conn.putrequest("POST", "/api/update")
            conn.putheader("Authorization", f"Bearer {self.token}")
            conn.putheader("Content-Type", "application/octet-stream")
            conn.putheader("Content-Length", str(file_size))
            conn.putheader("X-Firmware-SHA256", sha256_hex)
            conn.putheader("Connection", "close")
            conn.putheader("User-Agent", "PAXG-OTA-Deploy/1.0")
            conn.endheaders()

            chunk_size = 65536
            sent = 0
            with open(firmware_path, "rb") as f:
                while True:
                    chunk = f.read(chunk_size)
                    if not chunk:
                        break
                    conn.send(chunk)
                    sent += len(chunk)
                    if progress_cb:
                        progress_cb(sent, file_size)

            res = conn.getresponse()
            if res.status in (301, 302, 303, 307, 308):
                raise RuntimeError(
                    f"HTTP redirect ({res.status}) received for POST /api/update. "
                    f"Redirects are strictly disallowed."
                )
            body = res.read().decode("utf-8", errors="replace")
            try:
                data = json.loads(body) if body else {}
            except json.JSONDecodeError:
                data = {"raw_body": body}
            return res.status, data
        finally:
            conn.close()


def request_with_retry(
    fn: Callable[[], Tuple[int, Dict[str, Any]]],
    retries: int = 3,
    delay: float = 0.25,
) -> Tuple[int, Dict[str, Any]]:
    """Execute read-only HTTP request with bounded retry for transient transport errors."""
    last_err: Optional[Exception] = None
    for attempt in range(retries):
        try:
            return fn()
        except (socket.error, http.client.HTTPException, ConnectionResetError, ConnectionRefusedError, OSError) as e:
            last_err = e
            if attempt < retries - 1:
                time.sleep(delay)
    if last_err:
        raise last_err
    raise RuntimeError("Request failed without error")


def print_progress(uploaded: int, total: int) -> None:
    """Print streaming upload progress bar."""
    pct = (uploaded / total * 100.0) if total > 0 else 0.0
    bar_width = 30
    filled = int(bar_width * uploaded // total) if total > 0 else 0
    bar = "=" * filled + ">" if filled < bar_width else "=" * bar_width
    bar = bar[:bar_width].ljust(bar_width)
    sys.stdout.write(
        f"\rUploading: [{bar}] {pct:5.1f}% ({uploaded:,} / {total:,} bytes)"
    )
    sys.stdout.flush()
    if uploaded >= total:
        sys.stdout.write("\n")
        sys.stdout.flush()


def run_platformio_build(project_dir: Path, env: str) -> Path:
    """Execute PlatformIO build for integrated project."""
    pio_executable = os.environ.get("PAXG_PIO_EXECUTABLE")
    cmd = []
    if pio_executable:
        if not Path(pio_executable).is_file():
            raise FileNotFoundError(
                f"PAXG_PIO_EXECUTABLE override not found: {pio_executable}"
            )
        cmd = [pio_executable, "run", "-d", str(project_dir), "-e", env]
    elif shutil.which("pio"):
        cmd = [shutil.which("pio"), "run", "-d", str(project_dir), "-e", env]
    elif shutil.which("platformio"):
        cmd = [shutil.which("platformio"), "run", "-d", str(project_dir), "-e", env]
    else:
        cmd = [sys.executable, "-m", "platformio", "run", "-d", str(project_dir), "-e", env]

    print(f"Building firmware via: {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, cwd=str(project_dir))
    if result.returncode != 0:
        raise RuntimeError(f"PlatformIO build failed with returncode {result.returncode}")

    bin_path = project_dir / ".pio" / "build" / env / "firmware.bin"
    if not bin_path.is_file():
        raise FileNotFoundError(f"Expected firmware binary not found after build: {bin_path}")
    return bin_path


def verify_device_reboot_and_health(
    client: OTAClient,
    initial_boot_id: str,
    initial_partition: Optional[str],
    expected_firmware: FirmwareInfo,
    expected_device_id: Optional[str],
    timeout: float,
) -> bool:
    """Poll device until timeout, ensuring clean reboot into new image with verified identity and health."""
    start_time = time.time()
    deadline = start_time + timeout
    print(f"Waiting for device to restart and verify new firmware (timeout: {timeout:.0f}s)...")

    # Give device brief window to trigger reboot
    initial_wait = min(1.0, timeout / 5.0)
    time.sleep(initial_wait)
    poll_sleep = max(0.5, min(POLL_INTERVAL, timeout / 10.0))

    reboot_detected = False

    while time.time() < deadline:
        elapsed = time.time() - start_time
        try:
            # 1. Check health
            h_status, h_data = client.get_health(req_timeout=4.0)
            if h_status != 200:
                time.sleep(poll_sleep)
                continue

            current_boot_id = str(h_data.get("boot_id", "")).strip()
            current_dev_id = str(h_data.get("device_id", "")).strip()

            # 2. Check if device has rebooted with different boot_id
            if current_boot_id == initial_boot_id:
                # Still running previous boot
                time.sleep(poll_sleep)
                continue

            reboot_detected = True

            # Strict health check on new boot
            if h_data.get("healthy") is not True:
                raise RuntimeError(
                    f"Device restarted with new boot_id '{current_boot_id}' but reported unhealthy! (/api/health)"
                )

            # 3. Check /api/version for full partition & descriptor verification
            v_status, v_data = client.get_version(req_timeout=4.0)
            if v_status != 200:
                time.sleep(poll_sleep)
                continue

            v_dev_id = str(v_data.get("device_id", "")).strip()
            v_boot_id = str(v_data.get("boot_id", "")).strip()
            v_proj = str(v_data.get("project_name", "")).strip()
            v_ver = str(v_data.get("version", "")).strip()
            v_elf_sha = str(v_data.get("elf_sha256", "")).lower().strip()
            v_slot = str(v_data.get("active_partition", "")).strip()

            # Cross-endpoint consistency check: version and health must agree
            if v_dev_id != current_dev_id:
                raise RuntimeError(
                    f"Post-reboot endpoint mismatch: /api/version device_id '{v_dev_id}' != /api/health device_id '{current_dev_id}'"
                )
            if v_boot_id != current_boot_id:
                raise RuntimeError(
                    f"Post-reboot endpoint mismatch: /api/version boot_id '{v_boot_id}' != /api/health boot_id '{current_boot_id}'"
                )

            # Active partition must be a valid app slot
            if v_slot not in ("app0", "app1"):
                raise RuntimeError(f"Post-reboot invalid active partition '{v_slot}', expected 'app0' or 'app1'")

            # Identity verification
            if expected_device_id and v_dev_id.lower() != expected_device_id.lower():
                raise RuntimeError(
                    f"Post-reboot device ID mismatch: expected '{expected_device_id}', got '{v_dev_id}'"
                )

            # Project name verification
            if v_proj != expected_firmware.project_name:
                raise RuntimeError(
                    f"Post-reboot project mismatch: expected '{expected_firmware.project_name}', got '{v_proj}'"
                )

            # Version verification
            if v_ver != expected_firmware.version:
                raise RuntimeError(
                    f"Post-reboot version mismatch (rollback suspected!): expected '{expected_firmware.version}', got '{v_ver}'"
                )

            # ELF SHA256 verification
            if v_elf_sha != expected_firmware.elf_sha256:
                raise RuntimeError(
                    f"Post-reboot ELF SHA-256 mismatch (rollback suspected!):\n"
                    f"  Expected: {expected_firmware.elf_sha256}\n"
                    f"  Active:   {v_elf_sha}"
                )

            # Switched slot verification (active partition must differ if initial was known)
            if initial_partition and v_slot:
                if v_slot == initial_partition:
                    raise RuntimeError(
                        f"Post-reboot partition mismatch (rollback suspected!): "
                        f"active partition is still '{v_slot}', expected switch away from '{initial_partition}'"
                    )

            # All 7 criteria verified successfully!
            print(
                f"\n[SUCCESS] OTA update verified in {elapsed:.1f}s!\n"
                f"  Device ID:        {v_dev_id}\n"
                f"  Boot ID:          {v_boot_id} (switched from {initial_boot_id})\n"
                f"  Active Partition: {v_slot} (switched from {initial_partition or 'unknown'})\n"
                f"  Project:          {v_proj}\n"
                f"  Version:          {v_ver}\n"
                f"  ELF SHA-256:      {v_elf_sha}\n"
                f"  Health:           Healthy (true)"
            )
            return True

        except (socket.error, http.client.HTTPException, TimeoutError):
            # Normal connection drop while device reboots
            time.sleep(POLL_INTERVAL)
        except RuntimeError:
            raise

    if reboot_detected:
        raise TimeoutError(
            f"Device rebooted but failed to reach healthy verified state within {timeout:.0f}s"
        )
    raise TimeoutError(
        f"Device did not reboot within {timeout:.0f}s (boot_id remained unchanged)"
    )


def build_arg_parser() -> argparse.ArgumentParser:
    """Create command line argument parser with full help descriptions."""
    parser = argparse.ArgumentParser(
        description="Deploy firmware to ESP32-S3 over local network via HTTP OTA API.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--host",
        type=str,
        default=None,
        help="Target device private IPv4 address (e.g. 192.168.1.50). mDNS (*.local) and public IPs are rejected.",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help="Target device HTTP OTA port.",
    )
    parser.add_argument(
        "--env",
        type=str,
        default=DEFAULT_ENV,
        help="PlatformIO environment name.",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="Execute PlatformIO build before deploying. In mutation flow, identity is checked before build.",
    )
    parser.add_argument(
        "--firmware",
        type=str,
        default=None,
        help="Path to firmware .bin file. Defaults to .pio/build/<env>/firmware.bin.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help="Reboot and verification timeout in seconds.",
    )
    parser.add_argument(
        "--status-only",
        action="store_true",
        help="Query and display device version and health status only (no build, no update).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Local dry-run validation (validates binary, compatibility, and parameters without upload). Can run offline without --host.",
    )
    parser.add_argument(
        "--device-id",
        type=str,
        default=None,
        help="Expected stable hardware device ID pin. Aborts if device reports a different identity.",
    )
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    repo_root, project_dir = get_repo_and_project_dirs()
    ini_config = load_config_defaults(repo_root, project_dir)

    # Resolve parameters with precedence: CLI > Env > INI > Default
    host = args.host or ini_config.get("host")
    port = args.port if args.port != DEFAULT_PORT else (ini_config.get("port") or DEFAULT_PORT)
    env = args.env if args.env != DEFAULT_ENV else (ini_config.get("env") or DEFAULT_ENV)
    timeout = args.timeout if args.timeout != DEFAULT_TIMEOUT else (ini_config.get("timeout") or DEFAULT_TIMEOUT)
    expected_device_id = args.device_id or ini_config.get("expected_device_id")

    firmware_arg = args.firmware or ini_config.get("firmware")
    if firmware_arg:
        firmware_path = Path(firmware_arg)
        if not firmware_path.is_absolute():
            firmware_path = (repo_root / firmware_path).resolve()
    else:
        firmware_path = project_dir / ".pio" / "build" / env / "firmware.bin"

    # Credentials: read from PAXG_OTA_TOKEN, NEVER echo this token
    token = os.environ.get("PAXG_OTA_TOKEN", "").strip()

    print("==================================================")
    print("PAXG-Ticker ESP32-S3 OTA Deploy Client")
    print("==================================================")
    print(f"Project Directory:   {project_dir}")
    print(f"Target Environment:  {env}")
    print(f"Target Device Host:  {host or '(none - offline)'}:{port}")
    print(f"Target Device ID Pin:{expected_device_id or '(none)'}")
    print(f"Firmware Binary:     {firmware_path}")
    print(f"Token Configured:    {'Yes' if token else 'No'}")
    print(f"Mode:                {'Status-Only' if args.status_only else 'Dry-Run' if args.dry_run else 'Deploy'}")
    print("==================================================")

    # Reject contradictory CLI mode / build combinations
    if args.status_only and (args.dry_run or args.build):
        print("Error: --status-only cannot be combined with --dry-run or --build.", file=sys.stderr)
        return 1

    # 1. Status-only mode
    if args.status_only:
        if not host:
            print("Error: --host is required for --status-only", file=sys.stderr)
            return 1
        if not token:
            print("Error: PAXG_OTA_TOKEN environment variable is required for device communication.", file=sys.stderr)
            return 1
        try:
            validated_ip = validate_and_resolve_host(host)
        except ValueError as e:
            print(f"Error: Host validation failed: {e}", file=sys.stderr)
            return 1

        client = OTAClient(validated_ip, port, token, timeout=10.0)
        try:
            v_status, v_data = request_with_retry(lambda: client.get_version(req_timeout=6.0))
            h_status, h_data = request_with_retry(lambda: client.get_health(req_timeout=4.0))
        except Exception as e:
            print(f"Error connecting to device: {e}", file=sys.stderr)
            return 1

        if v_status != 200:
            print(f"Error: /api/version returned HTTP {v_status}: {v_data}", file=sys.stderr)
            return 1

        dev_id = str(v_data.get("device_id", ""))
        print("\n[Device Version Info]")
        print(f"  Device ID:        {dev_id}")
        print(f"  Boot ID:          {v_data.get('boot_id')}")
        print(f"  Chip ID:          {v_data.get('chip_id')} ({'ESP32-S3' if v_data.get('chip_id') == 9 else 'Unknown'})")
        print(f"  Project Name:     {v_data.get('project_name')}")
        print(f"  Version:          {v_data.get('version')}")
        print(f"  Git Commit:       {v_data.get('git_commit')}")
        print(f"  ELF SHA-256:      {v_data.get('elf_sha256')}")
        print(f"  Active Partition: {v_data.get('active_partition')}")
        print(f"  OTA Max Size:     {v_data.get('ota_max_size'):,} bytes")

        print("\n[Device Health Info]")
        healthy = h_data.get("healthy", False)
        print(f"  Healthy:          {'Yes' if healthy else 'No'}")
        for k, v in h_data.items():
            if k not in ("healthy", "device_id", "boot_id"):
                print(f"  {k}: {v}")

        if expected_device_id and dev_id.lower() != expected_device_id.lower():
            print(
                f"\nWarning: Device ID '{dev_id}' does NOT match pinned expected ID '{expected_device_id}'!",
                file=sys.stderr,
            )
            return 1

        return 0 if (healthy and (not expected_device_id or dev_id.lower() == expected_device_id.lower())) else 1

    # 2. Local Dry-Run mode
    if args.dry_run:
        print("\nExecuting Local Dry-Run...")
        device_version_info: Optional[Dict[str, Any]] = None
        device_ota_max_size: Optional[int] = None

        # Optional online check during dry run if host and token are provided
        if host:
            try:
                validated_ip = validate_and_resolve_host(host)
                if token:
                    client = OTAClient(validated_ip, port, token, timeout=10.0)
                    v_status, v_data = client.get_version()
                    if v_status == 200:
                        device_version_info = v_data
                        device_ota_max_size = v_data.get("ota_max_size")
                        reported_id = str(v_data.get("device_id", ""))
                        print(f"Online Check: Connected to {validated_ip}:{port} (Device ID: {reported_id})")
                        if expected_device_id and reported_id.lower() != expected_device_id.lower():
                            print(
                                f"Error: Device ID mismatch in dry-run: expected '{expected_device_id}', got '{reported_id}'",
                                file=sys.stderr,
                            )
                            return 1
            except Exception as e:
                print(f"Notice: Host check skipped or failed in dry-run: {e}")

        # If --build specified in dry-run, execute build
        if args.build:
            try:
                firmware_path = run_platformio_build(project_dir, env)
            except Exception as e:
                print(f"Error during dry-run build: {e}", file=sys.stderr)
                return 1

        # Validate firmware binary offline
        try:
            fw_info = parse_and_validate_firmware(firmware_path, EXPECTED_PROJECT_NAME)
        except Exception as e:
            print(f"Error: Firmware binary validation failed: {e}", file=sys.stderr)
            return 1

        print("\n[Firmware Binary Validation Succeeded]")
        print(fw_info)

        if device_ota_max_size is not None:
            if fw_info.size > device_ota_max_size:
                print(
                    f"Error: Firmware size ({fw_info.size:,} bytes) exceeds device inactive OTA partition capacity ({device_ota_max_size:,} bytes)",
                    file=sys.stderr,
                )
                return 1
            print(f"Capacity Check: Fits in inactive partition ({fw_info.size:,} / {device_ota_max_size:,} bytes)")

        if device_version_info is not None:
            dev_proj = device_version_info.get("project_name")
            if dev_proj and dev_proj != fw_info.project_name:
                print(
                    f"Error: Incompatible project: device running '{dev_proj}', binary is '{fw_info.project_name}'",
                    file=sys.stderr,
                )
                return 1

        print("\nDry-run completed successfully. No firmware was uploaded.")
        return 0

    # 3. Live Deployment flow
    if not host:
        print("Error: --host is required for firmware deployment (use --dry-run for offline check).", file=sys.stderr)
        return 1
    if not token:
        print("Error: PAXG_OTA_TOKEN environment variable is required for device authentication.", file=sys.stderr)
        return 1

    # Enforce configured pinned device ID before any live mutating flow
    if not expected_device_id:
        print(
            "Error: Configured pinned device ID (--device-id, PAXG_OTA_DEVICE_ID, or ota-deploy.local.ini) "
            "is strictly required before any live mutating deployment.",
            file=sys.stderr,
        )
        return 1

    try:
        validated_ip = validate_and_resolve_host(host)
    except ValueError as e:
        print(f"Error: Host validation failed: {e}", file=sys.stderr)
        return 1

    client = OTAClient(validated_ip, port, token, timeout=10.0)

    # Phase 1: Identity & Status check BEFORE build (enforce identity pin on mutation)
    print(f"\n[Phase 1: Device Identity & Compatibility Check]")
    print(f"Connecting to http://{validated_ip}:{port}/api/version and /api/health...")
    try:
        v_status, v_data = request_with_retry(lambda: client.get_version(req_timeout=6.0))
        h_status, h_data = request_with_retry(lambda: client.get_health(req_timeout=4.0))
    except Exception as e:
        print(f"Error connecting to device: {e}", file=sys.stderr)
        return 1

    if v_status != 200:
        print(f"Error: /api/version returned HTTP {v_status}", file=sys.stderr)
        return 1
    if h_status != 200:
        print(f"Error: /api/health returned HTTP {h_status}", file=sys.stderr)
        return 1

    reported_device_id = str(v_data.get("device_id", "")).strip()
    initial_boot_id = str(v_data.get("boot_id", "")).strip()
    initial_partition = str(v_data.get("active_partition", "")).strip()
    ota_max_size = v_data.get("ota_max_size")
    reported_chip_id = v_data.get("chip_id")
    device_project = str(v_data.get("project_name", "")).strip()

    h_dev_id = str(h_data.get("device_id", "")).strip()
    h_boot_id = str(h_data.get("boot_id", "")).strip()

    # Pre-deploy checks: cross-endpoint consistency
    if reported_device_id != h_dev_id:
        print(
            f"Error: Pre-deploy endpoint mismatch: /api/version device_id '{reported_device_id}' != /api/health device_id '{h_dev_id}'",
            file=sys.stderr,
        )
        return 1
    if initial_boot_id != h_boot_id:
        print(
            f"Error: Pre-deploy endpoint mismatch: /api/version boot_id '{initial_boot_id}' != /api/health boot_id '{h_boot_id}'",
            file=sys.stderr,
        )
        return 1

    # Pre-deploy check: healthy strictly True
    if h_data.get("healthy") is not True:
        print("Error: Device reported pre-deploy health status is not strictly true.", file=sys.stderr)
        return 1

    # Pre-deploy check: current active_partition must be app0 or app1
    if initial_partition not in ("app0", "app1"):
        print(
            f"Error: Invalid active partition '{initial_partition}', expected 'app0' or 'app1'",
            file=sys.stderr,
        )
        return 1

    # Pre-deploy check: ota_max_size positive integer
    if not isinstance(ota_max_size, int) or ota_max_size <= 0:
        print(
            f"Error: Device reported invalid or non-positive ota_max_size: {ota_max_size}",
            file=sys.stderr,
        )
        return 1

    print(f"  Target Device ID:     {reported_device_id}")
    print(f"  Current Boot ID:      {initial_boot_id}")
    print(f"  Active Partition:     {initial_partition}")
    print(f"  OTA Inactive Slot:    {ota_max_size:,} bytes")
    print(f"  Running Project:      {device_project}")
    print(f"  Running Version:      {v_data.get('version')}")

    # Enforce identity pin
    if reported_device_id.lower() != expected_device_id.lower():
        print(
            f"\nError: Device ID mismatch! Pinned ID is '{expected_device_id}', but target reported '{reported_device_id}'. "
            f"Aborting deployment immediately to prevent mutating the wrong hardware device.",
            file=sys.stderr,
        )
        return 1
    print("  Identity Pin Check:   MATCH")

    # Enforce chip id and project check BEFORE build
    if reported_chip_id != ESP32_S3_CHIP_ID:
        print(
            f"Error: Device reported chip ID {reported_chip_id}, expected {ESP32_S3_CHIP_ID} (ESP32-S3)",
            file=sys.stderr,
        )
        return 1
    if device_project != EXPECTED_PROJECT_NAME:
        print(
            f"Error: Incompatible project on device: '{device_project}' (expected '{EXPECTED_PROJECT_NAME}')",
            file=sys.stderr,
        )
        return 1

    print("  Pre-deploy Health:    HEALTHY (strictly true)")

    # Phase 2: Build (only after identity verification)
    if args.build:
        print(f"\n[Phase 2: Building Firmware for environment '{env}']")
        try:
            firmware_path = run_platformio_build(project_dir, env)
        except Exception as e:
            print(f"Build failed: {e}", file=sys.stderr)
            return 1

    # Phase 3: Binary Inspection & Validation
    print(f"\n[Phase 3: Validating Firmware Binary: {firmware_path.name}]")
    try:
        fw_info = parse_and_validate_firmware(firmware_path, EXPECTED_PROJECT_NAME)
    except Exception as e:
        print(f"Error: Firmware validation failed: {e}", file=sys.stderr)
        return 1

    print(fw_info)

    # Check size against device slot
    if ota_max_size is not None and fw_info.size > ota_max_size:
        print(
            f"\nError: Image size ({fw_info.size:,} bytes) exceeds device inactive OTA partition capacity ({ota_max_size:,} bytes).",
            file=sys.stderr,
        )
        return 1

    # Phase 4: Streaming Upload via POST /api/update
    print(f"\n[Phase 4: Streaming Upload to {validated_ip}:{port}/api/update]")
    try:
        up_status, up_data = client.post_update(
            firmware_path=fw_info.path,
            sha256_hex=fw_info.sha256,
            file_size=fw_info.size,
            progress_cb=print_progress,
            req_timeout=timeout,
        )
        if up_status != 202:
            err_msg = up_data.get("message") or up_data.get("error") or f"HTTP {up_status}"
            print(f"\nError: Upload rejected by device: {err_msg}", file=sys.stderr)
            return 1
        msg = up_data.get("message", "Writing and scheduling restart")
        print(f"\nUpload accepted by device (HTTP 202): {msg}")
    except (socket.error, http.client.HTTPException, TimeoutError) as e:
        # Ambiguous transport: device may have closed connection to reboot or flash
        print(
            f"\nWarning: Ambiguous transport during upload ({e}). "
            f"Per safety protocol, upload will NOT be automatically retried. "
            f"Entering polling phase to verify if device received the update..."
        )
    except Exception as e:
        print(f"\nFatal error during upload: {e}", file=sys.stderr)
        return 1

    # Phase 5: Reconnect, Verify Reboot, Slot Switch, and Health
    print(f"\n[Phase 5: Verifying Restart, Slot Switch, and Healthy State]")
    try:
        verify_device_reboot_and_health(
            client=client,
            initial_boot_id=initial_boot_id,
            initial_partition=initial_partition,
            expected_firmware=fw_info,
            expected_device_id=expected_device_id or reported_device_id,
            timeout=timeout,
        )
    except Exception as e:
        print(f"\nVerification FAILED: {e}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
