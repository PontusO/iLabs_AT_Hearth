# Generic Switch + Color Light Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add MatterGenericSwitch (0x000F, event-driven, via the AT
surface's first event-emission command) and MatterColorLight (0x010D,
library-only) across firmware and library, reaching 18 of 20 upstream
classes.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-04-switch-colorlight-design.md`): one
firmware table row with the fourth abort-trap handled, one new command
AT+MTSWITCH backed by a ChipStackLock bridge calling esp-matter's
one-line event sender; two library classes mirroring upstream, one of
which needs no firmware at all.

**Tech Stack:** C (mt_at.c), C++ (main.cpp, mt_devtypes.cpp, library
classes), esp-matter 21aa3d1 pinned, MockStream host tests.

## Global Constraints

- **No em dashes** anywhere. mt_at.c stays C (no esp_matter/CHIP headers
  in C translation units); bridge functions hold ChipStackLock.
- **AT grammar division**: bare ERROR = wrong form; +MTERR:1 = bad
  value; the data-model lookup codes are MT_ERR_NO_ENDPOINT 2 and
  MT_ERR_NO_CLUSTER 3 (main/mt_at.c:44-46).
- **IDs via get_device_type_id(), never literals** in the firmware
  table; SDK patchset untouched (scripts/apply-sdk-patches.sh --check
  answers "applied" x2 before and after any build).
- All three firmware images build green at task boundaries (commands and
  SDKCONFIG redirects exactly as in the repo docs; IDF v5.4.1 sourced,
  never 5.5.4; foreground builds, no pgrep-style watchers).
- Library work on a new branch `switch-colorlight` in
  /home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth (Dropbox:
  edit in place, never symlink). Parity surfaces stay byte-identical;
  library style 2-space /* */; decimal IDs in expect() strings; no
  binaries in any commit (each new test binary's .gitignore line lands
  with its Makefile line); fixes are NEW commits, never amends; TDD
  with red excerpts captured in reports.
- Host tests green in the touched repo at every boundary
  (`make -C test/host run`).
- Commit messages explain why and end with exactly:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task S1: Firmware: the switch row, bridge and AT+MTSWITCH

**Files:**
- Modify: `main/mt_devtypes.cpp` (one thunk + one row)
- Modify: `main/main.cpp` (one bridge function, near mt_matter_attr_write)
- Modify: `main/include/mt_matter.h` (one declaration)
- Modify: `main/mt_at.c` (one handler + one table row)

**Interfaces:**
- Produces: `extern "C" int mt_matter_switch_click(uint16_t ep);`
  returning 0 on success or an MT_ERR_* positive code; the AT command
  `AT+MTSWITCH=<ep>[,<action>]` per the spec grammar.

- [ ] **Step 1: the thunk and row** in mt_devtypes.cpp. Verify the
config field and feature namespace in the pinned headers first
(esp_matter_endpoint.h generic_switch ~:372-383; the switch cluster
config and momentary feature in esp_matter_cluster.h ~:684-698 and
esp_matter_feature.h; quote the lines in the report). The cluster
create runs VALIDATE_FEATURES_EXACT_ONE on latching vs momentary
(esp_matter_cluster.cpp ~:2167-2224): a zero feature_flags ASSERTS at
boot. Abort trap number four; comment in the same voice as the other
three:

```cpp
static endpoint_t *mk_generic_switch(node_t *n)
{
    generic_switch::config_t c;
    /* Exactly one of Latching/Momentary is mandatory: a default
     * feature_flags of 0 ASSERTS in cluster create
     * (VALIDATE_FEATURES_EXACT_ONE, esp_matter_cluster.cpp). Momentary
     * matches the upstream arduino-esp32 class, and the feature add
     * also registers the InitialPress event metadata that
     * mt_matter_switch_click() emits. */
    c.switch_cluster.feature_flags =
        cluster::switch_cluster::feature::momentary_switch::get_id();
    return generic_switch::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}
```

Row: `{ generic_switch::get_device_type_id(), mk_generic_switch, "generic_switch" }`.

- [ ] **Step 2: the bridge** in main.cpp, mirroring
mt_matter_attr_write's placement and lock discipline (main.cpp:632):

```cpp
/*
 * AT+MTSWITCH: emit the Switch cluster's InitialPress event, which is
 * what the upstream class's click() does. Events are fire-and-forget
 * toward subscribed controllers; nothing echoes on the AT link.
 * EventLogging requires the Matter stack lock off the CHIP thread
 * (EventLogging.h), which ChipStackLock provides, same as every other
 * bridge function here.
 */
extern "C" int mt_matter_switch_click(uint16_t ep)
{
    ChipStackLock lock;
    endpoint_t *e = esp_matter::endpoint::get(node::get(), ep);
    if (e == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    cluster_t *c = esp_matter::cluster::get(e, chip::app::Clusters::Switch::Id);
    if (c == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    esp_err_t err = esp_matter::cluster::switch_cluster::event::send_initial_press(ep, 1);
    return (err == ESP_OK) ? 0 : MT_ATTR_ERR_TYPE;
}
```

(Verify the MT_ATTR_ERR_* constants' names and values in mt_matter.h:
they must map onto +MTERR:2/3 exactly as the attr path does; verify the
send_initial_press signature in esp_matter_event.h ~:158-176. If the
sender wants the cluster_t or different args, follow the header and
record the delta.) Declaration in mt_matter.h beside the attr bridges.

- [ ] **Step 3: the handler** in mt_at.c, following cmd_mtattr's shape
(mt_at.c:260) and the table registration (mt_at.c:630):

```c
/*
 * AT+MTSWITCH=<ep>[,<action>] -> emit a switch event on <ep>. Action 0
 * (the default) is InitialPress, the upstream click(). Other actions
 * are reserved for the richer switch features and answer +MTERR:1
 * until they exist. The first event-emission command on this surface:
 * nothing echoes back, controllers see it via their subscriptions.
 */
static int cmd_mtswitch(at_type_t type, char *args)
{
    char *f[2];
    int n = at_split_args(args, f, 2);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n < 1) {
        return MT_ERR_BAD_PARAM;
    }
    unsigned long ep, action = 0;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    if (n >= 2 && (!parse_u(f[1], &action) || action != 0)) {
        return MT_ERR_BAD_PARAM;
    }
    int rc = mt_matter_switch_click((uint16_t)ep);
    return (rc == 0) ? AT_R_OK : rc;
}
```

Table row `{ "MTSWITCH", cmd_mtswitch }` in alphabetical-ish placement
matching the table's existing order convention (read it; it is grouped,
not strictly alphabetical: put it after MTATTR's data-model group).
(Verify parse_u/at_split_args names and the AT_R_OK return convention
against the neighboring handlers; follow the file.)

- [ ] **Step 4: builds.** All three images green; host tests green
(nothing host-testable changed; confirm no regressions).
`scripts/apply-sdk-patches.sh --check` before and after.

- [ ] **Step 5: commit** (`feat: generic switch device type and AT+MTSWITCH event emission` + why-body + trailers).

### Task S2: Firmware docs and 0.3.1

**Files:**
- Modify: `docs/AT_MT_SPEC.md` (AT+MTSWITCH section; device table row 18;
  the event-emission paragraph)
- Modify: `docs/ARCHITECTURE.md` (8.2: fourth abort trap; the event
  surface sentence)
- Modify: `README.md` (table gains 0x000F; parked list shrinks to two)
- Modify: `main/include/mt_at_config.h` (MT_FW_VERSION "0.3.1")

**Interfaces:** none new.

- [ ] **Step 1:** AT_MT_SPEC.md: the AT+MTSWITCH section per the spec
grammar (set form only; action 0 = InitialPress position 1; reserved
actions +MTERR:1; +MTERR:2/3 for endpoint/cluster lookups; the
event-emission paragraph: events are fire-and-forget to subscribed
controllers, never echo as URCs, not readable over AT). Device table:
0x000F generic_switch row (18 rows). Match voice and formats.
- [ ] **Step 2:** ARCHITECTURE.md 8.2: trap list to four (the
exact-one validation is a different macro than the at-least-one traps:
say so); one sentence introducing the event-emission bridge. README
table + parked list update.
- [ ] **Step 3:** MT_FW_VERSION to "0.3.1". Rebuild build_b4 only;
verify with strings; host tests green.
- [ ] **Step 4: commit** (`docs: AT+MTSWITCH spec, the fourth trap, 0.3.1`).

### Task S3: Library: the two classes

**Files (library repo, branch switch-colorlight):**
- Create: `src/MatterEndpoints/MatterGenericSwitch.{h,cpp}`,
  `src/MatterEndpoints/MatterColorLight.{h,cpp}`
- Test: `test/host/test_genericswitch.cpp`, `test/host/test_colorlight.cpp`
- Modify: `test/host/Makefile` (+2 binaries), `.gitignore` (+2 lines)

**Interfaces:**
- Consumes: MatterEndPoint base, HearthColorUtil conversions,
  Hearth.hearthCommand for the raw switch command.
- Produces: the two class names S4 registers.

- [ ] **Step 1: read the upstream mirrors end to end**
(~/.arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/Matter/src/MatterEndpoints/MatterGenericSwitch.{h,cpp}
and MatterColorLight.{h,cpp}): API surfaces verbatim, cluster/attr IDs
verified there (ColorLight: OnOff 0x0006/0x0000 bool, LevelControl
0x0008/0x0000 u8, ColorControl 0x0300 CurrentHue 0x0000 u8 /
CurrentSaturation 0x0001 u8; GenericSwitch: no attributes driven).
- [ ] **Step 2: tests first, red evidenced.** GenericSwitch minimum
list (adapted: no cache/URC cases): begin declares 0x000F with no AT
traffic; click() sends exactly `AT+MTSWITCH=<ep>` and returns true on
OK; click() on `+MTERR:3\r\nERROR\r\n` returns false; click() before
reconcile returns false without traffic; re-begin refused. ColorLight:
the recipe minimum list (mirror test_enhancedcolor's cases minus
color-temp: begin/adopt, setColorHSV wire sequence hue-then-sat-then-
level, controller-driven hue/sat/level URCs, failed-write cache rules,
re-begin refused; decimal IDs).
- [ ] **Step 3: implement.** GenericSwitch: begin() = hearthDeclare
(0x000F) only; click() = started/reconciled guard, then
`Hearth.hearthCommand("AT+MTSWITCH=<ep>")` returning rc==0;
attributeChangeCB present as a documented no-op; hearthAttrTypeFor
falls through. ColorLight: mirror upstream (HSV cache, setColorRGB via
HearthColorUtil, the hue/sat/level write sequence and echo-ordering
discipline copied from MatterEnhancedColorLight.cpp's committed
pattern); deviation note: the wire carries XY/CT attributes this class
never touches (superset via the shared 0x010D row).
- [ ] **Step 4:** full suite green (22 binaries). Commit
(`feat: generic switch and color light endpoint classes`).

### Task S4: Library integration and 0.3.1

**Files:**
- Modify: `src/Hearth.h` (two includes), `keywords.txt` (two KEYWORD1 +
  new callback typedefs), `README.md` (18 supported / 2 parked + the
  sub-deferrals; the switch documented as event-driven: click() emits,
  nothing arrives back), `library.properties` (0.3.1)
- Create: `examples/MatterGenericSwitch/`, `examples/MatterColorLight/`
  (byte-identical upstream .ino copies)

**Interfaces:** consumes S3's classes.

- [ ] **Step 1:** umbrella, keywords (TAB-checked), README, version.
- [ ] **Step 2:** copy both upstream examples; cmp for byte-identity;
arduino-cli compile both for pico:rp2040:challenger_2350_wifi6_ble5
(compile-only; kill discovery daemons afterwards per the standing bench
lesson, record it).
- [ ] **Step 3:** full suite green; commit
(`feat: integration for the switch and color light, 0.3.1`).

### Task S5: Bench verification (operator present, no unplugs)

**Files:** none committed (report only).

- [ ] **Step 1:** flash rebuilt build_b4 (0.3.1), factory-fresh;
compose 0x000F + 0x010D via AT (EPCLEAR/EP/EP/EPAPPLY); boot rebuild
clean (the fourth trap proves out); AT+MTVER? 0.3.1.
- [ ] **Step 2:** negative pins over AT: `AT+MTSWITCH=<light-ep>`
answers +MTERR:3; `AT+MTSWITCH=99` answers +MTERR:2;
`AT+MTSWITCH=<switch-ep>,1` answers +MTERR:1; bare `AT+MTSWITCH`
answers ERROR.
- [ ] **Step 3:** commission (chip-tool, .env rules); subscribe or
read Switch events (`chip-tool switch subscribe-event initial-press
...` or read-event after) and prove `AT+MTSWITCH=<switch-ep>` lands as
an InitialPress event with position 1 on the controller side.
- [ ] **Step 4:** the library path: upload a smoke sketch (scratch)
declaring both classes; GenericSwitch.click() observed controller-side;
ColorLight setColorHSV round-trip (chip-tool readback of hue/sat) and a
controller move-to-hue-and-saturation dispatching the class callback.
- [ ] **Step 5:** restore the bench (bridge with --bridge espnow
explicitly: the --bridge-only default is the serial bridge, known
trap; AT+MTFRESET; composition 0x0100), full report with verbatim
evidence.
