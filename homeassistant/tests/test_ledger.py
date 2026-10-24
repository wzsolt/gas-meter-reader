import asyncio
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import unittest

path = Path(__file__).parents[1] / "custom_components/gas_reader/ledger.py"
spec = importlib.util.spec_from_file_location("gas_reader_ledger", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
Ledger, ProtocolError = module.Ledger, module.ProtocolError
INSTANCE = "0123456789abcdef0123456789abcdef"


def packet(seq=1, pulses=4, **changes):
    report = {"protocol": 1, "instance_id": INSTANCE, "sequence": str(seq),
              "pulses": str(pulses), "battery_mv": 3910, "firmware": "1.0.0"}
    report.update(changes)
    return json.dumps(report)


class LedgerTests(unittest.IsolatedAsyncioTestCase):
    async def test_reset_ack_and_new_consumption(self):
        await self.ledger.process(packet())
        reset = packet(seq=2, pulses=5, operation="discard")
        self.assertEqual((await self.ledger.process(reset))["result"], "ok")
        self.assertEqual(self.disk["total_m3"], "1234.60")
        self.assertEqual(self.disk["instances"][INSTANCE]["sequence"], "2")
        await self.ledger.process(packet(seq=3, pulses=2))
        self.assertEqual(self.disk["total_m3"], "1234.62")
        self.assertEqual((await self.ledger.process(reset))["result"], "stale")
        self.assertEqual(self.disk["total_m3"], "1234.62")

    async def test_reset_lost_ack_preserves_later_consumption(self):
        reset = packet(pulses=5, operation="discard")
        await self.ledger.process(reset)
        await self.ledger.process(packet(instance_id="f" * 32, pulses=2))
        restarted = Ledger("0", "0.01", self.save, self.disk)
        self.assertEqual((await restarted.process(reset))["result"], "duplicate")
        self.assertEqual(self.disk["total_m3"], "1234.58")
        self.assertEqual((await restarted.process(packet(pulses=5)))["result"], "conflict")

    async def test_reset_save_failure_does_not_ack_or_change_reading(self):
        before = deepcopy(self.ledger.state)
        async def fail(data): raise OSError("disk full")
        self.ledger._save = fail
        with self.assertRaises(OSError): await self.ledger.process(packet(operation="discard"))
        self.assertEqual(self.ledger.state, before)
        self.ledger._save = self.save
        await self.ledger.process(packet(operation="discard"))
        self.assertEqual(self.disk["total_m3"], "1234.56")

    async def test_legacy_reset_preserves_reading_and_fingerprint(self):
        legacy = packet(operation="reset")
        self.assertEqual((await self.ledger.process(legacy))["result"], "ok")
        self.assertEqual(self.disk["total_m3"], "1234.56")
        fingerprint = self.disk["instances"][INSTANCE]["fingerprint"]
        await self.ledger.correct("2000")
        restarted = Ledger("0", "0.01", self.save, self.disk)
        self.assertEqual((await restarted.process(legacy))["result"], "duplicate")
        self.assertEqual(self.disk["total_m3"], "2000")
        self.assertEqual(self.disk["instances"][INSTANCE]["fingerprint"], fingerprint)
        self.assertEqual((await restarted.process(packet(operation="discard")))["result"], "conflict")

    async def test_reset_unknown_operation_and_sequence_gap_rejected(self):
        for value in [True, "report", "erase", None]:
            with self.assertRaises(ProtocolError): await self.ledger.process(packet(operation=value))
        self.assertEqual((await self.ledger.process(packet(seq=2, operation="discard")))["result"], "sequence_gap")
        self.assertIsNone(self.disk)

    async def asyncSetUp(self):
        self.disk = None
        self.writes = 0
        async def save(data):
            self.disk = deepcopy(data)
            self.writes += 1
        self.save = save
        self.ledger = Ledger("1234.56", "0.01", save)

    async def test_count_and_duplicate(self):
        self.assertEqual((await self.ledger.process(packet()))["result"], "ok")
        self.assertEqual(self.disk["total_m3"], "1234.60")
        self.assertEqual(self.disk["consumption_m3"], "0.04")
        self.assertEqual((await self.ledger.process(packet()))["result"], "duplicate")
        self.assertEqual(self.disk["total_m3"], "1234.60")

    async def test_restart_after_lost_ack(self):
        await self.ledger.process(packet())
        restarted = Ledger("0", "0.01", self.save, self.disk)
        self.assertEqual((await restarted.process(packet()))["result"], "duplicate")
        self.assertEqual(restarted.state["total_m3"], "1234.60")
        await restarted.process(packet(seq=2, pulses=2))
        self.assertEqual(restarted.state["total_m3"], "1234.62")

    async def test_save_failure_keeps_live_state(self):
        old = deepcopy(self.ledger.state)
        async def fail(data):
            raise OSError("disk full")
        self.ledger._save = fail
        with self.assertRaises(OSError):
            await self.ledger.process(packet())
        self.assertEqual(self.ledger.state, old)
        self.ledger._save = self.save
        self.assertEqual((await self.ledger.process(packet()))["result"], "ok")

    async def test_failure_after_durable_write_recovers(self):
        async def uncertain_write(data):
            await self.save(data)
            raise OSError("directory fsync interrupted")
        self.ledger._save = uncertain_write
        with self.assertRaises(OSError):
            await self.ledger.process(packet())
        restarted = Ledger("0", "0.01", self.save, self.disk)
        self.assertEqual((await restarted.process(packet()))["result"], "duplicate")
        self.assertEqual(restarted.state["total_m3"], "1234.60")

    async def test_concurrent_duplicates_are_serialized(self):
        replies = await asyncio.gather(*(self.ledger.process(packet()) for _ in range(20)))
        self.assertEqual(sum(r["result"] == "ok" for r in replies), 1)
        self.assertEqual(self.ledger.state["total_m3"], "1234.60")

    async def test_changed_batch_is_conflict(self):
        await self.ledger.process(packet())
        old = deepcopy(self.disk)
        for data in (packet(pulses=5), packet(battery_mv=3900), packet(firmware="2.0.0")):
            self.assertEqual((await self.ledger.process(data))["result"], "conflict")
        self.assertEqual(self.disk, old)

    async def test_gap_and_stale_never_add(self):
        self.assertEqual((await self.ledger.process(packet(seq=2)))["result"], "sequence_gap")
        self.assertIsNone(self.disk)
        await self.ledger.process(packet())
        await self.ledger.process(packet(seq=2))
        self.assertEqual((await self.ledger.process(packet()))["result"], "stale")
        self.assertEqual(self.disk["total_m3"], "1234.64")

    async def test_zero_pulse_status(self):
        await self.ledger.process(packet(pulses=0))
        self.assertEqual(self.disk["total_m3"], "1234.56")
        self.assertEqual(self.disk["battery_mv"], 3910)
        self.assertIsNotNone(self.disk["last_seen"])

    async def test_new_instance_and_old_duplicate(self):
        await self.ledger.process(packet())
        await self.ledger.process(packet(instance_id="f" * 32))
        self.assertEqual((await self.ledger.process(packet()))["result"], "duplicate")
        self.assertEqual(self.disk["total_m3"], "1234.64")

    async def test_correction_keeps_duplicate_ledger(self):
        await self.ledger.process(packet())
        await self.ledger.correct("2000.12")
        await self.ledger.process(packet())
        self.assertEqual(self.disk["total_m3"], "2000.12")
        await self.ledger.process(packet(seq=2, pulses=1))
        self.assertEqual(self.disk["total_m3"], "2000.13")

    async def test_exact_large_counters(self):
        self.ledger = Ledger("0", "0.01", self.save)
        await self.ledger.process(packet(pulses=2**53 + 1))
        self.assertEqual(self.disk["total_m3"], "90071992547409.93")
        self.assertEqual(self.disk["consumption_m3"], "90071992547409.93")

    async def test_corrections_and_initial_reading_never_become_consumption(self):
        self.assertEqual(self.ledger.state["consumption_m3"], "0")
        await self.ledger.initialize()
        since = self.disk["consumption_since"]
        await self.ledger.process(packet(pulses=7))
        checkpoints = deepcopy(self.disk["instances"])
        for reading in ("20000.123456789", "1", "0", "1234.56"):
            await self.ledger.correct(reading)
            self.assertEqual(self.disk["total_m3"], reading)
            self.assertEqual(self.disk["consumption_m3"], "0.07")
            self.assertEqual(self.disk["instances"], checkpoints)
            self.assertEqual(self.disk["consumption_since"], since)
        restarted = Ledger("9999", "0.01", self.save, self.disk)
        self.assertEqual((await restarted.process(packet(pulses=7)))["result"], "duplicate")
        await restarted.process(packet(seq=2, pulses=3))
        self.assertEqual(self.disk["total_m3"], "1234.59")
        self.assertEqual(self.disk["consumption_m3"], "0.10")
        self.assertEqual(self.disk["consumption_since"], since)

    async def test_rejected_duplicate_zero_and_discard_never_add_consumption(self):
        await self.ledger.process(packet())
        for report, result in ((packet(), "duplicate"), (packet(pulses=9), "conflict"),
                               (packet(seq=3), "sequence_gap")):
            self.assertEqual((await self.ledger.process(report))["result"], result)
            self.assertEqual(self.disk["consumption_m3"], "0.04")
        await self.ledger.process(packet(seq=2, pulses=0))
        await self.ledger.process(packet(seq=3, pulses=99, operation="discard"))
        await self.ledger.process(packet(seq=4, pulses=99, operation="reset"))
        self.assertEqual((await self.ledger.process(packet()))["result"], "stale")
        self.assertEqual(self.disk["consumption_m3"], "0.04")
        await self.ledger.process(packet(instance_id="f" * 32, pulses=2))
        self.assertEqual(self.disk["consumption_m3"], "0.06")

    async def test_migrate_legacy_reading_preserves_dedup_without_inventing_consumption(self):
        await self.ledger.process(packet())
        await self.ledger.correct("20000")  # Cannot infer pulse history from the reading.
        legacy = deepcopy(self.disk)
        legacy["schema"] = 1
        del legacy["consumption_m3"], legacy["consumption_since"]
        original = deepcopy(legacy)
        migrated = Ledger("0", "0.01", self.save, legacy)
        self.assertEqual(legacy, original)
        self.assertEqual(migrated.state["total_m3"], "20000")
        self.assertEqual(migrated.state["consumption_m3"], "0")
        await migrated.initialize()
        self.assertEqual(self.disk["schema"], 2)
        self.assertEqual(self.disk["instances"], original["instances"])
        since, writes = self.disk["consumption_since"], self.writes
        restarted = Ledger("0", "0.01", self.save, self.disk)
        await restarted.initialize()
        self.assertEqual(self.writes, writes)  # Already migrated; baseline must not move.
        self.assertEqual((await restarted.process(packet()))["result"], "duplicate")
        await restarted.process(packet(seq=2, pulses=2))
        self.assertEqual(self.disk["total_m3"], "20000.02")
        self.assertEqual(self.disk["consumption_m3"], "0.02")
        self.assertEqual(self.disk["consumption_since"], since)

    async def test_migration_save_failure_remains_retryable(self):
        async def fail(data): raise OSError("disk full")
        self.ledger._save = fail
        with self.assertRaises(OSError): await self.ledger.initialize()
        self.assertIsNone(self.disk)
        self.ledger._save = self.save
        await self.ledger.initialize()
        self.assertEqual(self.disk, self.ledger.state)

    async def test_corrupt_consumption_is_not_silently_zeroed(self):
        await self.ledger.initialize()
        for field, value in (("consumption_m3", None), ("consumption_m3", "-1"),
                             ("consumption_m3", "NaN"), ("consumption_since", None),
                             ("consumption_since", "not-a-date"),
                             ("consumption_since", "2026-10-15T12:00:00")):
            corrupt = deepcopy(self.disk)
            corrupt[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ProtocolError):
                Ledger("0", "0.01", self.save, corrupt)

    async def test_invalid_reports_and_retained(self):
        variants = [packet(protocol=True), packet(sequence=1), packet(pulses=-1), packet(pulses=True),
                    packet(instance_id="bad"), packet(battery_mv=0), packet(battery_mv=True),
                    packet(firmware="<script>"), packet(pulses="01"), packet(pulses=str(2**64)),
                    packet(sequence="0"), packet(extra="unexpected"), "[]", "{}", "not JSON",
                    packet().replace('"protocol": 1', '"protocol": 1, "protocol": 1')]
        for invalid in variants:
            with self.subTest(invalid=invalid), self.assertRaises(ProtocolError):
                await self.ledger.process(invalid)
        with self.assertRaises(ProtocolError):
            await self.ledger.process(packet(), retained=True)
        self.assertEqual(self.writes, 0)

    async def test_corrupt_storage_and_changed_factor(self):
        await self.ledger.process(packet())
        corrupt = deepcopy(self.disk); corrupt["schema"] = 99
        with self.assertRaises(ProtocolError):
            Ledger("0", "0.01", self.save, corrupt)
        with self.assertRaises(ProtocolError):
            Ledger("0", "0.1", self.save, self.disk)

    async def test_invalid_correction(self):
        for value in ("NaN", "Infinity", "-1", "1e99", "0.0000000001"):
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                await self.ledger.correct(value)


if __name__ == "__main__":
    unittest.main()
