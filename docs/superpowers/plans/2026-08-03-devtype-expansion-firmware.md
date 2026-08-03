# Device-Type Expansion, Stage F (Firmware) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the firmware device-type table from 4 to 17 rows, fix
the latent start_up_current_level bug in the existing light rows, and
bench-prove the risk set, so the library stage lands against firmware
that already accepts everything.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-03-devtype-expansion-design.md`): rows
and thunks in `main/mt_devtypes.cpp` only; the AT surface, composition
codec and SDK patchset are untouched. The two abort-trap types (window
covering, thermostat) get feature_flags set in their thunks; extended
color light gets the hue/saturation bolt-on.

**Tech Stack:** C++ (main/mt_devtypes.cpp), esp-matter 21aa3d1 pinned,
IDF v5.4.1, bench verification via probe scripts + the C5 harness.

## Global Constraints

- **No em dashes** anywhere: code, comments, commit messages, docs.
- **IDs always via `<ns>::get_device_type_id()`, never literals** (the
  table's own rule, main/mt_devtypes.cpp:72).
- **The SDK patchset is untouched**: these are app-level changes; the
  esp-matter checkout stays at pin 21aa3d1 with both patchsets applied
  (`scripts/apply-sdk-patches.sh --check` must still say "applied").
- **Every config field path must be verified against the pinned SDK
  headers before use** (~/esp/esp-matter/components/esp_matter/data_model/
  esp_matter_endpoint.h, esp_matter_cluster.h, esp_matter_feature.h);
  the plan's field names come from a read of those headers but the
  implementer re-verifies and quotes the header lines in the report.
  arduino-esp32's .cpp files are NOT config templates (1.4.1 shapes).
- All three images build green at every task boundary, from the repo
  root, both SDKs sourced (`source ~/esp/esp-idf-v5.4.1/export.sh &&
  source ~/esp/esp-matter/export.sh`, IDF v5.4.1 NOT 5.5.4):
  `idf.py -B build_b4 build`; build_thread and build_combined with their
  SDKCONFIG redirects exactly as in TESTING.md/ARCHITECTURE.md.
- Host tests keep passing: `make -C test/host run` (the codec and
  transport tests; no changes expected).
- Hardware only in Task F3.
- Commit messages explain why and end with exactly:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task F1: The 13 rows, the level-null helper, and the retrofits

**Files:**
- Modify: `main/mt_devtypes.cpp` (all changes in this one file)

**Interfaces:**
- Consumes: `mt_startup_on_off_null(on_off_with_lighting_config *)` at
  main/mt_devtypes.cpp:40-43; the `s_devtypes[]` table at :73-78.
- Produces: `mt_devtype_is_known()`/`mt_devtype_create()` unchanged in
  signature, now covering 17 IDs.

- [ ] **Step 1: verify the config shapes in the pinned headers.** For
each of the 13 namespaces below, read its `config_t` in
esp_matter_endpoint.h (base structs around lines 182-213, namespaces at
the lines noted) and confirm the field paths this plan uses; quote each
confirmed path in the report. Namespaces and header anchors:
on_off_plug_in_unit :386, dimmable_plug_in_unit :400, contact_sensor
:613, occupancy_sensor :599, humidity_sensor :585, pressure_sensor :641,
rain_sensor :805, water_freeze_detector :791, water_leak_detector :777,
fan :412, window_covering :557, thermostat :426, extended_color_light
:314.

- [ ] **Step 2: add the level-null helper and retrofit the existing
rows.** Below `mt_startup_on_off_null()`:

```cpp
/*
 * StartUpCurrentLevel has the same B63 shape as StartUpOnOff: esp-matter
 * defaults it to 0 (esp_matter_feature.h:256-257), which tells CHIP's
 * LevelControl to force level 0 at every boot and persist it, so a
 * stored brightness never survives a reboot. Null means "previous
 * value". Found while adding the plug-in units, whose upstream class
 * nulls both fields for exactly this reason.
 */
static void mt_startup_level_null(dimmable_light::config_t *c)
{
    c->level_control_lighting.start_up_current_level = nullable<uint8_t>();
}
```

NOTE the parameter type: verify what struct actually carries
`level_control_lighting` in the pinned header (the dimmable base config;
dimmable_plug_in_unit and extended_color_light configs derive from or
embed the same shape; if they are distinct types, make the helper a
small template or overloads, matching whichever is cleanest against the
real declarations; record the choice). Then call it from
`mk_dimmable_light` and `mk_color_temperature_light` (both carry the
field; verify for color_temperature_light and skip with a comment if its
config genuinely lacks it).

- [ ] **Step 3: the nine clean thunks.** Same shape as
`mk_temperature_sensor` (default config, create, return), one per
namespace, plus the two plug-in units which call the null helpers:

```cpp
static endpoint_t *mk_on_off_plug_in_unit(node_t *n)
{
    on_off_plug_in_unit::config_t c;
    mt_startup_on_off_null(&c);
    return on_off_plug_in_unit::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_dimmable_plug_in_unit(node_t *n)
{
    dimmable_plug_in_unit::config_t c;
    mt_startup_on_off_null(&c);
    mt_startup_level_null(&c);   /* adjust per Step 2's helper typing */
    return dimmable_plug_in_unit::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_contact_sensor(node_t *n)
{
    contact_sensor::config_t c;
    return contact_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}
```

and identically-shaped `mk_humidity_sensor`, `mk_pressure_sensor`,
`mk_rain_sensor`, `mk_water_freeze_detector`, `mk_water_leak_detector`,
`mk_fan` (default configs, no tweaks). For `mk_occupancy_sensor`:

```cpp
static endpoint_t *mk_occupancy_sensor(node_t *n)
{
    occupancy_sensor::config_t c;
    /* The sensor-type feature must be selected explicitly; 1.5.1 calls
     * the field feature_flags (1.4.1 called it features). PIR is the
     * default upstream presents. */
    c.occupancy_sensing.feature_flags =
        cluster::occupancy_sensing::feature::passive_infrared::get_id();
    return occupancy_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}
```

(Verify the exact feature namespace path in esp_matter_feature.h and
quote it.)

- [ ] **Step 4: the two abort-trap thunks.** These MUST set
feature_flags before create() or cluster creation calls
ABORT_CLUSTER_CREATE (assert) and the device dies at boot
(esp_matter_cluster.cpp:2137 for window covering lift/tilt,
:1445 for thermostat heat/cool):

```cpp
static endpoint_t *mk_window_covering(node_t *n)
{
    window_covering::config_t c;
    /* Default feature_flags of 0 ASSERTS in cluster create: at least
     * one of Lift/Tilt is mandatory. Enable both, position-aware, which
     * is the surface the host library's percent100ths API drives. */
    c.window_covering.feature_flags =
        cluster::window_covering::feature::lift::get_id() |
        cluster::window_covering::feature::tilt::get_id() |
        cluster::window_covering::feature::position_aware_lift::get_id() |
        cluster::window_covering::feature::position_aware_tilt::get_id();
    return window_covering::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_thermostat(node_t *n)
{
    thermostat::config_t c;
    /* Default feature_flags of 0 ASSERTS in cluster create: at least
     * one of Heat/Cool is mandatory. Enable both; the setpoint feature
     * configs default to 2000/2600 (20 C / 26 C hundredths). */
    c.thermostat.feature_flags =
        cluster::thermostat::feature::heating::get_id() |
        cluster::thermostat::feature::cooling::get_id();
    return thermostat::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}
```

(Verify: whether position_aware_lift/tilt are separate feature bits or
config sub-structs (`features.position_aware_lift`), and whether the
thermostat setpoint configs need explicit population or ride along with
the feature bits; follow the header, quote it, and if the validation
macro demands a different arrangement, match the header reality and
record the delta from this sketch.)

- [ ] **Step 5: the extended color light thunk with the hue/sat
bolt-on** (user decision: upstream's HSV API must work):

```cpp
static endpoint_t *mk_extended_color_light(node_t *n)
{
    extended_color_light::config_t c;
    mt_startup_on_off_null(&c);
    mt_startup_level_null(&c);   /* adjust per Step 2's helper typing */
    endpoint_t *ep = extended_color_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    /* The standard namespace enables color-temp and XY only. The host
     * library mirrors arduino-esp32's HSV-driven class, so CurrentHue
     * and CurrentSaturation must exist: bolt the HueSaturation feature
     * onto the color control cluster (plain u8 attributes, fully inside
     * the generic AT+MTATTR surface). */
    cluster_t *cc = cluster::get(ep, chip::app::Clusters::ColorControl::Id);
    if (cc != nullptr) {
        cluster::color_control::feature::hue_saturation::config_t hs;
        cluster::color_control::feature::hue_saturation::add(cc, &hs);
    }
    return ep;
}
```

(Verify the feature add() signature and config type in
esp_matter_feature.h; if add() takes no config, drop the struct. If the
cluster get() needs the esp_matter cluster-id constant instead of the
CHIP one, match what main.cpp already does for cluster lookups.)

- [ ] **Step 6: the 13 table rows** appended to `s_devtypes[]`, same
formatting, names matching the namespace strings (e.g.
"on_off_plug_in_unit", "window_covering", "extended_color_light").

- [ ] **Step 7: build all three images green; host tests green.** Run
`scripts/apply-sdk-patches.sh --check` first (expect "applied" x2, exit
0). Then the three builds per Global Constraints and
`make -C test/host run`.

- [ ] **Step 8: commit**

```bash
git add main/mt_devtypes.cpp
git commit -m "feat: 13 new device types, and the StartUpCurrentLevel retrofit"
```

(Body: the two assert traps, the bolt-on, the retrofit rationale;
trailers.)

### Task F2: Spec table, ARCHITECTURE note, +MTVER to 0.3.0

**Files:**
- Modify: `docs/AT_MT_SPEC.md` (section 3.9 area: the supported
  device-types table)
- Modify: `docs/ARCHITECTURE.md` (device-type section: 17 rows note, the
  retrofit, the bolt-on divergence)
- Modify: wherever the +MTVER string lives (grep for it: likely a
  #define in main/ or set from the project version; find the single
  source, do not invent a second)

**Interfaces:** none new.

- [ ] **Step 1: find the +MTVER source.** `grep -rn "MTVER\|0\\.1\\.0" main/` and
read how `AT+MTVER?` answers (the C4-era library query saw "0.1.0").
Change the single source to `0.3.0` (0.2.x was the transport contract,
already tagged; the devtype expansion is the next minor). Record in the
report where it lives.
- [ ] **Step 2: AT_MT_SPEC.md.** Add/extend the supported-device-types
table under the AT+MTEP section: 17 rows, columns ID (hex) and name,
with one sentence above it: IDs outside the table answer +MTERR:6; the
table is the firmware's mt_devtypes.cpp table and grows by rows. Note
the hue/saturation addition on extended_color_light (hosts see
CurrentHue/CurrentSaturation alongside XY and mireds, unlike stock
esp-matter). Match the spec's voice and table style.
- [ ] **Step 3: ARCHITECTURE.md.** One paragraph in the device-type/
composition area: the 13-row expansion, the two assert traps and why
the thunks own feature_flags (a host must not be able to brick the boot
by declaring a thermostat), the StartUpCurrentLevel retrofit (B63
sibling), the bolt-on. Reference the design spec by path.
- [ ] **Step 4: rebuild build_b4 only** (version string change), host
tests, commit (`docs: the 17-type table, and +MTVER finally tells the
truth` + body noting I94 closure + trailers).

### Task F3: Bench verification (operator present)

**Files:** none committed (report only; fixture/baseline changes are a
STOP-and-report event, they are not expected).

**Interfaces:** consumes the flashed build_b4 from F1/F2.

- [ ] **Step 1: flash** the rebuilt build_b4
(`python3 fw/flash.py --build-dir build_b4 --port /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_83C29C7F51EC55B4-if00 --bridge espnow`),
wait +MTREADY, `AT+MTVER?` must answer 0.3.0, AT+MTFRESET to
factory-fresh.
- [ ] **Step 2: the risk composition.** Probe script (harness helpers):
AT+MTEPCLEAR; AT+MTEP= each of dimmable_plug_in_unit 0x010B, thermostat
0x0301, window_covering 0x0202, extended_color_light 0x010D,
contact_sensor 0x0015 (IDs for the probe only; firmware matched them
from the SDK); AT+MTEPAPPLY; after the reboot, AT+MTEP? lists all five
with stable endpoint ids. THE POINT: the boot rebuild survives the two
abort-prone types. Console capture must show five "created ... as
endpoint" lines and no assert/abort.
- [ ] **Step 3: per-type attribute round-trips** over AT+MTATTR (write
mode 1 where writable, read back): OnOff bool + CurrentLevel u8 on the
plug-in unit; SystemMode u8 + OccupiedHeatingSetpoint i16 on the
thermostat; TargetPositionLiftPercent100ths u16 on the covering;
CurrentHue u8 + CurrentSaturation u8 + ColorTemperatureMireds u16 on the
light (proves the bolt-on); StateValue bool read on the contact sensor.
Unknown-type check: AT+MTEP=0x9999 answers +MTERR:6.
- [ ] **Step 4: the retrofit pin (B63 sibling).** Compose a plain
dimmable_light, write CurrentLevel=137 mode 1, reboot via SWD reset (NOT
AT+MTRESET: that is the Matter reset), read CurrentLevel back: must be
137. Repeat once cold (operator power cycle if convenient, else note
warm-only).
- [ ] **Step 5: one full regression run** (WiFi mode, full flags per
TESTING.md): must stay 89/89 PASS with the existing baseline (the new
rows change nothing the suite exercises).
- [ ] **Step 6: restore** the standard bench composition (AT+MTFRESET,
AT+MTEP=0x0100, AT+MTEPAPPLY), report with all evidence.
