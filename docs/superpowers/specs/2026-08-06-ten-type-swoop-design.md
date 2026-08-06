# Ten-type swoop design: the mechanical remainder of Matter 1.5.1

Date: 2026-08-06. Adds the ten remaining device types that need no new
mechanism: every one rides the existing thunk table, the generic
AT+MTATTR surface, and (for the library) the established MatterEndPoint
class recipe. Grounded in a full census of esp-matter 21aa3d1's 70
endpoint namespaces crossed with the 1.5.1 Device Library Specification
(matter/23-27351-009, local only, gitignored). Survey record: graph
D132.

## 1. Decisions taken with the user (2026-08-06)

- Scope: exactly the ten mechanical types below; firmware rows AND ten
  library classes in one round (the 13-type round's proven shape).
- All ten library classes are Hearth originals (upstream arduino-esp32
  parity was completed at 20/20; its API conventions still guide the
  shapes, but the surfaces are ours).
- Tier 2 recorded, not built: smoke/CO alarm (+MTCMD SelfTest + the
  Power Source device mandate), Mode Select (label-manager transport in
  the MTTEMPLEVELS mold), Water Valve (delegate forwarding through
  +MTCMD). Tier 3: washer/dishwasher/dryer behind one shared
  OperationalState delegate. Composition-tree types (refrigerator,
  oven, irrigation, cook surface) blocked on a parent-child AT+MTEP
  capability, a future round of its own.

## 2. The ten types (SDK census + spec crosscheck, both verified)

Spec-side: each type's MANDATORY server clusters are exactly what
esp-matter's endpoint create()/add() produces; none is provisional in
1.5.1; all are legal standalone endpoints.

| # | Type | ID | Thunk shape | Firmware notes |
|---|---|---|---|---|
| 1 | Light Sensor | 0x0106 | plain | identify + illuminance_measurement; sensor clone |
| 2 | Flow Sensor | 0x0306 | plain | identify + flow_measurement; sensor clone |
| 3 | Air Quality Sensor | 0x002C | plain | identify + air_quality (AirQuality enum); Kconfig lift CONFIG_SUPPORT_AIR_QUALITY_CLUSTER (sdkconfig.defaults:113, seven-cluster precedent) |
| 4 | Mounted On/Off Control | 0x010F | plain | cluster set byte-identical to on_off_plug_in_unit; null StartUpOnOff (B63 discipline) |
| 5 | Mounted Dimmable Load Control | 0x0110 | plain | config_t aliases dimmable_light's; null both start-up fields (B63) |
| 6 | Air Purifier | 0x002D | plain | identify + fan_control; the fan row's shape |
| 7 | Extractor Hood | 0x007A | plain | fan_control only; spec disallows Rocking/Wind/AirflowDirection features, which the default config does not enable (verify at implementation) |
| 8 | Room Air Conditioner | 0x0072 | pre-satisfied | esp-matter's endpoint.cpp ORs in thermostat cooling + on_off dead_front before create (cpp:1279-1287): the thermostat trap is satisfied by the SDK, not our thunk; seed setpoints like the thermostat row |
| 9 | Cooktop | 0x0078 | plain | on_off + feature::off_only::add() done by endpoint add() itself; standalone legal (spec: cook surfaces MAY be zero) |
| 10 | Pump | 0x0303 | feature-flag | pump_configuration_and_control has VALIDATE_FEATURES_AT_LEAST_ONE (esp_matter_cluster.cpp:2675-2677) across constant-pressure/-flow/-speed/proportional-pressure: the thunk sets constant_speed as the default operation mode (the least-constrained choice; the trap comment follows house convention as trap number six) |

All attribute surfaces are integers/bools/enums served by AT+MTATTR
unchanged. No type has a mandatory delegate; fan_control's optional
delegate stays null exactly as the existing fan row runs. No commands
need adjudication; on/off and level resolve autonomously in ember.

Device type IDs are never transcribed: rows call
<ns>::get_device_type_id(); the table grows from 20 to 30 rows;
MT_COMP_MAX_ENDPOINTS 16 is untouched (composition size is a separate
axis from table size).

## 3. Firmware scope

- main/mt_devtypes.cpp: ten thunks + ten rows, max_variant 0
  throughout. Pump's trap comment cites esp_matter_cluster.cpp:2675.
  Mounted rows reuse mt_startup_on_off_null/mt_startup_level_null.
- sdkconfig.defaults: lift CONFIG_SUPPORT_AIR_QUALITY_CLUSTER=n (and
  any other =n the linker demands; report evidence per precedent).
- Docs: AT_MT_SPEC 3.9 table to 30 rows; README table; ARCHITECTURE
  one paragraph (the census verdict classes and the tier 2/3 ledger).
- MT_FW_VERSION "0.4.0" (30 types is a milestone; minor bump).
- Composition v1/v2 codec, MTTEMPLEVELS, +MTCMD: all untouched.

## 4. Library scope (ten Hearth-original classes)

House recipe per class (MatterEndPoint subclass, hearthDeclare in
begin(), cache discipline, attributeChangeCB, re-begin refusal,
MockStream tests both directions, umbrella/keywords/README):

- MatterLightSensor, MatterFlowSensor: the sensor-class recipe
  (MatterPressureSensor is the nearest sibling): begin(), setMeasuredValue
  (raw + double convenience), onChange callback.
- MatterAirQualitySensor: begin(), setAirQuality(enum u8), enum
  constants from the spec (Unknown..ExtremelyPoor), onChange.
- MatterMountedOnOffControl, MatterMountedDimmableLoadControl: clones
  of the plug-in unit classes (onOff/level surface).
- MatterAirPurifier, MatterExtractorHood: the MatterFan surface
  (fan mode + percent) on the new IDs.
- MatterRoomAirConditioner: MatterThermostat's setpoint/mode surface
  plus onOff with DeadFront semantics documented (writing OnOff false
  dead-fronts the thermostat per spec; one README sentence).
- MatterCooktop: onOff with the OffOnly caveat documented: remote ON
  is not part of the device class; off() and state readback only.
- MatterPump: onOff plus the pump attribute surface (setpoints and
  the read-only measured attributes: max pressure/speed/flow,
  EffectiveOperationMode/EffectiveControlMode as read-only getters fed
  by URCs).
- library.properties 0.4.0; README: parity table stays 20/20, the
  Hearth originals section grows by ten with one-line surfaces.

## 5. Verification

- Host: full suites both repos; per-class tests at the recipe minimum
  (wire pins with decimal IDs, cache discipline, URC dispatch,
  re-begin refusal); firmware host tests unchanged (no codec change).
- Bench (one session, no unplugs): flash 0.4.0; compose a risk set
  (pump + room AC + cooktop + air quality + one mounted control:
  the trap, the pre-satisfied trap, OffOnly, the Kconfig lift, and a
  clone) plus 0x0100; boot rebuild survives; attribute round trips per
  type incl. a negative (+MTERR:6 for unknown ID still); commission and
  spot-read AirQuality + pump attributes from chip-tool; library smoke
  for two representative classes (pump, room AC); regression pins for
  MTTEMPLEVELS and +MTCMD stay green; restore.

## 6. Out of scope, recorded (the road past 30)

- Tier 2: smoke/CO alarm (SelfTestRequest via +MTCMD, plus the spec's
  mandatory Power Source device on the node: needs a power_source
  endpoint decision); Mode Select (SupportedModes label manager, an
  AT+MTMODES in the MTTEMPLEVELS mold); Water Valve (Open/Close are
  pure-virtual delegate methods: a delegate that forwards through
  +MTCMD is the natural second consumer of that frame).
- Tier 3: laundry washer, dishwasher, laundry dryer: one shared
  OperationalState delegate unlocks all three; pause/resume/stop
  adjudication would ride +MTCMD.
- Composition trees: refrigerator, oven, irrigation system, cook
  surface: all require parent-child endpoint composition (PartsList
  semantics) that flat AT+MTEP deliberately does not express today.
- HEAVY tier (RVC, microwave, EVSE, energy composites, cameras) and
  the 21 NO-NAMESPACE types: recorded in D132, not planned.
