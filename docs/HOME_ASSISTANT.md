# Home Assistant integration

[Back to README](../README.md) · [MQTT protocol](PROTOCOL.md)

The `gas_reader` custom integration uses HA's existing MQTT connection. It is installed manually from source; no release ZIP or HACS setup is required.

Current integration source: **1.6.0**. The test environment is pinned to Home Assistant **2026.9.4 / Python 3.14**. This is a tested baseline, not a claim of compatibility with every older or newer HA release.

## Prerequisites and installation

1. Have an MQTT broker reachable by both HA and the ESP. Configure the built-in [HA MQTT integration](https://www.home-assistant.io/integrations/mqtt/).
2. Locate your HA configuration directory, often `/config` on HA OS/container systems. It is the directory containing `configuration.yaml`.
3. Create `custom_components` if necessary and copy the repository folder:

```text
homeassistant/custom_components/gas_reader/
                     ↓
<HA_CONFIG>/custom_components/gas_reader/
```

The result must be `<HA_CONFIG>/custom_components/gas_reader/manifest.json`, not an extra nested `gas_reader/gas_reader` directory. Use your HA file-transfer method; a local filesystem copy can look like:

```sh
mkdir -p /path/to/ha-config/custom_components
cp -R homeassistant/custom_components/gas_reader /path/to/ha-config/custom_components/
```

4. Restart HA.
5. Go to **Settings → Devices & services → Add integration → Gas Reader**.

| Input | Value |
|---|---|
| Device ID | Twelve lowercase hexadecimal characters from the six-byte Wi-Fi MAC, without colons; example `aabbccddeeff` |
| Initial meter reading | Physical meter baseline in m³, entered with a decimal point |
| Volume per pulse | Normally `0.01`; verify your meter's actual pulse constant |

Choose the baseline before any pending device pulses are added. The broker account alone is not enough: without a configured Gas Reader entry, the ESP receives no HA application ACK and its sync times out.

## Entities

| Entity | Purpose |
|---|---|
| Gas meter reading | Editable physical reading; begins at your configured baseline and increases with normal pulses |
| Gas consumption | Pulse-only cumulative consumption; use this for Energy, Utility Meter and consumption graphs |
| Set gas meter reading (m³) | Text field to correct the physical reading without changing consumption |
| Battery voltage | Last report's calibrated voltage, in V |
| Battery level (estimated) | Derived Li-ion voltage estimate, in % |
| Last contact | Time HA last committed the report |
| Firmware | Reported application version |
| Maintenance state | Sleeping / maintenance |
| Counter state | Counting fault indicator |

The physical reading deliberately has no statistics state class. Consumption uses `device_class: gas`, unit `m³`, `state_class: total`: it is a persistent cumulative quantity that is not reset by manual reading corrections. Both are updated atomically with the message checkpoint. HA sensor/statistics concepts are described in the [sensor documentation](https://developers.home-assistant.io/docs/core/entity/sensor/).

Sleep does not make the gas entities unavailable. Battery/last-contact values are snapshots, not live polling. Normal pulses trigger 30-minute syncs and the daily empty sync refreshes telemetry. During an outage, the last valid values remain visible.

## Energy and daily/monthly charts

Select **Gas consumption**, not **Gas meter reading**, as the gas source in HA's Energy dashboard or as a Utility Meter input. A manual meter correction is not gas usage.

Example statistics card; replace the entity ID with the actual consumption entity created by your HA:

```yaml
type: statistics-graph
title: Monthly gas consumption
chart_type: bar
period: month
days_to_show: 365
stat_types:
  - change
entities:
  - entity: sensor.your_gas_consumption
    name: Gas consumption
```

For daily bars use `period: day`, for example with `days_to_show: 30`. Statistics require HA's normal statistics processing before data appears.

Reports account for consumption when received. After an extended network outage, several days of pending usage can arrive in one report. Total volume is preserved for checkpointed packets, but individual pulse timestamps are not stored/reconstructed.

## Correct the physical reading

First synchronize pending device data and confirm the ESP pending count is zero. Then edit **Set gas meter reading (m³)** on the device page. Both decimal point and comma are accepted by the text field.

Alternatively use **Developer tools → Actions**:

```yaml
action: gas_reader.set_meter_reading
data:
  device_id: aabbccddeeff
  value: "1234.56"
```

The correction persists before the entity updates. It preserves consumption and MQTT deduplication. Changing the reading from 1000 to 2000 m³ does not add 1000 m³ to consumption; three subsequent pulses at the default scale add 0.03 m³ to both readings.

The ESP web page's **counter reset** is different: it discards pending ESP pulses, preserving the HA reading and previously processed consumption. It cannot undo gas that has already been accepted. See [discard behavior](PROTOCOL.md#discarding-pending-pulses).

## Battery percentage

This is a **generic, uncalibrated estimate** for a single 4.2 V Li-ion cell, computed in HA from `battery_mv`. The source curve in `battery.py` uses:

| V | % | V | % | V | % |
|---|---|---|---|---|---|
| 3.30 | 0 | 3.70 | 35 | 4.00 | 80 |
| 3.50 | 10 | 3.80 | 55 | 4.10 | 90 |
| 3.60 | 20 | 3.90 | 70 | 4.20 | 100 |

The implementation interpolates and rounds between points, clamping to 0–100%. Missing voltage stays unknown. `estimated: true` identifies the approximation. Cell type, temperature, aging and charging/load affect accuracy; 0% is not a claim that no physical energy remains. Use the voltage and last-contact entity alongside the estimate.

## Upgrading an existing integration

Back up HA, replace the `custom_components/gas_reader` source folder and restart. **Keep the existing integration entry and storage journal**; do not remove and re-add the device just to upgrade.

Version 1.6.0 separates consumption from the editable reading. When upgrading an older schema-1 journal:

- The meter reading, pulse factor and processed-packet identities remain intact.
- The new consumption counter starts at zero at migration. `consumption_since` records its start.
- Migration to schema 2 is saved before sensors/subscriptions start. Storage failure does not produce an ACK.
- Already processed packets replayed after migration remain duplicates; they do not become new consumption.
- Historical consumption cannot be recovered from a total that mixed corrections and pulses. Old HA graph/statistics jumps are not rewritten or deleted.
- Change Energy/chart sources to the new consumption entity; it will not contain the whole month preceding migration.

Older integration versions cannot read the schema-2 journal. If downgrading, restore a matching full backup and reconcile reports processed since that backup. The firmware's pulse storage format is independent and unchanged.

## Storage and troubleshooting

The durable journal is `<HA_CONFIG>/.storage/gas_reader.<device_id>.json`. Back it up with the HA configuration and database. It includes the physical reading, consumption, pulse factor, telemetry and per-instance deduplication checkpoints. Never manually edit/delete it to clear an error.

| Symptom | Check |
|---|---|
| Device joins Wi-Fi but sync fails | Broker host/1883, broker login, HA MQTT connection, installed/configured Gas Reader entry, topic ACLs |
| Battery/version looks old | A previously saved outbox is immutable; its retry keeps old telemetry. A fresh report refreshes it |
| Reading correction changes a graph | The graph should use Gas consumption, not the editable reading; old statistics are not retroactively repaired |
| Integration does not appear | Folder nesting, HA restart and HA logs |
| Journal/config error | Preserve files, inspect logs/backups and reconcile physical reading; do not erase automatically |

A successful report is acknowledged only after durable HA storage succeeds. MQTT broker PUBACK alone is not enough. The clock field used for daily scheduling is also sent only in successful HA ACKs.
