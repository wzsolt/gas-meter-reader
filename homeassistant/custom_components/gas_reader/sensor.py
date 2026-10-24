"""Push sensors remain available while the battery device sleeps."""
from datetime import datetime
from decimal import Decimal
from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import EntityCategory, PERCENTAGE, UnitOfVolume, UnitOfElectricPotential
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity
from .const import DOMAIN
from .battery import estimated_battery_percent


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([GasReaderSensor(entry.runtime_data, key) for key in
        ("gas", "consumption", "battery", "battery_percent", "last_seen", "firmware", "maintenance", "counter_fault")])


class GasReaderSensor(CoordinatorEntity, SensorEntity):
    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, coordinator, key):
        super().__init__(coordinator)
        self.key = key
        self._attr_unique_id = f"{coordinator.device_id}_{key}"
        self._attr_translation_key = key
        self._attr_device_info = DeviceInfo(identifiers={(DOMAIN, coordinator.device_id)},
            name=f"Gas Reader {coordinator.device_id}", manufacturer="DIY / Seeed Studio", model="XIAO ESP32-C6")
        if key in ("gas", "consumption"):
            self._attr_device_class = SensorDeviceClass.GAS
            self._attr_native_unit_of_measurement = UnitOfVolume.CUBIC_METERS
            self._attr_suggested_display_precision = 2
            # Only pulse consumption belongs in Energy/statistics. The physical
            # reading can be corrected in either direction without consuming gas.
            if key == "consumption":
                self._attr_state_class = SensorStateClass.TOTAL
        elif key == "battery":
            self._attr_device_class = SensorDeviceClass.VOLTAGE
            self._attr_state_class = SensorStateClass.MEASUREMENT
            self._attr_native_unit_of_measurement = UnitOfElectricPotential.VOLT
            self._attr_suggested_display_precision = 3
        elif key == "battery_percent":
            self._attr_device_class = SensorDeviceClass.BATTERY
            self._attr_state_class = SensorStateClass.MEASUREMENT
            self._attr_native_unit_of_measurement = PERCENTAGE
            self._attr_suggested_display_precision = 0
        elif key == "last_seen":
            self._attr_device_class = SensorDeviceClass.TIMESTAMP
        elif key == "maintenance":
            self._attr_device_class = SensorDeviceClass.ENUM
            self._attr_options = ["sleeping", "maintenance"]
        if key not in ("gas", "consumption"):
            self._attr_entity_category = EntityCategory.DIAGNOSTIC

    @property
    def native_value(self):
        state = self.coordinator.ledger.state
        if self.key == "gas":
            return Decimal(state["total_m3"])
        if self.key == "consumption":
            return Decimal(state["consumption_m3"])
        if self.key == "battery":
            return state.get("battery_mv") / 1000 if state.get("battery_mv") is not None else None
        if self.key == "battery_percent":
            return estimated_battery_percent(state.get("battery_mv"))
        if self.key == "last_seen":
            return datetime.fromisoformat(state["last_seen"]) if state.get("last_seen") else None
        if self.key == "firmware":
            return state.get("firmware")
        if self.key == "maintenance":
            return self.coordinator.mode
        return "fault" if self.coordinator.counter_fault else "ok"

    @property
    def extra_state_attributes(self):
        if self.key == "consumption":
            return {"pulse_m3": str(self.coordinator.ledger.pulse_m3),
                "consumption_since": self.coordinator.ledger.state["consumption_since"]}
        if self.key == "gas":
            return {"pulse_m3": str(self.coordinator.ledger.pulse_m3)}
        if self.key == "battery_percent":
            return {"estimated": True, "estimate_method": "li_ion_voltage_curve"}
        return None
