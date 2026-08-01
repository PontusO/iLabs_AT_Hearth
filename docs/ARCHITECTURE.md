# Architecture & Design Decisions — iLabs AT Hearth (ESP32-C6)

Status: **living document.** Consolidates the design of the Matter co-processor
firmware and the decisions taken during Phase B. The phased build plan and the
merge strategy live in the
[ESP-NOW + Matter integration plan](hearth-integration-plan.md);
this document is the Matter-firmware view and the decision record.

## 1. What this is

`iLabs_AT_Hearth` turns an ESP32-C6 into a **Matter co-processor** driven by a
host MCU (RP2350) over a UART AT interface (`AT+MT…`, see `AT_MT_SPEC.md`). It is
the sibling of `iLabs_AT_ESP-now`, which exposes ESP-NOW over `AT+EN…`.

## 2. Two firmwares, one engine

- **Two single-purpose images, shared design.** ESP-NOW and Matter ship as
  separate binaries that share the `components/at_core` component. The host
  reflashes the C6 over UART to switch personality (see `FIRMWARE_UPDATE_SPEC.md`).
  There is no combined ESP-NOW + Matter binary in v1.
- **`at_core`** (lives in the `iLabs_AT_ESP-now` repo, pulled cross-repo via
  `EXTRA_COMPONENT_DIRS`) provides the subsystem-agnostic AT parser engine
  (`at_parser`), the UART transport (`at_uart`), and the single radio owner
  (`link_mgr`). Each personality registers its own command table
  (`at_register_commands`) and error namespace, so a future merged binary is a
  mechanical "register both tables" step.
- **Matter personality files** (this repo, `main/`):
  - `main.cpp` (C++) — the esp_matter runtime: node + on/off-light endpoint +
    `esp_matter::start()`, the commissioning event callback, and the C-linkage
    `mt_matter_*` bridge into CHIP/esp_matter.
  - `mt_at.c` (C) — the `AT+MT` command handlers on `at_core`.
  - `mt_matter.h` / `mt_at.h` — C-linkage bridges so C (handlers) and C++
    (esp_matter) call each other without name-mangling.

## 3. Transport decision: Matter-over-WiFi (not Thread) for v1

- **WiFi, 2.4 GHz.** BLE is used for commissioning only. Chosen because it needs
  **no Thread Border Router** and reuses the WiFi stack the board already has.
- **Thread analysis (measured):** adding Thread *alongside* WiFi
  (`c6_wifi_thread`) costs **~164 KB** of flash. Two reasons it was deferred,
  neither of which is the flash:
  1. Thread requires a **Border Router** on-site to be reachable/commissioned;
     WiFi does not.
  2. On the current **4 MB** C6 with dual-OTA it was very tight (~4% free).
     The single-app partition (§4) removed that pressure entirely.

- **Thread-only variant, built and measured 2026-07-28.** `+164 KB` turns out
  to be the wrong number for what this firmware actually wants, because it
  measures WiFi **plus** Thread. Replacing WiFi with Thread is cheaper than
  WiFi:

  | image | size | free in the 3.75 MB slot |
  |---|---|---|
  | WiFi (`sdkconfig.defaults`) | 1,618,352 | 59% |
  | Thread (`+ sdkconfig.defaults.thread`) | 1,517,936 | 61% |

  Thread-only is **100,416 bytes smaller**, since OpenThread costs less than
  the WiFi stack it replaces. Flash was never going to be the obstacle.

- **Hardware-verified 2026-07-29.** Commissioned over BLE onto a Thread network
  and driven from a controller end to end. The device's CASE session addressed
  chip-tool at the border router's on-mesh (OMR) address, confirming the Matter
  traffic ran over 802.15.4 rather than any fallback. Rig: `otbr-agent` built
  from `ot-br-posix` on the dev workstation, radio a Nabu Casa ZBT-2 reflashed
  from its stock Zigbee EZSP firmware to `ot-rcp`.
  Two notes for whoever repeats this. `script/setup` is **not** required and
  should be avoided on a workstation: running `otbr-agent` directly yields a
  `wpan0` that a local commissioner can use, and the setup script chains
  firewall, NAT64, `dhcpcd` and `systemd-networkd` installers. The only
  persistent changes needed were `/var/lib/thread` and the D-Bus policy file
  `src/agent/otbr-agent.conf`, without which the agent aborts at
  `dbus_agent.cpp:67` and `ot-ctl` reports only a reset socket.

- **Dual-transport is not a config flip, and this is the real finding.**
  esp-matter's `c6_wifi_thread` reference adds a *secondary network
  commissioning endpoint* to the data model (`THREAD_NETWORK_ENDPOINT_ID=2`,
  `WIFI_NETWORK_ENDPOINT_ID=0`) and pins
  `ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=3`. Both collide head-on with C1: the
  host declares up to 16 endpoints over `AT+MTEP` and assumes it owns the
  endpoint space above 0. An endpoint the host never declared, sitting in the
  middle of that space, desynchronises every cached endpoint id. Supporting
  both transports in one image *concurrently* is therefore a **composition
  redesign**, not a Kconfig addition. See §3.1: one binary carrying both, with
  only one active at a time, avoids this specific collision.
- **Radio ownership:** in this *separate* Matter firmware, `esp_matter::start()`
  brings up WiFi/netif itself, so `at_core`'s `link_mgr` `LINK_MODE_MATTER` path
  is a no-op here and is reserved for the future merged binary (one radio owner).

### 3.1 Decision: one binary, transport selected at boot (agreed 2026-07-28, not built)

The goal is a single C6 image supporting both transports with **only one active
at a time**, the choice persisted and applied at boot. Not concurrent
operation, so the `secondary_network_interface` endpoint does not arise and
C1's endpoint contract is preserved.

| shape | switch cost | risk | lets a *commissioned* device change network |
|---|---|---|---|
| Two images, host reflashes | seconds | none; reuses the existing personality-switch pattern | no |
| **One binary, boot-time select** | a reboot | **high, see below** | no |
| Genuine dual-interface | none | C1 composition redesign | yes |

**Chosen: one binary, boot-time select**, and recorded honestly: this carried
the highest implementation risk of the three and was chosen knowingly.

The risk is that compiling both stacks in *is* the dual-interface case as far
as CHIP is concerned. `THREAD_NETWORK_ENDPOINT_ID` and
`WIFI_NETWORK_ENDPOINT_ID` are compile-time, both network commissioning
drivers register, and a Matter data model is meant to be static per device.
Presenting exactly one at runtime means suppressing the other by hand. Two
hooks look relevant and **neither has been traced yet**:
`CONFIG_ESP_MATTER_ENABLE_OPENTHREAD=n`, which defers Thread stack startup out
of `esp_matter::start()` "with more flexibility" (esp_matter Kconfig line 236),
and the per-driver `CONFIG_*_NETWORK_COMMISSIONING_DRIVER` switches, which are
compile-time and so unlikely to suffice alone. Tracing those is the first task
of this work, not an implementation detail of it.

**What this does not buy.** A transport switch needs recommissioning whichever
shape is used: a device booted into Thread holds no Thread dataset, so it is on
no network, and the fabric credentials surviving in NVS cannot be reached.
Reboot-switch and reflash-switch are both install-time operations ending in
"commission the device". The single binary wins one artifact and convenience,
not a capability.

**Precondition:** the Thread image must be proven on hardware first, so this is
built on something known to work rather than on two unknowns at once.

**The suppression hook, traced.** Both mechanisms above turned out
insufficient alone: `CONFIG_ESP_MATTER_ENABLE_OPENTHREAD=n` removes Thread
from the build entirely rather than choosing it at runtime, and the
`CONFIG_*_NETWORK_COMMISSIONING_DRIVER` switches are compile-time. With both
drivers compiled in, `ESPMatterNetworkCommissioningClusterServerInitCallback`
registers both network commissioning clusters unconditionally and the second
one to touch the shared endpoint `VerifyOrDie`s in
`ServerClusterInterfaceRegistry::Create`. Suppressing the inactive driver
needs a change inside esp-matter itself, so the fix lives as a small, pinned
patchset against the SDK checkout rather than in app code:
`sdk-patches/esp-matter/0001-hearth-runtime-transport-selection.patch`, base
commit `21aa3d1` (`release/v1.5`). It edits two files:
`network_commissioning_integration.cpp`, so exactly one driver registers per
boot, and `esp_matter_core.cpp`, so `esp_matter::start()` initializes only the
active transport's stack and the OpenThread launch is skipped when WiFi is
active. Both edits gate on a weak C hook, `mt_active_transport_is_thread()`,
absent meaning WiFi; the app supplies the real definition once boot-time
transport selection exists. Every patched line sits inside a
`CONFIG_THREAD_NETWORK_COMMISSIONING_DRIVER &&
CONFIG_WIFI_NETWORK_COMMISSIONING_DRIVER` (or the CHIP-level WiFi-and-Thread
equivalent) guard, so a build with only one driver compiled in preprocesses
identically to stock esp-matter: this is what lets the WiFi-only and
Thread-only images built earlier keep working unmodified once the patch is
applied. `scripts/apply-sdk-patches.sh` applies, checks, and reverts the
patchset against the checkout named by `ESP_MATTER_PATH` (default
`~/esp/esp-matter`), and refuses outright if that checkout's `HEAD` has moved
off the pin, so an SDK bump forces a deliberate re-evaluation of the patches
rather than a silent, possibly-broken re-apply.

**The combined build variant, built and measured 2026-08-01.**
`sdkconfig.defaults.combined` is the third overlay, alongside the WiFi-only
base defaults and `sdkconfig.defaults.thread`: it starts from the Thread
overlay (OpenThread platform settings, `CONFIG_ENABLE_OTA_REQUESTOR=n`) and
adds back `CONFIG_ENABLE_WIFI_STATION=y` from the base defaults, so both
stacks compile into one image. Both network-commissioning endpoint IDs are
left at their default 0 (`CONFIG_THREAD_NETWORK_ENDPOINT_ID=0`,
`CONFIG_WIFI_NETWORK_ENDPOINT_ID=0`, neither set by the overlay), matching
the no-secondary-endpoint requirement above.

```
scripts/apply-sdk-patches.sh
idf.py -B build_combined -D SDKCONFIG=build_combined/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.combined" \
  build
```

This is the first build that actually compiles the patched code paths:
`build_b4` and `build_thread` each preprocess the guard away, since only one
driver's Kconfig symbol is set in either. It built clean, no new errors
inside the patch hunks. With no app-side definition of
`mt_active_transport_is_thread()` yet, the unresolved weak symbol reads as
null, which the patch treats as "not Thread," so this image boots
WiFi-active; wiring the real boot-time selection is later work; this task is
build-and-measure only.

Measured (`idf.py -B <dir> size`, `.bin` size on disk):

| variant | `.bin` size | DIRAM used | `.bss` |
|---|---|---|---|
| `build_b4` (WiFi only) | 1,618,112 B | 220,981 B | 79,520 B |
| `build_thread` (Thread only) | 1,517,792 B | 184,271 B | 90,944 B |
| `build_combined` (both) | 1,912,704 B | 255,765 B | 108,296 B |

Combined vs `build_b4`: +294,592 B flash, +34,784 B DIRAM, +28,776 B
`.bss`, the dormant-Thread tax on top of the WiFi image. Combined vs
`build_thread`: +394,912 B flash, +71,494 B DIRAM, +17,352 B `.bss`, the
dormant-WiFi tax on top of the Thread image, larger because the WiFi stack
itself is bigger than OpenThread's. Both deltas land in the tens-of-KB range
the design predicted, not hundreds.

The factory slot is 3,840 KB (`0x3C0000`, `partitions.csv`); the combined
image leaves 2,019,456 bytes (51%) free, per `esptool`'s own accounting
during the build (`0x1ed080 bytes (51%) free`), comfortably inside the
~2.1 MB margin the single-app partition switch (section 4) was sized for.

## 4. Partition layout: single-app + host OTA

- The C6 does **not** self-update; the RP2350 host flashes it (see
  `FIRMWARE_UPDATE_SPEC.md`). So the C6 uses a **single-app partition** (one
  ~3.6 MB app slot) rather than dual-OTA, and disables the Matter OTA Requestor.
  This gives ~1.9 MB free for the ~1.84 MB WiFi+Thread image.
- **Done, 2026-07-28.** `partitions.csv` is single-app (`factory`, 0x20000,
  3840 KB) and `CONFIG_ENABLE_OTA_REQUESTOR=n`. Measured: the app went from
  0x196410 to 0x18b1b0, so dropping the OTA Requestor also *shrank* it by about
  45 KB, and free space went from 15% of a 1.88 MB slot to 59% of a 3.75 MB one,
  i.e. 301 KB to 2.31 MB. Thread's ~164 KB now lands with ~2.1 MB to spare
  rather than the ~4 to 7% the dual-OTA table left.
- **NVS is deliberately unmoved by the switch,** which is what lets an already
  commissioned device survive it. `nvs` stays at 0x10000/48K and `nvs_keys` at
  0x1c000/4K; only `phy_init` shifts down by the removed `otadata`'s 8 KB, and
  that partition is unused because `CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` is
  not set. `flash.py` writes 0x0, 0xC000 and 0x20000 only, never 0x10000.
- **Verified on hardware, 2026-07-28.** A device commissioned under the
  dual-OTA table was reflashed with the single-app one and came back on the
  same fabric, still controllable from the controller and from the local
  button. Changing the partition layout is therefore not a recommissioning
  event, provided `nvs` keeps its offset. Any future layout change must hold
  that invariant or say loudly that it does not.

## 5. UART topology

- **AT link:** `at_core` UART1 on the host-bridge pins **GPIO16/17** (where the
  RP2350 bridge is wired). This is the `AT+MT` channel.
- **Console/logs:** moved to **GPIO2** (RX parked on GPIO4) so boot/Matter logs
  never share a pin with the AT stream. Same arrangement as the ESP-NOW firmware.

## 6. Toolchain

- **ESP-IDF v5.4.1** + **esp-matter release/v1.5** (esp-matter's validated IDF
  for C6). This is *separate* from the ESP-NOW firmware's IDF v5.5.4 — esp-matter
  does not build on 5.5.4 (fails at `chip_gn`). The two firmwares therefore build
  against different IDFs; fine, since they are separate images. `at_core` compiles
  cleanly under both.
- Build: source `~/esp/esp-idf-v5.4.1/export.sh` then `~/esp/esp-matter/export.sh`,
  then `idf.py set-target esp32c6 && idf.py build`. Requires `iLabs_AT_ESP-now`
  checked out as a sibling (provides `at_core`).

## 7. Commissioning

- BLE-commissioned onto a WiFi fabric. Development uses esp-matter test creds
  (discriminator `3840`, passcode `20202021`); `AT+MTCODES?` returns the QR /
  manual codes. Controller for bring-up: `chip-tool` (built with the Avahi mDNS
  backend on the dev host). Production DAC/factory-data story: TBD.

## 8. Repository layout

```
CMakeLists.txt                 esp-matter integration + at_core sibling dir
partitions.csv                 single-app, one 3840 KB slot (see §4)
sdkconfig.defaults[.esp32c6]   Matter config + console/AT UART split
main/
  main.cpp                     esp_matter runtime + mt_matter_* bridge
  mt_at.c                      AT+MT command handlers on at_core
  include/  mt_at.h, mt_matter.h, mt_at_config.h
fw/  flash.py, RP2350USB2Serial.ino.uf2   host-bridge flasher (PC-side)
docs/  AT_MT_SPEC.md, FIRMWARE_UPDATE_SPEC.md, ARCHITECTURE.md (this file)
```

## 8.1 Portability: what a different co-processor would cost

Recorded 2026-07-29, not a plan. Prompted by asking whether `AT+MT` could serve
an nRF54L or an EFR32 instead of the C6. The answer turned out to be measurable
rather than speculative, and the measurement is the kind of thing that is
obvious today and invisible in six months.

**`AT+MT` is a host contract, not a description of what the C6 happens to do.**
`AT_MT_SPEC.md` contains five references to ESP anything, all incidental: the
title, the model string, a comparison to the arduino-esp32 API, and the
flow-control section, which is board wiring. The command grammar, the `+MTERR`
code space, the URC set and the event bit map are Matter and CHIP concepts that
every vendor's SDK shares, because they are all connectedhomeip underneath.

**Measured split of this firmware:**

| | lines | real dependency |
|---|---|---|
| `mt_at.c` | 635 | `vTaskDelay`, `esp_restart`. Two calls. |
| `mt_composition.c` | 80 | none; already host-tested |
| `mt_comp_store.c` | 118 | NVS only |
| `include/mt_matter.h` | 116 | none; pure C declarations |
| `main.cpp` + `mt_devtypes.cpp` | 804 | the Matter SDK itself |

Every `esp_matter` and `CHIP` string in `mt_at.c` and `mt_matter.h` is in a
comment. So the per-vendor surface is **804 lines of C++ behind a 116-line C
header**: about a dozen `mt_matter_*` bridge functions, a device type table,
storage, and UART plumbing. Above that line the entire AT surface moves
unchanged, and `iLabs_Hearth` on the host never learns that anything changed,
because it speaks a UART protocol and has no opinion about the silicon.

**The seam was not designed for this.** `mt_matter.h` exists because no
esp_matter header may enter a C translation unit (a build-hygiene rule, see
CLAUDE.md), and enforcing that produced exactly the boundary a port needs.

**The hard part, and it is not the line count.** `AT+MTEP` lets the *host*
declare an endpoint composition at runtime, which works because `esp_matter`
provides a dynamic data model. Both Nordic's nRF Connect SDK and Silicon Labs'
Matter SDK default to **ZAP-generated static endpoints**. connectedhomeip does
support dynamic endpoints underneath (`emberAfSetDynamicEndpoint`, the route
bridge devices take, and both vendors ship bridge samples), so it is possible,
but it means building the vendor equivalent of `esp_matter`'s data model layer
rather than getting it from the SDK. That single problem dominates the port on
both. The weaker alternative is a query-only `AT+MTEP?` with fixed compositions
per image, which is much less work and a materially worse product.

**One thing gets easier on nRF54L.** It is 802.15.4 and BLE only, no WiFi, so
the transport is Thread by construction: no build-time variant, `AT+MTNET?`
always answers `THREAD`, and the whole transport-mismatch problem (§3.12.1 of
the spec, and open question P2) cannot arise.

## 9. Decision log (summary)

| Decision | Choice | Why |
|---|---|---|
| Matter transport | WiFi (not Thread) v1 | No border router; reuse WiFi. |
| Combined ESP-NOW+Matter binary | No (two images) | 4 MB flash; single-purpose; mode-switch by reflash. |
| IDF version | v5.4.1 (Matter) / v5.5.4 (ESP-NOW) | esp-matter release/v1.5 needs 5.4.1. |
| OTA | Host serial flash (single-app) | Robust, flash-efficient, host-owned. |
| Matter OTA Requestor | Disabled | Redundant with host OTA; frees flash. |
| Radio owner (Matter) | esp_matter (link_mgr reserved) | Separate image; link_mgr for the merge. |
| Co-processor vendor | ESP32-C6 for now, not locked in | `AT+MT` is vendor-neutral; port surface is 804 lines behind a C header (§8.1). |
