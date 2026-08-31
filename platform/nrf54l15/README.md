# nRF54L15 platform: Ophelia-IV module with CPico dev bridge

## What this platform is

This is the Nordic sibling of the ESP32-C6 Matter co-processor, running MCUboot with UART serial recovery on a Wurth Ophelia-IV module (nRF54L15). The bootloader is software-installed via SWD, locked against modification, and can always be reached by holding the recovery strap. See `superpowers/specs/2026-08-26-nrf54l15-bootloader-design.md` in the iLabs_Hearth_docs repository for the full design rationale and specification.

Matter-over-Thread with the commissioning core is bench-proven (design:
`superpowers/specs/2026-08-28-nrf54l15-matter-core-design.md` in the same
repository). The device-type catalogue serves the milestone slice plus
catalogue batch 1 (attribute-only): `0x0100` On/Off Light, `0x0101`
Dimmable Light, `0x0302` Temperature Sensor, `0x0015` Contact Sensor,
`0x0107` Occupancy Sensor, `0x0307` Humidity Sensor, `0x0305` Pressure
Sensor, `0x0044` Rain Sensor, `0x0041` Water Freeze Detector, `0x0043`
Water Leak Detector, `0x0106` Light Sensor, `0x0306` Flow Sensor, `0x010A`
On/Off Plug-in Unit, `0x010B` Dimmable Plug-in Unit; and catalogue batch 2
(the server-interaction types): `0x010C` Color Temperature Light, `0x010D`
Extended Color Light, `0x0301` Thermostat, `0x002B` Fan, `0x0202` Window
Covering, `0x002C` Air Quality Sensor; catalogue batch 3 (the
command-verdict types): `0x000A` Door Lock, `0x0042` Water Valve; and
catalogue batch 4 (the appliance and notification types): `0x0011` Power
Source, `0x0027` Mode Select, `0x0073` Laundry Washer, `0x0075`
Dishwasher, `0x007C` Laundry Dryer, `0x0076` Smoke/CO Alarm, `0x0146`
Chime; and catalogue batch 5 (the standalone remainder): `0x002D` Air
Purifier, `0x010F` Mounted On/Off Control, `0x0110` Mounted Dimmable Load
Control, `0x000F` Generic Switch, `0x0303` Pump, `0x0072` Room Air
Conditioner, `0x0074` Robotic Vacuum Cleaner; and catalogue batch 7a (the
energy foundation): `0x0510` Electrical Sensor, `0x0514` Electrical
Meter, `0x0309` Heat Pump, `0x0017` Solar Power, `0x0511` Electrical
Utility Meter, `0x050D` Device Energy Management; and catalogue batch 7b
(the delegate-served energy pair): `0x050F` Water Heater, `0x0018`
Battery Storage; and catalogue batch 8 (the composed appliances): `0x0078`
Cooktop, `0x007B` Oven, `0x007A` Extractor Hood, `0x0070` Refrigerator,
`0x0071` Temperature Controlled Cabinet, `0x0077` Cook Surface, `0x0079`
Microwave Oven; and the EVSE round: `0x050C` Energy EVSE. **Fifty-two device
types, the whole C6 catalogue.** Anything else answers `+MTERR:6`.

Batch 7a boundaries worth knowing before a bench session:

- The electrical sensor and meter are the first VARIANT-carrying types on
  this platform: variant `0` is power + energy, variant `1` power only
  (each variant is its own declared cluster list; there is no runtime
  teardown). The meter's variant `1` and solar power's variant `1` are
  disclosed sub-conformances, restated at their declarations.
- Every measurement value is `Instance`-served from the host's `AT+MTMEAS`
  push store (`0x0090`/`0x0091`; `0x0098` for DEM): `AT+MTATTR` reads
  answer the live store through the carve-out, writes answer `+MTERR:11`,
  and no `+MTATTR` URC ever fires for them. The EEM struct attributes,
  the meter's strings and `PowerThreshold` answer `+MTERR:5`.
- The energy types are the first whose capacity is POOL-bound rather than
  table- or heap-bound: the EPM/PowerTopology pools serve 8
  measurement-bearing endpoints per composition (`MT_MEAS_MAX`, shared by
  sensors, meters, heat pumps and solar), DEM serves 4 (`MT_DEM_MAX`),
  utility meters 2 (`MT_METER_MAX`), the C6's own depths (ruling DE407).
  Exhaustion aborts the boot rebuild at the offending endpoint with the
  pool named, the standing stop-at-failure prefix semantics.
- On an accepted `PowerAdjustRequest` the firmware owns the ESAState
  transition and both PA events; a null `AT+MTDEMCAP` capability means a
  controller's `PowerAdjustRequest` answers `ConstraintError` without the
  host ever seeing a `+MTCMD`.
- `AT+MTMETERID` is the only write path to the meter identity and has no
  read-back verb; the meter endpoint is not conformant against the
  superset's TimeSyncCond (no Time Synchronization on endpoint 0),
  disclosed on both platforms.

Batch 7b boundaries on top of those:

- The water heater's `Boost` verdict and its `BoostState` are two
  different moments ("the host decided" versus "the host actually did
  it"): an allowed `Boost` caches its parameters and moves nothing; the
  `AT+MTMEAS` `0x0094` `BoostState` push is what transitions the served
  state and derives `BoostStarted` (carrying the cached parameters,
  consumed on emission, duration 0 when there is none) and `BoostEnded`.
  `CancelBoost` while already Inactive answers Success without waking the
  host and emits nothing.
- `AT+MTMEAS` `0x0094` pushes to the feature-gated fields (TankVolume,
  EstimatedHeatRequired, TankPercentage) answer `+MTERR:3` on the bare
  variant 1, which carries neither feature. The water heater's Thermostat
  reuses the standalone list under a heating-only FeatureMap, so its
  cooling setpoints are declared but disclosed over-declarations (the C6
  creates none).
- Battery storage adds no AT surface at all: its nine battery attributes
  are ordinary `AT+MTATTR` integers over the full `uint32` domain (B263;
  the two fault lists answer `+MTERR:5`), and variant 0's DEM pair is the
  batch 7a machinery whole, `AT+MTDEMCAP` included.
- Two more pool bounds: water heaters draw on `MT_WHM_MAX` (4); battery
  storage draws on the measurement pools and, on variant 0, `MT_DEM_MAX`,
  so four DEM-bearing endpoints is the composition's total across battery
  storage and standalone DEM rows.
- The two types are the registry's first with a VARIANT-DEPENDENT
  device-type span: the water heater's variant 1 loses the sensor graft
  and its `0x0510` id together, battery storage's variant 1 the DEM pair
  and `0x050D`.

EVSE boundaries, the last device type and the only one with its own capacity
cap:

- **An `AT+MTEP` composition may carry at most TWO Energy EVSE endpoints**
  (`MT_EVSE_MAX`, the C6's own constant). A third fails its create loudly at
  the next boot and the composition serves the prefix before it, the standing
  stop-at-failure semantics, exactly as the measurement pools do. The cap
  exists because the charging-target store lives in the delegate: 1,968 bytes
  of it, against the catalogue's next largest per-endpoint object at 304.
- Ruling DE408 put this device type out of tier when the board sat at 96.67
  percent of RAM. The memory reclaim rounds lifted it: the exclusion is gone
  and the cap is what replaces it.
- The **charging schedule is USER DATA**. It survives `AT+MTRESET` and a power
  cycle and only `AT+MTFRESET` erases it, the exact opposite of the endpoint
  composition, which survives a factory reset because it is a product
  definition. Both live in the same `settings_storage` partition with
  deliberately opposite lifetimes. The stored blob format is byte-for-byte the
  C6's.
- **`AT+MTROW` has a consumer here for the first time.** A host stages rows,
  `AT+MTROWAPPLY` MERGES them by day (days the set names replace those days
  entirely, days it does not name are untouched) and a count of 0 clears the
  whole schedule, which is the AT surface's only clear verb. A controller's
  `SetTargets` is adjudicated: the firmware stages the proposal, raises
  `+MTCMD` with the row count and the affected-day mask, the host pulls the
  rows with `AT+MTROWGET=<ep>,1,,<seq>` and answers, and only then is anything
  merged. The affected-day mask is on the wire because a day being EMPTIED
  carries no row to pull.
- **Two variants split on SOC reporting**, and it is wire-visible rather than
  decorative: on variant 0 every charging target must carry a SoC, on variant
  1 a target's SoC must be absent or exactly 100. The same rule is enforced on
  both the fabric path (by the SDK) and the AT path (by the port), so a host
  cannot install a schedule a controller would have been refused.
- `AT+MTMEAS` `0x0099` is the ONLY way to set any EnergyEvse attribute: all 23
  are served by the cluster's own Instance from the delegate cache, none of
  them ever raises a `+MTATTR` URC, and `AT+MTATTR` reads answer the live
  cache through the carve-out while writes answer `+MTERR:11`. Fields 7, 8 and
  13 (`UserMaximumChargeCurrent`, `RandomizationDelayWindow`,
  `ApproximateEVEfficiency`) answer `+MTERR:4` on every variant, because
  esp-matter creates none of the three and this port declares none of them
  either; fields 14 and 15 answer `+MTERR:4` on variant 1.
- **V2X, RFID and Plug-and-Charge are never built and the EVSE advertises no
  `EnableDischarging` and no `StartDiagnostics`.** The Instance derives its
  advertised command list from the feature mask this port hands it, so the
  mask and the declared attribute list are one decision.
- The EVSE draws SIX per-endpoint cluster objects at once, more than any other
  device type, and 1,144 B of endpoint block, the widest in the catalogue.
  Both consequences are in "Endpoint capacity" below.

Batch 8 boundaries, the first COMPOSED appliances on this platform:

- Parenting is real. `AT+MTEP=<devtype>,<variant>,<parent_idx>` was already
  parsed, staged, persisted in the v3 blob and re-emitted by `AT+MTEP?` on
  this platform; what batch 8 added is a `mt_devtype_parent_ok()` with
  opinions and the cluster sets that follow from them. **Cook Surface
  (`0x0077`) requires a Cooktop (`0x0078`) parent** and answers `+MTERR:1` on
  the `AT+MTEP=` line for the unparented form or any other parent;
  **Temperature Controlled Cabinet (`0x0071`) is legal unparented or under a
  Refrigerator (`0x0070`) or an Oven (`0x007B`) only.**
- The cabinet's cluster set is **derived from its parent at every boot**, not
  stored: unparented it is bare TemperatureControl, under a Refrigerator it
  gains `RefrigeratorAndTemperatureControlledCabinetMode` (Cooler), under an
  Oven it gains `OvenMode` and `OvenCavityOperationalState` (Heater). Six
  realised shapes across the two variants. Changing a cabinet's parent
  changes the cabinet.
- Unlike the C6, this port SELECTS the whole cluster set before the endpoint
  is served, where the C6 augments after parenting. A composed endpoint
  therefore appears complete or not at all; there is no window in which a
  parent's `PartsList` names a half-built child.
- **A composed appliance costs two or more of the 16 servable endpoints.** A
  four-cavity oven is five `AT+MTEP` rows. See "Endpoint capacity" below.
- `AT+MTTEMPLEVELS` does something on this platform for the first time,
  against a **block-resident** 273 B label store charged only to
  TemperatureLevel-variant endpoints (the C6's equivalent is about 7,728 B
  of unconditional `.bss`). `+MTERR:3` for an endpoint with no
  TemperatureControl cluster, `+MTERR:4` for a TemperatureNumber-variant one.
- A controller's `SetTemperature` is **not** adjudicated: the SDK handles it
  end to end and the write lands in this port's arena, so it reaches the host
  as an ordinary `+MTATTR` URC and the host has no veto. No other cluster in
  this firmware behaves that way.
- Cooktop and Cook Surface carry OnOff with the **OffOnly** feature: `Off` is
  the only accepted command, so turning one on is always the host's act, an
  `AT+MTATTR` write of `0x0006`/`0x0000`.
- `MicrowaveOvenMode` has **no** `ChangeToMode` command at all; the cooking
  mode is selected through `MicrowaveOvenControl`'s `SetCookingParameters`.
  `AT+MTMODES` still feeds its list, and the tag-0 default is `kNormal`,
  which is what lets `SetCookingParameters` work before the host has fed one.
- The microwave's `CookTime` and `PowerSetting` are Instance-owned: they
  raise no `+MTATTR` URC and `AT+MTATTR` reads their boot shadow, which goes
  stale after the first cooking command. Read them through a commissioned
  controller.
- `OvenCavityOperationalState` accepts `Stop` and `Start` only (`Pause` and
  `Resume` are `disallowConform` on the derived cluster) and adds no states:
  `AT+MTOPSTATE` takes `0`, `1` and `2` on a cavity exactly as on a washer.

Batch 2 takes on fewer cluster features than the specs allow, but never
fewer commands than a feature it does take on requires. The boundaries are
worth knowing before a bench session:

- Color: the command surface is **complete** for the features each type
  advertises, since every ColorControl command is `mandatoryConform` on a
  feature. `0x010C` advertises feature CT alone and accepts
  MoveToColorTemperature, MoveColorTemperature, StepColorTemperature and
  StopMoveStep. `0x010D` advertises HS|XY|CT (HS is `optionalConform`
  inside the Extended Color Light device type, and taking it is the C6's
  own step beyond the mandatory set, so a host library's HSV class has
  CurrentHue and CurrentSaturation to write) and accepts all fourteen:
  MoveToHue, MoveHue, StepHue, MoveToSaturation, MoveSaturation,
  StepSaturation, MoveToHueAndSaturation, MoveToColor, MoveColor,
  StepColor, MoveToColorTemperature, MoveColorTemperature,
  StepColorTemperature and StopMoveStep. EnhancedHue and ColorLoop are the
  two features not taken, so their five commands are correctly absent.
  Both types boot in color-temperature mode at 250 mireds, because
  StartUpColorTemperatureMireds is seeded non-null exactly as on the C6.
  Bench note: a ColorControl command sent to a light whose OnOff attribute
  is FALSE is discarded and still answers Success, and nothing moves. That
  is the cluster's ExecuteIfOff rule, not a defect: with an On/Off server on
  the same endpoint, an OnOff of FALSE and the Options bit
  ExecuteIfOff clear, the server stops after Options processing
  (`shouldExecuteIfOff`, `color-control-server.cpp:515`). Turn the light on
  first, or send the command with OptionsMask/OptionsOverride set.
- Thermostat: Heating|Cooling, SetpointRaiseLower, setpoints seeded 16 C /
  24 C. No Presets, schedules, Auto mode or occupancy: those need a
  delegate this firmware does not provide.
- Fan: FanMode, FanModeSequence, PercentSetting and PercentCurrent only,
  FeatureMap 0. No Step/Rock/Wind/AirflowDirection (delegate territory).
  The server's own FanMode-to-PercentSetting coupling does not run on a
  dynamic endpoint, so the host owns it; both attributes read and write
  correctly over AT and over a controller's IM.
- Window covering: Lift|PositionAwareLift, and UpOrOpen / DownOrClose /
  StopMotion / GoToLiftPercentage all land the fabric's request in
  TargetPositionLiftPercent100ths, emit the `+MTATTR` URC and answer
  Success. The host moves the motor and writes CurrentPosition back. No
  tilt this round.
- Air quality: the AirQuality attribute with all four optional quality
  levels advertised (Fair, Moderate, VeryPoor, ExtremelyPoor), so a host
  library's seven-value enum can never report a level the endpoint's
  feature map rejects.

Batch 2 was the first round to declare attributes whose ZCL type is one of
Matter's semantic **aliases** for an integer: the thermostat's setpoints are
`temperature`, the fan's are `percent`, the window covering's lift positions
are `percent100ths`. The AT attribute bridge's type classifier only knew the
base integer codes, so those attributes answered `+MTERR:5` on an
`AT+MTATTR` read and emitted no `+MTATTR` URC when a controller changed
them, while working perfectly over the controller's own IM. The failure was
easy to miss on the bench because `+MTERR:5` is also the honest answer for a
nullable attribute currently holding null, the AT grammar having no null
literal. Fixed by normalising through the SDK's own alias table
(`AttributeBaseType()`, `app/util/ember-io-storage.cpp`) before classifying,
so the AT integer family tracks the SDK's definition instead of a
hand-copied list. The C6 never had this gap because esp-matter maps the
aliases down to its own value types before its AT bridge sees them.

The BooleanState cluster (Contact/Rain/Water Freeze/Water Leak) is served
over real Matter reads by a CHIP-registered `BooleanStateCluster` object
rather than this build's external-storage arena directly;
`MatterPostAttributeChangeCallback`
(`port/mt_matter_zephyr.cpp`) bridges the two, so an AT+MTATTR write of
StateValue both answers a later AT+MTATTR read from this arena and reaches
a real Matter controller's read, emitting the cluster's StateChange event
in the process -- see the comment at that call site and on
`booleanStateAttrs` (`port/mt_devtypes_zephyr.cpp`) for the mechanism.

Level commands (the MoveToLevel family) on the dimmable types (`0x0101`,
`0x010B`) settle CurrentLevel correctly on dynamic endpoints (fixed:
graph bug B388, which traced to dynamic endpoints never running the
per-endpoint LevelControl server init that caches Min/MaxLevel; `port/
mt_devtypes_zephyr.cpp` now invokes it by hand at endpoint create time).
Direct AT+MTATTR writes to CurrentLevel and OnOff coupling both work
correctly; store coherence (AT, controller, URCs) is fine throughout.

### Batch 3: the command verdict frame (`+MTCMD`)

The door lock and the water valve are the first device types on this
platform whose commands need an **application verdict**. When a controller
invokes `LockDoor`, `UnlockDoor`, valve `Open` or valve `Close`, the
firmware does not decide on its own: it raises
`+MTCMD:<seq>,<ep>,<cluster>,<cmd>` to the host and waits up to 1000 ms for
`AT+MTCMDRESP=<seq>,<verdict>`. A missed window answers `+MTCMDTO:<seq>` and
the command is denied. All four forwards are the payload-less form, cluster
`257` commands `0`/`1` for the lock and cluster `129` commands `0`/`1` for
the valve, exactly as `AT_MT_SPEC.md` 3.17 registers them.

None of that protocol is new or platform-specific: sequence numbers, the
window, the default-deny and the `+MTCMDTO` URC are core's
(`core/mt/mt_cmdbox.c`), identical to the C6's, and this platform only calls
into it from the right SDK hooks. What is new here is where those hooks are
and what the SDK does with the answer, and the two types differ on that
second point:

- **Door lock: the verdict IS the wire response.** The forward happens in
  the ember command callbacks `emberAfPluginDoorLockOnDoorLockCommand` /
  `...OnDoorUnlockCommand`, and the server answers the controller
  `Success` on allow and `Failure` on deny, plus a `LockOperationError`
  event carrying `Unspecified`. A deny genuinely fails the command.
- **Water valve: the verdict cannot fail the command.** The forward happens
  in the `ValveConfigurationAndControl::Delegate`'s `HandleOpenValve` /
  `HandleCloseValve`, and the SDK discards what they return: the controller
  sees `Success` either way. This is an SDK property, not a firmware
  choice, traced in this tree and documented in `AT_MT_SPEC.md` 3.19. The
  verdict gates only whether the host actually moves the valve.

**A `129`/`1` (valve `Close`) forward is not always a controller's doing.**
The valve server closes the valve itself when a timed open expires, and that
path re-enters the same delegate hook, so the host sees a **second,
unsolicited** `+MTCMD:<seq>,<ep>,129,1` that no controller asked for. It is
reachable on this build because `DefaultOpenDuration` is writable and
mandatory: a controller that writes it, or that sends `Open` carrying a
duration, arms the countdown, and at zero the server calls `CloseValve()`,
which calls the delegate.

Three consequences a host author needs:

- **The verdict on that forward is meaningless.** `CloseValve()` has already
  set `TargetState` to Closed, set `CurrentState` to Transitioning, nulled
  `OpenDuration` and `RemainingDuration`, and emitted `ValveStateChanged`
  **before** the delegate is consulted. A deny undoes none of it; the
  fabric-visible state moves either way.
- **It arrives from timer context and blocks.** The forward stalls the CHIP
  event loop for up to the full 1000 ms window, once per timed open. A host
  that lets that window time out costs the stack a second.
- **So treat a `129`/`1` forward as possibly server-initiated.** Nothing on
  the wire distinguishes it from a controller's `Close`; a host that wants
  to act only on controller-driven closes has to infer it from its own state
  (it knows whether it saw the matching `Open`).

This is the SDK's structure, not a firmware choice: `HandleCloseValve()` is
the only hook offered and it cannot tell an invoke from an auto-close. The
C6 behaves identically for the same reason. Documented rather than worked
around; the mechanism with line citations is on the water valve's audit note
in `port/mt_devtypes_zephyr.cpp`.

**Reporting back is the host's job, on both types.** An allowed verdict
never changes `LockState` or `CurrentState` by itself, because only the host
knows when the bolt or the valve actually moved. The host reports the
outcome with `AT+MTLOCK=<ep>,<state>[,<source>]` (3.18) or
`AT+MTVALVE=<ep>,<state>[,<level>]` (3.19), and both go through the
cluster's own setter rather than a raw attribute write, so the
`LockOperation` / `ValveStateChanged` event a subscribed controller expects
is actually emitted.

Boundaries worth knowing before a bench session:

- **Door lock, FeatureMap 0.** No PIN, RFID, user, schedule or Aliro
  features, which is conformant: every DoorLock feature is optional, and
  `USR` is mandatory only once a credential feature is claimed. So the
  command surface is `LockDoor` and `UnlockDoor` only; `UnlockWithTimeout`
  and `UnboltDoor` are not declared. `AutoRelockTime` **is** declared even
  though it is optional: without it the SDK's own `SetLockState` reports
  failure for an unlock that actually happened, which would turn every host
  `AT+MTLOCK` unlock into a bare `ERROR` with the state changed underneath
  (the C6's bug B129, which reproduces verbatim in this tree). Seeded 0,
  which disables auto-relock; a controller may write it non-zero and the
  server then relocks on its own timer, which the host sees as a `+MTATTR`
  URC.
- **Door lock, boot state.** `LockState` boots **null**, not
  `NotFullyLocked`: the SDK's per-endpoint init sets it null and
  `ActuatorEnabled` true, and the firmware never invents a lock state. A
  bench `AT+MTATTR` read of `LockState` before any `AT+MTLOCK` therefore
  answers `+MTERR:5`, which is the AT grammar having no null literal, not a
  fault.
- **Water valve, FeatureMap 0.** No `TS` (time sync) and no `LVL` (level).
  `TS` is left clear deliberately, not by omission: setting it without a
  Time Synchronization cluster server on the image makes **every** `Open`
  answer `Failure`. Without `LVL`, `CurrentLevel` and `TargetLevel` are not
  declared at all, so `AT+MTVALVE`'s `<level>` argument is accepted and does
  nothing observable, the same as on the C6 and for the same reason
  (`AT_MT_SPEC.md` 3.19).
- **Water valve, `RemainingDuration`.** Served by the cluster's own
  wildcard `AttributeAccessInterface` from a private shadow store, not from
  this build's arena, so a subscribed controller sees the server's live
  countdown while an `AT+MTATTR` read answers from the arena. Deliberately
  not bridged the way BooleanState is: it is a countdown the server owns
  and ticks, with no host write path in the AT contract to push into it.
- **Both, RAM.** Neither type widens the endpoint block heap's worst case;
  see the capacity table below.

## Endpoint capacity

**Acceptance and capacity are two different numbers on this platform, and
an integrator needs both.**

*Acceptance* is `MT_COMP_MAX_ENDPOINTS` (28). It is a core constant and part
of the `AT+MT` wire contract, identical on the C6 and here: a host may
DECLARE up to 28 endpoints over `AT+MTEP`, and the composition store keeps
all 28 intact.

*Capacity* is `kServiceableEndpoints` (16, `port/mt_port_ids.h`). It is how
many of those endpoints this build stands up and serves at once, and it is a
port decision about a 256 KB part rather than a contract change. The C6 has
no such split because it serves the full 28.

**What a host sees past capacity.** Nothing at declare time: `AT+MTEP`
accepts the endpoint and the composition persists. The wall is at the next
boot, in the rebuild. `mt_devtype_create()` logs which of the two limits was
hit and how far away the other one was, returns an error, and the rebuild
stops there.

The abort is **stop-at-failure, not roll-back** (`AT_MT_SPEC.md` 501-506).
The endpoints created before the failure **stay live as a prefix, with their
ids unchanged**; the failed entry and everything after it are absent, and
`AT+MTEP?` reports the live prefix. So a 20-endpoint composition on this
build serves endpoints **1 through 16**, and endpoints 17 to 20 are simply
not there.

What the rule prevents is a **renumbered** model, not a partial one: skipping
the failed entry and carrying on would shift every later endpoint down by
one, and a commissioned controller would silently get a different data model
than the one it was commissioned against (design spec 12.1). That is the same
stop-at-failure a bad parent or an unknown device type already triggers.

**Capacity is bounded by two resources, not one.** A composition can exhaust
either first:

1. **The header table**, `kServiceableEndpoints` entries, 16 bytes each.
2. **The endpoint block heap**, `HEARTH_EP_HEAP_BYTES` (8 KB), a dedicated
   `K_HEAP_DEFINE(hearth_ep_heap)`. Each created endpoint takes one block
   holding its `DataVersion` array and its attribute slots, sized for its
   own device type rather than for the widest one in the catalogue.

Sixteen sensors exhaust the table with the heap barely touched; fourteen
extended colour lights exhaust the heap with headers to spare.

A third resource exists since memory reclaim round A and is **deliberately
sized so it can never be the first to run out**: the **cluster-object
heap**, `HEARTH_OBJ_HEAP_BYTES` (11,264 B since the EVSE round, 7,168 B
since catalogue batch 8, 6,528 B before it), a second
`K_HEAP_DEFINE(hearth_obj_heap)` holding the per-endpoint CHIP Delegate
objects and their Instances for the appliance, mode, chime, valve, energy
and meter families. It replaced fourteen fixed pools, and the per-family
caps those pools carried (`MT_MEAS_MAX` 8, `MT_DEM_MAX` 4, `MT_WHM_MAX` 4,
`MT_METER_MAX` 2, `MT_EVSE_MAX` 2, `kModeBasePoolSlots` 20) are unchanged
and are still what a composition hits first. 11,184 usable bytes against a
worst reachable draw of **10,912 B**, a margin of 272 B, which is the
maximum over every composition the other two walls admit, found by
exhaustive search rather than by a greedy fill:

| Count | Device type | Object bytes | Block bytes |
|---|---|---|---|
| 2 | `0x050C` Energy EVSE (v1) | 5,392 | 2,256 |
| 8 | `0x0079` Microwave Oven | 3,520 | 4,288 |
| 3 | `0x0073` / `0x0075` / `0x007C` washer, dishwasher, dryer | 792 | 432 |
| 2 | `0x0511` Electrical Utility Meter | 624 | 256 |
| 1 | `0x0018` Battery Storage (v0) | 584 | 856 |
| **16** | | **10,912** | **8,088** of 8,112 |

`MT_EVSE_MAX` and `MT_METER_MAX` are both saturated, the endpoint count is
saturated and the block heap is 24 B from its own wall. The arithmetic is at the end of
`port/mt_matter_zephyr.cpp` and a `static_assert` pins it, so the claim
cannot go stale. Exhaustion, if a future round ever makes it possible, is
the same loud stop-at-failure prefix as either older wall.
Note that 10,912 is a function of the block budget: raising
`HEARTH_EP_HEAP_BYTES` for the LM20 tier admits object-heavier compositions
and this heap has to be re-derived with it.

**The EVSE round moved this maximum and the heap with it (7,168 to
11,264 B), by much the largest step it has taken.** An Energy EVSE endpoint
draws **six** per-endpoint objects at once, EnergyEvse,
DeviceEnergyManagement, two ModeBase aliases, ElectricalPowerMeasurement and
PowerTopology, and the first of them carries the whole charging-target store:
2,696 object bytes on a 1,128 B block, 2.39 per block byte against the
microwave's 0.82. It is capped at two, which is exactly why it needs a
bigger OBJECT heap and not a bigger endpoint heap: the cap bounds how many
blocks a composition holds and does nothing about how heavy each one's
objects are. **Variant 1 is the EVSE the search picks**, because the two
variants draw identical objects (SOC changes two attributes, not an object)
while variant 1's block is 16 bytes narrower.

Catalogue batch 8 moved it before (6,528 to 7,168 B), and the Microwave Oven
was that reason: **three** per-endpoint object pairs at once,
OperationalState, MicrowaveOvenMode and MicrowaveOvenControl, for a 536 B
block. The pre-batch-8 maximum of 6,336 B (4 Battery Storage + 4 RVC + 2
meter + 6 washer) and the batch-8 maximum of 6,928 B (13 microwave + 2 meter
+ 1 battery) are both still admissible; they are simply no longer the
largest.

### Per-endpoint cost

Block payload is `4 x clusters + 16 x slots`; Zephyr charges
`roundup(payload + 4, 8)`.

| Device type | Clusters | Slots | Heap cost |
|---|---|---|---|
| `0x050C` Energy EVSE (v0; 528 B payload + two 306 B mode stores) | 8 | 31 | 1,144 B |
| `0x050C` Energy EVSE (v1, no SOC pair; 512 B payload + both stores) | 8 | 30 | 1,128 B |
| `0x0074` Robotic Vacuum Cleaner (244 B payload + two 306 B mode stores) | 5 | 14 | 864 B |
| `0x0018` Battery Storage (v0; 540 B payload + one 306 B mode store) | 7 | 32 | 856 B |
| `0x050F` Water Heater (v0; 492 B payload + one 306 B mode store) | 7 | 29 | 808 B |
| `0x0071` Temp Controlled Cabinet (Heater, v1; 176 B payload + one 306 B mode store + one 273 B label store) | 4 | 10 | 760 B |
| `0x0071` Temp Controlled Cabinet (Cooler, v1; 108 B payload + both stores) | 3 | 6 | 696 B |
| `0x050F` Water Heater (v1; 320 B payload + one 306 B mode store) | 4 | 19 | 632 B |
| `0x010D` Extended Colour Light | 5 | 36 | 600 B |
| `0x010C` Colour Temperature Light | 5 | 32 | 536 B |
| `0x0079` Microwave Oven (224 B payload + one 306 B mode store) | 4 | 13 | 536 B |
| `0x0071` Temp Controlled Cabinet (Heater, v0; 224 B payload + one 306 B mode store) | 4 | 13 | 536 B |
| `0x0070` Refrigerator (208 B payload + one 306 B mode store) | 4 | 12 | 520 B |
| `0x050D` Device Energy Management (v0; 462 B payload incl. one 306 B mode store) | 3 | 9 | 472 B |
| `0x0071` Temp Controlled Cabinet (Cooler, v0; 156 B payload + one 306 B mode store) | 3 | 9 | 472 B |
| `0x0018` Battery Storage (v1, no DEM pair, no store) | 5 | 23 | 392 B |
| `0x0077` Cook Surface (v1; 108 B payload + one 273 B label store) | 3 | 6 | 392 B |
| `0x0071` Temp Controlled Cabinet (unparented, v1; 56 B payload + one 273 B label store) | 2 | 3 | 336 B |
| `0x0101` / `0x010B` / `0x0110` Dimmable Light, Dimmable Plug, Mounted Dimmable Load Control | 4 | 20 | 344 B |
| `0x0303` / `0x0072` Pump, Room Air Conditioner | 4 | 18 | 312 B |
| `0x0301` Thermostat | 3 | 15 | 256 B |
| `0x0309` / `0x0017` Heat Pump, Solar Power (v0) | 5 | 13 | 232 B |
| `0x0202` Window Covering | 3 | 13 | 224 B |
| `0x000A` Door Lock | 3 | 12 | 208 B |
| `0x0100` / `0x010A` / `0x010F` On/Off Light, On/Off Plug, Mounted On/Off Control | 3 | 11 | 192 B |
| `0x0042` Water Valve | 3 | 11 | 192 B |
| `0x002B` / `0x002D` Fan, Air Purifier | 3 | 10 | 176 B |
| `0x0302` `0x0307` `0x0305` `0x0106` `0x0306` `0x0107` sensors | 3 | 9 | 160 B |
| `0x0077` Cook Surface (v0) | 3 | 9 | 160 B |
| `0x0510` Electrical Sensor (v0) | 4 | 8 | 152 B |
| `0x000F` Generic Switch | 3 | 8 | 144 B |
| `0x0511` Electrical Utility Meter | 3 | 7 | 128 B |
| `0x0015` `0x0044` `0x0041` `0x0043` `0x002C` boolean-state, air quality | 3 | 7 | 128 B |
| `0x0514` Electrical Meter (v0) | 3 | 6 | 112 B |
| `0x0071` Temp Controlled Cabinet (unparented, v0) | 2 | 6 | 112 B |
| `0x007A` Extractor Hood | 2 | 6 | 112 B |
| `0x007B` Oven | 2 | 4 | 80 B |
| `0x0078` Cooktop | 2 | 3 | 64 B |

### Worked examples

Against 8 KB of heap (about 8,100 usable after the heap's own header and
bucket table) and 16 header slots:

| Composition | Heap | Fits? |
|---|---|---|
| 16 sensors of any kind | 2,560 B | **yes**, table-bound, heap 68% idle |
| 16 on/off or dimmable lights and plugs | up to 5,504 B | **yes**, table-bound |
| 16 thermostats / window coverings / fans | up to 4,096 B | **yes**, table-bound |
| 16 door locks or water valves | up to 3,328 B | **yes**, table-bound |
| 2 extended colour + 2 dimmable + 12 sensors | 3,808 B | **yes**, comfortably |
| 15 colour temperature lights | 8,040 B | **yes**, one short of 16 |
| 13 extended colour lights | 7,800 B | **yes**, three short of 16 |
| 16 extended colour lights | 9,600 B wanted | **no**, fails at the 14th |
| 13 mode selects (store in-block since the reclaim round) | 7,592 B | **yes**, three short of 16 (bench-observed 2026-08-30) |
| 16 mode selects | 9,344 B wanted | **no**, fails at the 14th |
| 16 chimes (store in-block) | 5,632 B | **yes**, table-bound |
| 9 robotic vacuum cleaners (two mode stores in-block each) | 7,776 B | **yes**, seven short of 16 |
| 16 robotic vacuum cleaners | 13,824 B wanted | **no**, fails at the 10th |
| 16 cooktops, ovens or extractor hoods | up to 1,792 B | **yes**, table-bound |
| 15 microwave ovens | 8,040 B | **yes**, one short of 16 |
| 16 microwave ovens | 8,576 B wanted | **no**, fails at the 16th |
| 1 oven + 10 Heater cabinets (v1, both stores in-block each) | 7,680 B | **yes**, 11 of the 16 endpoints |
| 1 oven + 4 Heater cabinets (v0), a four-cavity oven | 2,224 B | **yes**, 5 of the 16 endpoints |
| 1 cooktop + 4 cook surfaces (v0) | 704 B | **yes**, 5 of the 16 endpoints |
| 2 energy EVSEs (v0), the `MT_EVSE_MAX` cap | 2,288 B | **yes**, 2 of the 16 endpoints |
| 3 energy EVSEs | 3,432 B | **no**, the third fails on `MT_EVSE_MAX`, not on a heap |

**A composed appliance costs two or more of the 16 endpoints**, and that is
the capacity consequence catalogue batch 8 introduced. An Oven or a
Refrigerator with no Temperature Controlled Cabinet child is a bare parent
(and sub-conformant against its own device-type XML); a Cook Surface cannot
be declared at all without a Cooktop parent. A realistic four-cavity oven is
five `AT+MTEP` rows.

So every device type in the catalogue reaches the full 16 **except** the two
colour lights (15 and 13), mode select (13, since the store reclaim round
moved its host-fed mode store into the endpoint block), the robotic
vacuum cleaner (9: its block carries TWO host-fed ModeBase stores; dropping
the optional RvcCleanMode would have doubled that capacity and was ruled
out, DE404, as a real cross-platform data-model divergence) and the Energy
EVSE. The EVSE is the one whose limit is NOT a heap: at 1,144 B it is the
catalogue's widest block and seven of them would fit, but `MT_EVSE_MAX`
stops the third, because its delegate carries the charging-target store and
two of those against 41 KB of free RAM is the trade that let the device type
exist at all. Sizing the
heap for 16 extended colour lights would want 9,600 B and buy a composition
nobody builds; the RAM is worth more elsewhere. A compile-time assertion
keeps the heap holding at least eight of whatever the widest UNCAPPED device
type happens to be, and `min(cap, 8)` of a capped one, so this table cannot
go stale unnoticed. The EVSE round is what made that assertion cap-aware:
demanding eight EVSE blocks would have demanded 9,152 bytes of an 8,112-byte
heap for a composition the create path refuses to build, and the reasoning is
written at the assertion in `port/mt_devtypes_zephyr.cpp`, because a floor
nobody understands is the one the next person weakens.

### The LM20 tier

The nRF54LM20 (512 KB RAM, supported upstream in this NCS) is where the 16
goes back up. Raising `kServiceableEndpoints` and `HEARTH_EP_HEAP_BYTES`
together in `port/mt_port_ids.h`, and the mirrored literal in
`src/chip_project_config.h`, is the whole change: the header table, CHIP's
per-endpoint pools and the block heap all follow from those two numbers, and
the static assertions catch a mirror that drifts. 28 serviceable endpoints
would want `28 x 600` = 16,800 B (about 16.4 KB) of block heap for an
all-extended-colour worst case, and would hand back to CHIP's per-endpoint
pools the roughly 6.7 KB this round reclaimed from them by going the other
way.

### Measured

`ophelia_cpico/nrf54l15/cpuapp`, pristine builds, 2026-08-29:

| | Flat arena (batch 2) | Per-composition (this round) |
|---|---|---|
| RAM used | 240,276 B (91.7%) | **223,884 B (85.4%)** |
| RAM free | 21,868 B | **38,260 B** |
| Flash | 789,488 B | 789,792 B |

A 16,392 byte reclaim, from three places: the endpoint table went from a
flat `28 x 648 B` arena to a `16 x 16 B` header table (-17,888 B), the new
8 KB block heap costs +8,192 B, and dropping
`CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT` from 28 to 16 shrank CHIP's own
compile-time per-endpoint pools by about 6,700 B, of which
`ColorControlServer` alone is 3,168 B. `nrf54l15dk` tracks it: 224,108 B
(85.5%), a 16,384 byte reclaim.

Catalogue batch 3 on top of that, pristine builds, 2026-08-29:

| | Batch 2 + per-composition | Batch 3 (door lock, water valve) |
|---|---|---|
| RAM used, `ophelia_cpico` | 223,884 B (85.4%) | **224,972 B (85.8%)** |
| RAM used, `nrf54l15dk` | 224,108 B (85.5%) | **225,196 B (85.9%)** |
| Flash, `ophelia_cpico` | 789,792 B | **806,208 B** |
| Flash, `nrf54l15dk` | not recorded | 814,100 B |

RAM `+1,088 B` on both boards, which is CHIP's own per-endpoint tables for
the two new servers (`DoorLockServer::mEndpointCtx`, the valve's delegate
and `RemainingDuration` shadow arrays, each sized fixed + 16 dynamic) plus
this port's 16-slot valve delegate pool. Flash `+16,416 B` is the two
cluster servers themselves. No change to `HEARTH_EP_HEAP_BYTES`: neither
device type is close to the widest, so the compile-time floor under the
heap sizing is untouched.

Catalogue batch 4 on top of that, pristine builds, 2026-08-29
(`ad37e7a`, including both fix rounds):

| | Batch 3 | Batch 4 (seven appliance/notification types) |
|---|---|---|
| RAM used, `ophelia_cpico` | 224,972 B (85.8%) | **242,852 B (92.64%)** |
| RAM used, `nrf54l15dk` | 225,196 B (85.9%) | **243,076 B (92.73%)** |
| Flash, `ophelia_cpico` | 806,208 B | **825,264 B (60.05%)** |
| Flash, `nrf54l15dk` | 814,100 B | **833,168 B (56.98%)** |

RAM `+17,880 B` on both boards, read from the linked image with `nm -S`:
the host-fed mode store (7,040 B: 16 x 8 label entries plus their
`ModeOptionStruct` CharSpan/List halves), the chime sound store (4,448 B:
16 x 8 pairs of 33 B), the `OperationalState::Instance` raw-storage pool
(3,840 B), the `ChimeServer` pool (640 B), the two delegate pools (448 B),
CHIP's PowerSource table (272 B), and about 1.2 KB of metadata growth.
Free RAM on `ophelia_cpico` was 19,292 B at the batch 4 merge; the store
reclaim round below took it back over 30 KB. Flash `+19,056 B` is the
five new cluster server translation units plus the two new generated
command dispatchers. No change to `HEARTH_EP_HEAP_BYTES` in batch 4
itself: the widest batch 4 type as merged (Smoke/CO Alarm, 224 B per
endpoint) was far under the extended colour light's 600 B.

### The store reclaim round (2026-08-30)

Batch 4's two 16-deep host-fed .bss stores (`s_mode_slots` 7,040 B,
`s_chime_slots` 4,448 B) were reserved even for compositions with no
mode-select or chime endpoint. The reclaim round moved both into the
per-endpoint block heap as type-conditional trailing regions
(`mt_mode_store_t` 436 B, `mt_chime_store_t` 273 B per endpoint), so only
compositions that declare those types pay:

| | Batch 4 (`ad37e7a`) | Store reclaim |
|---|---|---|
| RAM used, `ophelia_cpico` | 242,852 B (92.64%) | **231,364 B (88.26%)** |
| RAM used, `nrf54l15dk` | 243,076 B (92.73%) | **231,588 B (88.34%)** |

Exactly `-11,488 B` on both arms (nm-verified: both store symbols gone
from the image), flash within a few hundred bytes. The per-endpoint heap
cost of the two types rises accordingly: mode select 144 to **584 B**
(the widest type in the catalogue until batch 5's RVC took the title),
chime 80 to **352 B**. The capacity consequence
is in the table above: an all-mode-select composition serves a
13-endpoint prefix under the stop-at-failure semantics, observed on the
bench exactly as computed; 16 chimes still fit. The
`OperationalState::Instance`, `ChimeServer` and delegate pools stayed in
.bss for this round (CHIP registration lifetime); memory reclaim round A
below moved them to their own heap, which preserves that lifetime exactly
because nothing on the new heap is ever freed.

Catalogue batch 5 on top of that, pristine builds, 2026-08-30 (`79c97ff`,
including the fix round):

| | Store reclaim | Batch 5 (seven standalone types) |
|---|---|---|
| RAM used, `ophelia_cpico` | 231,364 B (88.26%) | **237,900 B (90.75%)** |
| RAM used, `nrf54l15dk` | 231,588 B (88.34%) | **238,124 B (90.84%)** |
| Flash, `ophelia_cpico` | 825,616 B | **836,036 B (60.84%)** |
| Flash, `nrf54l15dk` | 833,416 B | **843,936 B (57.71%)** |

RAM `+6,536 B` on both arms: the ModeBase Instance and delegate pools
(sized 18 per ruling DE404: two per RVC endpoint times the nine the arena
serves), the RvcOperationalState Instance and delegate pools (16), and
metadata growth for the five new clusters. Flash `+10.4 KB` is the three
new cluster server directories (switch, pump configuration and control,
mode base); RvcOperationalState rides the already-compiled
operational-state server. The RVC's two in-block ModeBase stores make it
the widest type in the catalogue (864 B per endpoint, capacity table
above); `HEARTH_EP_HEAP_BYTES` is unchanged.

Catalogue batch 7a on top of that, pristine builds, 2026-08-30 (`f614936`,
including the fix round):

| | Batch 5 | Batch 7a (six energy foundation types) |
|---|---|---|
| RAM used, `ophelia_cpico` | 237,900 B (90.75%) | **251,812 B (96.06%)** |
| RAM used, `nrf54l15dk` | 238,124 B (90.84%) | **252,036 B (96.14%)** |

RAM `+13,912 B` on both arms, dominated by one item this port does not
own: the SDK's `ElectricalEnergyMeasurement` server keeps a static
17-entry `gMeasurements` table (measured 8,432 B) charged the moment the
cluster enters `hearth.zap`, composition or not, indexed from inside the
SDK and therefore not reclaimable by the endpoint-heap technique. The
rest is the measurement, DEM and meter pools at the C6's own depths
(8/4/2 per ruling DE407, never 16) plus metadata for six new clusters.
Free RAM was 10,332 B at the batch 7a merge; batch 7b below starts from
here, and the remaining sizing levers, including the nRF54LM20 tier, are
recorded in the batch 7 audit.

Catalogue batch 7b on top of that, pristine builds, 2026-08-30:

| | Batch 7a | Batch 7b (water heater, battery storage) |
|---|---|---|
| RAM used, `ophelia_cpico` | 251,812 B (96.06%) | **253,420 B (96.67%)** |
| RAM used, `nrf54l15dk` | 252,036 B (96.14%) | **253,644 B (96.76%)** |
| Flash, `ophelia_cpico` | 864,232 B (62.89%) | **870,488 B (63.34%)** |
| Flash, `nrf54l15dk` | 872,144 B (59.64%) | **878,388 B (60.07%)** |

RAM `+1,608 B`, exactly equal on both arms, read from the linked image
with `nm -S`: the `MT_WHM_MAX` pool (356 B: 4 x 48 B `HearthWhmDelegate`
plus 4 x 40 B Instance raw storage plus the counter), the water heater's
declared cluster/attribute lists and endpoint structs (568 B of `.data`),
battery storage's (676 B, the 19-entry rechargeable Power Source list
included), and ep240 static storage for the two new clusters. No new SDK
static table this time (WaterHeaterManagement keeps no `gMeasurements`
analogue) and no new pool beyond `MT_WHM_MAX`: battery storage reuses the
batch 7a pools whole. Flash `+6,256 B` is the water-heater-management
server translation unit plus the port's own code; WaterHeaterMode rides
the already-compiled mode-base server. `HEARTH_EP_HEAP_BYTES` unchanged:
both new blocks are compile-time floor candidates and both lose to the
RVC (856 and 808 B against 864). Free RAM on `ophelia_cpico` is
**8,724 B**; the Energy EVSE (ruling DE408) and anything after it are
LM20-tier work.

### Memory reclaim round A (2026-08-30)

Batch 7b left the board at 96.67 percent of RAM with 8,724 B free and the
composed-appliance catalogue still to come. Round A returns 22,960 B,
**exactly equal on both arms**, without changing a single AT command, URC,
error code or seed. Pristine builds, 2026-08-30, including the fix round
that corrected the cluster-object heap's sizing:

| | Batch 7b (`6c31f09`) | Memory reclaim A |
|---|---|---|
| RAM used, `ophelia_cpico` | 253,420 B (96.67%) | **230,460 B (87.91%)** |
| RAM used, `nrf54l15dk` | 253,644 B (96.76%) | **230,684 B (88.00%)** |
| RAM free, `ophelia_cpico` | 8,724 B | **31,684 B** |
| RAM free, `nrf54l15dk` | 8,500 B | **31,460 B** |
| Flash, `ophelia_cpico` | 870,600 B (63.35%) | **867,712 B (63.14%)** |
| Flash, `nrf54l15dk` | 878,516 B (60.08%) | **875,608 B (59.88%)** |

(The batch 7b flash column is re-measured at `6c31f09` on 2026-08-30 for
this round's own before-and-after, and reads 112 B above the batch 7b row
in the table above it. Same commit, same toolchain; the difference is the
`ncs_commit.h` generation the batch 7b measurement did not carry. RAM is
identical to the byte in both measurements, which is what this round is
about.)

Three items, each measured on its own with `nm -S` and the linked image:

| Item | RAM, `ophelia_cpico` | Flash |
|---|---|---|
| The device-type catalogue moves to flash | **-8,736 B** | +4 B |
| The fourteen Instance and Delegate pools move to a heap | **-7,440 B** | -2,780 B |
| ZMS lookup cache 512 to 64, Thread children 32 to 16 | **-6,784 B** | -112 B |

1. **The catalogue is `const`.** All 41 device types' cluster lists,
   attribute lists and endpoint descriptors sat in `.data` purely because
   CHIP's `DECLARE_DYNAMIC_*` macros declare their arrays without `const`.
   A port-local `HEARTH_DECLARE_CONST_*` family adds it and nothing else:
   `datas` -8,740 B and `rodata` +8,740 B exactly, no SDK patch, and the
   safety argument (nothing in the SDK writes endpoint, cluster or
   attribute metadata at runtime, and the one const-stripping cast in
   `emAfLoadAttributeDefaults()` is a source buffer inside a
   `!IsExternal()` branch this port's attributes never enter) is recorded
   at the macros in `port/mt_devtypes_zephyr.cpp`.
2. **The cluster-object heap.** 14,368 B of fixed Delegate and Instance
   pools became pointer tables into `K_HEAP_DEFINE(hearth_obj_heap)`,
   6,528 B, allocated during the boot composition rebuild: the third use
   of the technique the dyn-arena and store reclaim rounds proved. Every
   depth constant, the allocate-only policy, the
   construct-after-`emberAfSetDynamicEndpoint()` ordering and the
   stop-at-failure prefix are unchanged, and the heap is sized above its
   own worst reachable draw so it can never be the binding wall (see
   "Endpoint capacity" above).
3. **Two Kconfig values.** `CONFIG_ZMS_LOOKUP_CACHE_SIZE` 512 to 64
   (`default_settings_zms.0` 4,192 to 608 B, exactly 448 x 8) and
   `CONFIG_OPENTHREAD_MAX_CHILDREN` 32 to 16 (`ot::gInstanceRaw` 25,288 to
   22,088 B, exactly 16 x 200). `CONFIG_MBEDTLS_HEAP_SIZE` and the FTD/MTD
   choice are untouched, per ruling DE412.

**The Thread router role is a deliberate product choice, now written
down.** `CONFIG_OPENTHREAD_FTD=y` is stated in `prj.conf` since this
round. It was already `y` before, but only because `OPENTHREAD_FTD` is
listed first in a Kconfig `choice` nobody set
(`zephyr/modules/openthread/Kconfig.thread:42-51`), which made a 9 KB
decision look like an accident in every config dump. Ruling DE412 keeps
the router role: this device must remain able to route and to parent
children. An MTD build would return roughly 9,121 B of `ot::Instance`
(the child table, router table, address resolver and the MeshCoP leader
and joiner-router state) plus a 4 KB message-pool floor, and is
deliberately not taken. `CONFIG_OPENTHREAD_MAX_CHILDREN=16` is the part
of that cost this round did trim.

### Memory reclaim round B (2026-08-31)

Round B is the cross-cutting half: one item in the SHARED `core/`, which the
ESP32-C6 firmware compiles too, and one in the SDK, which needed a patch
mechanism on this arm. It returns 15,632 B on `ophelia_cpico` and 15,624 B on
`nrf54l15dk` and, like round A, changes no AT command, URC, error code or
seed. Pristine builds, 2026-08-31:

| | Batch 8 (`abe72e7`) | Memory reclaim B |
|---|---|---|
| RAM used, `ophelia_cpico` | 231,204 B (88.20%) | **215,572 B (82.23%)** |
| RAM used, `nrf54l15dk` | 231,420 B (88.28%) | **215,796 B (82.32%)** |
| RAM free, `ophelia_cpico` | 30,940 B | **46,572 B** |
| RAM free, `nrf54l15dk` | 30,724 B | **46,348 B** |
| Flash, `ophelia_cpico` | 883,120 B (64.26%) | **884,100 B (64.34%)** |
| Flash, `nrf54l15dk` | 891,012 B (60.93%) | **892,004 B (61.00%)** |

(`abe72e7` is round A plus catalogue batch 8, which is why the baseline reads
744 B above the round A row in the table above it. Same measurement, one
merge later.)

Two items, `.bss` measured per symbol with `nm -S`:

| Item | `.bss`, `ophelia_cpico` |
|---|---|
| The two row stages leave `.bss` | **-11,208 B** |
| The EEM measurement table becomes an instance pool | **-4,439 B** |

`.bss` falls 15,647 B in total, `datas` rises 20 B and `noinit` does not
move; the linked RAM span falls 15,632 B, the difference being section
alignment. Flash rises 980 B.

1. **The row stages leave `.bss`, and WHICH heap they land in is the whole
   of item 1.** `core/mt/mt_at.c` held two `mt_row_stage_t` buffers of
   5,608 B each, measured: `s_row_stage` for the host's own `AT+MTROW` sets
   and `s_row_inbound` for the ones a controller's `SetTargets` brings in.
   The host's is now allocated when a staging session opens and released
   when it ends; the fabric's is committed once, when a composition declares
   an endpoint whose fabric commands carry rows, which on this platform
   means an Energy EVSE. That was never, when this round shipped and the
   EVSE was out of tier (DE408); the EVSE round below took the device type
   back with a cap, and `mt_matter_evse_reserve()` is what commits the
   fabric stage now.

   **The capacity and the wire contract are unchanged**: a host still stages
   `MT_ROW_MAX_ROWS` rows and still gets every error code `AT_MT_SPEC.md`
   3.28 specifies for every input. This is deliberately NOT a compile-time
   removal of the row family on the platforms whose EVSE is out of tier
   (ruling DE419): removing it would have made the reclaim conditional on
   that exclusion and would have had to be undone to take the EVSE back.
   The mechanism is platform-neutral, with no per-platform `#ifdef` in the
   four verb handlers, because `core/` is shared.

   **The memory comes from `hearth_stage_alloc()`, not from `malloc()`, and
   that is the round's one hard-won lesson.** This application is linked
   with `--wrap=malloc` because `CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE=y`, so
   plain `malloc()` resolves to connectedhomeip's own
   `sHeapMemory[CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE]`, 10,240 B, from which the
   entire Matter stack allocates. A 5,608 B session taken that way would
   hold 55% of the stack's working memory for as long as a host left a set
   staged, and exhaustion there is a commissioning failure. Meanwhile the
   `.bss` this round frees goes to the libc arena
   (`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1`, so the arena is every byte of
   SRAM the image does not statically use), which nothing else in the image
   draws on, precisely because every other `malloc` is wrapped away. So the
   port takes the arena by name, through `__real_malloc`, and the round's
   reclaim and the round's spend are the same memory:

   | | `ophelia_cpico` | `nrf54l15dk` |
   |---|---|---|
   | Arena (`_end` to the end of SRAM) | 46,572 B | 46,348 B |
   | With a host staging session live | about 40.9 KB | about 40.7 KB |
   | With both sessions live (needs the EVSE) | about 35.2 KB | about 35.0 KB |

   The arena figure is exact, being a linker span. The two below it are
   deliberately approximate: they are that span less the payload, and a
   `sys_heap` also spends a chunk header and a rounding-up per live block,
   which is on the order of 150 to 200 bytes across two blocks and is not
   worth pretending to know to the byte from a static measurement. Read them
   as "about 35 KB with everything open", which is the number that matters.

   **Nothing else drawing on this arena is a standing condition, not an
   enforced one.** It was checked at the disassembly level: the only callers
   of the unwrapped libc `malloc` and `free` in the image are the hook's two
   functions. But only `malloc`, `calloc`, `realloc` and `free` are wrapped,
   so `aligned_alloc`, `memalign`, `posix_memalign`, `reallocarray`,
   `strdup` and `strndup` all reach this arena directly, and a future caller
   of any of them becomes a silent second tenant that no build gate would
   notice. `port/hearth_port_zephyr.c` carries the full note beside the
   implementation.

   `hearth_port.h` states what any implementation of the hook must satisfy,
   and the C6 backs it with its ordinary allocator, where the general
   internal heap has around 106 KB free at a one-endpoint composition.

   **What this changed on the C6, stated because it is a shipped platform.**
   The fabric staging buffer is committed at composition time there rather
   than allocated per forward, which is what keeps `SetTargets`' answers on
   the Matter wire exactly what they were. The cost is that the failure
   moved rather than disappeared: a composition declaring an EVSE now takes
   **5,608 bytes of the C6's heap permanently**, from the boot rebuild until
   the next reboot, and a composition that cannot get them fails at
   `mt_matter_evse_reserve()` like any other exhausted pool. Against about
   106 KB free at one endpoint and about 47 KB at twenty-eight that is
   affordable, and a composition with no EVSE pays nothing where it used to
   pay 5,608 bytes on every image. It is still a change, and the C6's
   standing heap figures should be re-measured with and without an EVSE
   before they are quoted again.

2. **The EEM measurement table, via an SDK patch.** `gMeasurements` in
   connectedhomeip's `ElectricalEnergyMeasurementCluster.cpp` is indexed by
   `emberAfGetClusterServerEndpointIndex()`, so it is declared as long as the
   whole dynamic endpoint space and charged in full the moment the cluster
   enters the build: 17 x 496 = 8,432 B measured, whether or not a
   composition ever declares an energy endpoint. The endpoint-block technique
   cannot reach it, because the indexing is inside the SDK. The patch caps it
   at `CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES` and claims
   slots by endpoint id: 3,968 B plus a 24 B claim table, so 4,440 B of
   `.bss` by symbol, against one byte for the port's own reserve counter.

   **The pool is 8 because `MT_MEAS_MAX` is 8.** A pool below this port's own
   measurement capacity would let a composition be accepted and then not
   served: past the pool the SDK answers `nullptr`, the port logs and
   continues, `AT+MTEPAPPLY` still says `OK`, and a controller then gets a
   Failure on the MANDATORY `Accuracy` attribute of a cluster the endpoint
   advertises. Before the patch that could not happen. So the pool is sized
   from capacity, `mt_matter_eem_reserve()` claims a slot before the endpoint
   is built and stops the rebuild naming the macro (what every other pool
   here does), and a `static_assert` keeps the two from drifting apart.

### The EVSE round (2026-08-31)

The last device type, and the only round whose cost is one object rather than
a table. Pristine builds, 2026-08-31:

| | Memory reclaim B (`7f69b44`) | EVSE round |
|---|---|---|
| RAM used, `ophelia_cpico` | 215,572 B (82.23%) | **220,548 B (84.13%)** |
| RAM used, `nrf54l15dk` | 215,796 B (82.32%) | **220,780 B (84.22%)** |
| RAM free, `ophelia_cpico` | 46,572 B | **41,596 B** |
| RAM free, `nrf54l15dk` | 46,348 B | **41,364 B** |
| Flash, `ophelia_cpico` | 884,100 B (64.34%) | **896,872 B (65.26%)** |
| Flash, `nrf54l15dk` | 892,004 B (61.00%) | **904,772 B (61.87%)** |

RAM `+4,976 B` on `ophelia_cpico` and `+4,984 B` on the DK, per symbol with
`nm -S`:

| Item | `.bss` |
|---|---|
| `HEARTH_OBJ_HEAP_BYTES` 7168 to 11264 | **+4,096 B** |
| `s_evse_blob`, the one encode/decode scratch buffer | **+856 B** |
| the pool table and its two counters | +16 B |

and 8 bytes of section alignment. Flash `+12,772 B` is the
`energy-evse-server` translation unit, the port's own delegate and store, and
the const catalogue rows; `EnergyEvseMode` rides the already-compiled
mode-base server. `HEARTH_EP_HEAP_BYTES` is unchanged at 8 KB, which is the
whole point of the cap: the EVSE's 1,144 B block is the catalogue's widest,
and two of them fit comfortably where eight would not have.

**The object heap is the round's real cost, and 4,096 B of it buys margin
rather than capability.** 2,024 B of the raise is the two pool slots
themselves; the rest is what keeps this heap from ever being the wall a
composition hits first, which is the property ruling DE413 chose and this
round preserves.

**What is still not proven:** every figure above is a build measurement. The
charging-target store's persistence path (Zephyr settings over ZMS, an
856-byte blob per endpoint) has not been exercised on hardware in this round,
and neither has the row apply path end to end. Both are bench work.

**The patch mechanism is `west patch`, documented in
`sdk-patches/README.md`.** It is not this project's first: the C6 arm has
carried `platform/esp32c6/sdk-patches` with its own apply script since the
combined-image round, and this directory copies its layout deliberately.
Patches live in this repository, not in the workspace; two commands apply and
remove them; and `CMakeLists.txt` REFUSES TO CONFIGURE against a tree that is
unpatched or patched at the wrong REVISION. That refusal is the load-bearing
part, and checking the revision rather than mere presence is the point of it:
the patch defaults to stock behaviour when its macro is unset, which is what
makes it upstreamable and is exactly what makes a missing or stale patch
invisible. Read `sdk-patches/README.md` before an SDK bump; `west update`
does not carry a patch forward.

**What this means for the EVSE.** Ruling DE408 put the Energy EVSE in the
LM20 tier; the round after this one took it back with a cap, so this
paragraph is the arithmetic that made that possible rather than a forecast.
The 11,216 B of row machinery is no longer standing cost on a platform that
never stages a row, and the two buffers an EVSE composition needs, host
staging and fabric staging live at once, come out of the arena rather than
out of `.bss`. The earlier claim that there is "no row-transfer tax to pay
first" was wrong in an important way and is withdrawn. There is a tax, it is
11,216 B, and it is now paid out of the pool with the most room in it rather
than reserved on every boot.

**The arena, re-measured after the EVSE round**, since that round spent
4,976 B of it on the object heap and the blob buffer:

| | `ophelia_cpico` | `nrf54l15dk` |
|---|---|---|
| Arena (`_end` to the end of SRAM) | 41,596 B | 41,364 B |
| An EVSE composition loaded (the fabric stage committed) | about 35.1 KB | about 34.9 KB |
| ... plus a host staging session open | about 29.5 KB | about 29.3 KB |

The arena figure is exact, being a linker span. The two below it are
deliberately approximate: they are that span less the 5,608-byte payloads,
and a `sys_heap` also spends a chunk header and a rounding-up per live block,
on the order of 150 to 200 bytes across two blocks. Read the last row as
"about 29.5 KB with everything open", which is the number that matters.

## Dev board wiring

The CPico RP2350 dev board carries the module and hosts the bridging firmware. This table is the soldering contract: a deviation means editing both the board `pinctrl` file and the sketch defines together.

| Line | From (CPico) | To (Ophelia-IV module) | Signal |
|---|---|---|---|
| Serial TX | GP0 | P1.15 (UARTE20 RX) | CPico transmit line |
| Serial RX | GP1 | P1.04 (UARTE20 TX) | CPico receive line |
| Reset | GP2 | nRESET pad | Active-low reset (asserted by host) |
| Recovery strap | GP3 | P2.03 (GPIO, pull-up) | Active-low strap sampled at boot |
| Console TX | P0.00 (console TX) | Debug Probe UART RX | Module console output |
| Debug data | SWDIO, SWDCLK pads | Debug Probe SWD | SWD programming interface |
| Power | 3V3, GND | Module supply | 3.3 V and ground |

The Recovery strap and nRESET lines are driven high-impedance (pulled up externally) when the host releases them. The Debug Probe is a Raspberry Pi Debug Probe or CMSIS-DAP compatible.

## One-time SWD install

Before any UART flashing, the MCUboot bootloader is installed over SWD. Do this once per module.

**NCS toolchain activation** (required for `west` commands on this machine).
The platform builds against NCS v3.3.4 (Matter 1.5.0 in the bundled CHIP
tree) since 2026-08-28; the module's installed bootloader was built with
v3.0.2 and stays, since the MCUboot image format and SMP recovery protocol
are stable across the bump:

```bash
TC=$HOME/ncs/toolchains/911f4c5c26
export PATH="$TC/bin:$TC/usr/bin:$TC/usr/local/bin:$TC/opt/bin:$TC/opt/nanopb/generator-bin:$TC/nrfutil/bin:$TC/opt/zephyr-sdk/arm-zephyr-eabi/bin:$TC/opt/zephyr-sdk/riscv64-zephyr-elf/bin:$PATH"
export LD_LIBRARY_PATH="$TC/lib:$TC/lib/x86_64-linux-gnu:$TC/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GIT_EXEC_PATH="$TC/usr/local/libexec/git-core"
export GIT_TEMPLATE_DIR="$TC/usr/local/share/git-core/templates"
export PYTHONHOME="$TC/usr/local"
export PYTHONPATH="$TC/usr/local/lib/python3.12:$TC/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$TC/opt/zephyr-sdk"
export ZEPHYR_BASE="$HOME/ncs/v3.3.4/zephyr"
```

CAUTION: this PYTHONHOME/PYTHONPATH breaks ordinary system Python in the same shell. Use a dedicated shell or subshell for west builds.

**Apply the SDK patches** (once per workspace, and again after every `west
update`). This platform carries patches against the pinned NCS tree; the
build refuses to configure without them and prints this command. See
`sdk-patches/README.md` for what they do and how to take them back out.

```bash
PATCHES=$PWD/sdk-patches
cd $ZEPHYR_BASE/.. && west patch -l $PATCHES/patches.yml -b $PATCHES apply
```

**Build the bootloader and app**:

From `platform/nrf54l15/`, with the toolchain activation applied:

```bash
west build -b ophelia_cpico/nrf54l15/cpuapp --pristine --sysbuild -- -DZEPHYR_BASE=$ZEPHYR_BASE -DBOARD_ROOT=$PWD
```

This produces two artifacts under `build/`:
- `mcuboot/zephyr/zephyr.hex`: the locked bootloader (installed once via SWD)
- `nrf54l15/zephyr/zephyr.signed.bin`: the signed application image

**Partition layout** (1428 KB flash_primary, carved by NCS Partition Manager):

| Partition | Size | Purpose |
|---|---|---|
| `mcuboot` | 48 KB | Locked bootloader |
| `mcuboot_pad` | 2 KB | Padding to align app slot |
| `app` | 1342 KB | Primary slot: signed application image |
| `settings_storage` | 32 KB | ZMS: heartbeat KV store and Matter persistence |
| `factory_data` | 4 KB | Reserved for production factory data |

**Install via SWD** (pyocd 0.44.1, requires the Debug Probe soldered to the module):

```bash
pyocd flash -t nrf54l build/mcuboot/zephyr/zephyr.hex
pyocd flash -t nrf54l build/nrf54l15/zephyr/zephyr.signed.bin
```

The pyocd tool is the SWD path for this platform. Recovery from any state (locked or corrupted) uses `pyocd erase -t nrf54l --chip` followed by the flash commands above: no serial port can reach a module with an invalid bootloader.

## Everyday flashing

2026-08-27: since the bootloader round, `sysbuild.conf` unconditionally sets `SB_CONFIG_BOOTLOADER_MCUBOOT=y`, so any `west build` from this directory pulls in MCUboot by default, sysbuild flag or not (there is no `.west/config` override). The stock `nrf54l15dk/nrf54l15/cpuapp` overlay used for core-sanity builds has no `mcuboot-button0` alias (only `ophelia_cpico`'s own dts wires the recovery strap to it), so MCUboot's serial-recovery build fails there with `#error "Serial recovery/USB DFU button must be declared in device tree as 'mcuboot_button0'"`. Build DK core-sanity targets with `--no-sysbuild` until the DK overlay gains recovery-button wiring or `sysbuild.conf` becomes board-conditional.

After SWD install, all updates go over UART via the serial recovery mechanism. Flashing is driven by the host's `fw/flash.py` script:

```bash
python3 fw/flash.py --image build/nrf54l15/zephyr/zephyr.signed.bin \
  --port $(ls /dev/serial/by-id/usb-*CPico*-if00 2>/dev/null || echo CHOOSE-BY-ID)
```

The `--port` flag **must use `/dev/serial/by-id`**, never `/dev/ttyACM<n>`. The reason: ttyACM device numbering changes whenever USB devices are plugged/unplugged; on this bench, ttyACM0 is the Thread RCP and a stray write to the wrong device kills `otbr-agent`. The by-id convention gives a stable, device-unique path that does not move.

When the flash succeeds, the application starts and prints `+MTREADY` on the same UART within a few seconds; its presence is the flasher's success criterion.

The bridge exposes nRESET and the recovery strap via CDC control signals: CDC DTR asserted holds the module in reset, and RTS asserted drives the recovery strap (released lines let it run). Stock terminal programs (picocom, minicom) assert DTR and often RTS on open, so a terminal opened with defaults silently holds the module in reset or drops it into recovery; open with DTR and RTS cleared to talk to the running application.

Watching the console: open the Debug Probe's CDC with DTR asserted.
Like the bridge's own USB stack, the probe's CDC discards output while
the host holds DTR low, so a reader that clears DTR sees permanent
silence from a perfectly healthy console (half a bench day, once).

## Recovery semantics

`CONFIG_BOOT_SERIAL_NO_APPLICATION` means the recovery strap is not the only way into serial recovery. A unit whose slot-0 image fails signature validation drops into serial recovery on its own at boot, with no strap required. Physical access to a unit in that state, plus a corrupted image, is therefore enough to reach an unauthenticated SMP flash-write port: no strap, no credentials, wire access alone. Signed-boot still refuses to run unsigned code, so a write does not equal a compromise of the running application; this is a deliberate availability-over-lockdown choice, appropriate for a module whose SWD pads are also exposed on test points (SWD access alone already grants at least as much).

One consequence: a successful SMP handshake through `flash.py --enter-only` does not by itself prove the strap path works. The same handshake succeeds whether the strap forced entry or the unit simply had a bad slot-0 image and fell into recovery on its own. Demonstrating the strap path specifically requires taking down a **running** application (a good image, already booted) with the strap held, and observing recovery entry despite that.

Another consequence: a field unit that ends up with a bad slot-0 image and no operator present to reflash it sits in serial recovery indefinitely. There is no timeout back to any other state; recovery is where it stays until someone flashes it.

## Data model regeneration

The Matter data model (endpoint 0 root node, plus the disabled catalogue
endpoint at id 240 carrying the union of milestone clusters) is
`src/default_zap/hearth.zap`, edited by hand as JSON, with the generated
`.matter` file and `zap-generated/` sources checked in next to it. To
regenerate after editing the `.zap` file:

```bash
cd ~/ncs/v3.3.4   # west zap-generate is an NCS workspace extension
west zap-generate -z $FW/platform/nrf54l15/src/default_zap/hearth.zap
```

`hearth.zap` stores its ZCL/template `package` paths as the ones the
original sample (`light_bulb.zap`, the starting point for this file) had
relative to its own location under `nrf/samples/matter/`; copied into this
repository those relative paths resolve outside the NCS tree entirely. The
`package` entries at the top of `hearth.zap` are pinned to absolute paths
under `~/ncs/v3.3.4/modules/lib/matter/...` for that reason; keep them
pinned there after any hand-edit or regeneration on this machine.

A zap edit that adds a cluster also needs a CMake reconfigure before the
next build: `zap_cluster_list.py` runs at CMake configure time
(`chip_data_model.cmake:40-56`), so an incremental `west build` after the
edit never compiles the new cluster's server sources and the link fails
on its `Matter*PluginServerInitCallback`. Run `cmake build/nrf54l15` (or
build pristine) after any cluster-list change; batch 4 paid for this once
(`fbe0a35`).

## Keys

Signing keys live in `keys/`. The DEVELOPMENT key (`hearth_dev_p256.pem`) is committed to the repository and deliberately shared; it protects nothing and ensures every dev build exercises the signature verification path. The PRODUCTION key is generated offline, never committed, and swapped in at fixture time to sign release images. See `keys/README.md` for the production key swap procedure.

## Toolchain note

This platform does not use the nrfutil toolchain-manager tool that appears in Nordic's documentation. Instead, the NCS activation block above configures the exact toolchain directory on this machine. Use that activation block before any `west` command; use a clean shell for `pyocd` and host Python tests.

## Matter commissioning on the bench

The milestone acceptance (2026-08-28) commissions with the CLI chip-tool
from the bench host over BLE into the live OTBR fabric:

```bash
# open the window over AT (AT+MTCOMMISSION), then, in a clean shell:
./fw/srp-aaaa-shim.sh &     # see the header comment: OTBR mDNS workaround
chip-tool pairing ble-thread <node-id> hex:<dataset> 20202021 3840
```

Dev credentials only (test VID 0xFFF1 / PID 0x8000, SPAKE2 passcode
20202021, discriminator 3840): consumer hubs are expected to refuse
them. The Thread dataset comes from the border router
(`ot-ctl dataset active -x`) and is a credential: never commit or log
it. The `srp-aaaa-shim.sh` workaround is required until the upstream
ot-br-posix mDNS host-update defect it documents is fixed.

Radio note: the module's 32 MHz HFXO needs the SoC-internal load
capacitors programmed (board DTS `&hfxo`); without them the radio is
silent while everything else runs. Bench scripts must wait for +MTREADY
after opening the AT port (the bridge's DTR line pulses reset on open).

Measured 2026-08-28 (build/, dev/nrf-matter-core at 5c19776 + hfxo fix):
app image 753,691 B of the 1,374,208 B slot (54.9%); settings_storage
raw occupancy 30,476 of 32,768 B non-erased after a day of commissioning
churn (ZMS is log-structured, stale entries count until collection; the
32 KB sizing watch item from the design spec stays open).
