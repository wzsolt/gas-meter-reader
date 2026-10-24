"""Overwrite the initial ota_0 application only; preserve NVS and OTA metadata.

For this device's first USB installation, before any OTA slot switch. Do not use
this helper after OTA has activated ota_1.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def flash_command(build, port):
    metadata = json.loads((build / "flasher_args.json").read_text())
    app = metadata["app"]
    offset = int(app["offset"], 0)
    if offset != 0x20000:
        raise ValueError("Expected this project's initial ota_0 slot at 0x20000")
    binary = (build / app["file"]).resolve()
    if not binary.is_file() or not 288 <= binary.stat().st_size <= 0x1e0000:
        raise ValueError("Application is missing or exceeds the OTA slot")
    with binary.open("rb") as stream:
        prefix = stream.read(288)
    if prefix[0] != 0xe9 or prefix[12:14] != b"\x0d\x00" or prefix[80:112].rstrip(b"\0") != b"gas_reader":
        raise ValueError("Expected an ESP32-C6 Gas Reader application image")
    return [sys.executable, "-m", "esptool", "--chip", "esp32c6", "--port", port,
            "write-flash", "--flash-size", "4MB", hex(offset), str(binary)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "firmware/build")
    parser.add_argument("--initial-slot", action="store_true", required=True,
                        help="Confirm no OTA has switched this device from ota_0")
    args = parser.parse_args()
    try:
        command = flash_command(args.build_dir.resolve(), args.port)
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print("Writing only ota_0 at 0x20000; NVS, bootloader, partition table and OTA metadata are preserved.", flush=True)
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
