# ESP-IDF setup, build and updates

[Back to README](../README.md) · [Installation order](INSTALL.md) · [Configuration](CONFIGURATION.md)

## Supported build baseline

- Seeed XIAO ESP32-C6, 4 MB flash.
- **ESP-IDF 6.1.0**, required by `firmware/main/idf_component.yml`.
- Espressif's RISC-V compiler, Python environment, CMake and Ninja from the selected IDF installation.
- Bash and `python3` for the project helpers. The helpers assume a macOS/Linux-style environment; native Windows command-line setup is not covered here.

Keep `firmware/dependencies.lock`, `sdkconfig.defaults` and `partitions.csv` in Git. Generated `sdkconfig`, `build*` and `managed_components` stay local. The firmware version is in `firmware/CMakeLists.txt`; the HA integration has its own version in its manifest.

## Recommended: VS Code and EIM

1. Install VS Code and the official Espressif **ESP-IDF** extension.
2. Run **ESP-IDF: Open ESP-IDF Installation Manager** and install ESP-IDF 6.1.0 and its ESP32-C6 tools.
3. Open this repository's `firmware` folder in VS Code.
4. Run **ESP-IDF: Select Current ESP-IDF Version** and select that installation.
5. Select target `esp32c6`, then run **ESP-IDF: Build Your Project**.

Follow the [official extension installation guide](https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/installation.html) for platform prerequisites and EIM. If macOS prerequisite checks report missing CMake/dfu-util, they can be installed with `brew install cmake dfu-util`; Ninja is also needed if the selected tool installation does not provide it.

`firmware/.vscode/settings.json` is ignored because the extension stores machine-specific SDK/compiler/port paths there. An optional portable starting point is provided:

```sh
cp -n firmware/.vscode/settings.example.json firmware/.vscode/settings.json
```

Let the extension select your SDK and compiler. The compile database is `firmware/build/compile_commands.json`; rebuild and refresh IntelliSense if includes are flagged while the actual build succeeds.

## Build from a terminal

Use the ESP-IDF terminal opened by the extension, or activate your SDK according to its installation instructions. From the repository root:

```sh
python3 tools/idf_env.py
bash tools/build.sh
```

The discovery helper uses, in order: active `IDF_PATH`, local VS Code `idf.currentSetup`, the conventional `~/.espressif/v6.1/esp-idf` location, then `.tools/esp-idf`. It obtains the matching tools/Python environment from environment variables or EIM's registry. An arbitrary SDK path should be selected explicitly; the scripts do not search every installed version.

For an explicitly activated environment, native IDF commands are also available:

```sh
idf.py -C firmware set-target esp32c6
idf.py -C firmware build
```

`set-target` is an initial-setup operation and regenerates local configuration. Later builds normally only need `build`. The first build downloads the locked managed components, so it needs access to Espressif's component registry.

Alternatively, `bash tools/bootstrap.sh` creates a separate project-local SDK/tool installation under ignored `.tools/` using the `v6.1` tag. This downloads the SDK/tools and is not needed when EIM is already configured. Do not mix Python or compiler paths from different installations.

## Provision and flash

The configuration is **not compiled into the app**. Create the local NVS image after editing `config.local.json`:

```sh
bash tools/idf-python.sh tools/provision.py config.local.json
bash tools/idf-python.sh -m serial.tools.list_ports -v
bash tools/idf-python.sh tools/flash_first_install.py --port YOUR_SERIAL_PORT --first-install
```

Use the last command for a **new bench device only**. It writes generated NVS as well as the normal build files, replacing all saved state in that partition. A normal IDF flash command does not add this project's private provisioning image for you.

In VS Code, **Tasks: Run Task** also exposes `Gas Reader: USB-portok listázása` (list ports) and `Gas Reader: Első USB-feltöltés` (first installation). Run a build and generate `provisioning/nvs.bin` first. The NVS-only task is also destructive to saved counters/identity and is not a configuration editor for an operating reader.

The app-only `flash_application.py --initial-slot` helper is limited to an initial `ota_0` installation before OTA slot switching. It is not a general update tool for an arbitrary active OTA slot. Use browser OTA for deployed readers.

## USB and logging

Deep sleep can remove the native USB Serial/JTAG port; this is expected. For reflashing, hold BOOT, tap RESET and release BOOT. Close any serial monitor before flashing. For an awake monitor session, enter maintenance mode and use **ESP-IDF: Monitor Your Device**. There is no temporary diagnostic firmware or 30-second startup window.

At startup, configuration/storage errors leave NVS intact and signal two flashes repeatedly. Do not solve these by automatically erasing a live device's NVS; back up and reconcile the physical reading first.

## Browser OTA

1. Build the updated source with `bash tools/build.sh`.
2. Hold the external button for 5 seconds; wait for blinking to become steady.
3. Open `http://gas-reader-<device_id>.local/` or the board's IP; authenticate as `admin`.
4. Upload **`firmware/build/gas_reader.bin`**.
5. Wait for validation and reboot. Three flashes after the new app verifies itself indicate successful first boot.

The HTTP page accepts a complete application image for ESP32-C6/project `gas_reader`. It rejects the wrong project/chip, oversized or invalid images. The bootloader uses two OTA app slots and rollback; Wi-Fi or HA availability is not part of first-boot acceptance.

The checked-in layout is:

| Partition | Offset | Size |
|---|---|---|
| NVS | `0x9000` | `0x10000` (64 KiB) |
| OTA data | `0x19000` | `0x2000` |
| PHY init | `0x1b000` | `0x1000` |
| `ota_0` | `0x20000` | `0x1e0000` |
| `ota_1` | `0x200000` | `0x1e0000` |

Do not upload the NVS, bootloader or a merged flash image through the web page. Keep the project name and storage compatibility when modifying firmware. Reboot has a brief counting blind interval even though pulses are counted during the upload itself.

## Local packaging

```sh
python3 tools/package.py
```

This requires a successful build and creates a versioned app `.bin`, HA ZIP and checksums in ignored `dist/`. None of these is required in the Git repository. After a fresh clone, build first and install HA directly from source; optional release packaging is a local convenience.
