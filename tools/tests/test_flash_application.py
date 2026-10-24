import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from flash_application import flash_command


class ApplicationFlashTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gas-reader-app-flash-")
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.binary = self.build / "gas_reader.bin"
        self.image = bytearray(288)
        self.image[0] = 0xe9
        self.image[12:14] = b"\x0d\x00"
        self.image[80:90] = b"gas_reader"
        self.binary.write_bytes(self.image)
        self.metadata = {
            "app": {"offset": "0x20000", "file": "gas_reader.bin"},
            "flash_files": {"0x0": "bootloader.bin", "0x9000": "private_nvs.bin",
                            "0x19000": "otadata.bin", "0x20000": "gas_reader.bin"},
        }
        self.write_metadata()

    def write_metadata(self):
        (self.build / "flasher_args.json").write_text(json.dumps(self.metadata))

    def test_preserves_nvs_and_boot_metadata(self):
        command = flash_command(self.build, "/dev/cu.test")
        after_write = command[command.index("write-flash") + 1:]
        self.assertEqual(after_write, ["--flash-size", "4MB", "0x20000", str(self.binary.resolve())])
        self.assertNotIn("0x9000", command)
        self.assertNotIn("0x19000", command)

    def test_rejects_nvs_or_other_slot_address(self):
        for offset in ["0x9000", "0x200000"]:
            self.metadata["app"]["offset"] = offset
            self.write_metadata()
            with self.assertRaises(ValueError): flash_command(self.build, "/dev/cu.test")

    def test_rejects_wrong_chip_or_project(self):
        for index in [0, 12, 80]:
            damaged = bytearray(self.image); damaged[index] ^= 1
            self.binary.write_bytes(damaged)
            with self.assertRaises(ValueError): flash_command(self.build, "/dev/cu.test")

    def test_rejects_truncated_or_oversized_application(self):
        self.binary.write_bytes(self.image[:100])
        with self.assertRaises(ValueError): flash_command(self.build, "/dev/cu.test")
        with self.binary.open("wb") as stream: stream.truncate(0x1e0001)
        with self.assertRaises(ValueError): flash_command(self.build, "/dev/cu.test")

    def test_missing_application(self):
        self.binary.unlink()
        with self.assertRaises(ValueError): flash_command(self.build, "/dev/cu.test")


if __name__ == "__main__": unittest.main()
