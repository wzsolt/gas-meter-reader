#!/usr/bin/env bash
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/idf-env.sh"
exec "$IDF_PYTHON_ENV_PATH/bin/python" "$@"
