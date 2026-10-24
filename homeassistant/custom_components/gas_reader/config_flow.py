"""Create a locally configured MQTT meter."""
import re
import voluptuous as vol
from homeassistant import config_entries
from .const import DOMAIN, CONF_DEVICE_ID, CONF_INITIAL_M3, CONF_PULSE_M3
from .ledger import decimal_value, ProtocolError


class GasReaderConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            device_id = user_input[CONF_DEVICE_ID].strip().lower()
            if not re.fullmatch(r"[0-9a-f]{12}", device_id):
                errors[CONF_DEVICE_ID] = "invalid_device_id"
            for key in (CONF_INITIAL_M3, CONF_PULSE_M3):
                try:
                    decimal_value(user_input[key], positive=key == CONF_PULSE_M3)
                except ProtocolError:
                    errors[key] = "invalid_decimal"
            if not errors:
                await self.async_set_unique_id(device_id)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title=f"Gas Reader {device_id}", data={**user_input, CONF_DEVICE_ID: device_id})
        return self.async_show_form(step_id="user", data_schema=vol.Schema({
            vol.Required(CONF_DEVICE_ID): str,
            vol.Required(CONF_INITIAL_M3, default="0"): str,
            vol.Required(CONF_PULSE_M3, default="0.01"): str,
        }), errors=errors)
