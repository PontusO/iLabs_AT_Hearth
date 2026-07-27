# iLabs_AT_Hearth

An AT command stack for the ESP32-C6 coprocessor.

`iLabs_AT_Hearth` turns an ESP32-C6 into a **Matter co-processor** that a host
MCU drives over a simple UART AT interface (`AT+MT...`), the same way
[`iLabs_AT_ESP-now`](https://github.com/PontusO/iLabs_AT_ESP-now) exposes ESP-NOW
over `AT+EN...`. The two firmwares are **single-purpose images that share one
engine**: the host reflashes the C6 over UART to switch personality. There is no
combined ESP-NOW + Matter binary (see *Design* below for why).

## Status

**Early scaffold (Phase B1).** This repo currently contains a working AT
skeleton that proves the shared `at_core` engine drives a second personality: it
answers a tiny `AT+MT...` table over the same UART, with its own `+MTERR` error
namespace. **No Matter stack is wired up yet** - commissioning, the data model
and the full `AT+MT` command set arrive in Phase B2-B4 of the
[integration plan](https://github.com/PontusO/iLabs_AT_ESP-now/blob/main/docs/matter-integration-plan.md).

## Architecture

```
                 components/at_core           (lives in iLabs_AT_ESP-now)
                  ╱                ╲            engine + at_uart + link_mgr
   iLabs_AT_ESP-now/main         iLabs_AT_Hearth/main
   AT+EN...  (ESP-NOW)           AT+MT...  (Matter, C6-only)
```

Both firmwares register their own command table with the same subsystem-agnostic
parser engine (`at_register_commands()`), speak the same AT conventions, and
bring the radio up through the same `link_mgr`. `at_core` is **not vendored
here**: it is pulled straight from the sibling ESP-NOW repo so there is a single
source of truth, and a future single-binary merge stays mechanical.

## Requirements

- **ESP-IDF v5.5.4** (the version esp-matter `release/v1.5` targets).
- **[`iLabs_AT_ESP-now`](https://github.com/PontusO/iLabs_AT_ESP-now) checked out
  as a sibling directory** - it provides `components/at_core`, referenced via
  `EXTRA_COMPONENT_DIRS` in the top-level `CMakeLists.txt`:
  ```
  src/git/
    iLabs_AT_ESP-now/     <- provides components/at_core
    iLabs_AT_Hearth/      <- this repo
  ```
- **esp-matter** (`release/v1.5`) - only needed once the Matter stack lands
  (Phase B2); the current skeleton builds without it.
- **Target: ESP32-C6 only.** The build fails fast on any other target.

## Repository layout

```
CMakeLists.txt              EXTRA_COMPONENT_DIRS -> ../iLabs_AT_ESP-now/components
sdkconfig.defaults          shared build defaults
sdkconfig.defaults.esp32c6  C6 overrides (console TX moved off the AT UART pins)
main/
  main.c                    boot: AT UART -> register AT+MT table -> start parser
  mt_at.c                   AT+MT command handlers + "+MTERR" engine config
  include/
    mt_at_config.h          identity, line length, parser task tuning
    mt_at.h
```

## Build

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## AT interface

Conventions are shared with the ESP-NOW firmware: `AT+CMD?` query,
`AT+CMD=<params>` set, `AT+CMD` execute; every command terminates in `OK` or
`ERROR`, with `+MTERR:<code>` on the line before `ERROR` for specific faults;
URCs (`+MT...`) may arrive at any time; the firmware emits `+MTREADY` once on
boot.

**Implemented today (skeleton):**

| Command | Reply |
|---|---|
| `AT` | `OK` |
| `ATE0` / `ATE1` | echo off / on, `OK` |
| `AT+CGMI` | `iLabs Electronics` |
| `AT+CGMM` | `ESP32-C6 Hearth` |
| `AT+CGMR` | firmware version |
| `AT+MTVER?` | `+MTVER:<version>` |
| unknown `AT+...` | `+MTERR:8`, `ERROR` |

**Planned (Phase B4):** `AT+MTINIT`, `AT+MTRESET`, `AT+MTSTATE?`,
`AT+MTCOMMISSION` (+ `+MTCOMMISSION:STARTED|COMPLETE|FAILED` URCs), `AT+MTCODES?`,
`AT+MTFABRICS?`, `AT+MTEP` (endpoint create), `AT+MTATTR` (attribute get/set,
with `+MTATTR:...` URCs on controller-driven writes).

## Design

The two-firmware-one-engine approach, the C6-only Matter-over-WiFi scope, the
mode-switch-by-reflash model, and the phased roadmap are documented in the
[ESP-NOW + Matter integration plan](https://github.com/PontusO/iLabs_AT_ESP-now/blob/main/docs/matter-integration-plan.md)
in the sibling repo.

## License

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Pontus Oldberg.
