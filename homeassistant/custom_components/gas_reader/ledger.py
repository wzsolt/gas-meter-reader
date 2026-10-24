"""Protocol and durable transaction logic; deliberately independent of Home Assistant."""
from __future__ import annotations

import asyncio
from copy import deepcopy
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation, localcontext
import hashlib
import json
import re
from collections.abc import Awaitable, Callable

UINT64_MAX = 2**64 - 1
INSTANCE = re.compile(r"[0-9a-f]{32}\Z")
INTEGER = re.compile(r"(?:0|[1-9][0-9]{0,19})\Z")


class ProtocolError(ValueError):
    """Malformed or inconsistent data must never change the meter."""


def decimal_value(value: str, *, positive: bool = False, maximum: Decimal = Decimal("1e15")) -> Decimal:
    if not isinstance(value, str) or len(value) > 32:
        raise ProtocolError("Invalid decimal value")
    try:
        number = Decimal(value)
    except InvalidOperation as exc:
        raise ProtocolError("Invalid decimal value") from exc
    if not number.is_finite() or number < 0 or (positive and number == 0) or number > maximum:
        raise ProtocolError("Decimal value out of range")
    if number.as_tuple().exponent < -9:
        raise ProtocolError("At most nine decimal places are supported")
    return number


def uint64(value: object) -> int:
    if not isinstance(value, str) or not INTEGER.fullmatch(value):
        raise ProtocolError("Counters must be canonical decimal strings")
    number = int(value)
    if number > UINT64_MAX:
        raise ProtocolError("Counter overflow")
    return number


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError("Duplicate JSON field")
        result[key] = value
    return result


def decode_report(payload: str) -> dict:
    if not isinstance(payload, str) or len(payload.encode("utf-8")) > 1024:
        raise ProtocolError("Report too large")
    try:
        report = json.loads(payload, object_pairs_hook=_unique_object)
    except (ValueError, RecursionError) as exc:
        raise ProtocolError("Invalid JSON") from exc
    fields = {
        "protocol", "instance_id", "sequence", "pulses", "battery_mv", "firmware"
    }
    if not isinstance(report, dict) or set(report) not in (fields, fields | {"operation"}):
        raise ProtocolError("Unexpected report fields")
    if "operation" in report and report["operation"] not in ("discard", "reset"):
        raise ProtocolError("Unsupported operation")
    if type(report["protocol"]) is not int or report["protocol"] != 1:
        raise ProtocolError("Unsupported protocol")
    if not isinstance(report["instance_id"], str) or not INSTANCE.fullmatch(report["instance_id"]):
        raise ProtocolError("Invalid instance")
    if uint64(report["sequence"]) == 0:
        raise ProtocolError("Sequence starts at one")
    uint64(report["pulses"])
    if type(report["battery_mv"]) is not int or not 1000 <= report["battery_mv"] <= 5000:
        raise ProtocolError("Invalid battery voltage")
    if not isinstance(report["firmware"], str) or not re.fullmatch(r"[A-Za-z0-9_.+-]{1,31}", report["firmware"]):
        raise ProtocolError("Invalid firmware version")
    return report


class Ledger:
    """One save commits meter reading + consumption + sequence before an ACK.

    Save must perform an immediate atomic durable write and propagate failures.
    Failed writes leave the live state unchanged. A retry after a successful
    write but lost ACK is recognized by the stored fingerprint.
    """

    def __init__(self, initial: str, pulse_m3: str, save: Callable[[dict], Awaitable[None]], state=None):
        self.pulse_m3 = decimal_value(pulse_m3, positive=True)
        self._save = save
        self._lock = asyncio.Lock()
        self.state = {
            "schema": 2, "total_m3": str(decimal_value(initial)),
            "consumption_m3": "0",
            "consumption_since": datetime.now(timezone.utc).isoformat(),
            "pulse_m3": str(self.pulse_m3), "instances": {},
            "battery_mv": None, "firmware": None, "last_seen": None,
        }
        self._needs_save = True
        if state is not None:
            self._restore(state)

    def _restore(self, state):
        if not isinstance(state, dict) or type(state.get("schema")) is not int or state["schema"] not in (1, 2):
            raise ProtocolError("Invalid stored ledger schema; storage was preserved")
        decimal_value(state.get("total_m3"), maximum=Decimal("1e30"))
        if decimal_value(state.get("pulse_m3"), positive=True) != self.pulse_m3:
            raise ProtocolError("Pulse factor differs from the stored ledger")
        if not isinstance(state.get("instances"), dict):
            raise ProtocolError("Invalid stored instances")
        for instance, record in state["instances"].items():
            if not INSTANCE.fullmatch(instance) or not isinstance(record, dict):
                raise ProtocolError("Invalid stored instance")
            if not uint64(record.get("sequence")) or not re.fullmatch(r"[0-9a-f]{64}", record.get("fingerprint", "")):
                raise ProtocolError("Invalid stored checkpoint")
        restored = deepcopy(state)
        if state["schema"] == 1:
            # Old totals include manual corrections; their pulse-only component
            # cannot be reconstructed. Keep the reading and dedup checkpoints.
            restored.update(schema=2, consumption_m3="0",
                consumption_since=self.state["consumption_since"])
        else:
            decimal_value(state.get("consumption_m3"), maximum=Decimal("1e30"))
            try:
                since = datetime.fromisoformat(state["consumption_since"])
                if since.tzinfo is None or since.utcoffset() is None:
                    raise ValueError("Missing timezone")
            except (KeyError, TypeError, ValueError) as exc:
                raise ProtocolError("Invalid consumption start time; storage was preserved") from exc
        self.state = restored
        self._needs_save = state["schema"] == 1

    async def initialize(self) -> None:
        """Persist the new baseline/migration before exposing entities or MQTT."""
        async with self._lock:
            if self._needs_save:
                await self._commit(self.state)

    async def _commit(self, candidate: dict) -> None:
        await self._save(candidate)
        self.state = candidate
        self._needs_save = False

    async def process(self, payload: str, *, retained: bool = False) -> dict:
        if retained:
            raise ProtocolError("Retained reports are not accepted")
        report = decode_report(payload)
        instance = report["instance_id"]
        sequence = uint64(report["sequence"])
        fingerprint = hashlib.sha256(json.dumps(report, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        ack = {"protocol": 1, "instance_id": instance, "sequence": report["sequence"]}
        async with self._lock:
            record = self.state["instances"].get(instance)
            last = uint64(record["sequence"]) if record else 0
            if sequence < last:
                return {**ack, "result": "stale"}
            if sequence == last:
                if record["fingerprint"] != fingerprint:
                    return {**ack, "result": "conflict"}
                result = "duplicate"
            elif sequence != last + 1:
                return {**ack, "result": "sequence_gap", "expected_sequence": str(last + 1)}
            else:
                result = "ok"
            candidate = deepcopy(self.state)
            if result == "ok":
                # Discard acknowledges removal of ESP pending pulses only.
                # Legacy reset packets must also preserve the current HA reading.
                if "operation" not in report:
                    with localcontext() as context:
                        context.prec = 64
                        volume = uint64(report["pulses"]) * self.pulse_m3
                        candidate["total_m3"] = str(Decimal(candidate["total_m3"]) + volume)
                        candidate["consumption_m3"] = str(Decimal(candidate["consumption_m3"]) + volume)
                candidate["instances"][instance] = {"sequence": report["sequence"], "fingerprint": fingerprint}
            candidate["battery_mv"] = report["battery_mv"]
            candidate["firmware"] = report["firmware"]
            candidate["last_seen"] = datetime.now(timezone.utc).isoformat()
            await self._commit(candidate)
            return {**ack, "result": result}

    async def correct(self, value: str) -> None:
        reading = decimal_value(value)
        async with self._lock:
            candidate = deepcopy(self.state)
            candidate["total_m3"] = str(reading)
            # A physical meter correction never changes pulse consumption.
            await self._commit(candidate)
