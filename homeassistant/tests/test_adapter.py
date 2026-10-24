"""Exercise the adapter using real Home Assistant classes, isolated MQTT callbacks."""
import asyncio
from copy import deepcopy
import json
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch
import pytest

sys.path.insert(0, str(Path(__file__).parents[1]))
from homeassistant.core import HomeAssistant, callback
from homeassistant.config_entries import ConfigEntry, ConfigEntries
from homeassistant.components.sensor import SensorDeviceClass, SensorStateClass
from homeassistant.components.text import TextMode
from homeassistant.const import EntityCategory, Platform
from homeassistant.exceptions import ConfigEntryNotReady, ServiceValidationError
from custom_components.gas_reader import PLATFORMS, GasReaderCoordinator, async_setup_entry, async_unload_entry
from custom_components.gas_reader.config_flow import GasReaderConfigFlow
from custom_components.gas_reader.ledger import Ledger
from custom_components.gas_reader.sensor import GasReaderSensor, async_setup_entry as setup_sensors
from custom_components.gas_reader.battery import estimated_battery_percent
from custom_components.gas_reader.storage import DurableStorage
from custom_components.gas_reader.text import GasReaderMeterReading, async_setup_entry as setup_text

INSTANCE = "0123456789abcdef0123456789abcdef"
DEVICE = "aabbccddeeff"

def make_entry():
    return ConfigEntry(domain="gas_reader", title="Gas Reader", version=1, minor_version=1,
        data={"device_id": DEVICE, "initial_m3": "0", "pulse_m3": "0.01"}, options={},
        source="user", unique_id=DEVICE, discovery_keys={}, subentries_data=[])


def message(seq=1):
    return SimpleNamespace(retain=False, payload=json.dumps({"protocol": 1,
        "instance_id": INSTANCE, "sequence": str(seq), "pulses": "4", "battery_mv": 3910, "firmware": "1.0.0"}))


@pytest.mark.asyncio
async def test_estimated_battery_sensor_setup_report_and_restart(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("1234.56", "0.01", store.save)
    entry = make_entry()
    coordinator = GasReaderCoordinator(hass, entry, ledger)
    entry.runtime_data = coordinator
    added = []
    await setup_sensors(hass, entry, added.extend)
    battery = next(entity for entity in added if entity.key == "battery_percent")
    voltage = next(entity for entity in added if entity.key == "battery")
    assert battery.unique_id == f"{DEVICE}_battery_percent"
    assert voltage.unique_id == f"{DEVICE}_battery"  # Preserve the existing voltage entity.
    assert battery.device_info == voltage.device_info
    assert battery.device_class == SensorDeviceClass.BATTERY
    assert battery.state_class == SensorStateClass.MEASUREMENT
    assert battery.native_unit_of_measurement == "%"
    assert battery.entity_category == EntityCategory.DIAGNOSTIC
    assert battery.native_value is None and battery.available
    assert battery.extra_state_attributes["estimated"] is True
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock):
        await coordinator.report(message())
    assert battery.native_value == 71 and voltage.native_value == 3.91
    assert ledger.state["total_m3"] == "1234.60"
    restored = Ledger("0", "0.01", store.save, await store.load())
    restored_sensor = GasReaderSensor(GasReaderCoordinator(hass, make_entry(), restored), "battery_percent")
    assert restored_sensor.native_value == 71
    update = message(2)
    update.payload = json.dumps({**json.loads(update.payload), "battery_mv": 4058})
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock):
        await coordinator.report(update)
    assert battery.native_value == 86 and voltage.native_value == 4.058


def test_battery_estimate_bounds_interpolation_and_unknown():
    assert estimated_battery_percent(3300) == 0
    assert estimated_battery_percent(3000) == 0
    assert estimated_battery_percent(3500) == 10
    assert estimated_battery_percent(3700) == 35
    assert estimated_battery_percent(3750) == 45
    assert estimated_battery_percent(4058) == 86
    assert estimated_battery_percent(4200) == 100
    assert estimated_battery_percent(5000) == 100
    for missing in (None, True, "3910", 3910.0, -1, 0):
        assert estimated_battery_percent(missing) is None
    estimates = [estimated_battery_percent(mv) for mv in range(2500, 4501)]
    assert estimates == sorted(estimates)
    assert all(type(value) is int and 0 <= value <= 100 for value in estimates)


@pytest.mark.asyncio
async def test_ack_time_is_fresh_only_after_successful_commit(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    ledger = Ledger("0", "0.01", AsyncMock())
    coordinator = GasReaderCoordinator(hass, make_entry(), ledger)
    with patch("custom_components.gas_reader.time.time", return_value=1792080000), \
         patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        await coordinator.report(message())
        first=json.loads(publish.call_args.args[2])
        assert first["result"]=="ok" and first["utc_time"]==1792080000
        assert ledger.state["instances"][INSTANCE]["sequence"]=="1"
    with patch("custom_components.gas_reader.time.time", return_value=1792080060), \
         patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        await coordinator.report(message())
        duplicate=json.loads(publish.call_args.args[2])
        assert duplicate["result"]=="duplicate" and duplicate["utc_time"]==1792080060
        assert ledger.state["total_m3"]=="0.04"
        await coordinator.report(message(3))
        assert "utc_time" not in json.loads(publish.call_args.args[2])
    ledger._save=AsyncMock(side_effect=OSError("disk full"))
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        await coordinator.report(message(2))
        publish.assert_not_called()


@pytest.mark.asyncio
async def test_commit_precedes_ack_and_failure_sends_none(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    order = []
    async def save(data): order.append("save")
    async def publish(*args, **kwargs): order.append("ack")
    entry = make_entry()
    coordinator = GasReaderCoordinator(hass, entry, Ledger("0", "0.01", save))
    with patch("custom_components.gas_reader.mqtt.async_publish", side_effect=publish):
        await coordinator.report(message())
    assert order == ["save", "ack"]
    coordinator.ledger._save = AsyncMock(side_effect=OSError("disk full"))
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish_mock:
        await coordinator.report(message(2))
        publish_mock.assert_not_called()
    assert coordinator.ledger.state["total_m3"] == "0.04"


@pytest.mark.asyncio
@pytest.mark.parametrize("operation", ["discard", "reset"])
async def test_reset_durable_ack_restart_and_duplicate_preserve_reading(tmp_path, operation):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("1234.56", "0.01", store.save)
    await ledger.process(message().payload)
    reset = message(2)
    reset.payload = json.dumps({**json.loads(reset.payload), "operation": operation})
    coordinator = GasReaderCoordinator(hass, make_entry(), ledger)
    async def ack_after_save(hass_arg, topic, payload, **kwargs):
        assert topic == f"gas_reader/{DEVICE}/ack"
        assert kwargs == {"qos": 1, "retain": False}
        assert json.loads(payload)["result"] == "ok"
        saved = await store.load()
        assert saved["total_m3"] == "1234.60"
        assert saved["instances"][INSTANCE]["sequence"] == "2"
        assert str(GasReaderSensor(coordinator, "gas").native_value) == "1234.60"
        assert GasReaderMeterReading(coordinator).native_value == "1234.60"
    with patch("custom_components.gas_reader.mqtt.async_publish", side_effect=ack_after_save) as publish:
        await coordinator.report(reset)
        assert publish.await_count == 1
    restored = Ledger("0", "0.01", store.save, await store.load())
    await restored.correct("0.02")
    coordinator = GasReaderCoordinator(hass, make_entry(), restored)
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        await coordinator.report(reset)
        assert json.loads(publish.call_args.args[2])["result"] == "duplicate"
    assert (await store.load())["total_m3"] == "0.02"
    assert str(GasReaderSensor(coordinator, "gas").native_value) == "0.02"


@pytest.mark.asyncio
@pytest.mark.parametrize("operation", ["discard", "reset"])
async def test_reset_storage_failure_sends_no_ack(tmp_path, operation):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("1234.56", "0.01", store.save)
    await ledger.process(message().payload)
    before = await store.load()
    coordinator = GasReaderCoordinator(hass, make_entry(), ledger)
    reset = message(2)
    reset.payload = json.dumps({**json.loads(reset.payload), "operation": operation})
    with patch("custom_components.gas_reader.storage.os.replace", side_effect=OSError("write failure")), \
         patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        await coordinator.report(reset)
        publish.assert_not_called()
    assert ledger.state == before and await store.load() == before
    assert str(GasReaderSensor(coordinator, "gas").native_value) == "1234.60"


@pytest.mark.asyncio
async def test_sensors_sleep_status_and_retained_rejection(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    ledger = Ledger("1234.56", "0.01", AsyncMock())
    await ledger.process(message().payload)
    coordinator = GasReaderCoordinator(hass, make_entry(), ledger)
    gas = GasReaderSensor(coordinator, "gas")
    assert gas.device_class == SensorDeviceClass.GAS
    assert gas.state_class is None  # Editable physical reading is not a consumption statistic.
    assert gas.available
    assert str(gas.native_value) == "1234.60"
    assert GasReaderSensor(coordinator, "battery").native_value == 3.91
    assert GasReaderSensor(coordinator, "last_seen").native_value.tzinfo is not None
    status = SimpleNamespace(retain=False, payload=json.dumps({"protocol": 1, "instance_id": INSTANCE,
                            "mode": "maintenance", "counter_fault": False}))
    coordinator.status(status)
    assert GasReaderSensor(coordinator, "maintenance").native_value == "maintenance"
    assert gas.available
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        retained = message(); retained.retain = True
        await coordinator.report(retained)
        publish.assert_not_called()


@pytest.mark.asyncio
async def test_real_storage_restart_and_atomic_failure(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("0", "0.01", store.save)
    await ledger.process(message().payload)
    restored = Ledger("0", "0.01", store.save, await store.load())
    assert (await restored.process(message().payload))["result"] == "duplicate"
    before = await store.load()
    with patch("custom_components.gas_reader.storage.os.replace", side_effect=OSError("write failure")):
        with pytest.raises(OSError): await restored.process(message(2).payload)
    assert await store.load() == before
    assert restored.state["total_m3"] == "0.04"
    assert not list(store.path.parent.glob(".gas_reader.*"))


@pytest.mark.asyncio
async def test_corrupt_storage_preserved(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    store.path.parent.mkdir()
    store.path.write_text("corrupted")
    with pytest.raises(ValueError): await store.load()
    assert store.path.read_text() == "corrupted"


@pytest.mark.asyncio
async def test_setup_subscriptions_correction_and_unload(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    entry = make_entry()
    hass.config_entries = ConfigEntries(hass, {})
    subscriptions, removed = {}, []
    async def subscribe(hass_arg, topic, fn, qos):
        assert qos == 1
        subscriptions[topic] = fn
        return lambda: removed.append(topic)
    with patch("custom_components.gas_reader.mqtt.async_subscribe", side_effect=subscribe), \
         patch.object(hass.config_entries, "async_forward_entry_setups", new_callable=AsyncMock), \
         patch.object(hass.config_entries, "async_unload_platforms", new_callable=AsyncMock, return_value=True), \
         patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        assert await async_setup_entry(hass, entry)
        subscriptions[f"gas_reader/{DEVICE}/report"](message())
        await asyncio.gather(*entry.runtime_data.tasks)
        assert publish.await_count == 1
        await hass.services.async_call("gas_reader", "set_meter_reading", {"device_id": DEVICE, "value": "2000.00"}, blocking=True)
        assert entry.runtime_data.ledger.state["total_m3"] == "2000.00"
        assert await async_unload_entry(hass, entry)
        assert len(removed) == 2
        assert not hass.services.has_service("gas_reader", "set_meter_reading")


@pytest.mark.asyncio
async def test_config_flow_invalid_input():
    flow = GasReaderConfigFlow()
    result = await flow.async_step_user({"device_id": "wrong", "initial_m3": "-1", "pulse_m3": "0"})
    assert result["type"] == "form"
    assert result["errors"] == {"device_id": "invalid_device_id", "initial_m3": "invalid_decimal", "pulse_m3": "invalid_decimal"}


@pytest.mark.asyncio
async def test_editable_reading_persists_updates_sensor_and_keeps_dedup(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("1234.00", "0.01", store.save)
    await ledger.process(message().payload)
    entry = make_entry()
    entry_before = deepcopy(dict(entry.data))
    coordinator = GasReaderCoordinator(hass, entry, ledger)
    entry.runtime_data = coordinator
    added = []
    await setup_text(hass, entry, added.extend)
    assert PLATFORMS == [Platform.SENSOR, Platform.TEXT]
    editor = added[0]
    sensor = GasReaderSensor(coordinator, "gas")
    assert editor.mode == TextMode.TEXT
    assert editor.entity_category == EntityCategory.CONFIG
    assert editor.device_info["identifiers"] == sensor.device_info["identifiers"]
    assert editor.available and not editor.should_poll
    assert editor.native_value == "1234.04"
    instances_before = deepcopy(ledger.state["instances"])
    await editor.async_set_value("2000,123")
    assert editor.native_value == "2000.123"
    assert str(sensor.native_value) == "2000.123"
    assert coordinator.data["total_m3"] == "2000.123"
    assert (await store.load())["total_m3"] == "2000.123"
    assert ledger.state["instances"] == instances_before
    assert dict(entry.data) == entry_before
    assert (await ledger.process(message().payload))["result"] == "duplicate"
    assert editor.native_value == "2000.123"
    restored = Ledger("0", "0.01", store.save, await store.load())
    restored_editor = GasReaderMeterReading(GasReaderCoordinator(hass, make_entry(), restored))
    assert restored_editor.native_value == "2000.123"
    assert (await restored.process(message().payload))["result"] == "duplicate"


@pytest.mark.asyncio
@pytest.mark.parametrize("value", ["", "-1", "NaN", "Infinity", "1e3", "123abc", "1,2.3", "1.1234567890", "1000000000000001"])
async def test_editable_reading_rejects_invalid_values_without_save(tmp_path, value):
    hass = HomeAssistant(str(tmp_path))
    save = AsyncMock()
    coordinator = GasReaderCoordinator(hass, make_entry(), Ledger("1234.56", "0.01", save))
    editor = GasReaderMeterReading(coordinator)
    with pytest.raises(ServiceValidationError):
        await editor.async_set_value(value)
    save.assert_not_awaited()
    assert editor.native_value == "1234.56"


@pytest.mark.asyncio
async def test_editable_reading_disk_failure_preserves_state(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("0", "0.01", store.save)
    await ledger.process(message().payload)
    before = await store.load()
    coordinator = GasReaderCoordinator(hass, make_entry(), ledger)
    editor = GasReaderMeterReading(coordinator)
    with patch("custom_components.gas_reader.storage.os.replace", side_effect=OSError("disk failure")), \
         patch.object(coordinator, "async_set_updated_data") as notify:
        with pytest.raises(ServiceValidationError):
            await editor.async_set_value("2000.00")
        notify.assert_not_called()
    assert ledger.state == before
    assert await store.load() == before
    assert editor.native_value == "0.04"


@pytest.mark.asyncio
async def test_editable_reading_decimal_precision_and_serialized_report(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    correction_started, allow_commit = asyncio.Event(), asyncio.Event()
    saved = []
    async def save(state):
        if state["total_m3"] == "1234.123456789":
            correction_started.set()
            await allow_commit.wait()
        saved.append(deepcopy(state))
    ledger = Ledger("0", "0.01", save)
    await ledger.process(message().payload)
    coordinator = GasReaderCoordinator(hass, make_entry(), ledger)
    editor = GasReaderMeterReading(coordinator)
    correction = asyncio.create_task(editor.async_set_value("1234.123456789"))
    await correction_started.wait()
    assert editor.native_value == "0.04"  # No optimistic update before the durable save.
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock):
        report = asyncio.create_task(coordinator.report(message(2)))
        allow_commit.set()
        await asyncio.gather(correction, report)
    assert saved[-2]["total_m3"] == "1234.123456789"
    assert saved[-2]["consumption_m3"] == "0.04"
    assert editor.native_value == "1234.163456789"
    assert ledger.state["consumption_m3"] == "0.08"
    assert ledger.state["instances"][INSTANCE]["sequence"] == "2"
    assert (await ledger.process(message(2).payload))["result"] == "duplicate"
    assert editor.native_value == "1234.163456789"


@pytest.mark.asyncio
async def test_consumption_statistic_ignores_manual_reading_and_survives_restart(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("12345.67", "0.01", store.save)
    await ledger.initialize()
    entry = make_entry()
    coordinator = GasReaderCoordinator(hass, entry, ledger)
    entry.runtime_data = coordinator
    added = []
    await setup_sensors(hass, entry, added.extend)
    reading = next(sensor for sensor in added if sensor.key == "gas")
    consumption = next(sensor for sensor in added if sensor.key == "consumption")
    assert reading.unique_id == f"{DEVICE}_gas"  # Preserve the displayed reading entity.
    assert reading.state_class is None
    assert consumption.unique_id == f"{DEVICE}_consumption"
    assert consumption.device_info == reading.device_info
    assert consumption.entity_category is None
    assert consumption.device_class == SensorDeviceClass.GAS
    assert consumption.state_class == SensorStateClass.TOTAL
    assert consumption.last_reset is None
    assert consumption.native_unit_of_measurement == "m³"
    assert consumption.native_value == 0 and consumption.available
    since = consumption.extra_state_attributes["consumption_since"]
    editor = GasReaderMeterReading(coordinator)
    with patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock):
        await coordinator.report(message())
        assert str(consumption.native_value) == "0.04"
        for value in ("25000,12", "0", "10000.123456789"):
            await editor.async_set_value(value)
            assert str(consumption.native_value) == "0.04"
            assert (await store.load())["consumption_m3"] == "0.04"
        await coordinator.report(message())  # Lost ACK after manual correction.
        assert str(consumption.native_value) == "0.04"
        await coordinator.report(message(2))
    assert str(consumption.native_value) == "0.08"
    assert str(reading.native_value) == "10000.163456789"
    restored = Ledger("0", "0.01", store.save, await store.load())
    restored_sensor = GasReaderSensor(GasReaderCoordinator(hass, make_entry(), restored), "consumption")
    assert str(restored_sensor.native_value) == "0.08"
    assert restored_sensor.extra_state_attributes["consumption_since"] == since


@pytest.mark.asyncio
async def test_legacy_migration_is_durable_before_entities_and_mqtt(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    hass.config_entries = ConfigEntries(hass, {})
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("12345.67", "0.01", store.save)
    await ledger.process(message().payload)
    legacy = deepcopy(ledger.state)
    legacy["schema"] = 1
    del legacy["consumption_m3"], legacy["consumption_since"]
    await store.save(legacy)
    async def subscribe(*args, **kwargs):
        saved = await store.load()
        assert saved["schema"] == 2
        assert saved["consumption_m3"] == "0"
        assert saved["total_m3"] == "12345.71"
        assert saved["instances"] == legacy["instances"]
        return lambda: None
    entry = make_entry()
    with patch("custom_components.gas_reader.mqtt.async_subscribe", side_effect=subscribe), \
         patch.object(hass.config_entries, "async_forward_entry_setups", new_callable=AsyncMock), \
         patch.object(hass.config_entries, "async_unload_platforms", new_callable=AsyncMock, return_value=True), \
         patch("custom_components.gas_reader.mqtt.async_publish", new_callable=AsyncMock) as publish:
        assert await async_setup_entry(hass, entry)
        await entry.runtime_data.report(message())
        assert json.loads(publish.call_args.args[2])["result"] == "duplicate"
        assert entry.runtime_data.ledger.state["consumption_m3"] == "0"
        await entry.runtime_data.report(message(2))
        assert entry.runtime_data.ledger.state["consumption_m3"] == "0.04"
        assert await async_unload_entry(hass, entry)


@pytest.mark.asyncio
async def test_failed_migration_does_not_expose_unpersisted_baseline(tmp_path):
    hass = HomeAssistant(str(tmp_path))
    hass.config_entries = ConfigEntries(hass, {})
    store = DurableStorage(hass, DEVICE)
    ledger = Ledger("12345.67", "0.01", store.save)
    legacy = deepcopy(ledger.state)
    legacy["schema"] = 1
    del legacy["consumption_m3"], legacy["consumption_since"]
    await store.save(legacy)
    with patch("custom_components.gas_reader.storage.os.replace", side_effect=OSError("disk full")), \
         patch("custom_components.gas_reader.mqtt.async_subscribe", new_callable=AsyncMock) as subscribe, \
         patch.object(hass.config_entries, "async_forward_entry_setups", new_callable=AsyncMock) as setup:
        with pytest.raises(ConfigEntryNotReady):
            await async_setup_entry(hass, make_entry())
        subscribe.assert_not_awaited()
        setup.assert_not_awaited()
    assert await store.load() == legacy
    assert "gas_reader" not in hass.data
