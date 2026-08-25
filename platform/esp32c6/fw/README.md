# iLabs Hearth AT firmware flasher

`flash.py` flashes the **Matter AT firmware** onto the ESP32-C6 on the
**Challenger RP2350 WiFi6/BLE5** board. It is the same elegant two-stage flasher
used by the ESP-NOW library, trimmed to this board and repointed at the local
IDF build output.

The C6 has no USB of its own; the RP2350 host bridges it. So `flash.py` runs:

1. **Bridge:** copies `RP2350USB2Serial.ino.uf2` onto the RP2350 mass-storage
   device (put the board in **BOOTSEL** mode). The RP2350 reboots as a
   USB-to-serial bridge for the C6.
2. **Flash:** writes the C6 images (`bootloader.bin`, `partition-table.bin`,
   `ilabs_at_hearth.bin`) over that serial link via esptool's Python API, with a
   progress bar. Recovers across bridge watchdog resets and baud fallbacks.

Unlike the ESP-NOW library (which ships prebuilt `bin/` bundles), this reads the
images straight from the **IDF build directory**, so it always flashes what you
just built.

## Usage

```sh
# build first (ESP32-C6, IDF v5.4.1 - the esp-matter toolchain)
idf.py set-target esp32c6
idf.py build

# then flash (board in BOOTSEL mode; port auto-detected after the bridge appears)
python3 fw/flash.py
python3 fw/flash.py --dry-run            # show what would happen, no copy/flash
python3 fw/flash.py --port /dev/ttyACM0  # force the bridge serial port
python3 fw/flash.py --build-dir build_x  # read images from a non-default build dir
python3 fw/flash.py --skip-bridge        # bridge already running: flash the C6 only
```

## Requirements

- **The iLabs fork of esptool (v5.x).** Mandatory: it adds the `RP2040Reset`
  strategy that holds IO0/DTR low so the C6 stays in its download bootloader
  while the RP2350 bridges. Stock pip esptool cannot flash this board.
  Point at your checkout if it is not already importable:

  ```sh
  export ILABS_ESPTOOL_PATH=~/bin/esptool
  ```

  Discovery order: `--esptool-path` -> `$ILABS_ESPTOOL_PATH` -> `fw/vendor/esptool`
  -> `fw/esptool` -> `~/bin/esptool`.
- **pyserial** (`pip install pyserial`).
- **rich** optional (`pip install rich`) for nicer output.

## Note on IDF version

The Matter firmware builds against **ESP-IDF v5.4.1** (esp-matter release/v1.5's
validated IDF), separate from the ESP-NOW firmware's v5.5.4. The flasher itself
is IDF-agnostic; it just reads whatever images are in the build directory.
