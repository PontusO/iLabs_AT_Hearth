# iLabs_AT_Hearth

An AT command stack for the ESP32-C6 coprocessor.

`iLabs_AT_Hearth` turns an ESP32-C6 into a **Matter co-processor** that a host
MCU drives over a simple UART AT interface (`AT+MT...`), the same way
[`iLabs_AT_ESP-now`](https://github.com/PontusO/iLabs_AT_ESP-now) exposes ESP-NOW
over `AT+EN...`. The two firmwares are **single-purpose images that share one
engine**: the host reflashes the C6 over UART to switch personality. There is no
combined ESP-NOW + Matter binary (see `ARCHITECTURE.md` in the docs repository
for why).

## Status

**Released at 1.0.0, feature complete, hardware-verified, not certified.** The
Matter stack runs, the device commissions and is controllable from a Matter
controller, and the host drives the whole lifecycle over `AT+MT`:
commissioning, fabric accounting, attribute read/write in both directions, a
host-declared endpoint composition persisted across power cuts, and a
subscribable platform event stream.

What 1.0.0 does **not** mean, stated here as plainly as in the tag message:
it is not a stability contract (the `AT+MT` wire surface is not frozen, and a
host may not infer that a breaking change requires a 2.0), it is not a
product claim (the firmware is uncertified and uses development credentials,
VID `0xFFF1`, so consumer hubs are expected to refuse it), and it is not a
field-update story (the host-driven serial update stays a draft).

Phases A and B are complete. Phase C, the protocol work behind the
arduino-esp32-parity host library, landed an unmodified upstream
`MatterOnOffLight` sketch commissioned end to end at task C4 and has since
gone well past parity: the device-type table is at **52 rows**, including the
composed appliances (refrigerator, oven, cooktop) and the whole Tier 3 energy
surface (electrical sensor and meter, water heater, heat pump, solar, battery,
device energy management, EVSE, electrical utility meter). 1.0.0 also ships
three images rather than one: WiFi only, Thread only, and a combined image
that picks its stack at runtime with `AT+MTTRANSPORT`.

The Arduino host library that drives all of this is
[`iLabs_Hearth`](https://github.com/PontusO/iLabs_Hearth), which bundles the
three prebuilt images and their flasher. The decision record and the design
history are in the docs repository (see [Documentation](#documentation)
below), which is tagged with the same version as this firmware.

## Architecture

```
core/                        portable C, no SDK header compiles here, ever
  at/                        the AT engine: parsing, dispatch, OK/ERROR/+xxERR
  mt/                        AT+MT semantics: composition, cmdbox, rows, transport
platform/esp32c6/             the ESP-IDF project: esp_matter runtime, port/
                             (the UART link, KV store and OS glue on IDF)
platform/nrf54l15/             NCS/Zephyr skeleton for the Ophelia-IV module
```

`core/` carries the whole `AT+MT` grammar and semantics; a platform directory
is a self-contained implementation of two port interfaces against its own
SDK, `hearth_port.h` downward (OS primitives, the UART link, a KV store,
logging) and `mt_matter.h` upward (the Matter runtime), and nothing else.
`core/at/` was imported from `iLabs_AT_ESP-now`'s `at_core` (provenance in
the import commit) and now diverges: it is this repository's own copy, not a
cross-repo reference, and the ESP UART transport that backs `hearth_port.h`'s
link side lives in `platform/esp32c6/port/`. Nothing in this repository's
build reaches into the sibling ESP-NOW repo any more.

## Requirements

- **ESP-IDF v5.4.1**, the version esp-matter `release/v1.5` validates against.
  Not v5.5.4, which the ESP-NOW firmware uses: esp-matter fails to build on it
  at `chip_gn`.
- **esp-matter** `release/v1.5`. Source both `export.sh` scripts before
  building, IDF first.
- **Target: ESP32-C6 only.** The build fails fast on any other target.

## Repository layout

```
core/
  at/                        at_parser.c/.h: line assembly, dispatch,
                             OK/ERROR/+xxERR mapping
  mt/                        mt_at.c, mt_cmdbox.c, mt_composition.c,
                             mt_rows.c, mt_comp_store.c, mt_transport.c:
                             the AT+MT semantics
  include/                   mt_*.h, at_parser.h, and the two port contracts:
                             hearth_port.h (downward), mt_matter.h (upward)
platform/esp32c6/
  CMakeLists.txt              EXTRA_COMPONENT_DIRS -> hearth_core, port
                             (both local; no cross-repo reference)
  sdkconfig.defaults          shared build defaults
  sdkconfig.defaults.esp32c6  C6 overrides (console TX moved off the AT UART pins)
  hearth_core/                IDF component wrapping core/ verbatim
  port/                       hearth_port.h implementation on ESP-IDF: the
                             UART link (at_uart.c), KV store, OS glue, log
  main/
    main.cpp                  C++: esp_matter runtime, callbacks, app_main,
                              and the mt_matter_* C-linkage bridge
    mt_devtypes.cpp           C++: device type ID -> esp_matter create thunks
    mt_evse.cpp, mt_meter.cpp C++: the EVSE and meter esp_matter thunks
  fw/flash.py                 two-stage flasher (RP2350 bridge, then the C6)
platform/nrf54l15/            NCS/Zephyr skeleton for the Ophelia-IV module
test/host/                    gcc unit tests for core/, the pure-C parts
```

## Build

```sh
cd platform/esp32c6
source ~/esp/esp-idf-v5.4.1/export.sh
source ~/esp/esp-matter/export.sh
idf.py -B build_wifi build

python3 fw/flash.py --build-dir build_wifi    # no BOOTSEL press needed
make -C ../../test/host run                 # host unit tests, no hardware
```

### The other two images

Thread-only and the combined image are build-time variants of the same
source, selected by an extra `sdkconfig.defaults` overlay. **`SDKCONFIG`
must be redirected into the build directory**: left at its default, the
variant's configuration is written to `./sdkconfig` and silently takes over
the WiFi build too.

```sh
idf.py -B build_thread -D SDKCONFIG=build_thread/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.thread" \
  build
```

The combined image additionally needs the SDK patchset, because runtime
transport selection is not something esp-matter or CHIP offer. Two patches,
pinned to the SDK commits this firmware builds against (`21aa3d1` for
esp-matter, `b87051a9` for the nested connectedhomeip checkout), live in
`sdk-patches/` and are applied by a script that refuses outright if either
checkout has moved off its pin, so an SDK bump forces a deliberate
re-evaluation rather than a silent re-apply:

```sh
scripts/apply-sdk-patches.sh          # --check to inspect, --revert to undo
idf.py -B build_combined -D SDKCONFIG=build_combined/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.combined" \
  build
```

The patches are inert in the other two builds: each preprocesses the guard
away, since only one network stack's Kconfig symbol is set in either. The
combined image chooses its stack at runtime with `AT+MTTRANSPORT`, stores
the choice, and reboots into it. Its endpoint capacity is lower when WiFi is
the active transport: see [Endpoint capacity](#endpoint-capacity) below.

## AT interface

Conventions are shared with the ESP-NOW firmware: `AT+CMD?` query,
`AT+CMD=<params>` set, `AT+CMD` execute; every command terminates in `OK` or
`ERROR`, with `+MTERR:<code>` on the line before `ERROR` for specific faults;
URCs (`+MT...`) may arrive at any time; the firmware emits `+MTREADY` once on
boot.

See `AT_MT_SPEC.md` in the docs repository for the authoritative command
reference. In brief: identity (`AT+CGMI/CGMM/CGMR`, `AT+MTVER?`), lifecycle
(`AT+MTSTATE?`, `AT+MTFABRICS?`, `AT+MTCOMMISSION`, `AT+MTCODES?`),
resets (`AT+MTRESET` keeps the endpoint composition, `AT+MTFRESET` erases it),
the data model (`AT+MTATTR` read/write with publish modes), the host-declared
endpoint composition (`AT+MTEP` / `AT+MTEPCLEAR` / `AT+MTEPAPPLY`), event
subscription (`AT+MTEVT`), the transport query (`AT+MTNET?`), and, on the
combined image only, the runtime stack selection (`AT+MTTRANSPORT`).

## Supported device types

`AT+MTEP=<id>` accepts these 52 device type IDs (firmware 1.0.0); anything
else answers `+MTERR:6`. The table is `platform/esp32c6/main/mt_devtypes.cpp`'s, rendered here
in its own order, and it grows by rows; IDs are read from esp-matter, never
transcribed. `AT_MT_SPEC.md` section 3.9 in the docs repository is the
authoritative copy.

| ID | Device type | | ID | Device type |
|---|---|---|---|---|
| 0x0100 | On/Off Light | | 0x007A | Extractor Hood |
| 0x0101 | Dimmable Light | | 0x0072 | Room Air Conditioner |
| 0x010C | Color Temperature Light | | 0x0078 | Cooktop |
| 0x0302 | Temperature Sensor | | 0x0303 | Pump |
| 0x010A | On/Off Plug-in Unit | | 0x0042 | Water Valve |
| 0x010B | Dimmable Plug-in Unit | | 0x0027 | Mode Select |
| 0x0015 | Contact Sensor | | 0x0073 | Laundry Washer |
| 0x0107 | Occupancy Sensor | | 0x0075 | Dishwasher |
| 0x0307 | Humidity Sensor | | 0x007C | Laundry Dryer |
| 0x0305 | Pressure Sensor | | 0x0076 | Smoke/CO Alarm |
| 0x0044 | Rain Sensor | | 0x0011 | Power Source |
| 0x0041 | Water Freeze Detector | | 0x0146 | Chime |
| 0x0043 | Water Leak Detector | | 0x0074 | Robotic Vacuum Cleaner |
| 0x002B | Fan | | 0x0079 | Microwave Oven |
| 0x0202 | Window Covering | | 0x0070 | Refrigerator |
| 0x0301 | Thermostat | | 0x007B | Oven |
| 0x010D | Extended Color Light | | 0x0077 | Cook Surface |
| 0x000F | Generic Switch | | 0x0510 | Electrical Sensor |
| 0x0071 | Temperature Controlled Cabinet | | 0x0514 | Electrical Meter |
| 0x000A | Door Lock | | 0x050F | Water Heater |
| 0x0106 | Light Sensor | | 0x0309 | Heat Pump |
| 0x0306 | Flow Sensor | | 0x0017 | Solar Power |
| 0x002C | Air Quality Sensor | | 0x0018 | Battery Storage |
| 0x010F | Mounted On/Off Control | | 0x050D | Device Energy Management |
| 0x0110 | Mounted Dimmable Load Control | | 0x050C | Energy EVSE |
| 0x002D | Air Purifier | | 0x0511 | Electrical Utility Meter |

The last fourteen rows, `0x0074` Robotic Vacuum Cleaner onwards, are the
beyond-parity work of the rounds after 0.5.0: the RVC and microwave batch,
the composed appliances (a refrigerator's cabinets, an oven's cavities and a
cooktop's surfaces are child endpoints declared with `AT+MTEP`'s parent
index), and the three Tier 3 energy rounds. Nine device types define an
`AT+MTEP` `<variant>`; the rest accept variant 0 only.

The extended color light carries a hue/saturation addition beyond stock
esp-matter, so hosts see `CurrentHue`/`CurrentSaturation` alongside XY and
mireds. The generic switch has no writable attribute (`CurrentPosition` and
`NumberOfPositions` are both read-only, though a host can still read
`CurrentPosition` over `AT+MTATTR` like any other attribute); it is driven
instead by `AT+MTSWITCH`, which raises a Switch cluster event. The
temperature controlled cabinet is the first device type with a meaningful
`AT+MTEP` variant: `AT+MTEP=0x0071` (variant 0) builds a numeric-setpoint
cabinet reachable entirely over `AT+MTATTR`; `AT+MTEP=0x0071,1` builds a
level-based one instead, whose `SupportedTemperatureLevels` label list is set
with the dedicated `AT+MTTEMPLEVELS` command rather than `AT+MTATTR`, since it
is a string list served by a CHIP delegate, not an ordinary attribute.
Deliberately absent, with reasons recorded in the design specs: Color Light
(no distinct device type ID exists; the host library's `MatterColorLight`
rides the `0x010D` row).

The Door Lock (`0x000A`) is the first device type beyond arduino-esp32
parity, and the first whose commands the firmware cannot answer on its own:
`LockDoor`/`UnlockDoor` are forwarded to the host as a `+MTCMD` URC and held
open for up to 1000 ms awaiting `AT+MTCMDRESP`, defaulting to deny on a
timeout, a missed link, or no answer at all. `AT+MTLOCK` is the separate
command the host uses to report the lock's actual state once it has moved,
so a subscribed controller sees the `LockOperation` event Matter expects.
`+MTCMD`/`AT+MTCMDRESP` are a generic frame, not specific to locks: a future
command needing the same kind of app-level verdict reuses it. See
`AT_MT_SPEC.md` sections 3.17 and 3.18.

Rows 31-38 (Water Valve, Mode Select, the OperationalState trio, Smoke/CO
Alarm, Power Source, Chime) reuse that same `+MTCMD` frame for their own
app-adjudicated commands (`Open`/`Close`, `Pause`/`Resume`/`Start`/`Stop`,
`PlayChimeSound`), each with its own dedicated state-reporting command -
`AT+MTVALVE`, `AT+MTMODES`, `AT+MTOPSTATE`, `AT+MTALARM`, `AT+MTCHIMESOUNDS`/
`AT+MTCHIME` - since none of their state lives behind a plain `AT+MTATTR`-
reachable attribute. Chime (`0x0146`) additionally papers over an esp-matter
SDK gap: the one function that registers its cluster with the data model
provider has no call site anywhere upstream, so the firmware calls it
manually. See `AT_MT_SPEC.md` sections 3.19-3.24 and `ARCHITECTURE.md`
section 8.6 for the full detail.

## Endpoint capacity

`MT_COMP_MAX_ENDPOINTS` is 28, and the WiFi-only and Thread-only images
serve all 28: measured free heap at startup on the harness's 28-endpoint
Phase 3 composition is 47,052 bytes on `build_wifi` and 117,040 on
`build_thread`, against 87,908 and 157,628 on a single light (firmware
0.12.0).

**The combined image is the constrained one, and only when WiFi is the
active transport.** It links both stacks, and the dormant one is a fixed
tax of about 32 KB, so it starts about 39.5 KB below `build_wifi`. Measured
on hardware 2026-08-20 with `test/mt_endpoint_cap.py`, WiFi-active:

| endpoints | free heap at startup | verdict |
|---|---|---|
| 1 | 48,360 | pass, full Phase 2 twice |
| 14 | 31,240 | pass |
| **20** | **24,204** | **pass, three full Phase 3 runs** |
| 21 | 23,076 | pass |
| 23 | 19,228 | pass |
| 24 | 17,392 | pass twice |
| 25 | 15,564 | **fail once, pass once, identical boot heap** |
| 28 | 7,608 | fail twice |

Thread-active, the same image serves the full 28 with 40,052 bytes free and
passes the whole operational criterion, so headroom is flat to within 160
bytes across a 27-endpoint span. **The cap is a WiFi-active property, not a
property of the image**, and stating it without that qualifier costs a
Thread user eight endpoints they actually have.

Composition acceptance is not the criterion: an over-large composition is
accepted, `AT+MTEPAPPLY` answers `OK`, the device often commissions, and it
then dies under controller traffic with lwIP `ERR_MEM` (CHIP error
`0x3000001`) on `SendMessage`, retransmission exhaustion and a CASE
timeout. Only real operational traffic sees it, which is what
`test/mt_endpoint_cap.py` drives.

The normative rule, because 25 failed and passed at the same boot heap and
because endpoints are not interchangeable:

1. **Keep `free heap at startup` at or above 24,000 bytes.** The firmware
   logs it on the console every boot (`mt_main: free heap at startup: N
   (BLE resident)`), so it is checkable on any composition without a bench.
2. Per-endpoint cost, derived from the table: about **1,166 bytes** for a
   simple type and about **2,210** for an energy type (electrical sensor
   and meter, water heater, heat pump, solar, battery, DEM, EVSE).
3. As a proxy at typical mixes, WiFi-active on the combined image: about
   **20** endpoints, or about **12** if the composition is energy-heavy.
   One light plus nineteen energy endpoints predicts 6,370 bytes free,
   below the 7,608 that failed twice, so a bare endpoint count is not a
   safe contract on its own.

**Re-checked on the 1.0.0 images**, same bench, two spot readings against
the curve above: 48,620 bytes free at one endpoint (table row: 48,360) and
24,272 at twenty (24,204). Both are within 260 bytes of the recorded
figures and the 20-endpoint reading is still above the 24,000 floor, so the
curve holds at 1.0.0. The table itself is left as the rig measured it
rather than being nudged by two spot readings, since a boot-to-boot spread
of a hundred-odd bytes on this bench is normal and the rows record what one
run produced.

Heap moves with the SDK, with cluster gates and with any
`sdkconfig.defaults*` edit, so re-measure with the rig rather than
re-deriving. The user-facing version of this, aimed at somebody choosing a
variant rather than changing the firmware, is in the `iLabs_Hearth` Arduino
library's `fw/README.md`.

## Documentation

The specifications, architecture decision record, testing plan and design
history live in a separate private repository,
[`iLabs_Hearth_docs`](https://github.com/PontusO/iLabs_Hearth_docs).

Source comments in this repository cite documents by name and section, for
example `AT_MT_SPEC.md 3.28`. Those citations resolve against that
repository. It is tagged with the same version as each firmware release, so
the documents matching a given firmware are a checkout rather than a guess.

## License

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Pontus Oldberg.

One exception: `platform/esp32c6/fw/flash.py` is **LGPL-2.1-or-later**,
because it imports esptool's Python API in-process. See the SBOM below.

## Software Bill of Materials

Recorded 2026-07-27 for firmware v0.1.0. Two separate concerns, and the
distinction matters: what is **linked into the firmware image** you flash, and
what is **host tooling** that never reaches the device.

### 1. First-party

| Component | Where | Licence |
|---|---|---|
| Hearth application (`platform/esp32c6/main/`) | this repo | MIT |
| `core/at/` AT engine | this repo (imported from `iLabs_AT_ESP-now`'s `at_core`, then diverging) | MIT |
| `platform/esp32c6/fw/flash.py` two-stage flasher | this repo | **LGPL-2.1-or-later** |
| `platform/esp32c6/fw/RP2350USB2Serial.ino.uf2` | this repo, prebuilt | MIT |

### 2. Linked into the firmware image

Everything here is permissive. **No copyleft licence reaches the image.**

| Component | Version | Licence |
|---|---|---|
| ESP-IDF | v5.4.1 | Apache-2.0 |
| esp-matter | release/v1.5 (`21aa3d1`) | Apache-2.0 |
| connectedhomeip (CHIP) | `b87051a9` | Apache-2.0 |
| Mbed TLS | bundled with ESP-IDF | Apache-2.0 **OR** GPL-2.0-or-later, recipient's choice. **Taken here under Apache-2.0.** |
| nlassert, nlio | CHIP third-party | Apache-2.0 |
| Espressif WiFi / Bluetooth libraries | bundled with ESP-IDF | Espressif proprietary, binary redistribution permitted on Espressif silicon. Not open source. |
| GCC runtime (`libgcc`) | RISC-V toolchain | GPL-3.0 **with GCC Runtime Library Exception**, which exempts compiled output |

Managed components resolved into the build tree (`managed_components/`). The
linker drops the ones the application does not reference, so not all of these
are present in the final image:

| Component | Version | Licence |
|---|---|---|
| `esp-serial-flasher` | 0.0.11 | Apache-2.0 |
| `esp_secure_cert_mgr` | 2.9.2 | Apache-2.0 |
| `esp_delta_ota` | 1.1.4 | Apache-2.0 |
| `esp_encrypted_img` | 2.3.0 | Apache-2.0 |
| `esp_insights` | 1.3.4 | Apache-2.0 |
| `esp_diagnostics` | 1.3.3 | Apache-2.0 |
| `esp_diag_data_store` | 1.1.1 | Apache-2.0 |
| `esp_rcp_update` | 1.3.1 | Apache-2.0 |
| `mdns` | 1.11.3 | Apache-2.0 |
| `json_generator` | 1.1.2 | Apache-2.0 |
| `json_parser` | 1.0.3 | Apache-2.0 |
| `rmaker_common` | 1.8.5 | Apache-2.0 |
| `rmaker_cmd_resp`, `rmaker_common_events`, `rmaker_console`, `rmaker_system_ctrl`, `rmaker_time_sync`, `rmaker_work_queue` | 1.0.x | Apache-2.0 |
| `button` | 4.2.0 | Apache-2.0 |
| `cmake_utilities` | 1.1.1 | Apache-2.0 |
| `led_strip` | 1.0.0 | Apache-2.0 |
| `cbor` (TinyCBOR) | 0.6.1~4 | MIT |
| `jsmn` | 1.1.0 | MIT |

### 3. Host tooling (never reaches the device)

| Component | Licence | Note |
|---|---|---|
| esptool (iLabs fork, adds `RP2040Reset`) | **GPL-2.0-or-later** | Imported in-process by `platform/esp32c6/fw/flash.py`. Redistributing the fork carries GPL source obligations. |
| pyserial | BSD-3-Clause | |
| rich (optional) | MIT | |
| `chip-tool` | Apache-2.0 | Test controller only |

### Obligations when distributing

- **Apache-2.0 (§4):** ship the licence text, retain attribution notices, mark
  files you modified, and reproduce any upstream `NOTICE` contents. In practice
  one third-party notices file accompanying the binary.
- **MIT:** retain the copyright and permission notice.
- **`platform/esp32c6/fw/flash.py`:** LGPL-2.1-or-later on its own; a combination distributed
  with esptool is GPL-2.0-or-later. If you would rather ship the flasher under
  MIT, invoke esptool as a subprocess instead of importing it, which makes the
  two mere aggregation.
- **Espressif blobs:** redistributable in binary form as part of a product using
  Espressif silicon, per Espressif's licence.

### Not a licensing matter

Matter is a trademark of the Connectivity Standards Alliance. Apache-2.0 grants
rights to the connectedhomeip *code*; it grants no trademark rights and implies
no certification. This firmware is not certified, uses development test
credentials (VID `0xFFF1`), and is not branded as a Matter product.
