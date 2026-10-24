"""Locate the SDK and Python used by VS Code/EIM, with a legacy local fallback."""
import argparse
import json
import os
from pathlib import Path
import shlex

ROOT = Path(__file__).resolve().parents[1]


def read_json(path):
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text())
    except (ValueError, OSError) as exc:
        raise ValueError(f"Cannot read ESP-IDF configuration {path}: {exc}") from exc


def find_idf(root=ROOT, home=None, env=None):
    home = Path.home() if home is None else Path(home)
    env = os.environ if env is None else env
    selected = env.get("IDF_PATH")
    if not selected:
        selected = read_json(root / "firmware/.vscode/settings.json").get("idf.currentSetup")
    if selected:
        sdk = Path(selected).expanduser()
    else:
        sdk = home / ".espressif/v6.1/esp-idf"
        if not (sdk / "tools/idf.py").is_file():
            sdk = root / ".tools/esp-idf"
    if not (sdk / "tools/idf.py").is_file():
        raise ValueError(f"ESP-IDF not found at {sdk}. Select v6.1 in VS Code or set IDF_PATH.")
    return sdk.resolve()


def find_environment(root=ROOT, home=None, env=None):
    home = Path.home() if home is None else Path(home)
    env = os.environ if env is None else env
    sdk = find_idf(root, home, env)
    registry = read_json(home / ".espressif/tools/eim_idf.json")
    setup = next((entry for entry in registry.get("idfInstalled", [])
                  if Path(entry.get("path", "")).expanduser().resolve() == sdk), {})
    default_tools = (root / ".tools/idf-tools" if sdk == (root / ".tools/esp-idf").resolve()
                     else home / ".espressif/tools")
    tools = Path(env.get("IDF_TOOLS_PATH") or setup.get("idfToolsPath") or default_tools).expanduser()
    if env.get("IDF_PYTHON_ENV_PATH"):
        candidates = [Path(env["IDF_PYTHON_ENV_PATH"]).expanduser() / "bin/python"]
    elif setup.get("python"):
        candidates = [Path(setup["python"]).expanduser()]
    else:
        candidates = [tools / "python/v6.1/venv/bin/python"]
        candidates.extend(sorted(tools.glob("python_env/idf6.1_py*_env/bin/python")))
    python = next((path for path in candidates if path.is_file() and os.access(path, os.X_OK)), None)
    if python is None:
        raise ValueError(f"ESP-IDF Python environment not found for {sdk}. Check the EIM installation.")
    # Keep the venv path; resolving Python's symlink would discard the virtual environment.
    result = {"IDF_PATH": str(sdk), "IDF_TOOLS_PATH": str(tools),
              "IDF_PYTHON_ENV_PATH": str(python.parent.parent)}
    if setup.get("activationScript"):
        activation = Path(setup["activationScript"]).expanduser()
        if not activation.is_file():
            raise ValueError(f"EIM activation script not found: {activation}")
        result["GR_IDF_ACTIVATION_SCRIPT"] = str(activation)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell", action="store_true", help="Print safely quoted shell exports")
    args = parser.parse_args()
    try:
        environment = find_environment()
    except ValueError as exc:
        parser.error(str(exc))
    if args.shell:
        for key, value in environment.items():
            print(f"export {key}={shlex.quote(value)}")
    else:
        print(json.dumps(environment, indent=2))


if __name__ == "__main__":
    main()
