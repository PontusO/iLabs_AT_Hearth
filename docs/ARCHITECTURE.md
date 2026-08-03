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
applied.

**The patchset widened to three files across two pinned repos, B83.** A
WiFi-active combined boot spun `DnssdServer::StartServer()` forever: the
same weak hook that correctly skips launching the OpenThread stack also
leaves `mOTInst` null, and `ChipDnssdInit()` calls into
`OpenThreadDnssdInit()`/`_ClearSrpHost()` unconditionally whenever both
`CHIP_DEVICE_CONFIG_ENABLE_THREAD` and `CHIP_DEVICE_CONFIG_ENABLE_THREAD_SRP_CLIENT`
are compiled in, so the null instance fails `Init()`, which resets
`DiscoveryImplPlatform`'s state and re-arms itself via the very
`kDnssdInitialized` repost meant to retry a transient failure: a tight,
un-backed-off ping-pong between a "succeeded" `mdns_init()` and a "failed"
OpenThread SRP call that starves `IDLE` and delays `+MTREADY` by seconds.
The fix mirrors the same weak-hook pattern one file deeper, in CHIP core
itself: `sdk-patches/connectedhomeip/0001-hearth-dnssd-transport-hook.patch`
against the nested CHIP checkout's `src/platform/ESP32/DnssdImpl.cpp`
(base commit `b87051a9`), skipping the OpenThread-touching branches of
`ChipDnssdInit`/`PublishService`/`RemoveServices`/`FinalizeServiceUpdate`
when the hook says WiFi is active, gated the same way: only inside the
already-both-stacks-compiled condition, so single-stack builds preprocess
unchanged. `scripts/apply-sdk-patches.sh` applies, checks, and reverts both
patchsets together against the checkouts named by `ESP_MATTER_PATH` (default
`~/esp/esp-matter`) and `CHIP_PATH` (default
`~/esp/esp-matter/connectedhomeip/connectedhomeip`), and refuses outright,
for both patchsets, if either checkout's `HEAD` has moved off its pin, so an
SDK bump forces a deliberate re-evaluation of the patches rather than a
silent, possibly-broken re-apply.

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
inside the patch hunks. At this point in the work the weak symbol still had
no app-side definition, so it resolved to null and the patch treated that as
"not Thread"; the app-side wiring described below closed that gap.

Measured (`idf.py -B <dir> size`, `.bin` size on disk):

| variant | `.bin` size | DIRAM used | `.bss` |
|---|---|---|---|
| `build_b4` (WiFi only) | 1,662,160 B | 220,981 B | 79,520 B |
| `build_thread` (Thread only) | 1,561,840 B | 184,271 B | 90,944 B |
| `build_combined` (both) | 1,958,016 B | 255,765 B | 108,296 B |

DIRAM and `.bss` columns measured 2026-08-01 (before device-type expansion).
The four newly linked cluster servers plus the B98 fix add approximately 1 KB
of `.bss` not yet remeasured; flash figures are current (2026-08-03).

Combined vs `build_b4`: +295,856 B flash, +34,784 B DIRAM, +28,776 B
`.bss`, the dormant-Thread tax on top of the WiFi image. Combined vs
`build_thread`: +396,176 B flash, +71,494 B DIRAM, +17,352 B `.bss`, the
dormant-WiFi tax on top of the Thread image, larger because the WiFi stack
itself is bigger than OpenThread's. Both deltas land in the tens-of-KB range
the design predicted, not hundreds.

The factory slot is 3,840 KB (`0x3C0000`, `partitions.csv`); the combined
image leaves 1,974,144 bytes (50%) free, comfortably inside the ~2.1 MB
margin the single-app partition switch (section 4) was sized for.

**All three flash figures above absorbed the device-type expansion's
~44 KB (2026-08-03).** `.bin` size moved by +44,048 B (`build_b4`),
+44,048 B (`build_thread`) and +45,312 B (`build_combined`) once the 13
new device types (§8.2) landed, because the change also removed four
`CONFIG_SUPPORT_*_CLUSTER=n` lines from `sdkconfig.defaults`
(`FAN_CONTROL`, `OCCUPANCY_SENSING`, `THERMOSTAT`, `WINDOW_COVERING`):
those clusters' server code was previously excluded at link time, and a
host must be able to compose a fan, an occupancy sensor, a thermostat or
a window covering on any image, so exclusion could no longer be
one-size-fits-all: with any of the four left off, the corresponding
thunk compiles fine but the final link fails with undefined references
to that cluster's `PluginServerInitCallback`/`SetDefaultDelegate`
symbols, a failure mode a source-only review would not have caught. The
old figures (1,618,112 / 1,517,792 / 1,912,704 B) are what the same
three images measured before that change.

**`app_main` wired to the latch, 2026-08-01.** `main/mt_transport.c` owns the
persisted choice (NVS `mt_cfg`/`transport`, default WIFI) and the strong
definition of the weak hook the patchset calls. `app_main`, combined build
only (`MT_COMBINED_IMAGE` in `mt_at_config.h`, derived from
`CONFIG_ENABLE_WIFI_STATION && CONFIG_OPENTHREAD_ENABLED`), does five things
in order, all before `esp_matter::start()`:

1. `mt_transport_latch_active()`, reading the persisted choice into a static
   once, before anything else can query it. Registration (the patch),
   stack launch (the patch), the feature map, and the diagnostics scrub
   below all read this one latch rather than the NVS value directly, so a
   concurrent `AT+MTTRANSPORT=` cannot change the answer mid-boot.
2. The root node's `NetworkCommissioning` feature map is set to
   `kWiFiNetworkInterface` or `kThreadNetworkInterface` on `node::config_t`
   before `node::create()`. Traced at implementation time (not assumed from
   the design's "app-settable" note): `root_node::create()` forwards
   `config->network_commissioning` straight into
   `cluster::network_commissioning::create()`
   (`esp_matter_endpoint.cpp`/`esp_matter_cluster.cpp`), which is where the
   feature map decides which optional attributes the cluster even creates.
   The struct's own default picks WiFi over Thread by `#if`/`#elif` on the
   compile-time `CHIP_DEVICE_CONFIG_ENABLE_*` macros
   (`esp_matter_cluster.h`), which is the wrong answer on a combined image
   where both macros are defined; this override replaces it.
3. Once `node::create()` returns, `esp_matter::cluster::get()` looks up the
   dormant transport's diagnostics cluster on endpoint 0
   (`WiFiNetworkDiagnostics::Id` or `ThreadNetworkDiagnostics::Id`) and
   `esp_matter::cluster::destroy()`s it if present, closing the leak section
   3's item 3 identified: both clusters attach at compile time with no
   mutual exclusion. This runs after `node::create()` and before the
   endpoint-composition rebuild, and needs no `ChipStackLock`: like the
   composition rebuild, it runs before `esp_matter::start()`, so there is no
   CHIP event loop yet to race.
4. The existing Thread-image OpenThread platform-config handoff (needed
   before `esp_matter::start()` calls `openthread_init_stack()`, which
   asserts if it is missing) gains one more condition on the combined
   image: it now runs only when the latch says Thread, so a WiFi-active
   combined boot never hands OpenThread a config for a stack the patched
   `esp_matter_core.cpp` was never going to launch anyway. The single-stack
   Thread build keeps the unconditional call, byte-identical to before this
   change.
5. The free-heap figure already logged once `esp_matter::start()` and
   `mt_at_start()` have run (next to the D1 BLE figures) gets one more line
   on the combined image naming the active transport, so the dormant-stack
   cost measured above is legible per boot rather than only at build time.

`chip::app::Clusters::WiFiNetworkDiagnostics::Id` and
`::ThreadNetworkDiagnostics::Id` needed no new include: both are reachable
transitively through `esp_matter.h` (`esp_matter_client.h` pulls in
`app-common/zap-generated/cluster-objects.h`, which pulls in every cluster's
generated `Attributes.h`, which pulls in that cluster's `ClusterId.h`).

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
| `main.cpp` + `mt_devtypes.cpp` | 1,254 | the Matter SDK itself |

Every `esp_matter` and `CHIP` string in `mt_at.c` and `mt_matter.h` is in a
comment. So the per-vendor surface is **1,254 lines of C++** (976 in
`main.cpp`, 278 in `mt_devtypes.cpp`) **behind a 116-line C header**: about
a dozen `mt_matter_*` bridge functions, a 17-row device type table (§8.2),
storage, and UART plumbing. Above that line the entire AT
surface moves unchanged, and `iLabs_Hearth` on the host never learns that
anything changed, because it speaks a UART protocol and has no opinion about
the silicon. The 2026-07-29 measurement above read 804 lines against a
4-row table; the 2026-08-03 device-type expansion (§8.2) is most of the
growth.

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

## 8.2 Device types: the 13-type expansion (2026-08-03)

Design record:
`docs/superpowers/specs/2026-08-03-devtype-expansion-design.md`. Firmware
change: commit `7cf4971`. `mt_devtypes.cpp` grew from 4 rows to 17:
on/off, dimmable and colour-temperature lights and the temperature sensor
were the C1 set; `AT+MTEP` now also accepts on/off and dimmable plug-in
units, contact/occupancy/humidity/pressure/rain sensors, water-freeze and
water-leak detectors, fan, window covering, thermostat and extended
colour light (`AT_MT_SPEC.md` §3.9 carries the full 17-row table with IDs).

**Three of the thirteen are abort traps, not error paths.** `window_covering`,
`occupancy_sensing`, and `thermostat` all default `feature_flags` to 0, and
esp-matter treats that as a hard failure rather than a recoverable one: all
three hit `VALIDATE_FEATURES_AT_LEAST_ONE` in cluster create
(`esp_matter_cluster.cpp:2137`, `:2341-2346`, and `:1445` respectively),
which aborts the whole device, not just the one endpoint. That is a harsher failure than the existing
composition rule that a failed `endpoint::create()` aborts the boot
rebuild (see "Things that will bite you" in CLAUDE.md): a bad device type
ID fails cleanly and is caught before any cluster is touched, a bad
`feature_flags` value panics the device outright. This is why the two
thunks own `feature_flags` rather than taking it from the host:
`window_covering` sets Lift, Tilt and both position-aware bits;
`thermostat` sets Heating and Cooling, and also seeds the Heating and
Cooling feature configs' setpoints to 1600/2400 hundredths (16 C / 24 C)
instead of esp-matter's own 2000/2600 defaults, matching the boot values
upstream arduino-esp32 devices use and that the host library's cache seeds
assume (cross-layer finding I1, host library final review): left at
esp-matter's defaults, a sketch's first setpoint write on a fresh device
matches the library's cache guess and is silently swallowed by its
equality check while the fabric still holds the un-upstreamed value. A
host declaring `AT+MTEP=0x0301` can never construct a thermostat that
bricks the boot, because the only `feature_flags` value the firmware will
ever build for it is the one that survives cluster create.

**The StartUpCurrentLevel retrofit is B63's sibling.** `dimmable_light` and
`color_temperature_light` already nulled `start_up_on_off` for B63
(esp-matter's config default of 0 forces the light off at every boot and
persists that forced value, so a stored state never survives a reboot),
but had never nulled `level_control_lighting.start_up_current_level`,
which defaults to 0 the same way and was doing the same thing to stored
brightness. Fixed in the same commit with a `mt_startup_level_null()`
helper, templated rather than typed to one `config_t` because
`dimmable_plug_in_unit::config_t` declares the same field independently
instead of inheriting it from `dimmable_light`.

**`extended_color_light` diverges from stock esp-matter on purpose.** The
standard namespace enables only ColorTemperature and XY; the host library
mirrors arduino-esp32's HSV-driven API, so the thunk bolts
`color_control::feature::hue_saturation` onto the cluster after the
standard `create()` returns (safe because `color_control::create()` has
no exclusivity validation between the three colour features). A host now
sees `CurrentHue` and `CurrentSaturation` alongside XY and mireds on the
same endpoint, all three colour representations live at once, with
`AT+MTATTR` reaching whichever one a controller actually wrote.

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
