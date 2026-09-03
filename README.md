# Hearth

**Hearth is a Matter co-processor firmware.** It turns a small radio module
into a complete Matter device that a host MCU drives over a plain UART AT
interface (`AT+MT...`): the host declares what the device is (its endpoint
composition), reads and writes attributes, answers the commands that need an
application verdict, and observes the Matter side through unsolicited
`+MT...` events. Everything Matter (commissioning, fabrics, the data model,
the network stack) lives on the module; everything product (what the device
does when the light turns on) stays on the host.

The product's identity is the **`AT+MT` wire contract**, specified in
`AT_MT_SPEC.md` in the docs repository. The same contract is served on more
than one silicon; a host written against it does not care which module is on
the other end of the UART.

## Platforms

| Platform | Module | Transports | Status |
|---|---|---|---|
| [`platform/esp32c6/`](platform/esp32c6/) | ESP32-C6 | WiFi, Thread, or runtime-selected combined | **Released 1.1.0**, feature complete, hardware-verified |
| [`platform/nrf54l15/`](platform/nrf54l15/) | Wurth Ophelia-IV (nRF54L15), Nordic nRF54L15 DK and nRF54LM20 DK | Thread | In development, not yet released: UART bootloader, persistence, the Matter commissioning core and the full 52-type catalogue are bench-proven on the Ophelia-IV; the two DKs are build targets |

Each platform directory is self-contained and carries its own README with
requirements, build instructions, flashing, platform-specific measurements
and its image's bill of materials.

Released firmware images and their flasher ship with the Arduino host
library, [`iLabs_Hearth`](https://github.com/PontusO/iLabs_Hearth).

## Status, stated plainly

1.1.0 (ESP32-C6) is released and hardware-verified: the device commissions,
is controllable from a Matter controller, and the host drives the whole
lifecycle over `AT+MT`. It is **not** a stability contract (the wire surface
is not frozen), **not** a product claim (the firmware is uncertified and
uses development credentials, VID `0xFFF1`, so consumer hubs are expected to
refuse it), and **not** a field-update story (the host-driven serial update
stays a draft on the C6; the nRF54L15 ships its own bootloader-based one).

## Architecture

```
core/                       portable C, no SDK header compiles here, ever
  at/                       the AT engine: parsing, dispatch, OK/ERROR/+xxERR
  mt/                       AT+MT semantics: composition, cmdbox, rows, transport
  include/                  the grammar headers and the two port contracts
platform/<name>/            one self-contained implementation per silicon
test/host/                  gcc unit tests for core/, no hardware needed
```

`core/` carries the whole `AT+MT` grammar and semantics. A platform
directory is a self-contained implementation of two port interfaces against
its own SDK, and nothing else:

- **`hearth_port.h`** downward: OS primitives, the UART link, a key-value
  store, logging.
- **`mt_matter.h`** upward: the Matter runtime (commissioning, the data
  model, the endpoint composition built at boot).

Both platforms build the same endpoint ids for the same composition, so a
host sees identical wire behaviour whichever module it drives.

## The AT interface

Conventions: `AT+CMD?` query, `AT+CMD=<params>` set, `AT+CMD` execute; every
command terminates in `OK` or `ERROR`, with `+MTERR:<code>` on the line
before `ERROR` for specific faults; URCs (`+MT...`) may arrive at any time;
the firmware emits `+MTREADY` once on boot.

`AT_MT_SPEC.md` in the docs repository is the authoritative command
reference. In brief: identity (`AT+CGMI/CGMM/CGMR`, `AT+MTVER?`), lifecycle
(`AT+MTSTATE?`, `AT+MTFABRICS?`, `AT+MTCOMMISSION`, `AT+MTCODES?`), resets
(`AT+MTRESET` keeps the endpoint composition, `AT+MTFRESET` erases it), the
data model (`AT+MTATTR` read/write with publish modes), the host-declared
endpoint composition (`AT+MTEP` / `AT+MTEPCLEAR` / `AT+MTEPAPPLY`), event
subscription (`AT+MTEVT`), the transport query (`AT+MTNET?`), and, on the
C6's combined image only, runtime stack selection (`AT+MTTRANSPORT`).

## Supported device types

`AT+MTEP=<id>` accepts these 52 device type IDs on the ESP32-C6 (firmware
1.1.0); anything else answers `+MTERR:6`. The nRF54L15 serves the same 52
(its README carries the per-batch list, the capacity rules and known
issues). `AT_MT_SPEC.md` section 3.9 in the docs repository is the
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

The catalogue includes composed appliances (a refrigerator's cabinets, an
oven's cavities and a cooktop's surfaces are child endpoints declared with
`AT+MTEP`'s parent index) and the whole Tier 3 energy surface. Nine device
types define an `AT+MTEP` `<variant>`; the rest accept variant 0 only.

Commands that need an application verdict (a lock's `LockDoor`, a valve's
`Open`, an operational-state `Start`) are forwarded to the host as a
`+MTCMD` URC and held open for `AT+MTCMDRESP`, defaulting to deny on a
timeout; each such device type has its own state-reporting command
(`AT+MTLOCK`, `AT+MTVALVE`, `AT+MTMODES`, `AT+MTOPSTATE`, `AT+MTALARM`,
`AT+MTCHIME`, ...). The generic switch is driven by `AT+MTSWITCH`, which
raises a Switch cluster event. See `AT_MT_SPEC.md` sections 3.17-3.24.

Platform-specific implementation notes (what rides an SDK quirk, measured
endpoint capacity, image variants) live in the platform READMEs.

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

Each platform README carries the bill of materials for its image and its
host tooling, including the one licence exception
(`platform/esp32c6/fw/flash.py` is LGPL-2.1-or-later; see the C6 README's
SBOM for the reasoning and the distribution obligations).

Matter is a trademark of the Connectivity Standards Alliance. Apache-2.0
grants rights to the connectedhomeip *code*; it grants no trademark rights
and implies no certification. This firmware is not certified, uses
development test credentials (VID `0xFFF1`), and is not branded as a Matter
product.
