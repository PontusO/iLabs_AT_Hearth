# nRF54L15 platform: Ophelia-IV module with CPico dev bridge

## What this platform is

This is the Nordic sibling of the ESP32-C6 Matter co-processor, running MCUboot with UART serial recovery on a Wurth Ophelia-IV module (nRF54L15). The bootloader is software-installed via SWD, locked against modification, and can always be reached by holding the recovery strap. See `superpowers/specs/2026-08-26-nrf54l15-bootloader-design.md` in the iLabs_Hearth_docs repository for the full design rationale and specification.

Matter-over-Thread with the commissioning core is bench-proven (design:
`superpowers/specs/2026-08-28-nrf54l15-matter-core-design.md` in the same
repository). The device-type catalogue serves the milestone slice plus
catalogue batch 1 (attribute-only): `0x0100` On/Off Light, `0x0101`
Dimmable Light, `0x0302` Temperature Sensor, `0x0015` Contact Sensor,
`0x0107` Occupancy Sensor, `0x0307` Humidity Sensor, `0x0305` Pressure
Sensor, `0x0044` Rain Sensor, `0x0041` Water Freeze Detector, `0x0043`
Water Leak Detector, `0x0106` Light Sensor, `0x0306` Flow Sensor, `0x010A`
On/Off Plug-in Unit, `0x010B` Dimmable Plug-in Unit; and catalogue batch 2
(the server-interaction types): `0x010C` Color Temperature Light, `0x010D`
Extended Color Light, `0x0301` Thermostat, `0x002B` Fan, `0x0202` Window
Covering, `0x002C` Air Quality Sensor. Anything else answers
`+MTERR:6` until the catalogue grows toward C6 parity.

Batch 2 takes on fewer cluster features than the specs allow, but never
fewer commands than a feature it does take on requires. The boundaries are
worth knowing before a bench session:

- Color: the command surface is **complete** for the features each type
  advertises, since every ColorControl command is `mandatoryConform` on a
  feature. `0x010C` advertises feature CT alone and accepts
  MoveToColorTemperature, MoveColorTemperature, StepColorTemperature and
  StopMoveStep. `0x010D` advertises HS|XY|CT (HS is `optionalConform`
  inside the Extended Color Light device type, and taking it is the C6's
  own step beyond the mandatory set, so a host library's HSV class has
  CurrentHue and CurrentSaturation to write) and accepts all fourteen:
  MoveToHue, MoveHue, StepHue, MoveToSaturation, MoveSaturation,
  StepSaturation, MoveToHueAndSaturation, MoveToColor, MoveColor,
  StepColor, MoveToColorTemperature, MoveColorTemperature,
  StepColorTemperature and StopMoveStep. EnhancedHue and ColorLoop are the
  two features not taken, so their five commands are correctly absent.
  Both types boot in color-temperature mode at 250 mireds, because
  StartUpColorTemperatureMireds is seeded non-null exactly as on the C6.
- Thermostat: Heating|Cooling, SetpointRaiseLower, setpoints seeded 16 C /
  24 C. No Presets, schedules, Auto mode or occupancy: those need a
  delegate this firmware does not provide.
- Fan: FanMode, FanModeSequence, PercentSetting and PercentCurrent only,
  FeatureMap 0. No Step/Rock/Wind/AirflowDirection (delegate territory).
  The server's own FanMode-to-PercentSetting coupling does not run on a
  dynamic endpoint, so the host owns it; both attributes read and write
  correctly over AT and over a controller's IM.
- Window covering: Lift|PositionAwareLift, and UpOrOpen / DownOrClose /
  StopMotion / GoToLiftPercentage all land the fabric's request in
  TargetPositionLiftPercent100ths, emit the `+MTATTR` URC and answer
  Success. The host moves the motor and writes CurrentPosition back. No
  tilt this round.
- Air quality: the AirQuality attribute with all four optional quality
  levels advertised (Fair, Moderate, VeryPoor, ExtremelyPoor), so a host
  library's seven-value enum can never report a level the endpoint's
  feature map rejects.

RAM is the batch's real cost and the number to watch. Measured 2026-08-29,
both figures from a pristine `build/` on `ophelia_cpico/nrf54l15/cpuapp`
(batch 2 at the branch tip, the baseline at `daddee0` in a scratch
worktree): the app now uses 240,276 B of the 262,144 B available (91.7%,
up from 222,052 B / 84.7% before the batch); flash 789,488 B, up from
755,380 B. Roughly 7.5 KB of the RAM growth is the dynamic endpoint slot
arena growing with `kMaxSlots` and `kMaxClusters`
(`port/mt_devtypes_zephyr.cpp` carries the arithmetic), and 7.7 KB is
CHIP's own `ColorControlServer` per-endpoint transition state, sized for
every dynamic endpoint this build can create. A third batch of this size
does not fit as things stand. Two reclamations, in order of payoff: sizing
the slot arena per device type instead of flat (only the two color lights
need all 38 slots; the median device type uses about 10), and lowering
`MT_COMP_MAX_ENDPOINTS` from 28, which sizes both that arena and, through
`CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT`, `ColorControlServer`'s own
arrays.

The BooleanState
cluster (Contact/Rain/Water Freeze/Water Leak) is served over real Matter
reads by a CHIP-registered `BooleanStateCluster` object rather than this
build's external-storage arena directly; `MatterPostAttributeChangeCallback`
(`port/mt_matter_zephyr.cpp`) bridges the two, so an AT+MTATTR write of
StateValue both answers a later AT+MTATTR read from this arena and reaches
a real Matter controller's read, emitting the cluster's StateChange event
in the process -- see the comment at that call site and on
`booleanStateAttrs` (`port/mt_devtypes_zephyr.cpp`) for the mechanism.

Level commands (the MoveToLevel family) on the dimmable types (`0x0101`,
`0x010B`) settle CurrentLevel correctly on dynamic endpoints (fixed:
graph bug B388, which traced to dynamic endpoints never running the
per-endpoint LevelControl server init that caches Min/MaxLevel; `port/
mt_devtypes_zephyr.cpp` now invokes it by hand at endpoint create time).
Direct AT+MTATTR writes to CurrentLevel and OnOff coupling both work
correctly; store coherence (AT, controller, URCs) is fine throughout.

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

**NCS toolchain activation** (required for `west` commands on this machine).
The platform builds against NCS v3.3.4 (Matter 1.5.0 in the bundled CHIP
tree) since 2026-08-28; the module's installed bootloader was built with
v3.0.2 and stays, since the MCUboot image format and SMP recovery protocol
are stable across the bump:

```bash
TC=$HOME/ncs/toolchains/911f4c5c26
export PATH="$TC/bin:$TC/usr/bin:$TC/usr/local/bin:$TC/opt/bin:$TC/opt/nanopb/generator-bin:$TC/nrfutil/bin:$TC/opt/zephyr-sdk/arm-zephyr-eabi/bin:$TC/opt/zephyr-sdk/riscv64-zephyr-elf/bin:$PATH"
export LD_LIBRARY_PATH="$TC/lib:$TC/lib/x86_64-linux-gnu:$TC/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GIT_EXEC_PATH="$TC/usr/local/libexec/git-core"
export GIT_TEMPLATE_DIR="$TC/usr/local/share/git-core/templates"
export PYTHONHOME="$TC/usr/local"
export PYTHONPATH="$TC/usr/local/lib/python3.12:$TC/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$TC/opt/zephyr-sdk"
export ZEPHYR_BASE="$HOME/ncs/v3.3.4/zephyr"
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

2026-08-27: since the bootloader round, `sysbuild.conf` unconditionally sets `SB_CONFIG_BOOTLOADER_MCUBOOT=y`, so any `west build` from this directory pulls in MCUboot by default, sysbuild flag or not (there is no `.west/config` override). The stock `nrf54l15dk/nrf54l15/cpuapp` overlay used for core-sanity builds has no `mcuboot-button0` alias (only `ophelia_cpico`'s own dts wires the recovery strap to it), so MCUboot's serial-recovery build fails there with `#error "Serial recovery/USB DFU button must be declared in device tree as 'mcuboot_button0'"`. Build DK core-sanity targets with `--no-sysbuild` until the DK overlay gains recovery-button wiring or `sysbuild.conf` becomes board-conditional.

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

## Data model regeneration

The Matter data model (endpoint 0 root node, plus the disabled catalogue
endpoint at id 240 carrying the union of milestone clusters) is
`src/default_zap/hearth.zap`, edited by hand as JSON, with the generated
`.matter` file and `zap-generated/` sources checked in next to it. To
regenerate after editing the `.zap` file:

```bash
cd ~/ncs/v3.3.4   # west zap-generate is an NCS workspace extension
west zap-generate -z $FW/platform/nrf54l15/src/default_zap/hearth.zap
```

`hearth.zap` stores its ZCL/template `package` paths as the ones the
original sample (`light_bulb.zap`, the starting point for this file) had
relative to its own location under `nrf/samples/matter/`; copied into this
repository those relative paths resolve outside the NCS tree entirely. The
`package` entries at the top of `hearth.zap` are pinned to absolute paths
under `~/ncs/v3.3.4/modules/lib/matter/...` for that reason; keep them
pinned there after any hand-edit or regeneration on this machine.

## Keys

Signing keys live in `keys/`. The DEVELOPMENT key (`hearth_dev_p256.pem`) is committed to the repository and deliberately shared; it protects nothing and ensures every dev build exercises the signature verification path. The PRODUCTION key is generated offline, never committed, and swapped in at fixture time to sign release images. See `keys/README.md` for the production key swap procedure.

## Toolchain note

This platform does not use the nrfutil toolchain-manager tool that appears in Nordic's documentation. Instead, the NCS activation block above configures the exact toolchain directory on this machine. Use that activation block before any `west` command; use a clean shell for `pyocd` and host Python tests.

## Matter commissioning on the bench

The milestone acceptance (2026-08-28) commissions with the CLI chip-tool
from the bench host over BLE into the live OTBR fabric:

```bash
# open the window over AT (AT+MTCOMMISSION), then, in a clean shell:
./fw/srp-aaaa-shim.sh &     # see the header comment: OTBR mDNS workaround
chip-tool pairing ble-thread <node-id> hex:<dataset> 20202021 3840
```

Dev credentials only (test VID 0xFFF1 / PID 0x8000, SPAKE2 passcode
20202021, discriminator 3840): consumer hubs are expected to refuse
them. The Thread dataset comes from the border router
(`ot-ctl dataset active -x`) and is a credential: never commit or log
it. The `srp-aaaa-shim.sh` workaround is required until the upstream
ot-br-posix mDNS host-update defect it documents is fixed.

Radio note: the module's 32 MHz HFXO needs the SoC-internal load
capacitors programmed (board DTS `&hfxo`); without them the radio is
silent while everything else runs. Bench scripts must wait for +MTREADY
after opening the AT port (the bridge's DTR line pulses reset on open).

Measured 2026-08-28 (build/, dev/nrf-matter-core at 5c19776 + hfxo fix):
app image 753,691 B of the 1,374,208 B slot (54.9%); settings_storage
raw occupancy 30,476 of 32,768 B non-erased after a day of commissioning
churn (ZMS is log-structured, stale entries count until collection; the
32 KB sizing watch item from the design spec stays open).
