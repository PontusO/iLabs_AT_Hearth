# Device-Type Expansion, Stage L (Library) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the 13 new endpoint classes to the iLabs_Hearth arduino-pico
library, mirroring arduino-esp32's API over the AT link, with full
MockStream host tests, against firmware 0.3.0 which already accepts all
17 device types.

**Architecture:** Each class extends `MatterEndPoint` exactly like the
existing four: declare-only `begin()` (no AT traffic until reconcile),
setters via `updateAttributeVal`, controller-change dispatch in
`attributeChangeCB` with no write-back, `hearthAttrTypeFor` type map.
The upstream arduino-esp32 3.3.8 class headers define the API surface to
mirror; upstream .cpp files are read for API and cluster/attribute IDs
only (their esp-matter internals do not apply here).

**Tech Stack:** C++ (Arduino), MockStream host tests, no hardware until L6.

## Global Constraints

- **Work in the LIBRARY repo**: /home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth,
  branch `devtypes-batch` created from main. Dropbox-synced: edit in
  place, never symlink. Plan and specs live in the firmware repo.
- **No em dashes** anywhere. 2-space indent, /* */ comments, camelCase.
- **Parity**: upstream API mirrored method-for-method from
  ~/.arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/Matter/src/MatterEndpoints/<Class>.h;
  where a method cannot work on this stack (noted per task) it exists,
  returns false, and its doc comment says why. src/Matter.h stays a pure
  shim; matterEvent_t untouched.
- **Host tests green at every task boundary**: `make -C test/host run`.
  TDD: the new class's test file first, red (compile failure), then green.
- Cluster/attribute constants are plain integers in an anonymous
  namespace with a comment naming their CHIP identifiers, exactly like
  MatterOnOffLight.cpp:8-15 (no connectedhomeip headers exist on host
  builds). VERIFY every ID against the upstream class's .cpp before use;
  the tables below are the plan's read of upstream, the upstream file is
  authoritative.
- Commits per task, why-messages, ending exactly with:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

## The per-class recipe (referenced by every task)

Every class in Tasks L1-L5 follows this exact procedure; each task
supplies the table of exact values and any deviations.

1. Read the upstream header AND .cpp for the class (path per Global
   Constraints) end to end. Extract: public API (methods, types,
   callback signatures), device type ID, cluster/attribute IDs, and
   which attributes are written vs only read.
2. Write the test file FIRST (`test/host/test_<name>.cpp`), modeled on
   test_onofflight.cpp's structure and conventions (check(), MockStream
   expect/injectURC, main() registration; Makefile line added to the
   binaries list). Minimum cases per class: begin+declare+adopt via the
   reconcile path; each setter sends the right AT+MTATTR write (mode 1)
   and updates the cache only on OK; a controller-driven +MTATTR URC
   dispatches the user callback with the correctly typed value; a failed
   write (ERROR reply) leaves the cache untouched; re-begin-after-
   reconcile refused. Run: compile failure expected (class missing).
3. Write the header in src/MatterEndpoints/, mirroring the upstream
   class declaration (same public methods and operators), with the
   header-top comment naming the upstream mirror path, like
   MatterOnOffLight.h:4-11.
4. Write the .cpp: anonymous-namespace constants; begin() ->
   hearthDeclare + cache init, NO AT traffic; setters build
   esp_matter_attr_val_t via the HearthCompat helpers
   (esp_matter_bool/uint8/uint16/int16) and call updateAttributeVal;
   attributeChangeCB matches endpoint/cluster/attribute, updates cache,
   invokes callback, never writes back; hearthAttrTypeFor returns the
   union tag per (cluster, attribute).
5. `make -C test/host run`: all binaries green.
6. Commit (class + test + Makefile line), why-message, trailers.

Umbrella includes, keywords.txt, README and examples are L6 ONLY: do not
touch them in L1-L5 (avoids five tasks colliding on the same files).

---

### Task L1: The plug-in units

**Files:**
- Create: src/MatterEndpoints/MatterOnOffPlugin.{h,cpp},
  src/MatterEndpoints/MatterDimmablePlugin.{h,cpp}
- Test: test/host/test_onoffplugin.cpp, test/host/test_dimmableplugin.cpp
- Modify: test/host/Makefile (two binary lines)

**Interfaces:** consumes MatterEndPoint (unchanged); produces the two
class names L6 registers.

Apply the recipe twice with:

| Class | Device type | Clusters/attributes | Types |
|---|---|---|---|
| MatterOnOffPlugin | 0x010A | OnOff 0x0006 / OnOff 0x0000 | bool |
| MatterDimmablePlugin | 0x010B | OnOff 0x0006/0x0000 bool; LevelControl 0x0008 / CurrentLevel 0x0000 | bool, u8 |

Deviations: none expected; these mirror MatterOnOffLight and
MatterDimmableLight nearly verbatim (different device type and class
names, same clusters). Do not copy the light classes blindly: mirror the
PLUGIN upstream headers (method names differ, e.g. the plugin API speaks
of On/Off state, not brightness aliases; take upstream's exact surface).

- [ ] Steps 1-6 of the recipe for MatterOnOffPlugin
- [ ] Steps 1-6 of the recipe for MatterDimmablePlugin (one commit for
  the task is fine if both land together; two commits also fine)

### Task L2: The boolean-state sensors

**Files:**
- Create: src/MatterEndpoints/Matter{ContactSensor,RainSensor,WaterFreezeDetector,WaterLeakDetector}.{h,cpp}
- Test: test/host/test_boolsensors.cpp (ONE file covering all four,
  mirroring how test_colortemp_sensor.cpp covers two classes)
- Modify: test/host/Makefile (one binary line)

**Interfaces:** consumes MatterEndPoint; produces four class names.

Apply the recipe four times with:

| Class | Device type | Cluster/attribute | Type |
|---|---|---|---|
| MatterContactSensor | 0x0015 | BooleanState 0x0045 / StateValue 0x0000 | bool |
| MatterRainSensor | 0x0044 | BooleanState 0x0045 / StateValue 0x0000 | bool |
| MatterWaterFreezeDetector | 0x0041 | BooleanState 0x0045 / StateValue 0x0000 | bool |
| MatterWaterLeakDetector | 0x0043 | BooleanState 0x0045 / StateValue 0x0000 | bool |

Deviations: the four upstream classes have slightly different setter
names (e.g. setContact vs setRain-style naming); mirror each exactly. A
shared private base is NOT wanted (upstream has none; four small flat
classes match the parity goal). The single test file exercises each
class's full recipe-step-2 case list.

- [ ] Recipe for all four classes, one commit

### Task L3: The measurement sensors

**Files:**
- Create: src/MatterEndpoints/Matter{HumiditySensor,PressureSensor,OccupancySensor}.{h,cpp}
- Test: test/host/test_measuresensors.cpp (one file, three classes)
- Modify: test/host/Makefile

**Interfaces:** consumes MatterEndPoint; produces three class names.

| Class | Device type | Cluster/attribute | Type |
|---|---|---|---|
| MatterHumiditySensor | 0x0307 | RelativeHumidityMeasurement 0x0405 / MeasuredValue 0x0000 | u16 (0.01 %) |
| MatterPressureSensor | 0x0305 | PressureMeasurement 0x0403 / MeasuredValue 0x0000 | i16 |
| MatterOccupancySensor | 0x0107 | OccupancySensing 0x0406 / Occupancy 0x0000 | u8 bitmap |

Deviations:
- Humidity/Pressure upstream expose double-taking convenience setters
  wrapping the raw integer; mirror both raw and double forms (the double
  form converts exactly as upstream does; no floats cross the AT link).
- Occupancy: the Occupancy attribute only. Upstream's HoldTime /
  HoldTimeLimits API exists in the mirror but returns false with a doc
  comment: the firmware defers those attributes (design spec section 3,
  AttributeAccessInterface territory). Test asserts the false return.

- [ ] Recipe for all three classes, one commit

### Task L4: Fan and window covering

**Files:**
- Create: src/MatterEndpoints/MatterFan.{h,cpp}, src/MatterEndpoints/MatterWindowCovering.{h,cpp}
- Test: test/host/test_fan.cpp, test/host/test_windowcovering.cpp
- Modify: test/host/Makefile

**Interfaces:** consumes MatterEndPoint; produces two class names.

| Class | Device type | Clusters/attributes | Types |
|---|---|---|---|
| MatterFan | 0x002B | FanControl 0x0202: FanMode 0x0000, PercentSetting 0x0002, PercentCurrent 0x0003 | u8, u8, u8 |
| MatterWindowCovering | 0x0202 | WindowCovering 0x0102: Type 0x0000, ConfigStatus 0x0007, OperationalStatus 0x000A, TargetPositionLiftPercent100ths 0x000B, TargetPositionTiltPercent100ths 0x000C, CurrentPositionLiftPercent100ths 0x000E, CurrentPositionTiltPercent100ths 0x000F | u8s and u16s |

Deviations:
- Fan: upstream's FanMode enum values are mirrored as the same enum
  (plain u8 on the wire). The mode/percent coupling logic upstream
  implements host-side (mode changes adjust percent) is mirrored as
  behavior, not just signatures; test it.
- WindowCovering: the percent100ths lift/tilt API is live. Upstream's
  absolute-position API (setLiftPosition, installed open/closed limits,
  NumberOfActuations) EXISTS in the mirror but returns false with doc
  comments: esp-matter 1.5.1 has no such attributes (design spec section
  3). Tests assert both the live percent paths and the false returns.
  Verify each attribute ID against the upstream .cpp; the tilt/current
  IDs in the table are the plan's read and MUST be checked.

- [ ] Recipe for both classes, one commit

### Task L5: Thermostat and extended color light

**Files:**
- Create: src/MatterEndpoints/MatterThermostat.{h,cpp},
  src/MatterEndpoints/MatterEnhancedColorLight.{h,cpp}
- Create if needed: a small color-conversion util (see deviation)
- Test: test/host/test_thermostat.cpp, test/host/test_enhancedcolor.cpp
- Modify: test/host/Makefile

**Interfaces:** consumes MatterEndPoint; produces two class names.

| Class | Device type | Clusters/attributes | Types |
|---|---|---|---|
| MatterThermostat | 0x0301 | Thermostat 0x0201: LocalTemperature 0x0000, OccupiedCoolingSetpoint 0x0011, OccupiedHeatingSetpoint 0x0012, SystemMode 0x001C | i16, i16, i16, u8 |
| MatterEnhancedColorLight | 0x010D | OnOff 0x0006/0x0000 bool; LevelControl 0x0008/0x0000 u8; ColorControl 0x0300: CurrentHue 0x0000, CurrentSaturation 0x0001, ColorTemperatureMireds 0x0007 | u8, u8, u16 |

Deviations:
- Thermostat: temperatures cross the wire as i16 hundredths of a degree;
  upstream's double-taking setters convert exactly as upstream does.
  Upstream mode enum mirrored. LocalTemperature is push-from-sketch (the
  read direction, like the temperature sensor).
- EnhancedColorLight: upstream's setColorHSV/setColorRGB APIs use
  espRgbColor_t/espHsvColor_t and conversion helpers from ESP32 core,
  which arduino-pico lacks. Port the minimal struct definitions and the
  two conversions (RGB<->HSV) upstream uses, with matching names and
  signatures, into a small library-internal header (follow how
  HearthCompat.h hosts other compat definitions; keep the port minimal,
  only what the class calls). CurrentHue/CurrentSaturation exist on the
  wire thanks to the firmware's hue_saturation bolt-on; the class drives
  them as plain u8 writes. Color temperature moves are mireds u16, as in
  the existing color-temp class.

- [ ] Recipe for both classes (plus the color util), one commit

### Task L6: Integration, examples, version, release prep

**Files:**
- Modify: src/Hearth.h (umbrella includes for all 13), keywords.txt
  (KEYWORD1 for 13 classes + any new public enums as LITERAL1),
  README.md (supported table to 17 rows; parked trio section with
  reasons; limitations updates), library.properties (version 0.3.0),
  .gitignore (the new test binaries, per-binary style)
- Create: examples/<Name>/ for each of the 13, copied BYTE-IDENTICAL
  from the upstream examples directories (the parity evidence pattern;
  MatterOccupancySensor uses the basic example only)
- Test: none new; the full suite must stay green

**Interfaces:** consumes all 13 classes from L1-L5.

- [ ] Umbrella includes + keywords + README + version + .gitignore
- [ ] Copy the 13 upstream examples; verify byte-identity with cmp
  against the upstream files; each must COMPILE with arduino-cli for the
  Challenger FQBN (compile-only, no upload)
- [ ] `make -C test/host run` full green; commit
- [ ] Final whole-branch review (most capable model) + one fix wave per
  the SDD process
- [ ] Bench smoke (OPERATOR GATE, coordinate with the user): flash
  firmware build_b4 0.3.0 stays as restored; upload a smoke sketch
  driving MatterOnOffPlugin, MatterThermostat, MatterWindowCovering,
  MatterEnhancedColorLight against a matching composition; verify
  setters reach the device (AT+MTATTR reads via a probe) and a
  controller-side change dispatches callbacks. Merge + tag decisions are
  the user's.
