#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_TOOLS_PATH="$project_root/.tools/idf-tools"
mkdir -p "$project_root/.tools"
if [ ! -d "$project_root/.tools/esp-idf" ]; then
    git clone --branch v6.1 --depth 1 --shallow-submodules --recursive \
        https://github.com/espressif/esp-idf.git "$project_root/.tools/esp-idf"
fi
"$project_root/.tools/esp-idf/install.sh" esp32c6
. "$project_root/.tools/esp-idf/export.sh"
python -m pip install cmake ninja
printf '%s\n' 'ESP-IDF ready. Build with: bash tools/build.sh'
