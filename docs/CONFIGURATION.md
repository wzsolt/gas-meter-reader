# Configuration and private credentials

[Back to README](../README.md) · [ESP-IDF](ESP_IDF.md)

`config.example.json` is the public template. **`config.local.json` is your private copy and is ignored by Git.** Credentials are written to NVS for the initial installation, not compiled into the application.

## Create your configuration

```sh
cp -n config.example.json config.local.json
chmod 600 config.local.json
```

Edit the copy with a text editor. Do not commit it, generated provisioning files, screenshots of credentials or exported HA configuration.

The template contains placeholders, not working credentials:

```json
{
  "ssid": "CHANGE_ME_WIFI",
  "wifi_password": "CHANGE_ME_WIFI_PASSWORD",
  "fallback_ssid": "",
  "fallback_wifi_password": "",
  "mqtt_uri": "mqtt://192.168.1.10:1883",
  "mqtt_user": "gas_reader",
  "mqtt_password": "CHANGE_ME_MQTT_PASSWORD",
  "ota_password": "CHANGE_ME_OTA_PASSWORD"
}
```

## Field reference

| Field | Required | Meaning / validation |
|---|---|---|
| `ssid` | Yes | Primary 2.4 GHz SSID; 1–32 UTF-8 bytes |
| `wifi_password` | Yes | Primary PSK: 8–63 UTF-8 bytes, or exactly 64 hexadecimal characters |
| `fallback_ssid` | No | Secondary SSID, at most 32 UTF-8 bytes; empty disables fallback |
| `fallback_wifi_password` | With fallback SSID | Same PSK rules; leave both fallback fields empty or fill both |
| `mqtt_uri` | Yes | `mqtt://host[:port]`, at most 255 UTF-8 bytes; no embedded username/password, query or fragment |
| `mqtt_user` | Yes | Device's broker username, at most 64 UTF-8 bytes |
| `mqtt_password` | Yes | Device's broker password, at most 128 UTF-8 bytes |
| `ota_password` | Yes | At least 12 characters and at most 128 UTF-8 bytes; web username is always `admin` |

All values must be strings. Unknown keys, control characters and unmodified `CHANGE_ME` placeholders are rejected. JSON must not contain comments. For the OTA password, a random ASCII password avoids browser character-encoding differences.

Open Wi-Fi networks are not supported. The firmware uses WPA2-or-better authentication settings. Broker credentials are separate from the Wi-Fi password and the OTA password. A dedicated MQTT account named `gas_reader` is only an example; you must create it on your broker.

The MQTT URI is the **broker** endpoint, typically TCP 1883. Do not put the Home Assistant web port 8123 there. This firmware currently accepts `mqtt://`, not `mqtts://`.

## Primary and fallback Wi-Fi

Each new connection starts with the primary SSID. With fallback configured, primary and fallback attempts each have a maximum ten-second join window, within the operation's shared 30-second budget. A join failure/timeout moves to fallback. Without fallback, the primary can use the available connection deadline.

A working connection is retained while maintenance runs; the ESP does not roam back to primary in the middle of that session. Exit to sleep, then start a new sync to retry primary at the final installation location.

The same broker must be reachable from both networks. MQTT/HA failure after a successful Wi-Fi join does not trigger a Wi-Fi network switch.

## Generate the initial NVS image

From the repository root:

```sh
bash tools/idf-python.sh tools/provision.py config.local.json
```

The generator validates the JSON, then creates:

| Local file | Contents |
|---|---|
| `provisioning/nvs.csv` | Plain-text provisioning input, including credentials |
| `provisioning/nvs.bin` | 64 KiB initial NVS image, also containing credentials |

The output directory and `.bin` files are ignored. Provisioning does not connect to the ESP and does not flash anything. The long JSON key `fallback_wifi_password` is stored under NVS key `fallback_pass` because NVS keys have a 15-character limit. Older NVS images without fallback keys remain supported.

## What requires rebuilding or reflashing?

| Change | Required action |
|---|---|
| Edit `config.local.json` before first install | Regenerate NVS; the application does not need recompiling |
| Change Wi-Fi/MQTT/OTA credentials on an installed reader | Editing JSON alone does nothing; the new values must be provisioned to device NVS |
| Change C source, GPIO assignment, daily time zone or embedded HTML | Rebuild and install the new application, normally through OTA |
| Change only HA integration source | Replace the HA component folder and restart HA |
| Correct the physical meter reading | Use the HA text entity/action; no firmware/NVS replacement |

**There is no non-destructive live credential editor in this version.** The first-install/NVS-only flash paths replace the entire provisioning partition, including saved pulse state and instance ID. On an operating meter, first synchronize and reconcile the physical reading, preserve backups, and treat reprovisioning as a new device instance. Do not rewrite NVS merely to install a firmware update. OTA preserves existing settings; it does not copy new values from your computer's JSON.

## MQTT permissions and local-network scope

The ESP needs permission to publish its `report` and `status`, and subscribe to its `ack` topic. HA needs the inverse permissions. Example topic rules for a device ID `aabbccddeeff`:

```text
# Example Mosquitto ACL fragment; adapt usernames and device ID.
user gas_reader
topic write gas_reader/aabbccddeeff/report
topic write gas_reader/aabbccddeeff/status
topic read gas_reader/aabbccddeeff/ack

user homeassistant
topic read gas_reader/+/report
topic read gas_reader/+/status
topic write gas_reader/+/ack
```

Integrate rules into your broker's existing access policy rather than replacing unrelated devices' permissions. The HA custom integration uses HA's configured MQTT connection; there is no separate broker password field in its config flow.

MQTT traffic and the maintenance HTTP page are not encrypted. NVS/provisioning images are not encrypted by this project either. Keep the reader and broker on a trusted local network and keep generated files private. Never expose the maintenance page as a public Internet service.
