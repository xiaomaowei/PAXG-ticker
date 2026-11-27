import os
from pathlib import Path
import subprocess

Import = globals().get("Import")
if Import:
    Import("env")
    env = globals().get("env")

    def _sync_ota_build_metadata(project_dir: Path, build_dir: Path) -> None:
        """Invalidate generated CMakeCache when Git HEAD/dirty status changes."""
        try:
            head_proc = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=str(project_dir),
                capture_output=True,
                text=True,
                check=False,
            )
            if head_proc.returncode != 0:
                return
            head_sha = head_proc.stdout.strip()
            if not head_sha:
                return

            status_proc = subprocess.run(
                ["git", "status", "--porcelain"],
                cwd=str(project_dir),
                capture_output=True,
                text=True,
                check=False,
            )
            fingerprint = f"{head_sha}:{'1' if status_proc.stdout.strip() else '0'}"
        except (OSError, ValueError):
            return

        build_dir.mkdir(parents=True, exist_ok=True)
        stamp_file = build_dir / ".git_build_metadata_stamp"
        prev_fingerprint = ""
        if stamp_file.exists():
            try:
                prev_fingerprint = stamp_file.read_text(encoding="utf-8").strip()
            except OSError:
                prev_fingerprint = ""

        if prev_fingerprint != fingerprint:
            # PlatformIO espressif32 builder checks CMakeCache.txt existence before reconfiguring
            cmake_cache = build_dir / "CMakeCache.txt"
            if cmake_cache.exists():
                try:
                    cmake_cache.unlink()
                except OSError:
                    # Invalidation failed; do not mark stamp as current so next run retries
                    return

            try:
                stamp_file.write_text(fingerprint, encoding="utf-8")
            except OSError:
                pass

    def _load_dotenv(dotenv_path: Path) -> dict:
        values = {}
        if not dotenv_path.exists():
            return values
        for raw_line in dotenv_path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip().strip('"').strip("'")
            if key:
                values[key] = value
        return values

    def _to_cpp_define_string(value: str) -> str:
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'\\"{escaped}\\"'

    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir_raw = env.subst("$BUILD_DIR")
    build_dir = Path(build_dir_raw) if build_dir_raw else (project_dir / ".pio" / "build")

    _sync_ota_build_metadata(project_dir, build_dir)

    root_dotenv = project_dir / ".." / ".." / ".env"
    dotenv_values = _load_dotenv(root_dotenv.resolve())

    ssid = dotenv_values.get("WIFI_SSID", "")
    password = dotenv_values.get("WIFI_PASSWORD", "")
    timezone = dotenv_values.get("APP_TIMEZONE", "CST-8")
    macrodroid_url = dotenv_values.get("MACRODROID_WEBHOOK_URL", "")
    queue_overflow_url = dotenv_values.get("MACRODROID_QUEUE_OVERFLOW_WEBHOOK_URL", "")

    cpp_defines = []
    if ssid and password:
        cpp_defines.extend([
            ("WIFI_SSID", _to_cpp_define_string(ssid)),
            ("WIFI_PASSWORD", _to_cpp_define_string(password)),
        ])

    if timezone:
        cpp_defines.append(("APP_TIMEZONE", _to_cpp_define_string(timezone)))

    if macrodroid_url:
        cpp_defines.append(("MACRODROID_WEBHOOK_URL", _to_cpp_define_string(macrodroid_url)))
    if queue_overflow_url:
        cpp_defines.append(("MACRODROID_QUEUE_OVERFLOW_WEBHOOK_URL", _to_cpp_define_string(queue_overflow_url)))

    if cpp_defines:
        env.Append(CPPDEFINES=cpp_defines)
