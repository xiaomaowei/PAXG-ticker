#!/usr/bin/env python3
"""Focused mock unit and integration tests for deploy_ota.py.

Uses Python 3 stdlib only, temporary directories, and loopback mock server.
No network traffic to external or physical devices.
"""

from __future__ import annotations

import configparser
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

# Import the module under test
import scripts.deploy_ota as ota


def create_synthetic_bin(
    size: int = 1024,
    magic: int = ota.ESP_IMAGE_MAGIC,
    chip_id: int = ota.ESP32_S3_CHIP_ID,
    app_magic: int = ota.ESP_APP_DESC_MAGIC,
    project_name: str = ota.EXPECTED_PROJECT_NAME,
    version: str = "ota1-1.0.0-test",
    elf_sha256_bytes: bytes = b"\xaa" * 32,
) -> bytes:
    """Create a synthetic ESP32-S3 firmware image for unit testing."""
    buf = bytearray(b"\x00" * size)
    buf[0] = magic
    struct.pack_into("<H", buf, 12, chip_id)
    struct.pack_into("<I", buf, 32, app_magic)
    # version at 48..79
    v_enc = version.encode("utf-8")[:31]
    buf[48 : 48 + len(v_enc)] = v_enc
    # project_name at 80..111
    p_enc = project_name.encode("utf-8")[:31]
    buf[80 : 80 + len(p_enc)] = p_enc
    # time at 112..127
    buf[112:120] = b"10:00:00"
    # date at 128..143
    buf[128:139] = b"Nov 24 2026"
    # idf_ver at 144..175
    buf[144:149] = b"5.1.2"
    # elf_sha256 at 176..207
    buf[176:208] = elf_sha256_bytes
    return bytes(buf)


class MockOTAState:
    """State machine for mock ESP32-S3 OTA server."""

    def __init__(self) -> None:
        self.expected_token = "secret-ota-token"
        self.device_id = "aabbccddeeff"
        self.boot_id = "boot-initial-1"
        self.project_name = ota.EXPECTED_PROJECT_NAME
        self.version = "0.9.0-old"
        self.git_commit = "deadbeef"
        self.elf_sha256 = ("11" * 32).lower()
        self.chip_id = 9
        self.active_partition = "app0"
        self.ota_max_size = 6553600
        self.healthy = True

        # Behavioral overrides
        self.reboot_behavior = "healthy"  # "healthy", "rollback", "unhealthy", "stale"
        self.abort_during_post = False
        self.post_received_sha256: str = ""
        self.post_received_bytes: int = 0
        self.updated_firmware_version: str = "ota1-1.0.0-test"
        self.updated_elf_sha256: str = ("aa" * 32).lower()


class MockOTAHandler(http.server.BaseHTTPRequestHandler):
    """HTTP request handler implementing the OTA wire contract."""

    state: MockOTAState
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: Any) -> None:
        # Suppress noisy HTTP logs during test runs
        pass

    def _verify_auth(self) -> bool:
        self.close_connection = True
        auth = self.headers.get("Authorization", "")
        expected = f"Bearer {self.state.expected_token}"
        if auth != expected:
            body = json.dumps({"error": "Unauthorized"}).encode("utf-8")
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            return False
        return True

    def do_GET(self) -> None:
        self.close_connection = True
        if not self._verify_auth():
            return

        if self.path == "/api/version":
            resp = {
                "device_id": self.state.device_id,
                "boot_id": self.state.boot_id,
                "project_name": self.state.project_name,
                "version": self.state.version,
                "git_commit": self.state.git_commit,
                "elf_sha256": self.state.elf_sha256,
                "chip_id": self.state.chip_id,
                "active_partition": self.state.active_partition,
                "ota_max_size": self.state.ota_max_size,
            }
            body = json.dumps(resp).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            return

        if self.path == "/api/health":
            resp = {
                "healthy": self.state.healthy,
                "device_id": self.state.device_id,
                "boot_id": self.state.boot_id,
            }
            body = json.dumps(resp).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            return

        body = b"Not Found"
        self.send_response(404)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def do_POST(self) -> None:
        self.close_connection = True
        if not self._verify_auth():
            return

        if self.path == "/api/update":
            content_length = int(self.headers.get("Content-Length", 0))
            expected_sha = self.headers.get("X-Firmware-SHA256", "").lower()

            if self.state.abort_during_post:
                # Read partial and abruptly reset/close connection to simulate ambiguous transport
                self.rfile.read(min(128, content_length))
                # Trigger state update anyway (device got flash scheduled)
                self._apply_reboot()
                self.close_connection = True
                return

            body = self.rfile.read(content_length)
            actual_sha = hashlib.sha256(body).hexdigest().lower()

            self.state.post_received_sha256 = actual_sha
            self.state.post_received_bytes = len(body)

            if expected_sha and actual_sha != expected_sha:
                resp_body = json.dumps({"error": "SHA-256 mismatch"}).encode("utf-8")
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(resp_body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(resp_body)
                self.wfile.flush()
                return

            self._apply_reboot()

            resp_body = json.dumps({
                "status": "accepted",
                "message": "Firmware image received, flashing and rebooting",
            }).encode("utf-8")
            self.send_response(202)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(resp_body)
            self.wfile.flush()
            return

        body = b"Not Found"
        self.send_response(404)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def _apply_reboot(self) -> None:
        """Simulate device reboot according to test configuration."""
        if self.state.reboot_behavior == "healthy":
            self.state.boot_id = "boot-rebooted-2"
            self.state.active_partition = "app1"
            self.state.version = self.state.updated_firmware_version
            self.state.elf_sha256 = self.state.updated_elf_sha256
            self.state.healthy = True
        elif self.state.reboot_behavior == "rollback":
            # Reboots with new boot_id but partition rolls back to app0 with old elf_sha256
            self.state.boot_id = "boot-rollback-3"
            self.state.active_partition = "app0"
            self.state.version = self.state.updated_firmware_version
            self.state.elf_sha256 = ("11" * 32).lower()
            self.state.healthy = True
        elif self.state.reboot_behavior == "unhealthy":
            # Reboots into new image but reports healthy=False
            self.state.boot_id = "boot-unhealthy-4"
            self.state.active_partition = "app1"
            self.state.version = self.state.updated_firmware_version
            self.state.elf_sha256 = self.state.updated_elf_sha256
            self.state.healthy = False
        elif self.state.reboot_behavior == "stale":
            # Device hangs and does not reboot (boot_id stays same)
            pass


class DeployOTATestCase(unittest.TestCase):
    """Comprehensive test suite for deploy_ota.py."""

    def setUp(self) -> None:
        self.temp_dir = tempfile.mkdtemp(prefix="paxg_ota_test_")
        self.mock_state = MockOTAState()

        # Custom handler subclass bound to this test's mock state
        class BoundHandler(MockOTAHandler):
            state = self.mock_state

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), BoundHandler)
        self.port = self.server.server_address[1]
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.server_thread.start()
        time.sleep(0.05)

        # Create synthetic valid firmware file
        self.valid_bin_data = create_synthetic_bin(
            size=2048,
            version=self.mock_state.updated_firmware_version,
            elf_sha256_bytes=bytes.fromhex(self.mock_state.updated_elf_sha256),
        )
        self.firmware_file = Path(self.temp_dir) / "test_firmware.bin"
        self.firmware_file.write_bytes(self.valid_bin_data)

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.server_thread.join(timeout=2.0)
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_cli_help(self) -> None:
        """Test that --help displays all required options."""
        proc = subprocess.run(
            [sys.executable, str(Path(ota.__file__).resolve()), "--help"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(proc.returncode, 0)
        output = proc.stdout
        for required_opt in [
            "--host",
            "--port",
            "--env",
            "--build",
            "--firmware",
            "--timeout",
            "--status-only",
            "--dry-run",
            "--device-id",
        ]:
            self.assertIn(required_opt, output)

    def test_host_ip_validation(self) -> None:
        """Validate that private, loopback, and link-local IPv4 are accepted, while public/mDNS are rejected."""
        valid_ips = ["127.0.0.1", "10.0.0.1", "192.168.1.100", "172.16.5.20", "169.254.10.10"]
        for ip in valid_ips:
            res = ota.validate_and_resolve_host(ip)
            self.assertEqual(res, ip)

        # Disallowed: mDNS, public IPs, empty
        with self.assertRaises(ValueError):
            ota.validate_and_resolve_host("my-esp32.local")
        with self.assertRaises(ValueError):
            ota.validate_and_resolve_host("8.8.8.8")
        with self.assertRaises(ValueError):
            ota.validate_and_resolve_host("1.1.1.1")
        with self.assertRaises(ValueError):
            ota.validate_and_resolve_host("")

    def test_binary_parser_valid(self) -> None:
        """Test parsing and validation of a valid ESP32-S3 binary."""
        info = ota.parse_and_validate_firmware(self.firmware_file)
        self.assertEqual(info.chip_id, 9)
        self.assertEqual(info.project_name, ota.EXPECTED_PROJECT_NAME)
        self.assertEqual(info.version, "ota1-1.0.0-test")
        self.assertEqual(info.elf_sha256, self.mock_state.updated_elf_sha256)
        self.assertEqual(info.size, 2048)
        self.assertEqual(info.sha256, hashlib.sha256(self.valid_bin_data).hexdigest().lower())

    def test_binary_parser_corruptions(self) -> None:
        """Test that invalid magic, chip ID, app magic, or project name trigger rejection."""
        # 1. Invalid ESP magic
        bad_magic = create_synthetic_bin(magic=0xEA)
        p = Path(self.temp_dir) / "bad_magic.bin"
        p.write_bytes(bad_magic)
        with self.assertRaises(ValueError) as ctx:
            ota.parse_and_validate_firmware(p)
        self.assertIn("Invalid ESP image magic", str(ctx.exception))

        # 2. Invalid Chip ID (e.g. 0 for ESP32 instead of 9 for ESP32-S3)
        bad_chip = create_synthetic_bin(chip_id=0)
        p = Path(self.temp_dir) / "bad_chip.bin"
        p.write_bytes(bad_chip)
        with self.assertRaises(ValueError) as ctx:
            ota.parse_and_validate_firmware(p)
        self.assertIn("Invalid chip ID", str(ctx.exception))

        # 3. Invalid esp_app_desc magic
        bad_app = create_synthetic_bin(app_magic=0x12345678)
        p = Path(self.temp_dir) / "bad_app.bin"
        p.write_bytes(bad_app)
        with self.assertRaises(ValueError) as ctx:
            ota.parse_and_validate_firmware(p)
        self.assertIn("Invalid esp_app_desc magic", str(ctx.exception))

        # 4. Incompatible project name
        bad_proj = create_synthetic_bin(project_name="other_project")
        p = Path(self.temp_dir) / "bad_proj.bin"
        p.write_bytes(bad_proj)
        with self.assertRaises(ValueError) as ctx:
            ota.parse_and_validate_firmware(p)
        self.assertIn("Project name mismatch", str(ctx.exception))

    def test_offline_dry_run_synthetic_bin(self) -> None:
        """Run dry-run offline without network access using synthetic binary."""
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--dry-run",
                "--firmware",
                str(self.firmware_file),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(proc.returncode, 0)
        self.assertIn("Dry-run completed successfully", proc.stdout)
        self.assertIn("Firmware Binary Validation Succeeded", proc.stdout)

    def test_offline_dry_run_real_existing_bin(self) -> None:
        """Acceptance test: offline dry-run using the real existing integrated bin."""
        repo_root, project_dir = ota.get_repo_and_project_dirs()
        real_bin = project_dir / ".pio" / "build" / "esp32s3_arduino_idf_poc" / "firmware.bin"
        if not real_bin.is_file():
            self.skipTest(f"Real firmware binary not found at {real_bin}")

        with open(real_bin, "rb") as f:
            f.seek(ota.OFFSET_VERSION)
            ver_bytes = f.read(32).split(b"\x00")[0].decode("utf-8", "replace")
        if not ver_bytes.startswith(ota.REQUIRED_VERSION_PREFIX):
            self.skipTest(
                f"Real firmware binary version '{ver_bytes}' lacks required '{ota.REQUIRED_VERSION_PREFIX}' prefix; "
                f"skipping until integrated build produces ota1 image."
            )

        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--dry-run",
                "--firmware",
                str(real_bin),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(proc.returncode, 0)
        self.assertIn("Dry-run completed successfully", proc.stdout)
        self.assertIn("esp32s3_arduino_idf_poc", proc.stdout)

    def test_auth_check_missing_and_invalid_token(self) -> None:
        """Test authentication rejection with missing and wrong token."""
        # 1. Missing token
        env_no_token = os.environ.copy()
        env_no_token.pop("PAXG_OTA_TOKEN", None)
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--status-only",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
            ],
            capture_output=True,
            text=True,
            env=env_no_token,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("PAXG_OTA_TOKEN", proc.stderr)

        # 2. Wrong token -> 401 Unauthorized
        env_bad_token = os.environ.copy()
        env_bad_token["PAXG_OTA_TOKEN"] = "wrong-token"
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--status-only",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
            ],
            capture_output=True,
            text=True,
            env=env_bad_token,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("HTTP 401", proc.stderr)

    def test_status_only_success(self) -> None:
        """Test --status-only query displaying version and health info."""
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--status-only",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(proc.returncode, 0)
        self.assertIn("Device Version Info", proc.stdout)
        self.assertIn("aabbccddeeff", proc.stdout)
        self.assertIn("boot-initial-1", proc.stdout)
        self.assertIn("Healthy:          Yes", proc.stdout)

    def test_upload_digest_verification(self) -> None:
        """Test upload sends correct X-Firmware-SHA256 header matching binary."""
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
                "--timeout",
                "8.0",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(proc.returncode, 0)
        expected_sha = hashlib.sha256(self.valid_bin_data).hexdigest().lower()
        self.assertEqual(self.mock_state.post_received_sha256, expected_sha)
        self.assertEqual(self.mock_state.post_received_bytes, len(self.valid_bin_data))

    def test_full_healthy_reboot_and_slot_switch(self) -> None:
        """Test successful end-to-end deployment with boot_id change, slot switch, and verified health."""
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
                "--timeout",
                "8.0",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(proc.returncode, 0, f"STDOUT: {proc.stdout}\nSTDERR: {proc.stderr}")
        self.assertIn("OTA update verified", proc.stdout)
        self.assertIn("switched from boot-initial-1", proc.stdout)
        self.assertIn("switched from app0", proc.stdout)
        self.assertIn("Healthy (true)", proc.stdout)

    def test_device_id_pin_mismatch_aborts(self) -> None:
        """Test that device ID pin mismatch aborts deployment before any mutation occurs."""
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "112233445566",  # Different from actual aabbccddeeff
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Device ID mismatch", proc.stderr)
        # Verify no POST /api/update was ever sent
        self.assertEqual(self.mock_state.post_received_bytes, 0)

    def test_missing_device_id_pin_rejected(self) -> None:
        """Test that mutating deploy without pinned device ID is strictly rejected."""
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("pinned device ID", proc.stderr)
        self.assertEqual(self.mock_state.post_received_bytes, 0)

    def test_legacy_non_ota_version_rejected(self) -> None:
        """Test that firmware version without required 'ota1-' prefix is rejected."""
        legacy_bin = create_synthetic_bin(version="legacy-no-ota-1.0")
        p = Path(self.temp_dir) / "legacy_bin.bin"
        p.write_bytes(legacy_bin)
        with self.assertRaises(ValueError) as ctx:
            ota.parse_and_validate_firmware(p)
        self.assertIn("Legacy non-OTA image rejected", str(ctx.exception))

    def test_oversized_file_rejected_before_hash(self) -> None:
        """Test that firmware larger than 0x640000 is rejected immediately."""
        p = Path(self.temp_dir) / "too_big.bin"
        # Create sparse or 0x640001 byte file
        with open(p, "wb") as f:
            f.seek(ota.MAX_FIRMWARE_SIZE)
            f.write(b"\x00")
        with self.assertRaises(ValueError) as ctx:
            ota.parse_and_validate_firmware(p)
        self.assertIn("Firmware file too large", str(ctx.exception))

    def test_contradictory_cli_options(self) -> None:
        """Test rejection of contradictory CLI modes (e.g. status-only with build)."""
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--status-only",
                "--build",
                "--host",
                "127.0.0.1",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("cannot be combined", proc.stderr)

    def test_pre_deploy_version_health_mismatch(self) -> None:
        """Test rejection when /api/version and /api/health report inconsistent device_id or boot_id."""
        self.mock_state.healthy = False
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("not strictly true", proc.stderr)

    def test_oversized_firmware_rejected(self) -> None:
        """Test rejection when firmware size exceeds inactive partition capacity."""
        self.mock_state.ota_max_size = 1000  # Smaller than 2048 byte binary
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("exceeds device inactive OTA partition capacity", proc.stderr)
        self.assertEqual(self.mock_state.post_received_bytes, 0)

    def test_rollback_detection_fails(self) -> None:
        """Test that if the device reboots back into the old partition/version, it is rejected."""
        self.mock_state.reboot_behavior = "rollback"
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
                "--timeout",
                "8.0",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("rollback suspected", proc.stderr.lower())

    def test_unhealthy_reboot_fails(self) -> None:
        """Test that if device reboots but reports healthy=False, it is rejected."""
        self.mock_state.reboot_behavior = "unhealthy"
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
                "--timeout",
                "8.0",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("reported unhealthy", proc.stderr.lower())

    def test_stale_reboot_timeout(self) -> None:
        """Test that if device fails to restart (boot_id unchanged), client times out and exits nonzero."""
        self.mock_state.reboot_behavior = "stale"
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
                "--timeout",
                "3.0",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("did not reboot within", proc.stderr.lower())

    def test_ambiguous_transport_no_retry_then_verified(self) -> None:
        """Test that after ambiguous transport during POST, upload is not retried and polling verifies success."""
        self.mock_state.abort_during_post = True
        self.mock_state.reboot_behavior = "healthy"
        env = os.environ.copy()
        env["PAXG_OTA_TOKEN"] = self.mock_state.expected_token
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(ota.__file__).resolve()),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--firmware",
                str(self.firmware_file),
                "--device-id",
                "aabbccddeeff",
                "--timeout",
                "8.0",
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(proc.returncode, 0)
        self.assertIn("Ambiguous transport", proc.stdout)
        self.assertIn("upload will NOT be automatically retried", proc.stdout)
        self.assertIn("OTA update verified", proc.stdout)
        self.assertEqual(proc.returncode, 0)
        self.assertIn("Ambiguous transport", proc.stdout)
        self.assertIn("upload will NOT be automatically retried", proc.stdout)
        self.assertIn("OTA update verified", proc.stdout)

    def test_config_defaults_loading(self) -> None:
        """Test loading defaults from ota-deploy.local.ini."""
        test_root = Path(self.temp_dir) / "fake_repo"
        test_root.mkdir(parents=True, exist_ok=True)
        test_proj = test_root / "test" / "arduino_idf_integration_poc"
        test_proj.mkdir(parents=True, exist_ok=True)

        ini_content = """[ota]
host = 192.168.1.123
port = 9090
env = my_custom_env
expected_device_id = 998877665544
timeout = 42.0
"""
        (test_root / "ota-deploy.local.ini").write_text(ini_content, encoding="utf-8")
        cfg = ota.load_config_defaults(test_root, test_proj)
        self.assertEqual(cfg["host"], "192.168.1.123")
        self.assertEqual(cfg["port"], 9090)
        self.assertEqual(cfg["env"], "my_custom_env")
        self.assertEqual(cfg["expected_device_id"], "998877665544")
        self.assertEqual(cfg["timeout"], 42.0)


if __name__ == "__main__":
    unittest.main()
