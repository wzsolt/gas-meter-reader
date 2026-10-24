# Getting started

[Back to README](../README.md)

These instructions install from source. A fresh clone contains no firmware `.bin`, generated NVS image or release ZIP. Run shell commands from the repository root unless a step says otherwise.

## 1. Assemble and check the hardware

Follow [Hardware and wiring](HARDWARE.md). Use D2/GPIO2 for the button, D1/GPIO1 for the reed and D0/GPIO0 for the fixed 1 MΩ / 330 kΩ divider. Verify all ground connections, battery polarity and the external LED before powering the board.

## 2. Prepare Home Assistant and MQTT

Install/configure an MQTT broker and HA's built-in MQTT integration. Copy `homeassistant/custom_components/gas_reader` into your HA configuration's `custom_components` directory and restart HA. See [Home Assistant](HOME_ASSISTANT.md).

The broker's TCP endpoint is usually port 1883; the HA web endpoint on port 8123 is not an MQTT broker. Broker credentials alone are not enough: the Gas Reader custom integration must be installed and configured to issue application ACKs.

## 3. Install ESP-IDF and build

Install/select **ESP-IDF 6.1.0** for ESP32-C6, preferably through Espressif's VS Code extension / Installation Manager. Open `firmware/` in VS Code and build the project, or use the helper from the repository root:

```sh
bash tools/build.sh
```

See [ESP-IDF setup](ESP_IDF.md) for SDK discovery, prerequisites and troubleshooting. Do not use Arduino IDE for this ESP-IDF project.

## 4. Create the private configuration and initial NVS image

```sh
cp -n config.example.json config.local.json
chmod 600 config.local.json
```

Edit the copy with your own SSID, Wi-Fi/MQTT credentials and an OTA password of at least 12 characters. Optional fallback Wi-Fi fields may be left empty. Every key and limit is documented in [Configuration](CONFIGURATION.md).

Generate the initial image after saving your edits:

```sh
bash tools/idf-python.sh tools/provision.py config.local.json
```

This creates private `provisioning/nvs.csv` and `provisioning/nvs.bin`. The generator does not flash the ESP. Both files contain credentials and are ignored by Git, together with `config.local.json`.

## 5. First USB installation — bench device only

Connect a data-capable USB cable, close serial monitors and list ports:

```sh
bash tools/idf-python.sh -m serial.tools.list_ports -v
```

If the port disappears because the firmware sleeps, hold BOOT, tap RESET, then release BOOT to enter download mode. Replace `YOUR_SERIAL_PORT` below with the port for your board:

```sh
bash tools/idf-python.sh tools/flash_first_install.py --port YOUR_SERIAL_PORT --first-install
bash tools/idf-python.sh -m esptool --chip esp32c6 --port YOUR_SERIAL_PORT read-mac
```

The installer writes bootloader, partition table, OTA data, application and **new NVS**. It overwrites saved configuration, counters and the device instance. Use it only for the initial bench installation, not for routine upgrades of a live reader.

Use the six-byte **Wi-Fi/base MAC**, without colons, as the twelve-character lowercase device ID. Do not use the extended IEEE 802.15.4 address.

## 6. Configure the HA device

In HA, open **Settings → Devices & services → Add integration → Gas Reader**. Enter the device ID, initial physical meter reading and pulse volume (normally `0.01`). The initial reading is the baseline before pending device pulses are added.

Short-press and release the external button. The LED stays on during sync, then flashes three times after HA ACK and ESP checkpoint success. Two flashes indicate failure; pending data is preserved. Cold boot also makes one sync attempt to learn the time.

Use **Gas consumption** as the Energy/Utility Meter source. Use **Gas meter reading** for the editable physical reading. Details and upgrade migration are in [Home Assistant](HOME_ASSISTANT.md).

## 7. Maintenance and later firmware updates

Hold the external button for 5 seconds. The LED blinks while connecting and becomes steady when the web page is ready. Open `http://gas-reader-<device_id>.local/`, or the DHCP IP, and log in as `admin` with `ota_password`.

Upload the locally built **`firmware/build/gas_reader.bin` application**, not a full flash image, NVS, bootloader or partition table. OTA preserves NVS and uses the inactive app partition with boot rollback. Changing the embedded HTML also requires a rebuild and OTA update.

To leave without uploading, release the entry press, then short-press and release the button again, or click **Exit**. An active upload/discard finishes before exit. Without activity the maintenance window ends after ten minutes; a started upload can extend up to three minutes beyond that deadline.

**Counter reset** discards pending ESP pulses; it preserves HA's configured reading and previously processed consumption. An older normal outbox must be resolved before the discard and may add consumption not previously processed. See [Protocol](PROTOCOL.md).

## 8. Daily operation and validation

Normal pending consumption is sent on a 30-minute schedule. Daily sync runs at 23:55 Hungarian local time even without pulses. HA ACK time supplies the clock; unknown time uses a 24-hour interval. Failed daily attempts retry after 30 minutes. Every new connection starts with the primary SSID.

Validate the actual pulse width, deep-sleep current, battery measurement and radio coverage before relying on unattended counting. See [Testing](TESTING.md). Pulses after the last flash checkpoint may be lost on power removal.
