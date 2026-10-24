# MQTT protocol and persistence

[Back to README](../README.md) · [Home Assistant](HOME_ASSISTANT.md)

## Identity and topics

`device_id` is the six-byte Wi-Fi MAC rendered as twelve lowercase hex characters. `instance_id` is a random 32-character lowercase hex string generated on first initialization and saved in NVS. OTA/battery replacement preserve it when NVS is preserved; reprovisioning replaces it.

| Topic | Publisher | Purpose |
|---|---|---|
| `gas_reader/<device_id>/report` | ESP | Consumption/telemetry or pending-pulse discard |
| `gas_reader/<device_id>/ack` | HA | Application commit acknowledgement |
| `gas_reader/<device_id>/status` | ESP | Maintenance/sleep and counting fault |

Messages use QoS 1 and are not retained. Retained reports and retained ACKs are rejected. The ESP subscribes to its ACK topic before publishing a report. Broker PUBACK does not acknowledge HA processing.

## Report

```json
{
  "protocol": 1,
  "instance_id": "0123456789abcdef0123456789abcdef",
  "sequence": "1",
  "pulses": "4",
  "battery_mv": 3910,
  "firmware": "1.5.0"
}
```

Sequence and pulse count are canonical decimal **strings**, without signs or leading zeroes; maximum UINT64_MAX. Sequence starts at 1. Voltage is an integer from 1000 to 5000 mV. Firmware is a 1–31-character version string. Report size is limited to 1024 UTF-8 bytes. Unknown fields/operations and duplicate JSON keys are rejected.

The normal report has exactly those six fields. A discard adds only `"operation":"discard"`. Legacy `"operation":"reset"` is accepted by the current HA integration without zeroing its totals. The full report, including operation, participates in its fingerprint.

Battery percentage is derived in HA from `battery_mv`; there is no extra percentage field. A saved packet keeps its original voltage/version on retry.

## ACK and time

```json
{
  "protocol": 1,
  "instance_id": "0123456789abcdef0123456789abcdef",
  "sequence": "1",
  "result": "ok",
  "utc_time": 1792080000
}
```

| Result | Meaning |
|---|---|
| `ok` | New packet committed |
| `duplicate` | Same latest packet fingerprint already committed |
| `conflict` | Same latest sequence with different content |
| `sequence_gap` | Out-of-order sequence; `expected_sequence` identifies the next expected value |
| `stale` | Packet predates the latest committed sequence |

Only matching instance/active sequence and `ok`/`duplicate` can clear the ESP outbox. Delayed ACKs for a previous packet cannot clear a newer one.

The HA adapter adds a fresh integer `utc_time` after successful durable processing, including duplicates. It does not add time to rejected results. The ESP accepts the optional time only on a matching successful ACK and within 2024–2100. Old firmware ignores it; old HA ACKs without it still work, with the device's 24-hour daily fallback.

Example status:

```json
{"protocol":1,"instance_id":"0123456789abcdef0123456789abcdef","mode":"maintenance","counter_fault":false}
```

## Transaction order

1. ESP snapshots pending pulses into a new sequenced outbox. Count, next sequence and immutable packet are saved as one NVS blob before publication.
2. ESP connects, subscribes to ACK and sends the packet. Pulses continue to accumulate while awake.
3. HA serializes processing for the device and checks instance/sequence/fingerprint.
4. For a new normal packet, HA adds `pulses × pulse_m3` to both physical `total_m3` and pulse-only `consumption_m3`, using decimal arithmetic. Discard/reset packets add nothing.
5. HA saves both totals, telemetry and deduplication checkpoint together: temporary file, file fsync, atomic replace and directory fsync. Storage errors propagate; uncommitted state is not ACKed.
6. HA publishes ACK only after the durable commit.
7. ESP subtracts only the acknowledged packet's pulses and clears the outbox in one NVS checkpoint. Pulses that arrived after the snapshot remain pending.
8. The same sync drains remaining pulses through new packets on the same connection, under one shared 30-second deadline. A daily sync also obtains a fresh report after resolving an older immutable packet, even if the new report has zero pulses.

Connection loss, ACK loss or checkpoint failure preserves the current outbox for replay. Replayed packets cannot be added twice. HA keeps the latest sequence/fingerprint per instance; old instance checkpoints are retained too.

## Discarding pending pulses

The authenticated `POST /api/reset` requires an empty body, `X-Gas-Reader-Reset: confirm` and an open maintenance window. The UI asks for confirmation.

An older normal outbox is resolved unchanged first: it might already have been committed with only the ACK lost. If not yet processed, its pulses may still be added. The remaining pending snapshot is then saved as a new discard packet. An already pending discard is retried unchanged.

HA durably records the discard sequence/fingerprint, leaving meter reading and consumption untouched. After successful ACK and ESP checkpoint, only that snapshot is removed. Newly arriving pulses remain for normal sync. The discard does not undo previously accepted usage or erase history, credentials, counting fault flags or duplicate protection.

Persistent outbox tags are 0 for normal, 1 for legacy reset and 2 for discard. The pulse/NVS schema remains 1. Keep a saved legacy packet unchanged to preserve its fingerprint. A pre-fix HA 1.3.0 legacy reset may already have zeroed a reading; the current integration does not automatically recover that historical reading. Reconcile it manually.

## Schedules and sleep

Ordinary consumption sync uses a 30-minute deadline, preserved across reed wakes. An empty ordinary timer does not start radio. Daily sync is independent: 23:55 Hungarian CET/CEST time, or every 24 hours without a valid HA clock. Cold boot/OTA reboot makes one bounded sync attempt to learn time. Failed daily/startup attempts retry after 30 minutes.

A separate CRC-protected RTC record contains the daily deadline, retry and UTC anchor. It survives deep sleep but is reinitialized on cold boot. The UTC anchor does not step the internal counting/network deadline clock. Time zone is `CET-1CEST,M3.5.0/2,M10.5.0/3`; local calendar conversion handles the 23/25-hour DST days. Other locations require a firmware time-zone change.

The sleep timer uses the earlier of the normal and daily deadlines. Daytime syncs/reed wakes do not postpone evening sync. Active maintenance/OTA is not interrupted; a due daily sync runs after maintenance finishes. A failed manual zero-pulse request alone does not start extra radio retries between scheduled daily attempts.

## Storage and limits

Firmware pulse state is versioned/CRC protected in RTC, checkpointed to NVS before sending, after ACK, around OTA and before maintenance exit. Normal reed pulses do not write flash. The separate daily schedule is RTC-only.

HA schema 2 stores physical reading, pulse-only consumption and per-instance deduplication in `.storage/gas_reader.<device_id>.json`. Migration from schema 1 preserves reading/identities, starts consumption at zero and commits before accepting MQTT. The old mixed reading cannot reconstruct historical pulse-only consumption. Back up this journal with HA configuration/database; reverting it independently requires reconciliation.

Power loss may lose uncheckpointed RTC pulses. Reset/reboot creates a short blind interval. The IRAM edge buffer holds 255 events; overflow sets `counter_fault` and prevents preparing new consumption packets. Neither firmware nor HA automatically erase damaged storage.

These guarantees apply to checkpointed packet replay, not lossless capture under every power/hardware fault. Consumption is attributed to report-arrival time, not original pulse timestamps. MQTT/HTTP are local plaintext services; see [Configuration](CONFIGURATION.md).
