"""Immediate atomic journal. Unlike HA Store, disk errors must reach the caller."""
import json
import os
from pathlib import Path
import tempfile
from .ledger import ProtocolError


class DurableStorage:
    def __init__(self, hass, device_id):
        self.hass = hass
        self.path = Path(hass.config.path(".storage", f"gas_reader.{device_id}.json"))

    def _load(self):
        try:
            return json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return None
        except (ValueError, OSError) as exc:
            raise ProtocolError(f"Gas Reader journal could not be read; preserved at {self.path}") from exc

    async def load(self):
        return await self.hass.async_add_executor_job(self._load)

    def _save(self, state):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temp_name = tempfile.mkstemp(prefix=f".{self.path.name}.", dir=self.path.parent)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump(state, stream, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temp_name, self.path)
            directory = os.open(self.path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            try:
                os.unlink(temp_name)
            except FileNotFoundError:
                pass

    async def save(self, state):
        await self.hass.async_add_executor_job(self._save, state)
