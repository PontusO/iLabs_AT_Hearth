# C6 Combined WiFi+Thread Image Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One C6 image carrying both network stacks with exactly one active
per boot, selected by a persisted `AT+MTTRANSPORT` setting, verified by the
full regression suite in both modes plus the transport-switch mismatch test.

**Architecture:** Per the design spec
(`docs/superpowers/specs/2026-08-01-c6-combined-image-design.md`): a
two-file pinned esp-matter patchset routes driver registration and stack
launch through one weak C hook the app defines; the app owns the NVS
setting, the AT surface, and the dormant-transport data-model scrub. The
single-transport images remain preprocessor-identical (every patched line
sits behind a both-stacks-compiled guard).

**Tech Stack:** C (mt_ layer), C++ (main.cpp bridge), esp-matter release/v1.5
pinned at 21aa3d1, IDF v5.4.1, gcc host tests in test/host.

## Global Constraints

- **No em dashes** anywhere. Colon, comma, parentheses or a full stop.
- **`mt_at.c` stays C**; anything touching esp_matter/CHIP goes in a `.cpp`
  behind an `extern "C"` bridge in `mt_matter.h`. `mt_transport.c` is C and
  must not include any esp_matter or CHIP header.
- **The SDK patches must leave single-stack builds preprocessor-identical**:
  every added or changed line in esp-matter sits inside
  `#if CHIP_DEVICE_CONFIG_ENABLE_WIFI && CHIP_DEVICE_CONFIG_ENABLE_THREAD`
  (or the exact both-compiled condition of its site), so `build_b4` and
  `build_thread` compile the same translation units as stock.
- **The weak hook contract**: `extern "C" int mt_active_transport_is_thread(void)`,
  weak-declared at each SDK use site; unresolved (address 0) or returning 0
  means WiFi, returning nonzero means Thread. The app defines it strongly
  only in the combined build.
- Registration and launch must key on the SAME hook result within one boot;
  never register a driver whose stack was not launched.
- The esp-matter checkout is at pinned commit 21aa3d1; the apply script
  refuses any other base. The checkout may carry our patches BUT NOTHING
  ELSE: `git -C ~/esp/esp-matter status` must show only the two patched
  files modified after apply, and `scripts/apply-sdk-patches.sh --revert`
  must restore pristine.
- Builds: `build_b4` (WiFi) and `build_thread` keep building green at every
  task boundary; `build_combined` from Task 2 on. The SDKCONFIG redirect
  rule (F38) applies to combined exactly as to thread.
- Hardware is touched ONLY in Tasks 5 and 6 (Tasks 1, 2 and 4 compile and
  link but never flash).
- Commit messages explain why and end with exactly:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task 1: The SDK patchset and its apply script

**Files:**
- Create: `sdk-patches/esp-matter/0001-hearth-runtime-transport-selection.patch`
- Create: `scripts/apply-sdk-patches.sh`
- Modify: `docs/ARCHITECTURE.md` (one paragraph: the patchset exists, why,
  base commit, apply script)

**Interfaces:**
- Consumes: the pinned esp-matter checkout at 21aa3d1.
- Produces: the weak-hook behavior in both SDK files; the script contract
  `apply-sdk-patches.sh [--revert|--check]` (exit 0 clean, 1 drift/fail,
  prints one line per action).

- [ ] **Step 1: edit `network_commissioning_integration.cpp`**

In `ESPMatterNetworkCommissioningClusterServerInitCallback`, replace the
two independent driver blocks' guards so that when BOTH drivers are
compiled in, exactly one registers, chosen by the hook. The Thread block
becomes:

```cpp
#ifdef CONFIG_THREAD_NETWORK_COMMISSIONING_DRIVER
    if (endpointId == CONFIG_THREAD_NETWORK_ENDPOINT_ID
#if defined(CONFIG_THREAD_NETWORK_COMMISSIONING_DRIVER) && defined(CONFIG_WIFI_NETWORK_COMMISSIONING_DRIVER)
        /* Hearth: both drivers compiled in and sharing an endpoint would
         * VerifyOrDie in ServerClusterInterfaceRegistry::Create. Exactly
         * one registers per boot, chosen by the app's persisted
         * transport (weak hook; absent means WiFi). */
        && mt_active_transport_is_thread_hook()
#endif
        ) {
        static DeviceLayer::NetworkCommissioning::GenericThreadDriver sThreadDriver;
        gServers[index].Create(endpointId, &sThreadDriver, gBreadcrumbTracker);
        gServers[index].Cluster().Init();
        (void)esp_matter::data_model::provider::get_instance().registry().Register(gServers[index].Registration());
    }
#endif
```

and the WiFi block symmetrically gains
`&& !mt_active_transport_is_thread_hook()` under the same both-compiled
condition. Above the callback (file scope, inside the both-compiled guard)
add the hook shim once:

```cpp
#if defined(CONFIG_THREAD_NETWORK_COMMISSIONING_DRIVER) && defined(CONFIG_WIFI_NETWORK_COMMISSIONING_DRIVER)
extern "C" int mt_active_transport_is_thread(void) __attribute__((weak));
static bool mt_active_transport_is_thread_hook(void)
{
    return (&mt_active_transport_is_thread != nullptr) && mt_active_transport_is_thread();
}
#endif
```

- [ ] **Step 2: edit `esp_matter_core.cpp`**

The same shim (C++ file scope, guarded by
`#if CHIP_DEVICE_CONFIG_ENABLE_WIFI && CHIP_DEVICE_CONFIG_ENABLE_THREAD`)
once near the top. Then:

WiFi init in `esp_matter::start()`:

```cpp
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    if (!mt_active_transport_is_thread_hook())
#endif
    {
        VerifyOrReturnError(chip::DeviceLayer::Internal::ESP32Utils::InitWiFiStack() == CHIP_NO_ERROR, ESP_FAIL,
                            ESP_LOGE(TAG, "Error initializing Wi-Fi stack"));
    }
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI
```

Thread launch in `init_thread_stack_and_start_thread_task()`: immediately
inside the existing `#if CHIP_DEVICE_CONFIG_ENABLE_THREAD` +
`#ifdef CONFIG_ESP_MATTER_ENABLE_OPENTHREAD`:

```cpp
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    if (!mt_active_transport_is_thread_hook()) {
        /* Hearth: WiFi is the active transport this boot; the OpenThread
         * stack stays unlaunched (and its platform-config assert
         * unreachable). CHIP's accessors null-guard the missing
         * instance. */
        return ESP_OK;
    }
#endif
```

- [ ] **Step 3: generate the patch and revert the checkout**

```bash
git -C ~/esp/esp-matter diff > sdk-patches/esp-matter/0001-hearth-runtime-transport-selection.patch
git -C ~/esp/esp-matter checkout -- .
```

- [ ] **Step 4: write `scripts/apply-sdk-patches.sh`**

```bash
#!/usr/bin/env bash
# Applies the Hearth esp-matter patchset to the pinned SDK checkout.
# Refuses on base-commit drift so an SDK bump re-evaluates the patches
# deliberately (design spec 2026-08-01, section 3).
set -euo pipefail
ESP_MATTER="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"
PINNED=21aa3d1
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES="$HERE/sdk-patches/esp-matter"

head=$(git -C "$ESP_MATTER" rev-parse --short HEAD)
if [ "$head" != "$PINNED" ]; then
    echo "refuse: esp-matter at $head, patchset pinned to $PINNED" >&2
    exit 1
fi
case "${1:-apply}" in
  --check)
    for p in "$PATCHES"/*.patch; do
        if git -C "$ESP_MATTER" apply --check --reverse "$p" 2>/dev/null; then
            echo "applied: $(basename "$p")"
        elif git -C "$ESP_MATTER" apply --check "$p" 2>/dev/null; then
            echo "not applied: $(basename "$p")"
        else
            echo "DRIFT: $(basename "$p") fits neither way" >&2; exit 1
        fi
    done ;;
  --revert)
    for p in "$PATCHES"/*.patch; do
        git -C "$ESP_MATTER" apply --reverse "$p" && echo "reverted: $(basename "$p")"
    done ;;
  apply)
    for p in "$PATCHES"/*.patch; do
        if git -C "$ESP_MATTER" apply --check --reverse "$p" 2>/dev/null; then
            echo "already applied: $(basename "$p")"
        else
            git -C "$ESP_MATTER" apply "$p" && echo "applied: $(basename "$p")"
        fi
    done ;;
  *) echo "usage: $0 [apply|--check|--revert]" >&2; exit 2 ;;
esac
```

`chmod +x`, then exercise all three modes (apply, `--check`, `--revert`,
then apply again to prove idempotence). Test the drift refusal without
touching the real checkout: run with `ESP_MATTER_PATH` pointing at any git
repo whose HEAD is not the pin (this repo itself works) and expect exit 1
with the refusal line.

- [ ] **Step 5: prove single-stack neutrality and build**

```bash
scripts/apply-sdk-patches.sh
source ~/esp/esp-idf-v5.4.1/export.sh && source ~/esp/esp-matter/export.sh
idf.py -B build_b4 build            # WiFi image, patches applied
idf.py -B build_thread -D SDKCONFIG=build_thread/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.thread" build
```

Both green. Neutrality argument goes in the task report: every patched
line is inside a both-compiled guard, so these two builds preprocess
identically to stock (spot-check with
`idf.py -B build_b4 build` on the reverted checkout producing the same
`ilabs_at_hearth.bin` SHA if convenient, and note that timestamp-bearing
sections may defeat byte identity: the preprocessor argument is the
binding one).

- [ ] **Step 6: ARCHITECTURE.md paragraph and commit**

```bash
git add sdk-patches scripts/apply-sdk-patches.sh docs/ARCHITECTURE.md
git commit -m "feat: pinned esp-matter patchset for runtime transport selection"
```

(Expand the body: why the patches exist, the VerifyOrDie collision, the
weak-hook contract, the neutrality guarantee; trailers.)

### Task 2: The combined build variant

**Files:**
- Create: `sdkconfig.defaults.combined`
- Modify: `docs/ARCHITECTURE.md` (build matrix table if present, else the
  build section: the third variant and its command)

**Interfaces:**
- Produces: `build_combined` building green with both stacks and the
  patches; measured size/BSS deltas recorded in the task report and
  ARCHITECTURE.md.

- [ ] **Step 1: write `sdkconfig.defaults.combined`**

Start from `sdkconfig.defaults.thread` (it carries the OpenThread platform
settings and restates `CONFIG_ENABLE_OTA_REQUESTOR=n` per F38) and ADD the
WiFi station enables from the base defaults; both network-commissioning
endpoint IDs stay at their default 0; both diagnostics clusters stay
enabled (the app scrubs at boot). Copy the exact option names from the two
existing defaults files rather than from memory.

- [ ] **Step 2: build and measure**

```bash
scripts/apply-sdk-patches.sh
idf.py -B build_combined -D SDKCONFIG=build_combined/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.combined" build
python3 -m esptool image_info build_combined/ilabs_at_hearth.bin 2>/dev/null | head -5
idf.py -B build_combined size | tail -15
idf.py -B build_b4 size | tail -15
```

Record: app size vs build_b4 and build_thread, DRAM/BSS delta (the
dormant-OT tax the design predicted in tens of KB), and that the factory
slot (3840 KB) holds it with margin.

- [ ] **Step 3: commit** (`feat: build_combined, both stacks in one image`
+ measured figures in the body + trailers).

### Task 3: mt_transport and AT+MTTRANSPORT

**Files:**
- Create: `main/mt_transport.c`, `main/include/mt_transport.h`
- Modify: `main/mt_at.c` (command registration + handlers), `main/CMakeLists.txt`
- Create: `test/host/test_mt_transport.c` (pure-parse tests)
- Modify: `test/host/Makefile`

**Interfaces:**
- Produces (in `mt_transport.h`):

```c
typedef enum { MT_TRANSPORT_WIFI = 0, MT_TRANSPORT_THREAD = 1 } mt_transport_t;
/* Parse "WIFI"/"THREAD" (exact, upper-case). 0 on success. Pure, host-testable. */
int mt_transport_parse(const char *arg, mt_transport_t *out);
const char *mt_transport_name(mt_transport_t t);
/* NVS-backed (namespace "mt_cfg", key "transport", default WIFI). */
mt_transport_t mt_transport_stored(void);
int mt_transport_store(mt_transport_t t);          /* 0 on success */
mt_transport_t mt_transport_active(void);          /* latched at boot */
void mt_transport_latch_active(void);              /* called once in app_main */
```

- The strong hook definition lives HERE (C, no SDK headers):

```c
/* The weak hook the SDK patches call. Latched before esp_matter::start()
 * so registration and stack launch key on one value per boot. */
int mt_active_transport_is_thread(void)
{
    return mt_transport_active() == MT_TRANSPORT_THREAD;
}
```

  Compiled into every image; on single-stack builds the SDK never calls it
  and the linker keeps or drops it harmlessly.

- [ ] **Step 1: host tests first** (`test/host/test_mt_transport.c`):
parse accepts exactly "WIFI" and "THREAD" (case-sensitive, per the AT
grammar's upper-case convention), rejects "", "wifi", "BOTH", "WIFI2";
name() round-trips both values. Wire into the host Makefile, run red
(missing file), implement the pure parts, run green
(`make -C test/host run`).

- [ ] **Step 2: NVS parts** (device-only, compile-checked): follow
`mt_comp_store.c`'s open/commit/close idiom for the `mt_cfg` namespace;
`mt_transport_stored()` returns WIFI on any read failure (fresh device);
`mt_transport_latch_active()` reads once into a static; `active()` returns
the latch (WIFI if never latched, which cannot happen in practice since
app_main latches before anything queries).

- [ ] **Step 3: AT handlers in mt_at.c** (follow the file's existing
handler table conventions exactly; read a neighbouring query/set command
first):

- `AT+MTTRANSPORT?` responds `+MTTRANSPORT:<active>,<stored>` then `OK`.
- `AT+MTTRANSPORT=<arg>`: `mt_transport_parse` failure answers `+MTERR:1`;
  success stores and answers `OK` (no reboot side effect; the composition
  set the staged-then-reboot pattern).
- Exec/no-arg forms follow the grammar rules (bare `ERROR` for a wrong
  form), matching the spec table added in Task 4.
- Per the design decision, the command exists ONLY on the combined image:
  single-transport images never register it, so they answer `+MTERR:8`
  (unknown command) naturally. Gate the registration on
  `MT_COMBINED_IMAGE`, a new define in `mt_at_config.h` derived from
  `sdkconfig.h` (the plain IDF config header, legal in a C file):

```c
#include "sdkconfig.h"
#if defined(CONFIG_ENABLE_WIFI_STATION) && defined(CONFIG_OPENTHREAD_ENABLED)
#define MT_COMBINED_IMAGE 1
#else
#define MT_COMBINED_IMAGE 0
#endif
```

  Verify the two option names against the generated sdkconfigs first
  (`grep CONFIG_ENABLE_WIFI_STATION build_b4/sdkconfig
  build_thread/sdkconfig` and the same for `CONFIG_OPENTHREAD_ENABLED`):
  the pair must be exclusive to each single-stack build and both present
  in the combined one. Substitute the pair that actually satisfies that
  if these names differ, and record the choice in the report.

- [ ] **Step 4: build all three images green, host tests green, commit**
(`feat: AT+MTTRANSPORT, the persisted transport selection` + why-body +
trailers).

### Task 4: app_main integration and the spec ride-alongs

**Files:**
- Modify: `main/main.cpp` (combined-build boot path), `main/include/mt_matter.h`
  if a bridge addition is needed
- Modify: `docs/AT_MT_SPEC.md` (MTTRANSPORT section; MTNET note; 3.12.1
  sentence), `docs/ARCHITECTURE.md` (DE35 section rewritten as implemented)

**Interfaces:**
- Consumes: `mt_transport_latch_active()`, `mt_transport_active()`, the
  hook, esp_matter public APIs (`cluster::get`, `cluster::destroy`), the
  root-node network-commissioning feature-map config.

- [ ] **Step 1: app_main (combined build only, guarded by the both-compiled
condition):**

```cpp
    mt_transport_latch_active();
    ESP_LOGI(TAG, "active transport: %s (stored: %s)",
             mt_transport_name(mt_transport_active()),
             mt_transport_name(mt_transport_stored()));
```

before `node::create()`; set the root node's network-commissioning
feature map from the choice (read the actual config struct path in
`esp_matter_endpoint.h` at implementation time; the trace confirmed it is
settable pre-create); after `node::create()` and before the composition
rebuild, destroy the dormant transport's diagnostics cluster:

```cpp
    {
        endpoint_t *root = esp_matter::endpoint::get(node, 0);
        uint32_t dormant = mt_active_transport_is_thread()
            ? chip::app::Clusters::WiFiNetworkDiagnostics::Id
            : chip::app::Clusters::ThreadNetworkDiagnostics::Id;
        cluster_t *c = esp_matter::cluster::get(root, dormant);
        if (c) {
            esp_matter::cluster::destroy(c);
            ESP_LOGI(TAG, "dormant %s diagnostics cluster removed",
                     mt_active_transport_is_thread() ? "WiFi" : "Thread");
        }
    }
```

(API names verified against the trace; confirm exact signatures in
`esp_matter_data_model.h` when implementing.) Hand OpenThread its platform
config ONLY when Thread is the active transport (the existing Thread-image
code path, now conditioned on the latch instead of the compile flag alone).
Log the free-heap figure after start next to the existing D1 line so the
dormant-stack cost is visible per boot.

- [ ] **Step 2: spec ride-alongs.** AT_MT_SPEC.md: the `AT+MTTRANSPORT`
section (command table row, grammar, persistence across BOTH resets,
`<active>,<stored>` semantics, combined-image-only availability with the
natural `+MTERR:8` elsewhere); the `AT+MTNET?` note (active transport on
the combined image); section 3.12.1's added sentence (mismatch reachable
by switch as well as reflash, identical semantics). Also update the
design spec's section 3 in place: its "setter" phrasing becomes the weak
hook as implemented (same contract, one choice read once at boot; the
hook avoids exporting an SDK header to the app). ARCHITECTURE.md:
rewrite the DE35 paragraph as implemented (patchset, hook, scrub, measured
figures from Task 2).

- [ ] **Step 3: all three builds green; host tests green; commit**
(`feat: combined image boots one transport and scrubs the other` +
why-body + trailers).

### Task 5: Bench verification A: combined image, WiFi mode

Bench-facing. Flash `build_combined` (espnow bridge), OTBR not needed.

- [ ] **Step 1: preflight pins (probe scripts + console capture):**
fresh combined boot defaults to WIFI (`AT+MTTRANSPORT?` reports
`WIFI,WIFI`); the console shows no OpenThread task launch and no OT netif;
`AT+MTNET?` reports WIFI; boot heap figure recorded against `build_b4`'s
(the dormant-OT tax, cross-checked with Task 2's static numbers).
Persistence pin, BEFORE any commissioning: `AT+MTTRANSPORT=THREAD`, then
`AT+MTFRESET`; after the reboot `AT+MTTRANSPORT?` must report
`THREAD,THREAD` (the setting survived factory reset, and the device is
factory-fresh so booting Thread-active is harmless); then
`AT+MTTRANSPORT=WIFI` + `AT+MTRESET` and confirm `WIFI,WIFI` (survives
soft reset too, bench restored to WiFi mode for the suite).
- [ ] **Step 2: chip-tool surface check** (after the suite's first
commissioning, or standalone paired session): read
`generaldiagnostics network-interfaces` and `descriptor server-list` on
endpoint 0: exactly one transport's interface, no ThreadNetworkDiagnostics
cluster. Quote the reads in the report.
- [ ] **Step 3: the full suite, WiFi mode**: two full-flag runs
(operator unplugs) byte-identical, exit 0. Result-name set must equal the
single-image baselines'; if the results are PASS-identical to
`wifi-lifecycle.json`, record that finding and DO NOT commit a new
baseline yet (the decision lands in Task 6 with both modes in hand).
- [ ] **Step 4: report** (no commit unless fixtures changed; findings and
figures only).

### Task 6: Bench verification B: Thread mode, the switch test, closure

Bench-facing. OTBR per the TESTING.md section 9 runbook (LAN backbone,
socket ACL).

- [ ] **Step 1: switch to Thread**: `AT+MTTRANSPORT=THREAD` on the
factory-fresh device, `AT+MTRESET`, verify `AT+MTTRANSPORT?` reports
`THREAD,THREAD`, `AT+MTNET?` reports THREAD, console shows the OT task and
no WiFi init error beyond the pinned expectation (verification watches for
the DriveStationState error line the design accepted; record what actually
prints).
- [ ] **Step 2: the full suite, Thread mode**: two full-flag runs,
byte-identical, exit 0.
- [ ] **Step 3: the switch/mismatch test (the T4 deferral lands):**
commission on WiFi mode (suite or manual), then `AT+MTTRANSPORT=THREAD` +
reboot: assert `+MTEVT:27`, `AT+MTNET?` mismatch flag, boot window open,
device NOT reachable at the old operational address; switch back to WIFI +
reboot: assert the mismatch clears, no window, and chip-tool reaches the
old node id again (fabric revived). Repeat in the Thread-first direction.
Every assertion via probe scripts with the harness helpers; verbatim
evidence in the report.
- [ ] **Step 4: baseline decision**: with both modes PASS-identical to the
single-image baselines, record THAT as the finding (no new baseline files;
TESTING.md gains one sentence saying the combined image is covered by the
existing baselines per mode). If any check name or outcome differs,
STOP and report BLOCKED with the diff: that is a design-level surprise.
- [ ] **Step 5: restore the bench**: reflash `build_b4`, factory-fresh with
composition, `AT+MTTRANSPORT` absent (+MTERR:8) confirmed, OTBR noted
(running or stopped per operator preference).
- [ ] **Step 6: commit** the TESTING.md sentence and any doc figure updates
(`test: combined image verified in both modes, switch contract proven` +
why-body + trailers), full report with all evidence.
