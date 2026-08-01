# C6: the combined WiFi+Thread image design

Date: 2026-08-01. Implements decision DE35 (commit c9d4ac7): one C6 image
carrying both network stacks, exactly one active at a time, the choice
persisted and applied at boot. What it buys is one artifact instead of two
(the host flips a setting and reboots instead of reflashing); it does not
buy a capability, since a transport switch still means recommissioning.
C1's endpoint contract survives because the two stacks are never live
together, so the secondary network-commissioning endpoint never exists.

## 1. Decisions taken with the user (2026-08-01)

- **Selection surface**: a new persisted command, `AT+MTTRANSPORT=WIFI|THREAD`,
  stored in NVS and applied at the next boot; `AT+MTTRANSPORT?` reports
  both the active and the stored choice so a host can see a pending
  switch. It survives `AT+MTRESET` AND `AT+MTFRESET`, like the
  composition: a product setting, not user data.
- **Switching with a live fabric is always allowed.** The P2
  transport-mismatch flow already covers the aftermath: fabric present
  but the chosen transport unprovisioned means a boot window, `+MTEVT:27`,
  and the mismatch flag on `AT+MTNET?`. Nothing is erased; the fabric
  revives if the host switches back. Refusing until factory reset was
  rejected for the same reason P2 rejected erasing.
- **All three images keep building** (`build_b4`, `build_thread`, and the
  new `build_combined`) until the combined image has passed the full
  regression suite in both modes on hardware. Retirement is a later,
  evidence-based decision.
- **Fresh-device default: WIFI**, matching shipping behavior and the C4
  host library's expectations.

## 2. What the SDK trace established (2026-08-01, both hooks now traced)

DE35 recorded two untraced hooks; the trace closed them with file:line
evidence (full report in the C6 task's graph node):

1. **Network-commissioning driver registration is compile-time only and
   crashes in the shape we need.** esp-matter's
   `network_commissioning_integration.cpp` registers the Thread and WiFi
   drivers from two independent Kconfig endpoint IDs; both default to
   endpoint 0, and with both drivers compiled in, the second
   registration hits `VerifyOrDie` in CHIP's
   `ServerClusterInterfaceRegistry` and aborts server bring-up. The
   stock `c6_wifi_thread` example avoids the crash only by moving Thread
   to a real secondary endpoint, the exact thing that wrecks C1. There
   is no app-level override: the callback is a strong symbol, compiled
   unconditionally.
2. **Stack bring-up is unconditional inside `esp_matter::start()`.**
   `InitWiFiStack()` plus station-mode enable run synchronously whenever
   WiFi is compiled in (no pre-init hook is reachable early enough), and
   `chip_init()` launches the OpenThread stack whenever Thread is
   compiled in, with `openthread_init_stack()` asserting if the platform
   config was not handed over first.
3. **The dormant transport leaks into the data model unless scrubbed.**
   `GeneralDiagnostics::NetworkInterfaces` enumerates every live
   `esp_netif`, and both `WiFiNetworkDiagnostics` and
   `ThreadNetworkDiagnostics` clusters attach to endpoint 0 at compile
   time with no mutual exclusion. Unlike 1 and 2, this is fixable with
   public APIs: skip the unused netif (falls out of fixing 2) and
   `cluster::destroy()` the unwanted diagnostics cluster before start.
   The network-commissioning cluster's feature map on endpoint 0 is also
   app-settable at runtime via the root-node config.
4. **Footprint**: a dormant WiFi stack costs approximately nothing
   static (its buffers are heap-allocated only on init), but a dormant
   OpenThread costs real BSS just by being linked: the static OT
   instance (tens of KB, including a ~5.6 KB message-buffer pool) plus
   a ~6.3 KB internal heap. The build measures the real number; the
   budget (~2.1 MB flash free, 168 KB boot heap on the WiFi image today)
   absorbs it, but the figure gets logged at boot like the D1 BLE cost
   so an SDK bump cannot quietly change the trade.

## 3. Consequence: a small, pinned SDK patchset

Hooks 1 and 2 require patching esp-matter (two files, both esp-matter's
own, not CHIP core): this design accepts that openly rather than
contorting around it.

- The repo gains `sdk-patches/esp-matter/*.patch` plus
  `scripts/apply-sdk-patches.sh`. The script checks the esp-matter
  checkout is at the pinned base commit (21aa3d1, release/v1.5),
  applies idempotently, and refuses with a clear message on drift. The
  build documentation makes it part of combined-image setup; the
  single-transport images build identically with or without the patches
  (the patched behavior is gated on both-stacks-compiled, which only the
  combined sdkconfig enables).
- **Patch 1** (`network_commissioning_integration.cpp`): when both
  drivers are compiled in, register exactly one on endpoint 0, chosen
  through a weak hook (`mt_active_transport_is_thread()`) rather than a
  setter: the patch itself declares `extern "C" int
  mt_active_transport_is_thread(void) __attribute__((weak))` and calls
  it, so the SDK carries no dependency on an app header, and the app's
  `mt_transport.c` supplies the strong definition. Same contract a setter
  would have given (one choice, read once at boot, the app decides
  before `esp_matter::start()`), reached without exporting an app-owned
  SDK header into the app for the SDK to include back. Single-stack
  builds keep the stock behavior byte-identically: the hook is only
  called when both drivers are compiled in.
- **Patch 2** (`esp_matter_core.cpp`): the WiFi stack init and the
  Thread stack launch each become conditional on the same runtime
  choice when both are compiled in. With WiFi chosen, OpenThread is
  never launched (its platform-config assert is therefore unreachable);
  with Thread chosen, the WiFi stack is never initialized.
- The patchset is deliberately minimal and upstream-shaped: an issue
  proposing runtime driver selection goes to esp-matter, and if it
  lands, the patchset retires.

Registration and launch must stay consistent per boot: a driver whose
stack never launched does not crash (CHIP null-guards the accessors) but
misbehaves silently, which is worse. One choice, read once at boot, feeds
both patches and the app-level scrub.

## 4. Firmware changes (app level)

- **`mt_transport.c/h`** (C, like the rest of the AT layer): owns the
  NVS key (`mt_cfg` namespace, `transport` u8, default WIFI), the
  read-at-boot, and the `AT+MTTRANSPORT` handlers.
  - `AT+MTTRANSPORT?` responds `+MTTRANSPORT:<active>,<stored>`
    (e.g. `WIFI,THREAD` while a switch is pending), then `OK`.
  - `AT+MTTRANSPORT=WIFI|THREAD` validates (else `+MTERR:1`), persists,
    answers `OK`. No reboot side effect: the host owns the reboot, via
    `AT+MTRESET` or power cycle, matching the staged-then-reboot pattern
    the composition set. On a single-transport image the command answers
    `+MTERR:1` for the transport the image cannot provide... rejected:
    simpler and more honest: the command exists only on the combined
    image; single-transport images answer `+MTERR:8` (unknown command)
    naturally, and `AT+MTNET?` already tells the host what the image is.
- **`app_main` (combined build only)**: latch the choice
  (`mt_transport_latch_active()`), which is what the patchset's weak hook
  reads back; set the root-node network-commissioning feature map to match;
  after `node::create()`, `cluster::destroy()` the dormant transport's
  diagnostics cluster; hand OpenThread its platform config only when
  Thread is chosen; log the active transport and the measured dormant
  cost at boot, next to the D1 BLE figures.
- **`mt_boot_window_policy` / P2**: the policy itself is unchanged, but its
  `mt_transport_is_provisioned()` helper cannot stay keyed on
  `CHIP_DEVICE_CONFIG_ENABLE_THREAD` on the combined image: that macro is 1
  there regardless of which stack actually booted, since both are compiled
  in. Left on the compile-time macro, a WiFi-active, WiFi-commissioned
  device would call `IsThreadProvisioned()`, get `false`, and be flagged as
  transport-mismatched on every boot: a wrong window, a spurious
  `+MTEVT:27`, on a device with nothing wrong. The helper dispatches on the
  same latch `app_main` reads (`mt_active_transport_is_thread()`), so it
  asks `IsWiFiStationProvisioned()` or `IsThreadProvisioned()` to match the
  stack that is actually running. Single-stack builds are unaffected: the
  macro is still the sole dispatch there, since `MT_COMBINED_IMAGE` is 0.
  The design asserts this on hardware rather than trusting the trace
  (verification, below).
- **`AT+MTNET?`**: already reports the transport family; on the combined
  image it reports the ACTIVE transport, so hosts and the regression
  harness keep working unmodified.

## 5. Build variant

`build_combined` with `sdkconfig.defaults.combined`: both
`CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION` and `..._ENABLE_THREAD` on, both
network-commissioning endpoint IDs left at 0 (the single-endpoint shape
the patches make legal), both diagnostics clusters compiled (the app
scrubs one at boot), BLE resident as always (D1), OTA requestor off, the
Thread task and OpenThread config matching `sdkconfig.defaults.thread`.
The `SDKCONFIG` redirect rule (F38) applies: the combined build redirects
its sdkconfig into `build_combined/` or it hijacks the WiFi build.

## 6. Spec ride-alongs

`AT_MT_SPEC.md`: the `AT+MTTRANSPORT` section (grammar, persistence
across both resets, pending-switch reporting, combined-image-only
availability); a note in the `AT+MTNET?` section that the transport field
reports the active stack on the combined image; the mismatch section
(3.12.1) gains one sentence: on the combined image the mismatch state is
reachable by a transport switch as well as a reflash, with identical
semantics. `ARCHITECTURE.md`: the DE35 section moves from "recorded
shape, parked" to the implemented design, including the patchset
rationale and the dormant-Thread BSS figure once measured.

## 7. Verification

1. **Bench preflight pins**: the measured dormant-OT BSS/heap figures;
   that a WiFi-mode combined boot never starts the OT task and a
   Thread-mode boot never inits WiFi (console evidence); that
   `GeneralDiagnostics::NetworkInterfaces` and endpoint 0's ServerList
   show exactly one transport per mode (chip-tool reads).
2. **The full regression suite runs on the combined image in BOTH
   modes**: the existing harness needs zero changes (transport detection
   via `AT+MTNET?` was built in T4); four full-flag runs total (two per
   mode) plus baselines `combined-wifi-lifecycle.json` and
   `combined-thread-lifecycle.json` if results prove mode-identical to
   the single images, or a decision to reuse the existing baselines if
   byte-identical.
3. **The switch test the T4 spec deferred finally lands**: commission on
   WiFi, `AT+MTTRANSPORT=THREAD`, reboot, assert the mismatch contract
   (+MTEVT:27, `AT+MTNET?` mismatch flag, boot window), switch back,
   assert the fabric revives and the device serves it. Same in the other
   direction. This doubles as the hardware proof for P2 semantics on the
   combined image.

## 8. Risks, recorded

- **SDK patch maintenance** is the real cost: bounded to two esp-matter
  files, pinned to the base commit, script-enforced, upstream-proposed.
  An esp-matter upgrade re-evaluates the patchset first.
- **Dormant OT BSS** shrinks the WiFi-mode heap by the measured figure on
  the combined image relative to `build_b4`; the boot log carries it.
- **Coexistence**: only one of WiFi/Thread is ever active alongside
  resident BLE, the same radio-sharing shape both single images already
  run; no new coexistence surface.
- **Descriptor-surface completeness**: the scrub covers the known leaks
  (netif enumeration, diagnostics clusters, feature map). The chip-tool
  reads in verification step 1 are the tripwire for any surface the
  trace missed.

## 9. Out of scope, recorded so they are not rediscovered

- Live transport switching without reboot, and dual-active transports:
  both explicitly out per DE35.
- Retiring the single-transport images (revisited only after combined
  passes both-mode regression).
- Upstreaming the patchset (proposed, but the design does not wait on
  it).
- Harness changes: none are expected; if verification finds any, they go
  through the harness's own process.
