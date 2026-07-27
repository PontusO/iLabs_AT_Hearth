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

One exception: `fw/flash.py` is **LGPL-2.1-or-later**, because it imports
esptool's Python API in-process. See the SBOM below.

## Software Bill of Materials

Recorded 2026-07-27 for firmware v0.1.0. Two separate concerns, and the
distinction matters: what is **linked into the firmware image** you flash, and
what is **host tooling** that never reaches the device.

### 1. First-party

| Component | Where | Licence |
|---|---|---|
| Hearth application (`main/`) | this repo | MIT |
| `at_core` AT engine | `iLabs_AT_ESP-now/components/at_core`, pulled cross-repo via `EXTRA_COMPONENT_DIRS` | MIT |
| `fw/flash.py` two-stage flasher | this repo | **LGPL-2.1-or-later** |
| `fw/RP2350USB2Serial.ino.uf2` | this repo, prebuilt | MIT |

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
| esptool (iLabs fork, adds `RP2040Reset`) | **GPL-2.0-or-later** | Imported in-process by `fw/flash.py`. Redistributing the fork carries GPL source obligations. |
| pyserial | BSD-3-Clause | |
| rich (optional) | MIT | |
| `chip-tool` | Apache-2.0 | Test controller only |

### Obligations when distributing

- **Apache-2.0 (§4):** ship the licence text, retain attribution notices, mark
  files you modified, and reproduce any upstream `NOTICE` contents. In practice
  one third-party notices file accompanying the binary.
- **MIT:** retain the copyright and permission notice.
- **`fw/flash.py`:** LGPL-2.1-or-later on its own; a combination distributed
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
