"""Create deliverables without bundling toolchains, caches, or credentials."""
import hashlib
import json
import re
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    binary = ROOT / "firmware/build/gas_reader.bin"
    prefix = binary.read_bytes()[:288]
    if len(prefix) != 288 or prefix[0] != 0xe9 or prefix[12:14] != b"\x0d\x00" or prefix[80:112].rstrip(b"\0") != b"gas_reader":
        raise ValueError("Build an ESP32-C6 Gas Reader application before packaging")
    if binary.stat().st_size > 0x1e0000:
        raise ValueError("Application exceeds the OTA slot")
    version = prefix[48:80].rstrip(b"\0").decode("ascii")
    if not re.fullmatch(r"[A-Za-z0-9_.+-]{1,31}", version):
        raise ValueError("Invalid application version")
    firmware = output / f"gas-reader-{version}.bin"
    shutil.copy2(binary, firmware)
    base = ROOT / "homeassistant"
    ha_version = json.loads((base / "custom_components/gas_reader/manifest.json").read_text())["version"]
    if not re.fullmatch(r"[A-Za-z0-9_.+-]{1,31}", ha_version):
        raise ValueError("Invalid integration version")
    archive = output / f"gas-reader-homeassistant-{ha_version}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for file in sorted((base / "custom_components/gas_reader").rglob("*")):
            if file.is_file() and "__pycache__" not in file.parts and file.suffix in (".py", ".json", ".yaml"):
                z.write(file, file.relative_to(base))
        z.write(ROOT / "docs/INSTALL.md", "INSTALL.md")
    with (output / "SHA256SUMS").open("w") as stream:
        for file in (firmware, archive):
            stream.write(f"{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.name}\n")
    print(f"Packaged {firmware.name} ({firmware.stat().st_size} bytes) and {archive.name}")


if __name__ == "__main__": main()
