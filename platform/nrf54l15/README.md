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
  Bench note: a ColorControl command sent to a light whose OnOff attribute
  is FALSE is discarded and still answers Success, and nothing moves. That
  is the cluster's ExecuteIfOff rule, not a defect: with an On/Off server on
  the same endpoint, an OnOff of FALSE and the Options bit
  ExecuteIfOff clear, the server stops after Options processing
  (`shouldExecuteIfOff`, `color-control-server.cpp:515`). Turn the light on
  first, or send the command with OptionsMask/OptionsOverride set.
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

Batch 2 was the first round to declare attributes whose ZCL type is one of
Matter's semantic **aliases** for an integer: the thermostat's setpoints are
`temperature`, the fan's are `percent`, the window covering's lift positions
are `percent100ths`. The AT attribute bridge's type classifier only knew the
base integer codes, so those attributes answered `+MTERR:5` on an
`AT+MTATTR` read and emitted no `+MTATTR` URC when a controller changed
them, while working perfectly over the controller's own IM. The failure was
easy to miss on the bench because `+MTERR:5` is also the honest answer for a
nullable attribute currently holding null, the AT grammar having no null
literal. Fixed by normalising through the SDK's own alias table
(`AttributeBaseType()`, `app/util/ember-io-storage.cpp`) before classifying,
so the AT integer family tracks the SDK's definition instead of a
hand-copied list. The C6 never had this gap because esp-matter maps the
aliases down to its own value types before its AT bridge sees them.

The BooleanState cluster (Contact/Rain/Water Freeze/Water Leak) is served
over real Matter reads by a CHIP-registered `BooleanStateCluster` object
rather than this build's external-storage arena directly;
`MatterPostAttributeChangeCallback`
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

## Endpoint capacity

**Acceptance and capacity are two different numbers on this platform, and
an integrator needs both.**

*Acceptance* is `MT_COMP_MAX_ENDPOINTS` (28). It is a core constant and part
of the `AT+MT` wire contract, identical on the C6 and here: a host may
DECLARE up to 28 endpoints over `AT+MTEP`, and the composition store keeps
all 28 intact.

*Capacity* is `kServiceableEndpoints` (16, `port/mt_port_ids.h`). It is how
many of those endpoints this build stands up and serves at once, and it is a
port decision about a 256 KB part rather than a contract change. The C6 has
no such split because it serves the full 28.

**What a host sees past capacity.** Nothing at declare time: `AT+MTEP`
accepts the endpoint and the composition persists. The wall is at the next
boot, in the rebuild. `mt_devtype_create()` logs which of the two limits was
hit and how far away the other one was, returns an error, and the rebuild
stops there.

The abort is **stop-at-failure, not roll-back** (`AT_MT_SPEC.md` 501-506).
The endpoints created before the failure **stay live as a prefix, with their
ids unchanged**; the failed entry and everything after it are absent, and
`AT+MTEP?` reports the live prefix. So a 20-endpoint composition on this
build serves endpoints **1 through 16**, and endpoints 17 to 20 are simply
not there.

What the rule prevents is a **renumbered** model, not a partial one: skipping
the failed entry and carrying on would shift every later endpoint down by
one, and a commissioned controller would silently get a different data model
than the one it was commissioned against (design spec 12.1). That is the same
stop-at-failure a bad parent or an unknown device type already triggers.

**Capacity is bounded by two resources, not one.** A composition can exhaust
either first:

1. **The header table**, `kServiceableEndpoints` entries, 16 bytes each.
2. **The endpoint block heap**, `HEARTH_EP_HEAP_BYTES` (8 KB), a dedicated
   `K_HEAP_DEFINE(hearth_ep_heap)`. Each created endpoint takes one block
   holding its `DataVersion` array and its attribute slots, sized for its
   own device type rather than for the widest one in the catalogue.

Sixteen sensors exhaust the table with the heap barely touched; fourteen
extended colour lights exhaust the heap with headers to spare.

### Per-endpoint cost

Block payload is `4 x clusters + 16 x slots`; Zephyr charges
`roundup(payload + 4, 8)`.

| Device type | Clusters | Slots | Heap cost |
|---|---|---|---|
| `0x010D` Extended Colour Light | 5 | 36 | 600 B |
| `0x010C` Colour Temperature Light | 5 | 32 | 536 B |
| `0x0101` / `0x010B` Dimmable Light, Dimmable Plug | 4 | 20 | 344 B |
| `0x0301` Thermostat | 3 | 15 | 256 B |
| `0x0202` Window Covering | 3 | 13 | 224 B |
| `0x0100` / `0x010A` On/Off Light, On/Off Plug | 3 | 11 | 192 B |
| `0x002B` Fan | 3 | 10 | 176 B |
| `0x0302` `0x0307` `0x0305` `0x0106` `0x0306` `0x0107` sensors | 3 | 9 | 160 B |
| `0x0015` `0x0044` `0x0041` `0x0043` `0x002C` boolean-state, air quality | 3 | 7 | 128 B |

### Worked examples

Against 8 KB of heap (about 8,100 usable after the heap's own header and
bucket table) and 16 header slots:

| Composition | Heap | Fits? |
|---|---|---|
| 16 sensors of any kind | 2,560 B | **yes**, table-bound, heap 68% idle |
| 16 on/off or dimmable lights and plugs | up to 5,504 B | **yes**, table-bound |
| 16 thermostats / window coverings / fans | up to 4,096 B | **yes**, table-bound |
| 2 extended colour + 2 dimmable + 12 sensors | 3,808 B | **yes**, comfortably |
| 15 colour temperature lights | 8,040 B | **yes**, one short of 16 |
| 13 extended colour lights | 7,800 B | **yes**, three short of 16 |
| 16 extended colour lights | 9,600 B wanted | **no**, fails at the 14th |

So every device type in the catalogue reaches the full 16 **except** the two
colour lights, which reach 15 and 13. Sizing the heap for 16 extended colour
lights would want 9,600 B and buy a composition nobody builds; the RAM is
worth more elsewhere. A compile-time assertion keeps the heap holding at
least eight of whatever the widest device type happens to be, so this table
cannot go stale unnoticed.

### The LM20 tier

The nRF54LM20 (512 KB RAM, supported upstream in this NCS) is where the 16
goes back up. Raising `kServiceableEndpoints` and `HEARTH_EP_HEAP_BYTES`
together in `port/mt_port_ids.h`, and the mirrored literal in
`src/chip_project_config.h`, is the whole change: the header table, CHIP's
per-endpoint pools and the block heap all follow from those two numbers, and
the static assertions catch a mirror that drifts. 28 serviceable endpoints
would want `28 x 600` = 16,800 B (about 16.4 KB) of block heap for an
all-extended-colour worst case, and would hand back to CHIP's per-endpoint
pools the roughly 6.7 KB this round reclaimed from them by going the other
way.

### Measured

`ophelia_cpico/nrf54l15/cpuapp`, pristine builds, 2026-08-29:

| | Flat arena (batch 2) | Per-composition (this round) |
|---|---|---|
| RAM used | 240,276 B (91.7%) | **223,884 B (85.4%)** |
| RAM free | 21,868 B | **38,260 B** |
| Flash | 789,488 B | 789,792 B |

A 16,392 byte reclaim, from three places: the endpoint table went from a
flat `28 x 648 B` arena to a `16 x 16 B` header table (-17,888 B), the new
8 KB block heap costs +8,192 B, and dropping
`CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT` from 28 to 16 shrank CHIP's own
compile-time per-endpoint pools by about 6,700 B, of which
`ColorControlServer` alone is 3,168 B. `nrf54l15dk` tracks it: 224,108 B
(85.5%), a 16,384 byte reclaim.

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
