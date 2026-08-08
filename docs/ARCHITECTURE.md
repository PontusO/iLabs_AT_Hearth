# Architecture & Design Decisions: iLabs AT Hearth (ESP32-C6)

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
  - `main.cpp` (C++): the esp_matter runtime: node + on/off-light endpoint +
    `esp_matter::start()`, the commissioning event callback, and the C-linkage
    `mt_matter_*` bridge into CHIP/esp_matter.
  - `mt_at.c` (C): the `AT+MT` command handlers on `at_core`.
  - `mt_matter.h` / `mt_at.h`: C-linkage bridges so C (handlers) and C++
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

**`generic_switch` (2026-08-04, commit `0aa85ea`) added a further
+3,184 B to all three images**, uniform because the Switch cluster's
server code is the same regardless of which network stack is compiled
in: `build_b4` 1,662,160 → 1,665,344 B (`0x196940`), `build_thread`
1,561,840 → 1,565,024 B (`0x17e160`), `build_combined` 1,958,016 →
1,961,200 B (`0x1decf0`). Same mechanism as the four-cluster delta
above: `CONFIG_SUPPORT_SWITCH_CLUSTER=n` had excluded the Switch
cluster's server implementation from the chip component archive
(`SwitchServer::Instance()`, `SwitchServer::OnInitialPress()`,
`MatterSwitchPluginServerInitCallback()`, one level different from the
delegate-based clusters above since Switch's server is a singleton
rather than a `SetDefaultDelegate` callback); `mt_devtypes.cpp`'s new
`generic_switch` thunk (§8.2) needed it, so the line was removed rather
than flipped, matching how the earlier four were handled.

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
  for C6). This is *separate* from the ESP-NOW firmware's IDF v5.5.4: esp-matter
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
a dozen `mt_matter_*` bridge functions, an 18-row device type table (§8.2),
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
colour light (`AT_MT_SPEC.md` §3.9 carries the current 18-row table with
IDs; the eighteenth row, `generic_switch`, landed afterward in commit
`0aa85ea`, 2026-08-04, and is covered below).

**Four device types are abort traps, not error paths.** `window_covering`,
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

**`generic_switch` is the fourth trap, and a different macro.** Its
`switch_cluster::config_t` defaults `feature_flags` to 0 the same way,
but `switch_cluster::create()` (`esp_matter_cluster.cpp:2192-2194`) checks
it with `VALIDATE_FEATURES_EXACT_ONE`, not `VALIDATE_FEATURES_AT_LEAST_ONE`:
Latching and Momentary Switch are mutually exclusive as well as each
individually optional, so zero bits set aborts exactly as before, but so
would two. The `mk_generic_switch()` thunk sets exactly one bit,
Momentary, matching the upstream arduino-esp32 class. The `generic_switch`
device type is also this firmware's first **event-emission** bridge:
`mt_matter_switch_click()` calls `switch_cluster::event::send_initial_press()`
under `ChipStackLock` to raise the Switch cluster's InitialPress event
(`AT+MTSWITCH`, `AT_MT_SPEC.md` §3.15) instead of writing an attribute
through the read/write bridge every other device type uses.

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

## 8.3 Temperature Controlled Cabinet: variants, the fifth trap, and the level delegate (2026-08-05)

Design record: `docs/superpowers/specs/2026-08-05-cabinet-templevels-design.md`.
Completes the 20-of-20 upstream device type set. Three firmware changes
landed across this round: the composition blob grew a variant byte (commit
`3c2882a`), the cabinet's two variants and `AT+MTEP`'s variant grammar landed
together (commit `d9e7ba6`), and the level-label delegate plus
`AT+MTTEMPLEVELS` closed it out. `AT_MT_SPEC.md` §3.9 and §3.16 are the
host-facing contract; this section is why it is shaped the way it is.

**Why a variant byte at all, rather than a second device type ID.** The
cabinet's two Matter personalities, TemperatureNumber and TemperatureLevel,
are not two device types in the Matter sense: both build the same
`temperature_controlled_cabinet` device type ID (`0x0071`) with the same
`TemperatureControl` cluster, differing only in which mutually-exclusive
feature bit that cluster carries. Giving them separate rows in
`mt_devtypes.cpp` would have meant inventing IDs Matter does not define. A
per-endpoint variant byte generalises past this one case: any future device
type that needs a build-time fork of its own cluster configuration gets one
for free, at the cost of one byte per endpoint in the composition.

**The composition blob format, v2.** `mt_composition.h`'s wire format grew a
version discriminator so v1 blobs already on a device's NVS keep decoding:

- v1 (legacy, decode-only): `u16 count`, then `count * u32` device type IDs,
  no version byte. Every legal v1 composition has `0..16` endpoints
  (`MT_COMP_MAX_ENDPOINTS`), so byte 0 (the low byte of `count`) is always
  `<= 16`. That is the fact that makes `0xFF` safe as an unambiguous v2
  sentinel: no legal v1 blob can ever start with it.
- v2 (current, encode and decode): sentinel byte `0xFF`, then version byte
  `0x02`, then `u16 count`, then per entry a `u32` device type ID followed by
  one `u8` variant. `mt_comp_encode()` always writes v2; `mt_comp_decode()`
  dispatches on byte 0 and accepts both, filling variant `0` for every entry
  of a v1 blob, since a device with no variant ever stored has none to
  recover. Blob max: `2 + 2 + 5 * MT_COMP_MAX_ENDPOINTS` = 84 bytes.
- No in-place migration. A stored v1 blob decodes fine (every variant reads
  as `0`, the only value that existed before this round) and is not rewritten
  until the next `AT+MTEPAPPLY`, at which point `mt_comp_encode()`'s
  always-v2 policy upgrades it as a side effect of the ordinary save path.

**`AT+MTEP=<devtype>[,<variant>]` grammar.** `<variant>` defaults to `0`,
which is legal for every device type that existed before this round (their
`max_variant` is `0`), so an existing host's `AT+MTEP=<devtype>` with no
second argument is unaffected. A variant that fails to parse or exceeds the
device type's `max_variant` answers `+MTERR:1`, the same code as any other
out-of-range parameter; an unknown device type ID still answers `+MTERR:6`
regardless of what follows it, checked first. `AT+MTEP?`'s fourth field
(`<variant>`) is printed only when nonzero, not as an always-present field
that happens to read `0`: an empty trailing field would still change the wire
bytes an existing host parses, and byte-identical output for every
zero-variant composition (which is every composition that predates this
round) was the requirement, not merely equivalent-when-parsed output.

**The fifth abort trap.** `window_covering`, `occupancy_sensing`,
`thermostat` and `generic_switch` were the first four (§8.2); the cabinet's
`TemperatureControl` cluster is the fifth, and shares `generic_switch`'s
macro rather than the other three's:

```cpp
        // check against O.a feature conformance for TN, TL
        VALIDATE_FEATURES_EXACT_ONE("TemperatureNumber,TemperatureLevel", feature::temperature_number::get_id(),
                                    feature::temperature_level::get_id());
```

(`esp_matter_cluster.cpp:2849-2850`, `namespace temperature_control { cluster_t
*create(...) }`.) TemperatureNumber and TemperatureLevel are mutually
exclusive and each individually optional in the Matter spec, so a
default-constructed `config_t` with `feature_flags = 0` aborts cluster
creation exactly as a default-constructed thermostat or window covering does,
and two bits set would abort it too. `mk_temperature_controlled_cabinet()`
(`mt_devtypes.cpp`) therefore branches on the composition variant with an
if/else, never two independent `if`s, so the thunk can never construct the
zero-bits or both-bits configuration CHIP rejects: variant `0` sets
`temperature_number::get_id()`, variant `1` sets
`temperature_level::get_id()`, and nothing else touches `feature_flags` for
this cluster.

**The level delegate.** `SupportedTemperatureLevels` is not an ordinary
attribute. `attribute::create_supported_temperature_levels()`
(`esp_matter_attribute.cpp`) marks it `ATTRIBUTE_FLAG_MANAGED_INTERNALLY`,
which only makes `esp_matter::attribute::get()` find it for a presence check;
the list content itself is served by CHIP's own
`TemperatureControlAttrAccess` (`temperature-control-server.cpp`), an
`AttributeAccessInterface` that queries a single, globally registered
`SupportedTemperatureLevelsIteratorDelegate`
(`supported-temperature-levels-manager.h`):

```cpp
    virtual uint8_t Size() = 0;
    virtual CHIP_ERROR Next(MutableCharSpan & item) = 0;
```

plus a non-virtual `Reset(EndpointId endpoint)` on the base class that sets
the protected `mEndpoint`/`mIndex` a derived class's `Size()`/`Next()` read.
esp-matter ships no default implementation of this interface; every app in
the vendored connectedhomeip tree that uses the TemperatureControl cluster's
Level feature (refrigerator, chef, all-clusters) supplies its own, and this
firmware's `HearthTempLevelsDelegate` (`main.cpp`) follows the same shape as
the chef example's `AppSupportedTemperatureLevelsDelegate`: one instance,
registered once via `SetInstance()` before `esp_matter::start()`, backed by a
small per-endpoint store (`s_temp_levels`, `MT_COMP_MAX_ENDPOINTS` slots of up
to `MT_TEMP_LEVEL_MAX_COUNT` labels each) that `Size()`/`Next()` linear-scan
for the entry matching `mEndpoint`.

`AT+MTTEMPLEVELS` is the only way to populate that store: the bridge,
`mt_matter_temp_levels_set()`, validates endpoint, cluster and attribute
presence the same three-level way `mt_matter_attr_read()`/`_write()` do
(`MT_ATTR_ERR_ENDPOINT`/`_CLUSTER`/`_ATTRIBUTE`), copies the labels in under
`ChipStackLock`, and then calls
`MatterReportingAttributeChangeCallback(ep, TemperatureControl::Id,
SupportedTemperatureLevels::Id)` (`app/reporting/reporting.h`) to mark the
attribute dirty. This is the general-purpose reporting hook every cluster
server in the SDK that mutates delegate-served state calls afterward (for
example `service-area-server.cpp`'s `MatterReportingAttributeChangeCallback`
calls after its own delegate mutations); there is no `esp_matter::attribute`
equivalent, because `esp_matter::attribute::update()` only knows how to push
its own internally-managed union value, and this attribute's real content
never lives there.

**Labels are not persisted, on purpose**, unlike the composition. The
composition is a product definition (survives `AT+MTRESET`, only
`AT+MTFRESET` erases it, §8.2/`AT_MT_SPEC.md` §3.10); the label list is
current display text, the same category as every other attribute's runtime
value, which a host is expected to (re-)send after `+MTREADY`. Persisting it
would have meant inventing a second NVS-survives-reset story for one
attribute where every other attribute already has a working one: the host
resends its state at startup.

## 8.4 Command forwarding: the verdict mailbox and the door lock (2026-08-06)

Design record: `docs/superpowers/specs/2026-08-06-command-forwarding-design.md`.
Takes on the challenge parked in the cabinet round (task-graph node I110):
some Matter commands need a synchronous app-level decision, and the app
lives on the other side of a UART. First consumer: the Door Lock device
type (`0x000A`), Hearth's first device type beyond arduino-esp32 parity
(upstream has no door lock class or example; confirmed against 3.3.8).
Landed in two parts: task C1 built the mailbox and `AT+MTCMDRESP` (commits
`06bf96a`..`ce6f890`); this section covers task C2, the door lock endpoint,
the ember callbacks that call into the mailbox, and `AT+MTLOCK`.
`AT_MT_SPEC.md` §3.17/§3.18 are the host-facing contract; this section is
why they are shaped the way they are.

**Why a synchronous host round-trip rather than a firmware policy.** The
alternative designs considered and rejected: always-allow (defeats the
point of a lock), always-deny (defeats the point of a controller), or a
firmware-local allow-list (re-invents access control the host already has,
duplicating a policy surface that must then be kept in sync over the AT
link). Forwarding the decision keeps exactly one copy of "who may lock this
door" and it lives where the rest of the product's access control already
lives: the host.

**One mailbox slot, not a queue.** `emberAfPluginDoorLockOnDoorLockCommand`
and `...OnDoorUnlockCommand` return `bool` inline
(door-lock-server.h:1112-1146) with no async completion path
(`HandleRemoteLockOperation` writes the command status in the same call
frame, door-lock-server.cpp:3711/:3726), and Matter serializes command
invokes on the CHIP event-loop task, so at most one adjudication is ever in
flight. `mt_cmdbox.c` (task C1) is therefore a single `{seq, ep, cluster,
command, verdict, state}` slot plus a FreeRTOS binary semaphore, not a
queue: a queue would be solving a concurrency problem this system does not
have, at the cost of bounded-memory reasoning a fixed slot gives for free.

**The lock-free responder.** `cmd_mtcmdresp()` (`mt_at.c`, runs on the AT
parser task) is the one `mt_at.c` handler in this firmware that does **not**
take `ChipStackLock` before touching Matter-adjacent state. Every other
bridge call does, by the pattern `mt_matter_attr_write()` and its siblings
established. Here that pattern would deadlock: the CHIP task is parked
inside `mt_cmd_forward()`'s `xSemaphoreTake()`, itself called from inside
the ember callback, which at that moment holds the CHIP stack's logical
ownership for the full duration of the wait. If `cmd_mtcmdresp()` blocked on
the same lock, it would be waiting on the very task it exists to unblock,
for the full 1000 ms, every single time, until `mt_cmd_forward()`'s own
timeout gave up and the "deadlock" resolved itself into "always times out".
`cmd_mtcmdresp()` therefore touches only the mailbox (`taskENTER_CRITICAL`
around `mt_cmdbox_answer()`) and gives the semaphore; nothing about the CHIP
stack. The two ember callbacks in `main.cpp` mirror this: no `ChipStackLock`
either, because they already run **on** the CHIP event-loop task (the same
reasoning `app_event_cb()` already established for platform events), so
taking it would self-deadlock a second way.

This freedom is `cmd_mtcmdresp()`'s own, not a guarantee about the whole
window: if `AT+MTCMDRESP` arrives while the AT parser task is still stuck
inside an *earlier* `mt_matter_*` handler blocked taking `ChipStackLock`
(held for the wait's duration by the very ember callback `mt_cmd_forward()`
is running inside of, per above), `cmd_mtcmdresp()` cannot be reached until
that handler unblocks, and the deadline expires regardless of how
lock-free it is (see `AT_MT_SPEC.md` §3.17's own note on the same
limitation).

**Default-deny.** Every outcome that is not an explicit `AT+MTCMDRESP=<seq>,1`
inside the 1000 ms window collapses to the same answer: timeout, the AT link
not being up (`s_at_up` false, e.g. a command that somehow fires before
`mt_at_start()`), or no host ever implementing `AT+MTCMDRESP` at all. All of
them return `false` from `mt_cmd_forward()`, the callback sets
`OperationErrorEnum::kUnspecified`, and the controller sees `Status::Failure`
plus the server's own `LockOperationError` event. A lock that fails open on
an ambiguous outcome is a worse defect than a spurious deny, so the mailbox
was built to make "deny" the only reachable answer for every code path that
is not a confirmed allow, rather than trying to enumerate every honest
failure and picking a bespoke response for each.

**The 1000 ms deadline and its queuing consequence.** Chosen against
`kExpectedIMProcessingTime` (2 s, `InteractionModelTimeout.h:27`) plus MRP
margins on the controller side, so a full window is still inside the
controller's own patience. The cost: while the CHIP task waits (up to 1 s),
every other `mt_matter_*` bridge call queues behind `ChipStackLock`, since
the CHIP task is what those calls' locks ultimately serialize against
holding. `AT+MTATTR` and friends are still correct when this happens, only
slower; the AT link itself stays responsive throughout, since the parser
task keeps running and `AT+MTCMDRESP` (§3.17) is exactly the one command
that was built not to queue behind that same lock. Accepted rather than
engineered away: the alternative was a second lock-free path for every
bridge call, which would have meant re-deriving the "why is this safe"
argument for each one instead of once.

**The door lock endpoint has no fifth-trap analogue.** `door_lock::config_t`
has no `feature_flags` field at all (`esp_matter_endpoint.h:543-555`,
`esp_matter_cluster.h:634-651`), and `cluster::door_lock::create()` runs no
`VALIDATE_FEATURES` macro (`esp_matter_cluster.cpp:2054-2098`): unlike
`window_covering`/`occupancy_sensing`/`thermostat`/`generic_switch`/
`temperature_control` (§8.2/§8.3), there is no default-construction abort
trap to route around, so `mk_door_lock()` is a plain thunk. The feature map
esp-matter hard-codes is `0` (`global::attribute::create_feature_map(cluster,
0)`, `esp_matter_cluster.cpp:2070`), which happens to be exactly what this
round wants (spec F3): with no PIN/USER/COTA features, a supplied PIN is
rejected by the server itself before the app callback ever runs
(door-lock-server.cpp:3653-3708), so bare `LockDoor`/`UnlockDoor` are the
only commands this firmware's callbacks are ever asked to adjudicate.

**The app owns the state change, deliberately.** The server does not set
`LockState` on a successful command; its own comment says so
(door-lock-server.cpp:3712, "The app should trigger the lock state change as
it may take a while"). This firmware's ember callbacks therefore never call
`SetLockState`: `AT+MTLOCK` (§3.18) is the host's own, separate report of
what the physical lock actually did, on its own schedule, through the 6-arg
`DoorLockServer::SetLockState(ep, state, opSource, ...)` overload
(door-lock-server.h:144-148) so the `LockOperation` event a subscribed
controller expects is actually emitted; the 2-arg overload
(door-lock-server.h:160) writes the attribute with no event and is not used.
Two separate commands for two separate authorities: `AT+MTCMDRESP` answers
"may this happen", `AT+MTLOCK` reports "this happened", and the firmware
never promotes one into the other on the host's behalf.

**`emberAfDoorLockClusterInitCallback` has no weak default anywhere in the
vendored connectedhomeip tree**, unlike the three `OnDoorLock`/`OnDoorUnlock`/
`OnDoorUnbolt` command callbacks, which `door-lock-server-callback.cpp:46-71`
does supply weak deny-everything defaults for. Verified by grepping `src/`
and `zzz_generated/` for the symbol: only the zap-generated declaration
exists. Confirmed at the linker: building with the callback commented out
(a deliberate, reverted experiment for this round) fails with exactly one
undefined reference,
`emberAfDoorLockClusterInitCallback(unsigned short)`, from
`esp_matter_cluster.cpp:2045`'s `function_list` array, which is where
`door_lock::create()` (task C2's `mk_door_lock()`, via `door_lock::create()`
in `esp_matter_endpoint.cpp`) registers it as a `CLUSTER_FLAG_INIT_FUNCTION`
callback, called once per DoorLock server instance regardless of ember's own
generic per-endpoint init dispatch. The implementation
(`DoorLockServer::Instance().InitServer(endpoint)`) is verbatim from
esp-matter's own example, `examples/door_lock/main/lock/
door_lock_callbacks.cpp:20-24`, minus that example's
`BoltLockMgr().InitLockState()` call, which belongs to its own unused
lock-state simulation rather than to `InitServer()` itself.

**`CONFIG_SUPPORT_DOOR_LOCK_CLUSTER` needed the same flip as five clusters
before it.** `sdkconfig.defaults` excluded it (`=n`) the same way
`FAN_CONTROL`/`OCCUPANCY_SENSING`/`THERMOSTAT`/`WINDOW_COVERING` (§8.2) and
`SWITCH`/`TEMPERATURE_CONTROL` (§8.3) were: with the line left in place,
`door_lock::create()` links fine (esp_matter's own config struct exists
either way) but the final image link fails with undefined references to
`DoorLockServer::Instance()`, `DoorLockServer::SetLockState()`,
`MatterDoorLockPluginServerInitCallback()`, and the three
`MatterDoorLockClusterServer*Callback` symbols the cluster's own
`function_list`/delegate-init path calls, plus
`emberAfDoorLockClusterLockDoorCallback`/`...UnlockDoorCallback`, the
generated command dispatch entries `esp_matter_command.cpp` calls into. The
line was removed rather than flipped to `=y`, matching how the five
predecessors were handled: this is now the sixth cluster that needed its
`CONFIG_SUPPORT_*_CLUSTER` exclusion lifted purely because a device type
using it exists in `mt_devtypes.cpp`.

**Measured 2026-08-06, firmware 0.3.3, all three images:**

| image | size | free in the 3.75 MB slot |
|---|---|---|
| `build_b4` (WiFi) | 1,691,712 B (`0x19b040`) | 57% |
| `build_thread` (Thread) | 1,583,840 B (`0x1828e0`) | 60% |
| `build_combined` (WiFi + Thread) | 1,975,344 B (`0x1e3430`) | 50% |

## 8.5 Ten-type swoop: survey verdict classes and the 20-to-30 lift (2026-08-06)

Design record: `docs/superpowers/specs/2026-08-06-ten-type-swoop-design.md`,
survey graph node D132. A full census of esp-matter 21aa3d1's endpoint
namespaces crossed against the Matter 1.5.1 Device Library Specification
found every remaining device type falls into one of four verdict classes,
recorded in full in the design spec's section 6: **mechanical** (rides the
existing thunk table and `AT+MTATTR` unchanged, no new firmware or host
mechanism needed: the ten types landed in this round), **tier 2** (needs one
new mechanism the firmware already has a shape for, e.g. `+MTCMD` forwarding
for Water Valve or a Power Source device mandate for Smoke/CO Alarm, but not
yet spent on this type), **tier 3** (several types sharing one not-yet-built
mechanism, e.g. the laundry/dishwasher trio behind a single OperationalState
delegate), and **composition-tree** (Refrigerator/Oven/Irrigation/Cook
Surface, blocked on parent-child `AT+MTEP` endpoint composition, a capability
this firmware does not have). Only the mechanical class was in scope here;
the other three are a ledger of future rounds, not a todo inside this one.
Two more `CONFIG_SUPPORT_*_CLUSTER` exclusions joined the door-lock-era
precedent (§8.4) in the same lift-on-demand shape: `AIR_QUALITY_CLUSTER`
(known in advance, since Air Quality Sensor's only cluster is excluded by
default) and `PUMP_CONFIGURATION_AND_CONTROL_CLUSTER` (found the same way
the prior six were: the image links, then fails at the final ELF link with
undefined references to `MatterPumpConfigurationAndControlPluginServerInitCallback`
and its sibling ember callbacks, because the esp_matter config struct exists
regardless of the Kconfig flag but the cluster's server implementation does
not compile into the chip component archive without it).

**Correction (task C1b, bug B139): Air Quality Sensor was misclassified as
mechanical.** The census checked whether esp-matter's endpoint namespace
existed and its attribute walk found `AirQuality`, but missed that
esp-matter creates the attribute `ATTRIBUTE_FLAG_MANAGED_INTERNALLY` with no
server behind it: CHIP's air-quality-server is Instance/
`AttributeAccessInterface`-based, the same shape as the C3 temperature-levels
delegate (§8.3), and needed one before `AT+MTATTR=<ep>,91,0` answered
anything but bare `ERROR`.

## 8.6 Seven-type batch: delegate pools, the valve return-ignored fact, and the Chime SDK gap (2026-08-07)

Design record: `docs/superpowers/specs/2026-08-07-seven-type-batch-design.md`,
survey graph node D149. Adds Water Valve (`0x0042`, task C2), Mode Select
(`0x0027`, task C3), the OperationalState trio - Laundry Washer `0x0073`,
Dishwasher `0x0075`, Laundry Dryer `0x007C` (task C4), Smoke/CO Alarm
(`0x0076`, task C5), Power Source (`0x0011`, riding along as a flat sibling
row, task C5), and Chime (`0x0146`, task C6): firmware rows 31-38, firmware
0.5.0. `AT_MT_SPEC.md` §§3.19-3.24 are the host-facing contract; this section
is why they are shaped the way they are.

**The delegate-pool pattern, generalized across four device types.** Four of
this round's six clusters (valve, the OperationalState trio, chime) need a
CHIP delegate object that exists before `esp_matter::endpoint::create()` is
called (the config struct consumes the pointer synchronously) but whose real
endpoint id is not known until `create()` returns it (the id-reproducibility
rule in `CLAUDE.md`: an endpoint's id is assigned inside `create()` itself).
Every one of the four solves this the same way: a static
`MT_COMP_MAX_ENDPOINTS`-sized array of delegate objects in `main.cpp`, handed
out one at a time by an `_alloc()` function the thunk (`mt_devtypes.cpp`)
calls before `create()`, with the real endpoint fixed up by a
`_set_endpoint()` call once `create()` returns. Both accessors trade in an
opaque `void*` (`mt_matter.h`), so `mt_devtypes.cpp` - which must stay free of
CHIP/esp_matter delegate types, per its own file comment - never has to name
`HearthValveDelegate`, `HearthOpStateDelegate`, or `HearthChimeDelegate`.
Mode Select is the one exception: F2 established that a *single* global
`SupportedModesManager` dispatches on endpoint id internally, so it needs no
per-endpoint pool at all, just one static instance every `mode_select`
endpoint's thunk points at. On pool exhaustion, every one of the four aborts
the whole composition rebuild the same way any other failed `create()` does
(`CLAUDE.md`, "A failed `endpoint::create()` consumes no endpoint ID"),
rather than skip the one entry: skipping would hand a commissioned device a
silently wrong data model, the same reasoning that governs every other
composition-rebuild failure in this firmware.

**The water valve's verdict cannot fail the command on the wire (F1).**
`ValveConfigurationAndControl`'s server calls the delegate's
`HandleOpenValve`/`HandleCloseValve` synchronously and discards what they
return (`TEMPORARY_RETURN_IGNORED` at both call sites,
`valve-configuration-and-control-cluster.cpp:303,361`): the controller always
sees `Status::Success`, whether the `+MTCMD` verdict this firmware forwarded
was an allow or a deny. This is the opposite of the door lock (§8.4, a deny
genuinely fails the command) and the opposite of the OperationalState trio
below (F7, a deny also genuinely fails the command, through a different
mechanism). Documented rather than fought: there is no return path on this
side of the delegate that could change it, so `AT+MTVALVE` (§3.19) is the
*only* place the real outcome reaches the fabric, entirely decoupled from
whatever verdict the host gave.

**The OperationalState trio's verdict, by contrast, IS the wire response
(F7).** `Instance::SetOperationalState()`'s caller copies the delegate's
filled `GenericOperationalError` straight into the command's
`OperationalCommandResponse`; a deny answers `ErrorStateID` `0x02`
(`UnableToCompleteOperation`), an allow `0x00` (`NoError`). One delegate
OBJECT per endpoint is mandatory here (`Delegate::SetInstance()`
`VerifyOrDie`s if a second `Instance` tries to share one), which is why this
is the one pool of the four that is *shared* across three different device
types (`dish_washer`/`laundry_washer`/`laundry_dryer` all wire the identical
base `OperationalState` cluster `0x0060`) rather than one pool per type.

**Chime's verdict is the third shape again: passed straight through, raw.**
`ChimeCluster::HandlePlayChimeSound()` takes whatever `Status` the delegate's
`PlayChimeSound()` returns and copies it directly into the command's
`InvokeResponse`, with no SDK-side remapping through an intermediate enum the
way the OperationalState trio's `GenericOperationalError` is. Three device
types in one round, three different relationships between a `+MTCMD` verdict
and what the controller actually sees on the wire: discarded (valve),
remapped through an SDK enum (OperationalState), passed through raw (chime).
None of the three is a bug; each is what its own cluster's SDK code does, and
`AT_MT_SPEC.md` documents each explicitly rather than implying they are
interchangeable.

That raw passthrough only applies once the delegate is actually called,
which `ChimeCluster::HandlePlayChimeSound()` (`ChimeCluster.cpp:273-282`)
does not always do: it answers the controller directly, with no delegate
call and therefore no `+MTCMD` at all, when `Enabled` is `false`
(`Status::Success`) or the command names a `chimeID` that is not one
`AT+MTCHIMESOUNDS` installed (`Status::NotFound`). A host that disables a
chime (`AT+MTCHIME=<ep>,1,0`) will see its `PlayChimeSound` `+MTCMD` URCs
simply stop arriving for that endpoint, not arrive and get denied; this is
the SDK's own short-circuit ahead of the delegate, not a firmware choice,
and `AT_MT_SPEC.md` §3.24 documents it at the command that controls it.

**The Chime SDK gap (F6), and why the workaround has to call one more
function than the design record originally named.** `cluster::chime::create()`
wires the delegate stash (`ChimeDelegateInitCB` ->
`chip::app::Clusters::Chime::SetDelegate()`) and a no-op legacy
plugin-server stub, but never calls
`ESPMatterChimeClusterServerInitCallback()` - the one function that actually
constructs a `ChimeCluster` and registers it with esp-matter's data model
provider. Grep evidence against the whole pinned esp-matter/connectedhomeip
tree: the only two occurrences of that symbol's name are its own definition
(`data_model_provider/clusters/chime_integration.cpp:36`) and its declaration
(`zap_common/app/ClusterCallbacks.h:67`). Left uncalled, `cluster::
chime::create()` still creates the cluster's ember-managed attributes, but
nothing in the data model actually serves a read, a write, or
`PlayChimeSound`. Recorded for the I90 upstream ride-along (design spec
section 8: "Chime upstream fix: the missing init call site rides the I90
list").

The firmware calls `ESPMatterChimeClusterServerInitCallback()` itself, in the
same pre-start registration window `mt_air_quality_register_all()` (§8.5's
correction) and the `AT+MTTEMPLEVELS` delegate already use - safe there for
the same reason those two are: nothing in that window touches the CHIP event
loop or arms a timer. What the design record's F6 entry did not anticipate,
found while verifying the callback's exact behaviour against the header
rather than assuming a one-line fix: `ESPMatterChimeClusterServerInitCallback()`
unconditionally dereferences `*gDelegates[endpointId]`
(`chime_integration.cpp:41`), and the only thing that populates that map is
`ChimeDelegateInitCB` - which does not run in this pre-start window at all.
It fires from `esp_matter::data_model::provider::Startup()`
(`data_model_provider/esp_matter_data_model_provider.cpp`), itself called
from `chip::Server::Init()`, itself called from `esp_matter::start()` -
strictly *after* the pre-start window closes. A version of the workaround
that called only `ESPMatterChimeClusterServerInitCallback()`, trusting
`ChimeDelegateInitCB` to have already run, would dereference a
default-constructed (null) map entry and crash on the very first boot with a
chime endpoint. The firmware's `mt_chime_register_all()` (`main.cpp`)
therefore calls `chip::app::Clusters::Chime::SetDelegate()` itself first,
ahead of `ESPMatterChimeClusterServerInitCallback()`; `ChimeDelegateInitCB`
still fires later, during `start()`, but only re-sets the same pointer to the
same delegate, which is harmless. Registering the cluster before
`esp_matter::start()` is separately safe on its own terms:
`ServerClusterInterfaceRegistry::Register()` only calls `Startup()` on a
newly-registered interface if a context is already set, and with none yet
(true here) it just links the registration into its list; `SetContext()`,
called later from inside `esp_matter::start()`, walks every already-
registered interface and starts each one then, which is what actually wires
the cluster to the interaction model.

**`ReportInstalledChimeSoundsChange()` does not exist on this SDK revision;
the header wins over its own doc comment.** `ChimeDelegate`'s doc comment on
`GetChimeSoundByIndex()`/`GetChimeIDByIndex()` (`ChimeCluster.h`) instructs
callers to "call the Instance's `ReportInstalledChimeSoundsChange` method" 
when the sound list changes - but no such method is defined anywhere on
`ChimeCluster` or its base `DefaultServerCluster` (grep evidence against the
whole `chime-server` directory: the string appears only in that doc comment,
twice, never in a declaration). This cluster was rewritten onto the
`DefaultServerCluster`/`ServerClusterInterfaceRegistry` architecture at some
point in this SDK's history, and the doc comment is a leftover reference to
whatever pre-rewrite, `Instance`-based API it used to describe.
`NotifyAttributeChanged()`, the mechanism `SetSelectedChime()`/`SetEnabled()`
use internally for the identical purpose, is `protected` on
`DefaultServerCluster`, and this firmware's bridge code is not a subclass, so
it is not reachable either. `mt_matter_chime_sounds_set()` (`main.cpp`) uses
the same substitute `mt_matter_temp_levels_set()`/`mt_matter_modes_set()`
already established for an identical problem two device types earlier:
`MatterReportingAttributeChangeCallback()`, which marks a path dirty through
`DataModel::Provider::Temporary_ReportAttributeChanged()` directly and is not
ember-specific, so it works the same for a provider-registered attribute
(Chime) as it does for an ember-managed one (TemperatureControl, ModeSelect).

**Reading `SelectedChime`/`Enabled` back needs the registry, not
`esp_matter::attribute::get()`.** Because Chime is registered directly with
the data model provider rather than through esp-matter's generic attribute
store, none of this file's other bridges' lookup pattern
(`esp_matter::cluster::get()` +  `esp_matter::attribute::get()`) reaches the
live `ChimeCluster` instance. `mt_matter_chime_set()` instead calls back into
`esp_matter::data_model::provider::get_instance().registry().Get()` - the
same public registry `mt_chime_register_all()` registered the instance into -
and `static_cast`s the returned `ServerClusterInterface*` to `ChimeCluster*`,
safe because this file is the only thing that ever registers one at that
path and the inheritance chain (`ChimeCluster` -> `DefaultServerCluster` ->
`ServerClusterInterface`) is single and non-virtual.

**Kconfig: six lifts, one linker-evidence-only.** `CHIME`, `MODE_SELECT`,
`OPERATIONAL_STATE`, `POWER_SOURCE`, `SMOKE_CO_ALARM`, and
`VALVE_CONFIGURATION_AND_CONTROL_CLUSTER`* joined the door-lock-era
`CONFIG_SUPPORT_*_CLUSTER=n` removals (§8.4/§8.5): the esp_matter config
struct for each exists regardless of the flag, but its actual cluster server
implementation - and, for Chime specifically, the object `chime_integration.cpp`
needs to link against at all - only compiles into the chip component archive
when the flag is lifted. Each was found and confirmed the same way: build,
read the exact undefined-reference symbol at the final link, lift the one
flag that supplies it, rebuild. (*`VALVE_CONFIGURATION_AND_CONTROL_CLUSTER`
was never in the disabled list to begin with, so task C2 needed no Kconfig
line removed for the cluster itself - only the knock-on `TIME_SYNCHRONIZATION_CLUSTER`
lift `sdkconfig.defaults`'s own comment documents, a link-time dependency of
`SetValveLevel()`'s `#ifdef ZCL_USING_TIME_SYNCHRONIZATION_CLUSTER_SERVER`
block, a macro esp_matter's bundled `gen_config.h` defines unconditionally
regardless of this repository's own Kconfig.)

**Task C11: the same "SDK ships helpers with no call site" failure family
hit the OperationalState trio and Smoke/CO Alarm too, the way it hit Chime
above (F6).** Bench verification (task C10, finding F-C10-1) found that
`operational_state::create()` and `smoke_co_alarm::create()`
(`esp_matter_cluster.cpp:1752-1756` and `:1991-2042`) create attributes and
events but call no `command::create_*` at all, so every `AcceptedCommandList`
on these clusters was empty and a controller's `Pause`/`Stop`/`Start`/
`Resume`/`SelfTestRequest` invoke was rejected `Status=0x81`
(`UNSUPPORTED_COMMAND`) before the delegate ever ran, exactly the shape of
gap F6 documents for Chime: the four `operational_state::command::create_*`
helpers and `smoke_co_alarm::command::create_self_test_request` are declared
(`esp_matter_command.h:295-299` and `:305`) and implemented, right next to
`valve_configuration_and_control::create()`'s own `command::create_open`/
`command::create_close` calls (`esp_matter_cluster.cpp:3451-3453`) that prove
the mechanism works when a cluster's `create()` actually calls it. Unlike
Chime, this gap needed no data-model-provider registration workaround: these
two clusters go through the ordinary ember-managed path, so the fix is the
five missing `command::create_*` calls, hand-added in `mt_devtypes.cpp` after
`create()` for each affected thunk, the same after-create() shape
`mk_power_source()` already used for `BatPercentRemaining`. `AT_MT_SPEC.md`
§§3.21-3.22 carry the DEFECT F-C10-1 write-up. Three SDK gaps of this exact
shape across one batch (F6, this one, and by extension anything the I90
upstream ride-along turns up) is worth treating as a standing risk for any
future device type this firmware adds: a cluster's `create()` populating
attributes and events is not evidence its commands were wired too, and the
only way to know is to read the `create()` body, not the header.

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
| App-adjudicated commands (door lock) | Host decides, firmware forwards and defaults to deny | One copy of access-control policy, on the host; single mailbox slot, 1000 ms deadline, lock-free responder (§8.4). |
