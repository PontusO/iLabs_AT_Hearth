# Ten-Type Swoop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The ten remaining mechanical Matter 1.5.1 device types: firmware
rows 21-30 and ten Hearth-original library classes, version 0.4.0 both
repos.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-06-ten-type-swoop-design.md`), whose
section 2 dossier is binding. No new mechanisms: thunk table + generic
AT+MTATTR firmware-side, the established MatterEndPoint recipe
library-side. One firmware task, three library class tasks grouped by
recipe family, integration, bench.

**Tech Stack:** esp-matter 21aa3d1 pinned, C/C++ firmware, MockStream
host tests on arduino-pico.

## Global Constraints

- **No em dashes** anywhere. Device type IDs in firmware always via
  `<ns>::get_device_type_id()`, never literals; every SDK name verified
  against the pinned headers with quotes in the report. The library
  transcribes protocol constants WITH quoted header evidence (its
  established convention).
- The ten types and IDs, verbatim from the spec: Light Sensor 0x0106,
  Flow Sensor 0x0306, Air Quality Sensor 0x002C, Mounted On/Off Control
  0x010F, Mounted Dimmable Load Control 0x0110, Air Purifier 0x002D,
  Extractor Hood 0x007A, Room Air Conditioner 0x0072, Cooktop 0x0078,
  Pump 0x0303. All max_variant 0.
- Pump is abort trap number six: pump_configuration_and_control runs
  VALIDATE_FEATURES_AT_LEAST_ONE across its operation-mode features
  (esp_matter_cluster.cpp:2675-2677); the thunk sets the constant_speed
  feature; trap comment in house convention citing that line.
- Room Air Conditioner's thermostat trap is pre-satisfied by esp-matter
  (endpoint.cpp ORs in cooling + dead_front before create); the thunk
  must NOT re-add feature flags, only seed setpoints like the existing
  thermostat row.
- The mounted controls null StartUpOnOff (both) and StartUpCurrentLevel
  (dimmable) via the existing mt_startup_* helpers (B63 discipline).
- Kconfig: lift CONFIG_SUPPORT_AIR_QUALITY_CLUSTER=n from
  sdkconfig.defaults; any further =n the linker demands is lifted with
  evidence per the seven-cluster precedent.
- MT_FW_VERSION "0.4.0"; library.properties version=0.4.0. The library
  parity table stays 20/20; the ten new classes go in the Hearth
  originals section.
- All three firmware images green at firmware task end; patch-check
  applied x2 around builds; full host suites green in the touched repo
  at every task boundary.
- Library work on branch `swoop` in
  /home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth (Dropbox: in
  place, never symlink). TDD with genuine red excerpts; decimal IDs in
  expect() strings; per-task .gitignore lines; no binaries in commits;
  NEW commits never --amend; 2-space /* */ style.
- Commit messages explain why and end exactly with:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task C1: Firmware rows 21-30, docs, 0.4.0

**Files:**
- Modify: `main/mt_devtypes.cpp` (ten thunks + ten rows),
  `sdkconfig.defaults` (AIR_QUALITY lift + linker-demanded lifts),
  `docs/AT_MT_SPEC.md` (3.9 table to 30 rows),
  `docs/ARCHITECTURE.md` (one paragraph: census verdict classes and the
  tier 2/3 ledger, referencing the spec's section 6),
  `README.md` (device table to 30), `main/include/mt_at_config.h`
  (MT_FW_VERSION "0.4.0")

**Interfaces:**
- Produces: ten table rows consumed by the existing
  `mt_devtype_is_known/variant_ok/create` machinery unchanged; no
  header changes.

- [ ] **Step 1: the ten thunks.** Sensor/fan/plug-in clones copy the
existing sibling thunk verbatim with the namespace swapped
(light_sensor, flow_sensor, air_quality_sensor, air_purifier,
extractor_hood, mounted_on_off_control with mt_startup_on_off_null,
mounted_dimmable_load_control with both null helpers, cooktop plain).
Room air conditioner:

```cpp
static endpoint_t *mk_room_air_conditioner(node_t *n, uint8_t variant)
{
    (void)variant;
    room_air_conditioner::config_t c;
    /* The thermostat trap (VALIDATE_FEATURES_AT_LEAST_ONE Heat,Cool,
     * esp_matter_cluster.cpp:1445) is pre-satisfied here: esp-matter's
     * own endpoint add() ORs in feature::cooling and on_off
     * dead_front_behavior before create (esp_matter_endpoint.cpp:1279-1287).
     * Do not re-add flags; seed the cooling setpoint like the
     * thermostat row. */
    c.thermostat.occupied_cooling_setpoint = 2600;
    return room_air_conditioner::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}
```

(Verify the config member path against esp_matter_endpoint.h before
use; if the config struct nests differently, follow the header and
quote it.) Pump:

```cpp
static endpoint_t *mk_pump(node_t *n, uint8_t variant)
{
    (void)variant;
    pump::config_t c;
    /* Sixth abort trap: pump_configuration_and_control::create() runs
     * VALIDATE_FEATURES_AT_LEAST_ONE across the operation-mode
     * features (esp_matter_cluster.cpp:2675-2677). Constant speed is
     * the least-constrained mode. */
    c.pump_configuration_and_control.feature_flags =
        cluster::pump_configuration_and_control::feature::constant_speed::get_id();
    return pump::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}
```

(Verify the feature namespace in esp_matter_feature.h; constant_speed
config may carry a struct: follow the header, quote lines in the
report.)
- [ ] **Step 2: ten rows appended** to s_devtypes, names matching the
namespaces, max_variant 0, IDs via get_device_type_id().
- [ ] **Step 3: Kconfig.** Remove CONFIG_SUPPORT_AIR_QUALITY_CLUSTER=n;
build; lift any further =n the linker names (undefined
PluginServerInitCallback references), recording each with the linker
line in the report.
- [ ] **Step 4: docs + version.** 3.9 and README tables to 30 rows
(IDs and names only, matching the existing format); ARCHITECTURE
paragraph; MT_FW_VERSION "0.4.0"; strings check on build_b4.
- [ ] **Step 5:** all three images green, patch-check x2, host suite
green, commit.

### Task C2: Library sensor classes (3)

**Files (branch swoop):**
- Create: `src/MatterEndpoints/MatterLightSensor.{h,cpp}`,
  `MatterFlowSensor.{h,cpp}`, `MatterAirQualitySensor.{h,cpp}`
- Test: `test/host/test_lightsensor.cpp`, `test_flowsensor.cpp`,
  `test_airquality.cpp`; `test/host/Makefile` + `.gitignore`

**Interfaces:**
- Produces (public API per class; model on MatterPressureSensor, the
  nearest sibling; verify wire IDs against the pinned CHIP headers and
  quote them):

```cpp
/* MatterLightSensor: IlluminanceMeasurement cluster 0x0400 (1024),
   MeasuredValue 0x0000, nullable u16 */
bool begin(uint16_t rawValue = 0);
bool setRawMeasuredValue(uint16_t v);   /* wire write, cache on OK */
uint16_t getRawMeasuredValue();
/* upstream-style convenience: lux = 10^((raw-1)/10000) documented,
   raw passthrough is the API; no float wire traffic */

/* MatterFlowSensor: FlowMeasurement 0x0404 (1028), MeasuredValue 0 */
bool begin(uint16_t rawValue = 0);
bool setRawMeasuredValue(uint16_t v);
uint16_t getRawMeasuredValue();

/* MatterAirQualitySensor: AirQuality 0x005B (91), AirQuality attr 0,
   enum8 */
enum AirQuality_t : uint8_t { kUnknown = 0, kGood, kFair, kModerate,
                              kPoor, kVeryPoor, kExtremelyPoor };
bool begin(AirQuality_t q = kUnknown);
bool setAirQuality(AirQuality_t q);
AirQuality_t getAirQuality();
```

- [ ] **Step 1: tests first** per class at the recipe minimum: begin
declares the right devtype (0x0106/0x0306/0x002C variant 0); set
methods pin exact decimal wire strings (e.g.
`AT+MTATTR=1,1024,0,5000,1`); failed write leaves cache; controller
URC dispatches attributeChangeCB and updates the getter; re-begin
refused. Genuine red, implement, green.
- [ ] **Step 2:** full suite green, commit(s).

### Task C3: Library actuator clones (5)

**Files (branch swoop):**
- Create: `src/MatterEndpoints/MatterMountedOnOffControl.{h,cpp}`,
  `MatterMountedDimmableLoadControl.{h,cpp}`, `MatterAirPurifier.{h,cpp}`,
  `MatterExtractorHood.{h,cpp}`, `MatterCooktop.{h,cpp}`
- Test: one test binary per class; Makefile + .gitignore

**Interfaces:**
- Produces: MatterMountedOnOffControl mirrors MatterOnOffPlugin's
  surface (begin(bool)/setOnOff/getOnOff/toggle) on devtype 0x010F;
  MatterMountedDimmableLoadControl mirrors MatterDimmablePlugin
  (adds setBrightness/getBrightness) on 0x0110; MatterAirPurifier and
  MatterExtractorHood mirror MatterFan (fan mode enum + speed percent)
  on 0x002D/0x007A; MatterCooktop:

```cpp
/* OffOnly semantics: remote ON is not part of the device class. */
bool begin();               /* declares 0x0078; device boots Off-capable */
bool off();                 /* the one remote action, OnOff write 0 */
bool getOnOff();            /* cache fed by URCs (local turn-on shows here) */
```

- [ ] **Step 1: tests first** per class: declaration pins, exact wire
strings decimal (OnOff cluster 6, LevelControl 8, FanControl 514),
cache discipline, URC dispatch, re-begin refusal; Cooktop additionally
pins that no method emits an OnOff-write-1 wire line (the class has no
on(); assert unexpected().empty() after exercising the full API).
Genuine red, implement, green.
- [ ] **Step 2:** full suite green, commit(s).

### Task C4: Library MatterRoomAirConditioner + MatterPump

**Files (branch swoop):**
- Create: `src/MatterEndpoints/MatterRoomAirConditioner.{h,cpp}`,
  `MatterPump.{h,cpp}`
- Test: `test/host/test_roomac.cpp`, `test_pump.cpp`; Makefile +
  .gitignore

**Interfaces:**
- Produces:

```cpp
/* MatterRoomAirConditioner: OnOff 6 + Thermostat 513 (LocalTemperature 0,
   OccupiedCoolingSetpoint 17, OccupiedHeatingSetpoint 18, SystemMode 28;
   verify + quote). DeadFront: setOnOff(false) dead-fronts the thermostat
   per spec; document, no special code. */
bool begin(bool on = false);
bool setOnOff(bool on);  bool getOnOff();
bool setLocalTemperature(double c);      /* i16 hundredths */
bool setCoolingSetpoint(double c);  double getCoolingSetpoint();
bool setHeatingSetpoint(double c);  double getHeatingSetpoint();
bool setMode(uint8_t systemMode);   uint8_t getMode();

/* MatterPump: OnOff 6 + PumpConfigurationAndControl 512. Writable:
   OperationMode 32 (enum8). Read-only telemetry the SKETCH publishes
   as the device: MaxPressure 0, MaxSpeed 1, MaxFlow 2 (i16/u16,
   nullable); EffectiveOperationMode 17 / EffectiveControlMode 18 are
   device-answered reads surfaced as getters fed by URCs. Verify every
   ID + type against the CHIP header and quote. */
bool begin(bool on = false);
bool setOnOff(bool on);  bool getOnOff();
bool setOperationMode(uint8_t m);  uint8_t getOperationMode();
bool setMaxPressure(int16_t v); bool setMaxSpeed(uint16_t v); bool setMaxFlow(uint16_t v);
uint8_t getEffectiveOperationMode(); uint8_t getEffectiveControlMode();
```

- [ ] **Step 1: tests first**: recipe minimum per class plus: RoomAC
pins the dead-front documentation contract only via API (setOnOff(false)
is an ordinary OnOff write on the wire, exact string pinned); Pump pins
OperationMode write + one telemetry write + EffectiveOperationMode URC
feeding the getter. Genuine red, implement, green.
- [ ] **Step 2:** full suite green, commit(s).

### Task C5: Library integration and 0.4.0

**Files:** `src/Hearth.h` umbrella (+10 includes),
`test/host/test_matter_umbrella.cpp` + Makefile, `keywords.txt`
(10 classes + public methods per existing granularity), `README.md`
(Hearth originals section grows by ten one-line surfaces; parity table
untouched at 20/20), `library.properties` (0.4.0),
`examples/HearthSensorsAndAppliances/HearthSensorsAndAppliances.ino`
(one Hearth-original example composing a representative subset: air
quality sensor + pump + room AC, CDC-driven like MatterDoorLockAdjudicated;
compile for pico:rp2040:challenger_2350_wifi6_ble5, kill discovery
daemons after, verify with pgrep).

- [ ] Umbrella + umbrella test + keywords + README + version + example
compile; full suite green; commit.

### Task C6: Bench verification (no unplugs)

- [ ] Flash rebuilt build_b4 (0.4.0), factory-fresh; compose the risk
set: 0x0303, 0x0072, 0x0078, 0x002C, 0x010F, 0x0100; +MTEP? exact;
boot rebuild survives (pump trap and pre-satisfied AC trap live).
- [ ] Attribute round trips per new type over AT (decimal cluster IDs:
1024/1028/91/6/8/514/513/512 as applicable); +MTERR:6 still fires for
an unknown ID; MTTEMPLEVELS and +MTCMD regression pins stay green
(compose a cabinet or lock only if needed for those pins; otherwise
grammar-level pins suffice).
- [ ] Commission; chip-tool reads AirQuality and pump attributes;
controller writes SystemMode/OccupiedCoolingSetpoint on the AC and the
host observes URCs.
- [ ] Library smoke: the C5 example driving pump + room AC + air
quality against the bench device; verdicts n/a (no adjudicated types
here), poll() loop per house rules.
- [ ] Restore bench (factory-fresh, 0x0100, espnow bridge), report
with verbatim evidence.
