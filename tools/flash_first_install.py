"""Flash the first USB installation, including a NEW NVS partition."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--first-install", action="store_true", required=True,
                        help="Explicitly confirm that overwriting NVS is intended")
    parser.add_argument("--nvs", type=Path, default=Path("provisioning/nvs.bin"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = root / "firmware/build"
    flash = json.loads((build / "flasher_args.json").read_text())
    files = {int(address, 0): (build / path).resolve() for address, path in flash["flash_files"].items()}
    files[0x9000] = args.nvs.resolve()
    for address, file in files.items():
        if not file.is_file():
            parser.error(f"Missing flash file: {file}")
        if address == 0x9000 and file.stat().st_size != 0x10000:
            parser.error("NVS image must be exactly 64 KiB")
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "--port", args.port,
           "write-flash", "--flash-size", "4MB"]
    for address, file in sorted(files.items()):
        cmd.extend([hex(address), str(file)])
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
