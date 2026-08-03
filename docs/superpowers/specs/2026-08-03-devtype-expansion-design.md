# Device-type expansion design: 13 new types, firmware and library

Date: 2026-08-03. Extends the C-phase surface (firmware 0.2.0, library
0.2.0) with every arduino-esp32 endpoint class that fits the existing
integer/bool AT attribute surface. The original C-phase design
(2026-07-26, section 12.2) already deferred these as "table rows, not
design work"; this spec is that deferral coming due, with the traps
planning found written down.

## 1. Decisions taken with the user (2026-08-03)

- **Scope: the 13 mechanical types.** The 3 design-heavy classes stay
  parked with recorded reasons: MatterGenericSwitch (Switch cluster
  emits EVENTS, and the AT surface has no event-emission command),
  MatterTemperatureControlledCabinet (needs AT+MTATTRX for the census's
  one array attribute), MatterColorLight (no esp-matter namespace exists
  in either SDK revision).
- **Firmware first**: all 13 table rows land and are bench-verified in
  one pass, then the library classes follow in batches against firmware
  that already accepts everything.
- **Test bar**: full MockStream host tests per library class; hardware
  smoke for a representative subset (plugin, thermostat, window
  covering, extended color light), not all 13.
- **Extended color light ships upstream-compatible HSV**: the firmware
  thunk bolts color_control::feature::hue_saturation onto the standard
  extended_color_light create(), so CurrentHue/CurrentSaturation exist
  alongside XY and mireds and upstream's setColorHSV API works verbatim.
  XY-only was rejected as breaking the parity promise in spirit;
  deferring the class was rejected as parking the most-requested rich
  light type.

## 2. How device support maps across the two layers

Firmware: one row in main/mt_devtypes.cpp per type: the ID always read
from `<ns>::get_device_type_id()` (never transcribed), a thunk calling
`esp_matter::endpoint::<ns>::create()` with a config, and per-type config
corrections where esp-matter defaults are wrong for a persistent device
(the B63 class of problem). AT+MTEP validates IDs against this table
(+MTERR:6 otherwise); AT+MTATTR is fully generic over integer/bool/enum
types and needs no per-type work.

Library: one class over MatterEndPoint per type, mirroring the
arduino-esp32 class API: device type and cluster/attribute ID constants,
setters via updateAttributeVal, controller-change dispatch in
attributeChangeCB, the hearthAttrTypeFor type map, plus a MockStream
host-test file. The only inter-layer contract is the device type ID and
the cluster/attribute IDs.

## 3. The 13 types (verified against esp-matter 21aa3d1 and arduino-esp32 3.3.8)

All namespaces exist in the pinned SDK with no name drift. Upstream
.cpp files are NOT config templates (their bundled esp-matter 1.4.1 has
different config_t shapes); only the cluster/attribute IDs they drive are
the spec for the library classes.

| Upstream class -> namespace (ID) | Attributes driven | Thunk requirements |
|---|---|---|
| MatterOnOffPlugin -> on_off_plug_in_unit (0x010A) | OnOff bool | null start_up_on_off (existing mt_startup_on_off_null; config type matches) |
| MatterDimmablePlugin -> dimmable_plug_in_unit (0x010B) | OnOff bool, CurrentLevel u8 | null start_up_on_off AND level_control_lighting.start_up_current_level (new helper) |
| MatterContactSensor -> contact_sensor (0x0015) | BooleanState bool | none |
| MatterOccupancySensor -> occupancy_sensor (0x0107) | Occupancy u8 | set feature_flags (1.5.1 field name; 1.4.1 called it features) to the PIR feature id. HoldTime/HoldTimeLimits deferred (needs AttributeAccessInterface) |
| MatterHumiditySensor -> humidity_sensor (0x0307) | MeasuredValue u16 | none |
| MatterPressureSensor -> pressure_sensor (0x0305) | MeasuredValue i16 | none (our config field names are measured_value etc, not upstream's pressure_*) |
| MatterRainSensor -> rain_sensor (0x0044) | BooleanState bool | none |
| MatterWaterFreezeDetector -> water_freeze_detector (0x0041) | BooleanState bool | none |
| MatterWaterLeakDetector -> water_leak_detector (0x0043) | BooleanState bool | none |
| MatterFan -> fan (0x002B) | FanMode u8, PercentSetting u8, PercentCurrent u8 | none (all four fan_control attributes are unconditional) |
| MatterWindowCovering -> window_covering (0x0202) | Type/ConfigStatus/OperationalStatus u8, lift and tilt Percent100ths u16 | MUST set feature_flags = lift|tilt and features.position_aware_lift/tilt: the default config ASSERTS in cluster create (VALIDATE_FEATURES_AT_LEAST_ONE, esp_matter_cluster.cpp:2137 -> ABORT_CLUSTER_CREATE). Absolute-position attributes do not exist in 1.5.1 at all |
| MatterThermostat -> thermostat (0x0301) | SystemMode u8, LocalTemperature i16, OccupiedHeatingSetpoint i16, OccupiedCoolingSetpoint i16 | MUST set feature_flags = heating|cooling plus the two setpoint feature configs (defaults 2000/2600): default config ASSERTS (esp_matter_cluster.cpp:1445) |
| MatterEnhancedColorLight -> extended_color_light (0x010D) | OnOff bool, CurrentLevel u8, CurrentHue u8, CurrentSaturation u8, ColorTemperatureMireds u16 | standard create() enables color-temp+XY only; additionally call color_control::feature::hue_saturation::add(); null both start_up_* fields |

**Retrofit found during planning**: the existing dimmable_light and
color_temperature_light rows null start_up_on_off but NOT
level_control_lighting.start_up_current_level (default 0), the exact B63
shape for brightness: the device would boot to level 0 and persist it
over a healthy restore. Fix in the same firmware task, with a bench pin.

The two ASSERT traps matter doubly here: the boot rebuild deliberately
aborts the whole composition on any endpoint::create() failure, and a
cluster-create assert is even harsher (device abort). A host that
declares a thermostat must never be able to brick the boot; the thunk
owning correct feature_flags is what guarantees it.

## 4. Firmware stage (this repo)

- mt_devtypes.cpp: 13 rows, mt_startup_level_null() helper, the
  retrofits, the three feature-flag thunks. IDs via get_device_type_id().
  App-level only: the SDK patchset is untouched; all three images build.
- Docs: AT_MT_SPEC.md gains a supported-device-types table (17 rows with
  IDs and the +MTERR:6 rule); ARCHITECTURE.md note.
- Version: the firmware version string becomes 0.3.0 so +MTVER reports
  the contract (closes the parked I94: it still says 0.1.0).
- Bench: one pass proving the risk set end to end: a composition of
  dimmable_plug_in_unit + thermostat + window_covering +
  extended_color_light + a boolean sensor rebuilds across reboot (the
  abort-prone types are the point), per-type AT+MTATTR round-trips
  including CurrentHue (proves the bolt-on), +MTERR:6 still fires for an
  unknown ID, the start_up_current_level retrofit pin (dimmable level
  survives reboot), and one full regression run staying 89/89.

## 5. Library stage (iLabs_Hearth repo, new branch)

Batches: plugins; boolean sensors (contact/rain/freeze/leak);
measurement sensors (humidity/pressure/occupancy); fan + window
covering; thermostat + extended color light; then integration (umbrella
header, keywords.txt, README table: 17 supported and 3 parked with
reasons, byte-identical upstream examples for all 13, version 0.3.0,
final review, representative hardware smoke).

Class-specific notes:
- WindowCovering mirrors upstream's API; the percent100ths methods are
  live, the absolute-position methods return false and say why in their
  doc comments (the attributes do not exist in esp-matter 1.5.1).
- Occupancy ships the Occupancy attribute only; HoldTime methods return
  false, documented deferred.
- ExtendedColorLight needs upstream's espRgbColor_t/espHsvColor_t
  color-conversion helpers: port the minimal functions with matching
  signatures into the library (HearthCompat or a small util file),
  since arduino-pico has no ESP32 color utils.

## 6. Verification

Host suites green in both repos at every task boundary. Bench: the
firmware pass in section 4; the library smoke in section 5 (plugin,
thermostat, window covering, extended color light driven from sketches
against the new firmware; representative upstream examples compile
byte-identical).

## 7. Out of scope, recorded (each gets a parked graph node)

- MatterGenericSwitch: needs an AT event-emission surface (design work).
- MatterTemperatureControlledCabinet: needs AT+MTATTRX (array attribute).
- MatterColorLight: no esp-matter namespace in 1.4.1 or 1.5.1.
- Occupancy HoldTime/HoldTimeLimits: AttributeAccessInterface territory.
- Window-covering absolute position: absent from esp-matter 1.5.1.
