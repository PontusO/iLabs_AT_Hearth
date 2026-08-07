# Seven-type batch design: the pattern-replication remainder

Date: 2026-08-07. Adds the seven device types reachable by replicating
shipped mechanisms: Water Valve 0x0042, Mode Select 0x0027, Chime
0x0146, Smoke/CO Alarm 0x0076, Laundry Washer 0x0073, Dishwasher
0x0075, Laundry Dryer 0x007C, plus Power Source 0x0011 as a supporting
row. Firmware 30 to 38 rows; library seven Hearth-original classes
(31 to 38) with FullAPI examples. Versions 0.5.0 both repos. Survey
record: graph D149; SDK dossiers in the session ledger.

## 1. Decisions taken with the user (2026-08-07)

- All seven in one round; Power Source rides along as row 38 because
  the smoke alarm's device-type mandate is node-scoped and a FLAT
  sibling endpoint satisfies it: no composition tree needed.
- The +MTCMD frame gains a notify-only form (seq 0, no answer
  expected) for void fire-and-forget callbacks.
- Valve verdicts cannot fail the command on the wire (SDK ignores the
  delegate returns): documented, not fought; the verdict gates host
  actuation only.
- Smoke-alarm state moves through an event-emitting bridge command,
  never raw attribute writes (the setters fire the spec events and
  auto-unmute; raw writes silently skip both).
- Chime stays in scope despite carrying an SDK-gap workaround (see
  F6): the gap is recorded for the I90 upstream ride-along.

## 2. SDK facts this design is built on (dossier, 2026-08-07)

- F1 Valve: Delegate has exactly three pure virtuals
  (HandleOpenValve returning Nullable<Percent>, HandleCloseValve
  returning CHIP_ERROR, HandleRemainingDurationTick void, delegate.h:
  34-42); the server calls Open/Close synchronously but IGNORES their
  returns and answers Success (cluster.cpp:361,303 with
  TEMPORARY_RETURN_IGNORED). App-side reporting:
  UpdateCurrentState/UpdateCurrentLevel emit ValveStateChanged. No
  VALIDATE trap. RemainingDuration is managed-internally; every other
  valve attribute is plain.
- F2 Mode Select: one GLOBAL SupportedModesManager dispatching on
  endpoint id; the provider is two raw pointers over contiguous
  ModeOptionStructs, nothing requires static/NVS backing, so a
  runtime host-fed store is legal. SemanticTags is mandatory but
  min-count-free: empty lists are legal. ChangeToMode validates
  membership and sets CurrentMode itself: no app hook. SupportedModes
  managed-internally; CurrentMode/StartUpMode plain.
- F3 Chime: delegate mandatory at create (create() returns NULL on a
  null delegate: a trap by pointer, not by feature). PlayChimeSound is
  truly synchronous and its Status IS the command response: a real
  verdict point. GetChimeSoundByIndex/GetChimeIDByIndex feed
  InstalledChimeSounds, temp-levels shape. All three attributes
  managed-internally; SelectedChime is set through
  ChimeCluster::SetSelectedChime.
- F4 Smoke/CO: exactly one mandatory link symbol,
  emberAfPluginSmokeCoAlarmSelfTestRequestCommand (void, no weak
  default: undefined = link failure). HandleRemoteSelfTestRequest
  replies Success BEFORE the physical test; completion is app-driven
  via SetTestInProgress(endpoint, false), which fires
  SelfTestComplete; no SDK timer exists. Eleven Set* methods on the
  SmokeCoAlarmServer singleton are the event-emitting state surface;
  the underlying attributes are plain ember storage (raw writes work
  but skip events and the critical-alarm auto-unmute). Trap:
  VALIDATE_FEATURES_AT_LEAST_ONE(SmokeAlarm, COAlarm)
  (esp_matter_cluster.cpp:2019); SmokeState/COState exist only when
  their feature is added.
- F5 Power Source: endpoint creates the power_source cluster only;
  trap VALIDATE_FEATURES_EXACT_ONE(Wired, Battery)
  (esp_matter_cluster.cpp:982). The battery feature adds
  BatChargeLevel/BatReplacementNeeded/BatReplaceability, all plain.
  BatPercentRemaining exists as a creator
  (attribute::create_bat_percent_remaining, plain nullable) but no
  endpoint path calls it: the thunk adds it by hand. No delegate
  anywhere.
- F6 Chime SDK gap: ESPMatterChimeClusterServerInitCallback (the only
  thing that constructs and registers the ChimeCluster server object)
  has NO call site in the entire pinned tree; chime::create() wires
  only the delegate-stash. Without a manual call the cluster serves
  nothing. Workaround: the firmware calls the integration init itself
  in the established pre-start registration window. Recorded for I90.
- F7 OperationalState: Delegate has seven pure virtuals
  (GetCountdownTime, GetOperationalStateAtIndex,
  GetOperationalPhaseAtIndex, HandlePause/Resume/Start/Stop
  StateCallback filling a GenericOperationalError by reference,
  server.h:279-372); the handlers run inline and the filled error IS
  the command response. One delegate OBJECT per endpoint is mandatory
  (SetInstance VerifyOrDies on sharing); esp-matter's own
  DelegateInitCB news one Instance per endpoint into a map. PhaseList
  null is legal and code-confirmed (CHIP_ERROR_NOT_FOUND at index 0
  encodes null). ALL SIX attributes are managed-internally: zero are
  reachable by generic AT+MTATTR; state moves only through Instance
  setters (SetOperationalState validates and rejects kError, which is
  reserved for OnOperationalErrorDetected). Washer, dishwasher and
  dryer all wire the same base cluster 0x0060 with identical add()
  bodies plus the OperationCompletion event.
- F8 Kconfig lifts required (all default y, all =n in
  sdkconfig.defaults today): CHIME, MODE_SELECT, OPERATIONAL_STATE,
  POWER_SOURCE, SMOKE_CO_ALARM, VALVE_CONFIGURATION_AND_CONTROL.
  The =n removes the CHIP source directory from the build outright.

## 3. The +MTCMD notify-only form (firmware)

- `+MTCMD:0,<ep>,<cluster>,<command>`: seq 0 (never issued by the
  mailbox, reserved since C1 of the forwarding round) means no answer
  is expected and none is accepted; AT+MTCMDRESP=0,... answers
  +MTERR:1. The CHIP task does NOT block: mt_cmd_notify() formats,
  raises through mt_at_urc(), returns. First consumer: SelfTestRequest.
- AT_MT_SPEC 3.17 documents the form; the library treats seq 0 as
  dispatch-without-reply (no MTCMDRESP enqueued).

## 4. New AT surfaces (firmware)

All four follow the established conventions: set-only unless noted,
decimal fields, +MTERR:1 grammar/value, +MTERR:2/3 lookup semantics
via attr_err_to_mterr, bridges under ChipStackLock, handlers in
mt_at.c with C-linkage bridges in mt_matter.h.

- `AT+MTVALVE=<ep>,<state>[,<level>]`: state 0 Closed / 1 Open /
  2 Transitioning (ValveStateEnum wire values); bridges to
  UpdateCurrentState and, when level present (0..100), also
  UpdateCurrentLevel. The host reports actuation reality; events emit
  server-side.
- `AT+MTMODES=<ep>,<mode>,"<label>"[,<mode>,"<label>"...]`: 1..8
  pairs; mode u8, unique within the list; labels 1..32 printable
  ASCII, no double quote (the cluster caps labels at 64; 32 keeps the
  line bounded); replaces the endpoint's mode list in the host-fed
  store; SemanticTags empty. Not persisted (re-sent per boot, the
  MTTEMPLEVELS philosophy). The store must outlive reads: static
  per-endpoint arrays, contiguous, temp-levels sizing discipline.
- `AT+MTOPSTATE=<ep>,<state>`: state per OperationalStateEnum wire
  values Stopped 0 / Running 1 / Paused 2 (kError 3 rejected with
  +MTERR:1: reserved for the error path, F7); bridges to
  Instance::SetOperationalState via the per-endpoint registry.
- `AT+MTALARM=<ep>,<field>,<value>`: field selects the
  event-emitting setter: 0 ExpressedState is NOT settable (derived),
  fields are 1 SmokeState, 2 COState, 3 BatteryAlert, 4 DeviceMuted,
  5 TestInProgress, 6 HardwareFaultAlert, 7 EndOfServiceAlert,
  8 InterconnectSmokeAlarm, 9 InterconnectCOAlarm,
  10 ContaminationState, 11 SmokeSensitivityLevel; values are the
  enum/bool wire values, validated per field against the header enums
  (never transcribed: accessors or bridge-side validation). Maps to
  the eleven SmokeCoAlarmServer setters; TestInProgress false after a
  self-test forward is the completion path (F4).
- Event mask: no new +MTEVT codes; the forwards are +MTCMD URCs.

## 5. Firmware rows and wiring

- Rows 31-38: water_valve (thunk allocates its per-endpoint delegate
  from a static pool and passes it in config, F1), mode_select
  (delegate = the single global manager object), chime (delegate from
  a static pool; create aborts without it, F3), smoke_co_alarm (trap:
  feature_flags = smoke_alarm | co_alarm, both features so both
  states exist), dish_washer / laundry_washer / laundry_dryer (each
  thunk allocates its own OperationalState delegate object from a
  shared pool: one class, per-endpoint objects, F7), power_source
  (trap: feature_flags = battery; thunk hand-adds
  create_bat_percent_remaining after create, F5). All max_variant 0.
- The pre-start registration window (B139 precedent) runs: the chime
  integration init per chime endpoint (F6 workaround, with the I90
  note in the comment); nothing else needs manual construction
  (valve/mode-select/opstate wire through create-time delegates).
- +MTCMD consumers registered: valve cluster 0x0081 Open 0 / Close 1
  (verdict gates actuation only, F1: the reply is documentation);
  chime 0x0556 PlayChimeSound 0 (verdict IS the wire response);
  operational_state 0x0060 Pause 0 / Stop 1 / Start 2 / Resume 3
  (verdict maps kNoError / kUnableToCompleteOperation);
  smoke_co_alarm 0x005C SelfTestRequest 0 notify-only. All ids
  verified against the pinned headers at implementation time, never
  transcribed.
- Kconfig: the six F8 lifts, with materialized sdkconfig handling per
  the established procedure.
- Docs: AT_MT_SPEC sections for the four commands + the notify-only
  form + 3.9 to 38 rows; ARCHITECTURE decision record (the
  return-ignored valve fact, the chime SDK gap, the per-endpoint
  delegate pools); README table 38. MT_FW_VERSION "0.5.0".

## 6. Library (seven Hearth-original classes)

House recipe throughout (hearthDeclare, cache-on-OK, URC dispatch,
re-begin refusal, MockStream TDD with the deferred-delivery mode where
ordering matters, FullAPI example per class in the same round):

- MatterWaterValve: begin(); onOpen(cb)/onClose(cb) returning bool
  (the verdict; documented that deny does not fail the command on the
  wire, it only withholds actuation); setValveState(state[, level])
  over AT+MTVALVE; getters URC-fed.
- MatterModeSelect: begin(); setSupportedModes(pairs) over AT+MTMODES
  (re-sent per reconcile, B120 norm); setCurrentMode/getCurrentMode
  over plain ATTR; onChangeMode (controller-writable CurrentMode).
- MatterChime: begin(); onPlayChime(cb) returning bool (real wire
  verdict); setInstalledChimeSounds(names) host-fed;
  setSelectedChime/getSelectedChime (bridge-backed, managed-internal);
  setEnabled/getEnabled.
- MatterSmokeCOAlarm: begin(); onSelfTest(cb) void (notify-only;
  the sketch runs the test and calls completeSelfTest());
  completeSelfTest() = AT+MTALARM TestInProgress false; setters for
  the eleven fields over AT+MTALARM with enum constants
  (header-verified transcriptions); getters cached.
- MatterLaundryWasher / MatterDishwasher / MatterLaundryDryer: one
  shared implementation core (a common base or duplicated per the
  clone convention, implementer's call with reviewer gate);
  onPause/onResume/onStart/onStop(cb) returning bool;
  setOperationalState over AT+MTOPSTATE; getOperationalState cached.
- MatterPowerSource (supporting class, counts as infrastructure not
  one of the seven): begin(); setBatChargeLevel/setBatPercentRemaining
  (x2 units halves per Matter), setBatReplacementNeeded over plain
  ATTR. Its FullAPI example composes it beside the smoke alarm.
- library.properties 0.5.0; keywords; README Hearth originals + the
  38-row narrative; FullAPI examples for all eight classes (the
  banner-as-checklist convention).

## 7. Verification

- Host suites both repos at every boundary; the +MTCMD notify-only
  form gets mailbox tests (seq 0 never blocks, MTCMDRESP=0 rejected)
  and library dispatch tests (no reply enqueued).
- Bench (one session; scripted hub power cuts available if needed):
  compose the full risk set (valve + mode select + chime + smoke
  alarm + power source + one opstate type + 0x0100); boot rebuild
  survives (three new traps live: smoke features, power EXACT_ONE,
  chime null-delegate); per-command grammar matrices; commissioned
  phase: chip-tool reads SupportedModes and InstalledChimeSounds
  (host-fed lists verbatim), ChangeToMode round trip, valve
  Open with host actuation report and ValveStateChanged event, chime
  PlayChimeSound allow AND deny (the one verdict visible on the
  wire), opstate Pause/Resume/Start/Stop with allow and deny, smoke
  alarm self-test end to end (notify URC, host completes,
  SelfTestComplete event read), AT+MTALARM SmokeState Warning with
  the SmokeAlarm event read controller-side; library smoke for two
  representative classes; regression pins for MTTEMPLEVELS, +MTCMD
  answered form, MTLOCK; restore.

## 8. Out of scope, recorded

- OperationalState phases, countdown, and OnOperationalErrorDetected:
  v2 surface once a consumer needs them (PhaseList ships null, F7).
- Chime upstream fix: the missing init call site rides the I90 list.
- StartUpMode/OnMode on Mode Select (plain attrs; hosts can drive
  them over raw ATTR today; class setters when asked for).
- The derived OperationalState clusters (RVC, Oven): different
  cluster ids, HEAVY tier.
- Composition trees and the energy family: unchanged from D149's
  ledger.
