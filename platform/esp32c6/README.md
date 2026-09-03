# Hearth on the ESP32-C6

The original Hearth platform: the ESP-IDF project that implements the two
port contracts (`hearth_port.h`, `mt_matter.h`) against esp_matter, released
at 1.1.0 (1.0.0 was the feature-complete milestone). Ships three images: WiFi only, Thread only, and a combined image
that picks its stack at runtime with `AT+MTTRANSPORT`.

The C6 shares its lineage with
[`iLabs_AT_ESP-now`](https://github.com/PontusO/iLabs_AT_ESP-now), which
exposes ESP-NOW over `AT+EN...`: the two firmwares are single-purpose
images started from one shared AT engine and now running diverged copies of
it (converging again once ESP-NOW folds into this stack). The host reflashes
the C6 over UART to switch personality; there is no combined ESP-NOW +
Matter binary (see `ARCHITECTURE.md` in the docs repository for why).

## Requirements

- **ESP-IDF v5.4.1**, the version esp-matter `release/v1.5` validates against.
  Not v5.5.4, which the ESP-NOW firmware uses: esp-matter fails to build on it
  at `chip_gn`.
- **esp-matter** `release/v1.5`. Source both `export.sh` scripts before
  building, IDF first.
- **Target: ESP32-C6 only.** The build fails fast on any other target.

## Directory layout

```
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
the active transport: see below.

## Device type implementation notes

The 52-row catalogue (see the top-level README for the table) is
`main/mt_devtypes.cpp`'s; IDs are read from esp-matter, never transcribed.
Platform-specific notes:

The extended color light carries a hue/saturation addition beyond stock
esp-matter, so hosts see `CurrentHue`/`CurrentSaturation` alongside XY and
mireds. The temperature controlled cabinet's level-based variant
(`AT+MTEP=0x0071,1`) serves its `SupportedTemperatureLevels` label list
through a CHIP delegate, set with `AT+MTTEMPLEVELS` rather than `AT+MTATTR`.
Chime (`0x0146`) papers over an esp-matter SDK gap: the one function that
registers its cluster with the data model provider has no call site anywhere
upstream, so the firmware calls it manually. See `AT_MT_SPEC.md` sections
3.19-3.24 and `ARCHITECTURE.md` section 8.6 for the full detail.

## Endpoint capacity

`MT_COMP_MAX_ENDPOINTS` is 28, and the WiFi-only and Thread-only images
serve all 28: measured free heap at startup on the harness's 28-endpoint
Phase 3 composition is 47,052 bytes on `build_wifi` and 117,040 on
`build_thread`, against 87,908 and 157,628 on a single light (firmware
0.12.0).

**2026-09-01, after the row-staging change: every figure in this section
is now conservative.** The shared core made the `AT+MTROW` staging buffers
session-allocated (they exist only while a transfer is open), and the one
point re-measured since, a single light on `build_wifi` at commit 1976ba7,
came out at 99,192 bytes free against the 87,908 above: about 11.3 KB
returned. The curve and the derived per-endpoint costs below have NOT been
re-run and keep their 2026-08-20 provenance; treat them as a safe floor,
not as current values, and re-run `test/mt_endpoint_cap.py` before
tightening any limit that leans on them.

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

## Licensing note

The repository is MIT, with one exception on this platform:
`fw/flash.py` is **LGPL-2.1-or-later**, because it imports esptool's Python
API in-process. See the SBOM below.

## Software Bill of Materials

Recorded 2026-07-27 for firmware v0.1.0. Two separate concerns, and the
distinction matters: what is **linked into the firmware image** you flash, and
what is **host tooling** that never reaches the device.

### 1. First-party

| Component | Where | Licence |
|---|---|---|
| Hearth application (`main/`) | this repo | MIT |
| `core/at/` AT engine | this repo (imported from `iLabs_AT_ESP-now`'s `at_core`, then diverging) | MIT |
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

- **Apache-2.0 (section 4):** ship the licence text, retain attribution
  notices, mark files you modified, and reproduce any upstream `NOTICE`
  contents. In practice one third-party notices file accompanying the binary.
- **MIT:** retain the copyright and permission notice.
- **`fw/flash.py`:** LGPL-2.1-or-later on its own; a combination distributed
  with esptool is GPL-2.0-or-later. If you would rather ship the flasher under
  MIT, invoke esptool as a subprocess instead of importing it, which makes the
  two mere aggregation.
- **Espressif blobs:** redistributable in binary form as part of a product
  using Espressif silicon, per Espressif's licence.
