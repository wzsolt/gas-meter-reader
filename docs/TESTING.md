# Testing and hardware validation

[Back to README](../README.md)

Software tests and a successful build do not establish pulse timing, battery life or radio coverage for a particular board/meter.

## Automated tests

From the repository root:

```sh
bash tools/test.sh
```

The script runs:

- Portable firmware C tests with Clang, AddressSanitizer and UndefinedBehaviorSanitizer.
- Python ledger tests independent of HA, including ACK loss, exact decimal arithmetic, correction/consumption separation, migration and duplicate/conflict handling.
- Environment/provisioning/flashing helper tests using temporary fixtures, not real credentials or hardware.
- Maintenance HTML/JavaScript tests through Node.js, if Node is available.
- Adapter tests using actual HA classes if `.venv-ha/bin/python` exists.

The HA environment is optional but needed to run the complete adapter suite:

```sh
python3.14 -m venv .venv-ha
.venv-ha/bin/python -m pip install -r homeassistant/requirements-test.txt
.venv-ha/bin/python -m pytest homeassistant/tests/test_adapter.py -q
```

The pinned requirements use Home Assistant 2026.9.4 and Python 3.14. The script clearly reports skipped Node/HA checks when their runtimes are absent. Tests do not flash an ESP or change a running HA installation.

Build the target separately using ESP-IDF 6.1.0:

```sh
bash tools/build.sh
```

This checks API compatibility and OTA slot size. Installing the optional drawing dependency and regenerating diagrams is covered in [Hardware](HARDWARE.md#rebuilding-the-diagrams).

## Covered behavior

| Area | Checks |
|---|---|
| Reed counting | Close/release bounce, held closure, cold-start baseline, buffered edges and snapshot/ACK invariants |
| Persistence | CRC, immutable outbox, exact ACK subtraction, save failure and replay |
| Complete sync | Old zero/nonzero outbox plus new pulses, continuous pulses, shared deadline, fresh daily telemetry |
| Wi-Fi | Primary/fallback decisions, join/stop failures and bounded shared time |
| Daily sync | Cold boot, 24-hour fallback, retry cooldown, RTC CRC, fixed deadline, HA time, 23:55/midnight and DST transitions |
| Maintenance | Entry hold ignored, new short release exits once, bounce, long hold ignored, upload/discard/exit ordering |
| OTA header | Wrong project/chip, malformed and oversized images |
| HA | Durable save before ACK, storage restart/error handling, schema-2 migration, separate consumption, corrections, retained-message rejection and optional fresh clock ACK |
| Battery estimate | Bounds, interpolation, monotonicity, missing/invalid data, sensor class/unit and restore |
| Web page | Confirm/cancel, guards, pending state, network errors and disabling conflicting controls |

## Hardware acceptance

Perform and record these checks on the actual board and meter before unattended use:

| Check | Acceptance target |
|---|---|
| Wiring | Independent D0/D1/D2/D4, common ground, correct battery/LED polarity, button on D2 |
| ADC | Compare battery/divider/HA readings with a multimeter at several voltages; investigate resistor ratio/ADC/leakage errors |
| Pulse count | Generate at least 1000 complete cycles at the meter's shortest real HIGH/LOW durations; exact count agreement |
| Debounce | Under-30-ms bounce must not create extra counts |
| Held reed | One-hour LOW interval gives one closure, without a rapid wake loop |
| Wake margin | Measure edge-to-running-counter latency; actual pulse widths must exceed it and debounce with margin |
| Sleep current | Measure on battery, USB disconnected, with reed open and closed; include the closed pull-up current |
| Sync energy | Measure a complete radio operation and startup voltage droop; do not infer battery life solely from chip sleep-current specifications |
| Ordinary/daily timing | Reed wakes do not defer deadlines; empty ordinary timer stays offline; daily timer sends at 23:55 or uses 24-hour fallback |
| Buttons | Short normal press syncs; 5-second hold enters; release then new short press exits; entry hold/release alone does not exit |
| Radio | Primary/fallback coverage at final placement, with broker reachable from both networks |
| Outages | Independently interrupt Wi-Fi, broker and HA; operation remains bounded, packet persists and later replay does not double count |
| ACK/reset points | Interrupt around HA commit/ACK/ESP checkpoint; reconcile the restored outbox and totals |
| OTA | Count during upload; test interruption, bad/wrong/oversized image and rollback on a deliberately failed boot self-check |
| Discard | HA reading/consumption remain intact; old report resolved first; post-snapshot pulses survive |
| Clock | Check HA clock, RTC drift, local evening schedule and DST behavior on real hardware |

Do not publish personal SSIDs, passwords, network dumps or provisioning images in test reports. Internal local verification notes, if present, are excluded from Git.
