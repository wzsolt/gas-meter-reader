#!/usr/bin/env bash
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/idf-env.sh"
if [ -z "${GR_IDF_ACTIVATION_SCRIPT:-}" ]; then
    . "$IDF_PATH/export.sh"
fi
"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" -C "$project_root/firmware" build
