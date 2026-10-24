#!/usr/bin/env bash
# Shared setup for project scripts. Source this file from Bash.
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_environment="$(python3 "$project_root/tools/idf_env.py" --shell)" || return 1
unset GR_IDF_ACTIVATION_SCRIPT
eval "$idf_environment"
# EIM distributions can use tool versions/layouts that differ from export.sh.
# Its -e mode only prints the environment and does not change EIM's selection.
if [ -n "${GR_IDF_ACTIVATION_SCRIPT:-}" ]; then
    idf_activation="$(bash "$GR_IDF_ACTIVATION_SCRIPT" -e)" || return 1
    while IFS='=' read -r idf_name idf_value; do
        case "$idf_name" in
            PATH) export PATH="$idf_value:$PATH" ;;
            IDF_*|ESP_*|OPENOCD_SCRIPTS) export "$idf_name=$idf_value" ;;
        esac
    done <<< "$idf_activation"
    unset idf_activation idf_name idf_value
fi
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
unset idf_environment
