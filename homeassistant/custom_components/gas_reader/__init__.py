"""Home Assistant adapter for the durable MQTT ledger."""
from __future__ import annotations
import asyncio
import json
import logging
import time
import voluptuous as vol
from homeassistant.components import mqtt
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, ServiceCall, callback
from homeassistant.config_entries import ConfigEntry
from homeassistant.exceptions import ConfigEntryError, ConfigEntryNotReady, ServiceValidationError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator
from .const import DOMAIN, CONF_DEVICE_ID, CONF_INITIAL_M3, CONF_PULSE_M3
from .ledger import Ledger, ProtocolError
from .storage import DurableStorage

_LOGGER = logging.getLogger(__name__)
PLATFORMS = [Platform.SENSOR, Platform.TEXT]


class GasReaderCoordinator(DataUpdateCoordinator):
    def __init__(self, hass, entry, ledger):
        super().__init__(hass, _LOGGER, name=entry.title, config_entry=entry)
        self.ledger = ledger
        self.device_id = entry.data[CONF_DEVICE_ID]
        self.mode = "sleeping"
        self.counter_fault = False
        self.tasks = set()
        self.unsubscribers = []
        self.async_set_updated_data(ledger.state)

    async def async_set_meter_reading(self, value: str) -> None:
        """Persist a correction before notifying the sensor and editable field."""
        try:
            await self.ledger.correct(value)
        except ProtocolError as exc:
            raise ServiceValidationError(
                translation_domain=DOMAIN, translation_key="invalid_meter_reading"
            ) from exc
        except OSError as exc:
            raise ServiceValidationError(
                translation_domain=DOMAIN, translation_key="meter_reading_save_failed"
            ) from exc
        self.async_set_updated_data(self.ledger.state)

    async def report(self, message):
        try:
            ack = await self.ledger.process(message.payload, retained=message.retain)
            self.async_set_updated_data(self.ledger.state)
            if ack["result"] not in ("ok", "duplicate"):
                _LOGGER.error("Gas Reader %s rejected report: %s", self.device_id, ack["result"])
            else:
                ack["utc_time"] = int(time.time())
            await mqtt.async_publish(self.hass, f"gas_reader/{self.device_id}/ack", json.dumps(ack), qos=1, retain=False)
        except ProtocolError as exc:
            _LOGGER.warning("Gas Reader %s invalid report: %s", self.device_id, exc)
        except Exception:
            # A storage failure must never acknowledge an uncommitted total.
            _LOGGER.exception("Gas Reader %s could not commit/publish; ESP will retry", self.device_id)

    @callback
    def status(self, message):
        if message.retain:
            return
        try:
            status = json.loads(message.payload)
            if status.get("protocol") != 1 or status.get("instance_id") not in self.ledger.state["instances"]:
                return
            if status.get("mode") not in ("maintenance", "sleeping") or type(status.get("counter_fault")) is not bool:
                return
            self.mode = status["mode"]
            self.counter_fault = status["counter_fault"]
            self.async_set_updated_data(self.ledger.state)
        except (ValueError, AttributeError):
            _LOGGER.warning("Gas Reader %s invalid status", self.device_id)

    @callback
    def enqueue(self, message):
        task = self.hass.async_create_task(self.report(message), "Gas Reader report")
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    store = DurableStorage(hass, entry.data[CONF_DEVICE_ID])
    try:
        ledger = Ledger(entry.data[CONF_INITIAL_M3], entry.data[CONF_PULSE_M3], store.save, await store.load())
        await ledger.initialize()
    except ProtocolError as exc:
        raise ConfigEntryError(str(exc)) from exc
    except OSError as exc:
        raise ConfigEntryNotReady("Gas Reader journal could not be saved") from exc
    coordinator = GasReaderCoordinator(hass, entry, ledger)
    entry.runtime_data = coordinator
    hass.data.setdefault(DOMAIN, {})[entry.data[CONF_DEVICE_ID]] = coordinator
    try:
        coordinator.unsubscribers.append(await mqtt.async_subscribe(hass,
            f"gas_reader/{coordinator.device_id}/report", coordinator.enqueue, qos=1))
        coordinator.unsubscribers.append(await mqtt.async_subscribe(hass,
            f"gas_reader/{coordinator.device_id}/status", coordinator.status, qos=1))
        await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    except Exception:
        for unsubscribe in coordinator.unsubscribers:
            unsubscribe()
        hass.data[DOMAIN].pop(coordinator.device_id, None)
        raise

    if not hass.services.has_service(DOMAIN, "set_meter_reading"):
        async def correct(call: ServiceCall):
            reader = hass.data.get(DOMAIN, {}).get(call.data[CONF_DEVICE_ID])
            if reader is None:
                raise ServiceValidationError("Gas Reader device is not loaded")
            await reader.async_set_meter_reading(call.data["value"])
        hass.services.async_register(DOMAIN, "set_meter_reading", correct, schema=vol.Schema({
            vol.Required(CONF_DEVICE_ID): str, vol.Required("value"): str,
        }))
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    if not await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        return False
    coordinator = entry.runtime_data
    for unsubscribe in coordinator.unsubscribers:
        unsubscribe()
    # Finish any durable transaction before releasing the entry; do not cancel a save midway.
    if coordinator.tasks:
        await asyncio.gather(*coordinator.tasks, return_exceptions=True)
    hass.data[DOMAIN].pop(coordinator.device_id, None)
    if not hass.data[DOMAIN]:
        hass.services.async_remove(DOMAIN, "set_meter_reading")
    return True
