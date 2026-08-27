# nRF54L15 platform: Ophelia-IV module with CPico dev bridge

## What this platform is

This is the Nordic sibling of the ESP32-C6 Matter co-processor, running MCUboot with UART serial recovery on a Wurth Ophelia-IV module (nRF54L15). The bootloader is software-installed via SWD, locked against modification, and can always be reached by holding the recovery strap. See `superpowers/specs/2026-08-26-nrf54l15-bootloader-design.md` in the iLabs_Hearth_docs repository for the full design rationale and specification.

## Dev board wiring

The CPico RP2350 dev board carries the module and hosts the bridging firmware. This table is the soldering contract: a deviation means editing both the board `pinctrl` file and the sketch defines together.

| Line | From (CPico) | To (Ophelia-IV module) | Signal |
|---|---|---|---|
| Serial TX | GP0 | P1.15 (UARTE20 RX) | CPico transmit line |
| Serial RX | GP1 | P1.04 (UARTE20 TX) | CPico receive line |
| Reset | GP2 | nRESET pad | Active-low reset (asserted by host) |
| Recovery strap | GP3 | P2.03 (GPIO, pull-up) | Active-low strap sampled at boot |
| Console TX | P0.00 (console TX) | Debug Probe UART RX | Module console output |
| Debug data | SWDIO, SWDCLK pads | Debug Probe SWD | SWD programming interface |
| Power | 3V3, GND | Module supply | 3.3 V and ground |

The Recovery strap and nRESET lines are driven high-impedance (pulled up externally) when the host releases them. The Debug Probe is a Raspberry Pi Debug Probe or CMSIS-DAP compatible.

## One-time SWD install

Before any UART flashing, the MCUboot bootloader is installed over SWD. Do this once per module.

**NCS toolchain activation** (required for `west` commands on this machine):

```bash
TC=$HOME/ncs/toolchains/7cbc0036f4
export PATH="$TC/bin:$TC/usr/bin:$TC/usr/local/bin:$TC/opt/bin:$TC/opt/nanopb/generator-bin:$TC/opt/zephyr-sdk/arm-zephyr-eabi/bin:$TC/opt/zephyr-sdk/riscv64-zephyr-elf/bin:$PATH"
export LD_LIBRARY_PATH="$TC/lib:$TC/lib/x86_64-linux-gnu:$TC/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GIT_EXEC_PATH="$TC/usr/local/libexec/git-core"
export GIT_TEMPLATE_DIR="$TC/usr/local/share/git-core/templates"
export PYTHONHOME="$TC/usr/local"
export PYTHONPATH="$TC/usr/local/lib/python3.12:$TC/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$TC/opt/zephyr-sdk"
export ZEPHYR_BASE="$HOME/ncs/v3.0.2/zephyr"
```

CAUTION: this PYTHONHOME/PYTHONPATH breaks ordinary system Python in the same shell. Use a dedicated shell or subshell for west builds.

**Build the bootloader and app**:

From `platform/nrf54l15/`, with the toolchain activation applied:

```bash
west build -b ophelia_cpico/nrf54l15/cpuapp --pristine --sysbuild -- -DZEPHYR_BASE=$ZEPHYR_BASE -DBOARD_ROOT=$PWD
```

This produces two artifacts under `build/`:
- `mcuboot/zephyr/zephyr.hex`: the locked bootloader (installed once via SWD)
- `nrf54l15/zephyr/zephyr.signed.bin`: the signed application image

**Partition layout** (1428 KB flash_primary, carved by NCS Partition Manager):

| Partition | Size | Purpose |
|---|---|---|
| `mcuboot` | 48 KB | Locked bootloader |
| `mcuboot_pad` | 2 KB | Padding to align app slot |
| `app` | 1342 KB | Primary slot: signed application image |
| `settings_storage` | 32 KB | ZMS: heartbeat KV store and Matter persistence |
| `factory_data` | 4 KB | Reserved for production factory data |

**Install via SWD** (pyocd 0.44.1, requires the Debug Probe soldered to the module):

```bash
pyocd flash -t nrf54l build/mcuboot/zephyr/zephyr.hex
pyocd flash -t nrf54l build/nrf54l15/zephyr/zephyr.signed.bin
```

The pyocd tool is the SWD path for this platform. Recovery from any state (locked or corrupted) uses `pyocd erase -t nrf54l --chip` followed by the flash commands above: no serial port can reach a module with an invalid bootloader.

## Everyday flashing

After SWD install, all updates go over UART via the serial recovery mechanism. Flashing is driven by the host's `fw/flash.py` script:

```bash
python3 fw/flash.py --image build/nrf54l15/zephyr/zephyr.signed.bin \
  --port $(ls /dev/serial/by-id/usb-*CPico*-if00 2>/dev/null || echo CHOOSE-BY-ID)
```

The `--port` flag **must use `/dev/serial/by-id`**, never `/dev/ttyACM<n>`. The reason: ttyACM device numbering changes whenever USB devices are plugged/unplugged; on this bench, ttyACM0 is the Thread RCP and a stray write to the wrong device kills `otbr-agent`. The by-id convention gives a stable, device-unique path that does not move.

When the flash succeeds, the application starts and prints `+MTREADY` on the same UART within a few seconds; its presence is the flasher's success criterion.

The bridge exposes nRESET and the recovery strap via CDC control signals: CDC DTR asserted holds the module in reset, and RTS asserted drives the recovery strap (released lines let it run). Stock terminal programs (picocom, minicom) assert DTR and often RTS on open, so a terminal opened with defaults silently holds the module in reset or drops it into recovery; open with DTR and RTS cleared to talk to the running application.

Watching the console: open the Debug Probe's CDC with DTR asserted.
Like the bridge's own USB stack, the probe's CDC discards output while
the host holds DTR low, so a reader that clears DTR sees permanent
silence from a perfectly healthy console (half a bench day, once).

## Recovery semantics

`CONFIG_BOOT_SERIAL_NO_APPLICATION` means the recovery strap is not the only way into serial recovery. A unit whose slot-0 image fails signature validation drops into serial recovery on its own at boot, with no strap required. Physical access to a unit in that state, plus a corrupted image, is therefore enough to reach an unauthenticated SMP flash-write port: no strap, no credentials, wire access alone. Signed-boot still refuses to run unsigned code, so a write does not equal a compromise of the running application; this is a deliberate availability-over-lockdown choice, appropriate for a module whose SWD pads are also exposed on test points (SWD access alone already grants at least as much).

One consequence: a successful SMP handshake through `flash.py --enter-only` does not by itself prove the strap path works. The same handshake succeeds whether the strap forced entry or the unit simply had a bad slot-0 image and fell into recovery on its own. Demonstrating the strap path specifically requires taking down a **running** application (a good image, already booted) with the strap held, and observing recovery entry despite that.

Another consequence: a field unit that ends up with a bad slot-0 image and no operator present to reflash it sits in serial recovery indefinitely. There is no timeout back to any other state; recovery is where it stays until someone flashes it.

## Keys

Signing keys live in `keys/`. The DEVELOPMENT key (`hearth_dev_p256.pem`) is committed to the repository and deliberately shared; it protects nothing and ensures every dev build exercises the signature verification path. The PRODUCTION key is generated offline, never committed, and swapped in at fixture time to sign release images. See `keys/README.md` for the production key swap procedure.

## Toolchain note

This platform does not use the nrfutil toolchain-manager tool that appears in Nordic's documentation. Instead, the NCS activation block above configures the exact toolchain directory on this machine. Use that activation block before any `west` command; use a clean shell for `pyocd` and host Python tests.
