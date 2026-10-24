import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from idf_env import find_environment, find_idf


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gas-reader-idf-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve() / "Home Projects/Gas Reader"
        self.home = Path(self.temp.name).resolve() / "home"
        self.sdk = self.home / ".espressif/v6.1/esp-idf"
        self.tools = self.home / ".espressif/tools"
        self.python = self.tools / "python/v6.1/venv/bin/python"
        self.make_sdk(self.sdk)
        self.make_python(self.python)

    def make_sdk(self, sdk):
        (sdk / "tools").mkdir(parents=True, exist_ok=True)
        (sdk / "tools/idf.py").touch()

    def make_python(self, python):
        python.parent.mkdir(parents=True, exist_ok=True)
        python.write_text("#!/bin/sh\nexit 0\n")
        python.chmod(0o700)

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    def test_eim_install_without_local_tools(self):
        self.write_json(self.tools / "eim_idf.json", {"idfInstalled": [{
            "path": str(self.sdk), "python": str(self.python), "idfToolsPath": str(self.tools)}]})
        found = find_environment(self.root, self.home, {})
        self.assertEqual(found["IDF_PATH"], str(self.sdk))
        self.assertEqual(found["IDF_PYTHON_ENV_PATH"], str(self.python.parent.parent))
        self.assertFalse((self.root / ".tools").exists())

    def test_vscode_choice_over_default(self):
        chosen = self.home / "custom SDK/esp-idf"
        self.make_sdk(chosen)
        self.write_json(self.root / "firmware/.vscode/settings.json", {"idf.currentSetup": str(chosen)})
        self.assertEqual(find_idf(self.root, self.home, {}), chosen)

    def test_environment_over_vscode(self):
        chosen = self.home / "activated SDK"
        self.make_sdk(chosen)
        self.write_json(self.root / "firmware/.vscode/settings.json", {"idf.currentSetup": str(self.sdk)})
        self.assertEqual(find_idf(self.root, self.home, {"IDF_PATH": str(chosen)}), chosen)

    def test_explicit_python_and_tools(self):
        custom_python = self.home / "custom Python/bin/python"
        self.make_python(custom_python)
        found = find_environment(self.root, self.home, {
            "IDF_PATH": str(self.sdk), "IDF_TOOLS_PATH": str(self.home / "custom tools"),
            "IDF_PYTHON_ENV_PATH": str(custom_python.parent.parent)})
        self.assertEqual(found["IDF_TOOLS_PATH"], str(self.home / "custom tools"))
        self.assertEqual(found["IDF_PYTHON_ENV_PATH"], str(custom_python.parent.parent))

    def test_legacy_local_install(self):
        (self.sdk / "tools/idf.py").unlink()
        legacy = self.root / ".tools/esp-idf"
        self.make_sdk(legacy)
        python = self.root / ".tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python"
        self.make_python(python)
        found = find_environment(self.root, self.home, {})
        self.assertEqual(found["IDF_PATH"], str(legacy))
        self.assertEqual(found["IDF_PYTHON_ENV_PATH"], str(python.parent.parent))

    def test_missing_selected_sdk_does_not_silently_use_another(self):
        with self.assertRaisesRegex(ValueError, "ESP-IDF not found"):
            find_environment(self.root, self.home, {"IDF_PATH": str(self.home / "missing")})

    def test_missing_selected_python_does_not_mix_installations(self):
        with self.assertRaisesRegex(ValueError, "Python environment not found"):
            find_environment(self.root, self.home, {"IDF_PYTHON_ENV_PATH": str(self.home / "missing")})


if __name__ == "__main__":
    unittest.main()
