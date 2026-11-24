import os
from pathlib import Path

Import = globals().get("Import")
if Import:
    Import("env")
    env = globals().get("env")

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
    root_dotenv = project_dir / ".." / ".." / ".env"
    dotenv_values = _load_dotenv(root_dotenv.resolve())

    ssid = dotenv_values.get("WIFI_SSID", "")
    password = dotenv_values.get("WIFI_PASSWORD", "")
    timezone = dotenv_values.get("APP_TIMEZONE", "CST-8")

    cpp_defines = []
    if ssid and password:
        cpp_defines.extend([
            ("WIFI_SSID", _to_cpp_define_string(ssid)),
            ("WIFI_PASSWORD", _to_cpp_define_string(password)),
        ])

    if timezone:
        cpp_defines.append(("APP_TIMEZONE", _to_cpp_define_string(timezone)))

    if cpp_defines:
        env.Append(CPPDEFINES=cpp_defines)
