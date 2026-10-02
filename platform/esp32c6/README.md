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

Every image needs the SDK patchset, and the combined image depends on all of
it: runtime transport selection is not something esp-matter or CHIP offer,
and the requestor has to be Hearth's (B665, below). Three patches, pinned to
the SDK commits this firmware builds against (`21aa3d1` for
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

The two transport patches (`0001-*`) are inert in the other two builds: each
preprocesses the guard away, since only one network stack's Kconfig symbol is
set in either. `esp-matter/0002-hearth-app-owned-ota-requestor.patch` acts on
every image: it stops esp-matter installing a requestor of its own when the
application defines `mt_app_owns_ota_requestor()`, which
`main/hearth_ota_esp.cpp` does. An image built from an unpatched tree still
links; without it the WiFi image happens to win the race and the combined
image does not. The
combined image chooses its stack at runtime with `AT+MTTRANSPORT`, stores
the choice, and reboots into it. Its endpoint capacity is lower when WiFi is
the active transport: see below.

### A stale `sdkconfig` silently beats your `sdkconfig.defaults`

An existing `sdkconfig` wins over `sdkconfig.defaults`, so changing a default
does nothing to a build directory that already has one, and the build is
perfectly happy about it. This is not only the `build_thread` trap: in the FOTA
round it bit **all three** variants at once, each carrying a
`# CONFIG_ENABLE_OTA_REQUESTOR is not set` line from an earlier session, and
the resulting images looked plausible (they grew by about 34 kB instead of
about 49, because Hearth's own files link either way while esp-matter's
requestor init is compiled out and endpoint 0 never gets the cluster).

Whenever a `sdkconfig.defaults*` edit is supposed to change an image, move the
build directory's `sdkconfig` aside, let the build regenerate it, and diff the
two, so nothing local is lost silently:

Mind where each variant's file lives: `build_wifi` uses the default location,
`./sdkconfig` at the platform root, while the other two keep theirs inside
their build directory because `SDKCONFIG` is redirected there.

```sh
cd platform/esp32c6
mv sdkconfig sdkconfig.stale                       # the wifi build's
idf.py -B build_wifi reconfigure                   # regenerates it
diff sdkconfig.stale sdkconfig                     # expect ONLY the intended symbol
grep -n 'CONFIG_ENABLE_OTA_REQUESTOR' sdkconfig

mv build_thread/sdkconfig build_thread/sdkconfig.stale
idf.py -B build_thread -D SDKCONFIG=build_thread/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.thread" \
  reconfigure
diff build_thread/sdkconfig.stale build_thread/sdkconfig
```

and the same again for `build_combined` with its own overlay. Deleting instead
of moving works too and throws away any local tuning without saying so, which
is why the recipe moves and diffs. In the round that found this, the diff came
back with the intended symbol and nothing else in all three variants, which is
the answer you want before you trust the rebuilt image.

## Firmware over the air

The C6 runs the Matter OTA Requestor and has **no image store of its own**:
every downloaded block is announced to the host, pulled over the AT link and
acknowledged, and the host stages the bundle, judges it, and flashes the C6
back over serial recovery. The single app partition is unchanged by that, and
the C6 never reboots itself on apply. The wire contract is `AT_MT_SPEC.md`
sections 3.30 to 3.33; the round record is `ARCHITECTURE.md` section 8.21.

**What this port wires.** `CONFIG_ENABLE_OTA_REQUESTOR=y` in all three
variants makes esp-matter add the Requestor and Provider client clusters to
endpoint 0. The requestor, driver, downloader and Hearth's image processor are
`platform/common/hearth_ota_requestor.cpp` (shared with the nRF port);
`main/hearth_ota_esp.cpp` is the C6's own half. It is wired from `app_main`
**immediately after `esp_matter::start()` returns**, under the CHIP stack lock.
esp-matter would start a requestor of its own on `kDnssdInitialized`, whose
download would go to an OTA partition this firmware does not have, and
whichever calls `SetRequestorInstance()` second is a silent no-op. Wiring
first was the original answer, and it is a race: on the combined image the
event is posted during `Server::Init()` (its platform DNS-SD backend's
`mdns_init()` answers synchronously), and Hearth lost on every
uncommissioned boot and 3 of 10 commissioned ones (B665, 2026-10-01). The SDK
patch above removes esp-matter's start instead. The console says which one is
wired, every boot; an `E hearth_ota: another OTA requestor was registered
first` line means the patch is missing:

```
I (617) hearth_ota_esp: product version 65541 "1.4.1" in force
I (1777) hearth_ota: OTA requestor wired, mode 0
```

The `NotifyUpdateApplied` that follows an applied update waits for the
network: the driver's own first-run path fires 1.7 s into boot, when no network
and no CASE session exist yet, and so does `kServerReady` on the combined
image. It goes out 5 s after an IPv6 address (WiFi) or 10 s after a Thread
attach (`hearth_ota_network_event()`), the same settle the MG24 port uses.

**Where the version comes from: the host, not this image.** `AT+MTSWVER`
declares it, `core/` persists it under `mt_cfg` (`swver`, `swverstr`), and a
`ConfigurationManager` subclass installed with `SetConfigurationMgr()` before
`esp_matter::start()` answers it. That subclass is the whole of the value
change: esp-matter creates Basic Information's two version attributes
`MANAGED_INTERNALLY`, so an attribute write is refused, and CHIP answers every
read out of `ConfigurationMgr()`. So a controller sees the declared version
live, with no reboot, and the compile-time `0` is never what anybody reads.
The requestor, however, latches the version at `Init()`, so **a version
declared after boot reaches `QueryImage` only from the next boot**. FOTA is
also off at every boot: the mode is not persisted, `AT+MTOTA?` reads `0` after
every reset, and only a host that enables it gets any `+MTOTA` line at all.

**What a bench operator types.** The host side of a transfer, in order:

```
AT+MTSWVER=65540,"1.4.0"     declare the running product version, then reboot
AT+MTOTA=1                   enable; +MTERR:8 means this image has no requestor
AT+MTOTA?                    -> +MTOTA:1,IDLE,0,wifi   (mode,state,percent,variant)
                             ... announce a provider from chip-tool ...
                             URC +MTOTA:QUERYING / AVAILABLE,65541 / DOWNLOADING,0
                             URC +MTOTA:BLOCK,<seq>,<len>
AT+MTOTAGET=<seq>            -> +MTOTABLK lines (96 bytes each), then OK
AT+MTOTAACK=<seq>            -> OK, and the next block is requested
                             URC +MTOTA:DOWNLOADED  (after the LAST ack)
AT+MTOTASTAGED=1             the bundle verified -> URC +MTOTA:APPLY
AT+MTOTASTAGED=0,<reason>    refused -> +MTOTA:ERROR,cancelled, then IDLE
AT+MTOTA=0                   abort anything in flight and go quiet
```

A block that is not acknowledged within 5 s aborts the transfer with
`+MTOTA:ERROR,abort`, and **every terminal `ERROR` is followed by a closing
`+MTOTA:IDLE`**. `AT+MTOTA=2` queries the `DefaultOTAProviders` attribute and
only that one, so it needs `chip-tool otasoftwareupdaterequestor write
default-otaproviders` first; an announcement does not fill that list.

**Running the bench phase against this port.** Phase 4 of the regression
harness is the same procedure, scored and baselined, with the harness playing
the host (`TESTING.md` section 9):

```sh
export MT_OTA_IMAGE_TOOL=~/esp/esp-matter/connectedhomeip/connectedhomeip/src/app/ota_image_tool.py
python3 test/mt_regression.py \
  --port $(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00) \
  --phase 4 --include-ota \
  --ota-provider ~/esp/esp-matter/connectedhomeip/connectedhomeip/out/provider/chip-ota-provider-app \
  --ota-image-tool "$MT_OTA_IMAGE_TOOL"
```

Without `--include-ota` every row reports `[SKIP]` and the run still exits 0.
The phase does not commission and does not wipe the chip-tool storage, so run
Phase 2 first on a factory-fresh device. The baseline is
`test/baselines/wifi-ota.json`, 64 rows, recorded 2026-09-07 on `build_wifi`.
The full 20,562-byte test image (21 blocks) relayed in **8.888 s** at 115200
on that run, host loop included, which the AT link and not the radio sets: a
block leaves as eleven `+MTOTABLK` lines of up to 192 hex digits. Raise the
link with `AT+MTBAUD` for a real bundle.

### What firmware over the air costs

`CONFIG_ENABLE_OTA_REQUESTOR` was `n` from the single-app-partition switch
(2026-07-28) until the FOTA round turned it back on for all three images. The
requestor downloads; the host stores and flashes, so the single app partition
stays and the C6 never reboots itself on apply. Measured on 2026-09-07, before
at commit `eb28dd1` (requestor off) and after at `843e8b4` (requestor on plus
`platform/common/hearth_ota_requestor.cpp` and `main/hearth_ota_esp.cpp`, the
review fixes included), each build directory reconfigured from a clean tree
first; the figure is
`idf.py`'s own `ilabs_at_hearth.bin binary size`:

| build directory | before, eb28dd1 | after, 843e8b4 | cost |
|---|---|---|---|
| `build_wifi` | 1,829,728 | 1,879,216 | +49,488 |
| `build_thread` | 1,730,992 | 1,781,056 | +50,064 |
| `build_combined` | 2,125,632 | 2,176,080 | +50,448 |

The three agree within a kilobyte, which is what you would expect of a cost
that is entirely CHIP's OTA requestor, BDX downloader and the relay glue, and
nothing transport-specific. It also matches, in the opposite direction, the
"~45 KB on its own" the 2026-07-28 note recorded for dropping the requestor.

Free heap at startup on `build_wifi`, the line the firmware logs every boot,
on the same one-endpoint composition (a single dimmable light, `0x0100`) both
times:

| | free heap at startup |
|---|---|
| before, eb28dd1 | 140,980 |
| after, 843e8b4 | 138,960 and 138,976 on two boots |

So about 2 KB, against a boot-to-boot spread of a few hundred bytes on this
bench. The block buffer is NOT in that figure: it is a `hearth_stage_alloc()`
block taken when a download starts and given back when it ends, so the idle
cost is the statics only.

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

**Re-measured after the pay-per-composition round (2026-09-03).** That round
moved the C6's host-fed stores and delegate pools from fixed `.bss` to
per-endpoint allocation (`ARCHITECTURE.md` 8.20), which returned about 42 KB
of free heap at one endpoint on the single-transport images and about 45 KB
on the combined image WiFi-active. The per-endpoint costs above are
unchanged (a light still costs a light's worth of esp_matter objects); the
round moved the curve's intercept, not its slope, so the whole curve shifts
up by that reclaim. The consequence for the row that mattered: the combined
image WiFi-active now serves the **full 28-endpoint table**, measured at
45,156 to 45,288 bytes free where the 2026-08-20 table recorded 7,608 and a
double failure. The transport-dependent cap is retired, and the "about 20 of
a typical mix" proxy with it; the 24,000-byte floor still governs, and the
combined image now clears it at 28 with about 21 KB to spare. The
2026-08-20 table is left with its date rather than redrawn from one
measured count, since only the full 28 was re-swept (by the Phase 3
acceptance run, which drives real controller traffic, the criterion the rig
uses). These figures ship with the firmware; the library's `fw/README.md`
and its bundled images move to them when a release is cut.

**Re-measured on the 1.3.0 images (2026-10-02).** The FOTA round turned the
OTA requestor on in every variant, and 1.3.0 also turns the CHIP shell off
(it was never linked, so that moves nothing). Measured with
`test/mt_endpoint_cap.py` on the release images, `build_{wifi,thread,combined}`
at `32799fe`, the first N entries of the harness's Phase 3 composition, BLE
resident, every trial commissioned and passing the operational criterion:

| image, active transport | 1 endpoint | 24 | 28 |
|---|---|---|---|
| `build_wifi`, WiFi | 138,100 | 100,816 | 80,960 |
| `build_thread`, Thread | | | 150,848 |
| `build_combined`, Thread | | | 74,136 |
| `build_combined`, WiFi | 98,796 | 61,360 | **41,364** |

The tightest row, the combined image WiFi-active at 28, reads 41,364 bytes
free against 45,156 to 45,288 at 1.2.0: the requestor costs about 3.9 KB
there, and the image still clears the 24,000-byte floor by about 17 KB, so
every variant still serves the full table. The Thread rows were measured at
28 only: each trial starts with a factory reset and commissions again, and
on this bench a re-keyed Thread device cannot be commissioned again on the
same border router until its old SRP registration is cleared (`ARCHITECTURE.md`
8.26, B684), so one trial per Thread row was the cost-effective point.

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
