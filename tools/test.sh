#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="$(mktemp "${TMPDIR:-/tmp}/gas-reader-tests.XXXXXX")"
trap 'rm -f "$test_binary"' EXIT
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/firmware/main" "$project_root/firmware/main/core.c" \
    "$project_root/firmware/main/daily_schedule.c" "$project_root/firmware/tests/test_daily_schedule.c" -o "$test_binary"
"$test_binary"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/firmware/main" "$project_root/firmware/main/core.c" \
    "$project_root/firmware/main/image_check.c" "$project_root/firmware/tests/test_core.c" -o "$test_binary"
"$test_binary"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/firmware/main" "$project_root/firmware/main/core.c" \
    "$project_root/firmware/main/meter_reset.c" "$project_root/firmware/tests/test_meter_reset.c" -o "$test_binary"
"$test_binary"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/firmware/main" "$project_root/firmware/main/wifi_join.c" \
    "$project_root/firmware/tests/test_wifi_join.c" -o "$test_binary"
"$test_binary"
python3 -m unittest discover -s "$project_root/homeassistant/tests" -p 'test_ledger.py' -v
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/firmware/main" "$project_root/firmware/main/core.c" \
    "$project_root/firmware/main/sync_drain.c" "$project_root/firmware/tests/test_sync_drain.c" -o "$test_binary"
"$test_binary"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/firmware/main" "$project_root/firmware/main/maintenance_control.c" \
    "$project_root/firmware/tests/test_maintenance_control.c" -o "$test_binary"
"$test_binary"
python3 -m unittest discover -s "$project_root/tools/tests" -v
if command -v node >/dev/null 2>&1; then
    node "$project_root/firmware/tests/test_maintenance_page.cjs"
else
    printf '%s\n' 'Maintenance page tests skipped: Node.js is unavailable.'
fi
if [ -x "$project_root/.venv-ha/bin/python" ]; then
    "$project_root/.venv-ha/bin/python" -m pytest "$project_root/homeassistant/tests/test_adapter.py" -q
else
    printf '%s\n' 'HA adapter tests skipped: create .venv-ha using homeassistant/requirements-test.txt.'
fi
