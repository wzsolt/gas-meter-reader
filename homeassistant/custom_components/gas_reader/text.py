"""Editable meter reading on the Gas Reader device's configuration section."""
from decimal import Decimal
import re

from homeassistant.components.text import TextEntity, TextMode
from homeassistant.const import EntityCategory
from homeassistant.exceptions import ServiceValidationError
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN

METER_READING_PATTERN = r"^[0-9]+([.,][0-9]{1,9})?$"


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([GasReaderMeterReading(entry.runtime_data)])


class GasReaderMeterReading(CoordinatorEntity, TextEntity):
    _attr_has_entity_name = True
    _attr_should_poll = False
    _attr_translation_key = "meter_reading"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_icon = "mdi:counter"
    _attr_mode = TextMode.TEXT
    _attr_native_min = 1
    _attr_native_max = 64
    _attr_pattern = METER_READING_PATTERN

    def __init__(self, coordinator):
        super().__init__(coordinator)
        self._attr_unique_id = f"{coordinator.device_id}_meter_reading"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, coordinator.device_id)},
            name=f"Gas Reader {coordinator.device_id}",
            manufacturer="DIY / Seeed Studio", model="XIAO ESP32-C6",
        )

    @property
    def native_value(self):
        # The durable ledger is the sole source of truth, also after a HA restart.
        return format(Decimal(self.coordinator.ledger.state["total_m3"]), "f")

    async def async_set_value(self, value: str) -> None:
        if not isinstance(value, str) or not re.fullmatch(METER_READING_PATTERN, value):
            raise ServiceValidationError(
                translation_domain=DOMAIN, translation_key="invalid_meter_reading"
            )
        await self.coordinator.async_set_meter_reading(value.replace(",", "."))
