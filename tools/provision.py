"""Generate a private initial NVS image. Does not connect to or flash a device."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlparse
from idf_env import find_idf

LIMITS = {"ssid": 32, "wifi_password": 64, "mqtt_uri": 255, "mqtt_user": 64,
          "mqtt_password": 128, "ota_password": 128}
OPTIONAL_LIMITS = {"fallback_ssid": 32, "fallback_wifi_password": 64}
# NVS keys have a 15-character limit; public JSON names remain descriptive.
NVS_KEYS = {"fallback_wifi_password": "fallback_pass"}


def validate(config):
    if (not isinstance(config, dict) or not set(LIMITS) <= set(config)
            or set(config) - (set(LIMITS) | set(OPTIONAL_LIMITS))):
        raise ValueError("Configuration must use the keys from config.example.json; fallback keys are optional")
    for key, limit in (LIMITS | OPTIONAL_LIMITS).items():
        value = config.get(key, "")
        if not isinstance(value, str) or (key in LIMITS and not value) or len(value.encode("utf-8")) > limit or any(ord(c) < 32 for c in value):
            raise ValueError(f"Invalid value/length for {key}")
        if "CHANGE_ME" in value:
            raise ValueError(f"Replace placeholder for {key}")
    uri = urlparse(config["mqtt_uri"])
    if uri.scheme != "mqtt" or not uri.hostname or uri.username or uri.password or uri.path not in ("", "/") or uri.query or uri.fragment:
        raise ValueError("mqtt_uri must be mqtt://host[:port] without embedded credentials")
    if uri.port is not None and not 1 <= uri.port <= 65535:
        raise ValueError("Invalid broker port")
    if bool(config.get("fallback_ssid")) != bool(config.get("fallback_wifi_password")):
        raise ValueError("Provide both fallback_ssid and fallback_wifi_password, or leave both empty")
    for key in ("wifi_password", "fallback_wifi_password"):
        password = config.get(key, "")
        if key in OPTIONAL_LIMITS and not password:
            continue
        if not 8 <= len(password.encode()) <= 63 and not (len(password) == 64 and all(c in "0123456789abcdefABCDEF" for c in password)):
            raise ValueError(f"{key} must be 8–63 bytes or a 64-digit hexadecimal PSK")
    if len(config["ota_password"]) < 12:
        raise ValueError("OTA password must contain at least 12 characters")
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--output", type=Path, default=Path("provisioning"))
    args = parser.parse_args()
    try:
        config = validate(json.loads(args.config.read_text()))
        generator = find_idf() / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
        if not generator.is_file():
            raise ValueError(f"NVS generator not found: {generator}")
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=True)
    csv_path = args.output / "nvs.csv"
    with csv_path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow(["config", "namespace", "", ""])
        for key, value in config.items():
            writer.writerow([NVS_KEYS.get(key, key), "data", "string", value])
    subprocess.run([sys.executable, str(generator), "generate", str(csv_path), str(args.output / "nvs.bin"), "0x10000"], check=True)
    print("Initial NVS generated. Keep provisioning/ private. Use only for the first USB installation.")


if __name__ == "__main__":
    main()
