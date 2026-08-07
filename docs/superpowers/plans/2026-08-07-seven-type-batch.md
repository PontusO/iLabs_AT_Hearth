# Seven-Type Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Seven pattern-replication device types plus the Power Source
supporting row (firmware rows 31-38), four new AT commands, the +MTCMD
notify-only form, seven Hearth-original library classes plus
MatterPowerSource with FullAPI examples, version 0.5.0 both repos.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-07-seven-type-batch-design.md`), whose
section 2 facts F1-F8 are binding. Six firmware tasks (one mechanism
each, reviewable in isolation), two library class tasks, one library
integration task, bench. Every mechanism replicates a shipped pattern:
the B139 registration window and delegate pools, the MTTEMPLEVELS
store shape, the +MTCMD frame, and the AT+MTLOCK-style reporting
bridges.

**Tech Stack:** esp-matter 21aa3d1 / CHIP b87051a9 pinned, C/C++
firmware, MockStream host tests on arduino-pico.

## Global Constraints

- **No em dashes** anywhere. mt_at.c stays C, no SDK headers; bridges
  hold ChipStackLock; the +MTCMD responder and mailbox rules from the
  forwarding round stand unchanged.
- The spec's F-facts govern; where a header contradicts the plan, the
  header wins and the report explains (established rule). SDK names
  verified with file:line quotes; the firmware never transcribes enum
  values (accessors or bridge-side validation); the library
  transcribes with quoted header evidence.
- New wire surfaces, exact grammars from the spec (all set-only,
  decimal fields, +MTERR:1 grammar/value, lookup codes via
  attr_err_to_mterr):
  - `+MTCMD:0,<ep>,<cluster>,<command>` notify-only: seq 0 reserved,
    never blocks, `AT+MTCMDRESP=0,...` answers +MTERR:1.
  - `AT+MTVALVE=<ep>,<state>[,<level>]` state 0/1/2, level 0..100.
  - `AT+MTMODES=<ep>,<mode>,"<label>"[,...]` 1..8 pairs, mode u8
    unique in list, labels 1..32 printable ASCII no double quote,
    not persisted.
  - `AT+MTOPSTATE=<ep>,<state>` state 0/1/2; 3 (Error) +MTERR:1.
  - `AT+MTALARM=<ep>,<field>,<value>` fields 1..11 per the spec's
    table; values validated per field.
- Trap thunks: smoke_co_alarm feature_flags = smoke_alarm | co_alarm
  (both, so both State attributes exist); power_source feature_flags =
  battery (EXACT_ONE trap) + hand-added
  create_bat_percent_remaining; chime aborts create on null delegate
  (the pool pointer IS the trap satisfaction).
- Kconfig lifts (F8): CHIME, MODE_SELECT, OPERATIONAL_STATE,
  POWER_SOURCE, SMOKE_CO_ALARM, VALVE_CONFIGURATION_AND_CONTROL; the
  materialized-sdkconfig procedure applies; any further lift needs
  linker evidence.
- All three firmware images green at every firmware task end;
  patch-check applied x2; host suites green both repos at every
  boundary.
- Library on branch `seven` in
  /home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth (Dropbox:
  in place, never symlink). TDD with genuine red; decimal IDs in
  expect(); per-task .gitignore lines; no binaries; NEW commits never
  --amend; 2-space /* */; FullAPI examples follow the
  banner-as-checklist convention; daemon cleanup by PID via
  ps -eo pid,comm (pkill -f self-matches).
- MT_FW_VERSION "0.5.0"; library.properties 0.5.0.
- Commit messages explain why and end exactly with:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task C1: The +MTCMD notify-only form

**Files:**
- Modify: `main/mt_at.c` (mt_cmd_notify + MTCMDRESP seq-0 rejection),
  `main/include/mt_at.h` (declaration), `docs/AT_MT_SPEC.md` (3.17
  gains the form)
- Test: `test/host/test_mt_cmdbox.c` (seq-0 contract additions)

**Interfaces:**
- Consumes: the C1-forwarding-round mailbox (mt_cmdbox seqs start at
  1; seq 0 has never been issued).
- Produces: `void mt_cmd_notify(uint16_t ep, uint32_t cluster,
  uint32_t command);` formats `+MTCMD:0,<ep>,<cluster>,<command>`,
  raises via mt_at_urc(), returns immediately; no mailbox slot, no
  semaphore, callable from the CHIP task. ALSO produces the reserved
  fifth field: `bool mt_cmd_forward_payload(uint16_t ep, uint32_t
  cluster, uint32_t command, uint32_t payload);` identical to
  mt_cmd_forward but formats `+MTCMD:<seq>,<ep>,<cluster>,<command>,
  <payload>` (the forwarding spec reserved this exact extension:
  existing parsers ignore a fifth field). First consumer: chime's
  PlayChimeSound chimeID (C6/C7).

- [ ] **Step 1: tests first.** test_mt_cmdbox.c: pin that
mt_cmdbox_open never returns 0 (loop a few opens after init; also pin
the first seq is 1) and that mt_cmdbox_answer(0, v) returns -1 in
every state (IDLE, PENDING with another seq). Run red if the seq-0
answer guard is not yet explicit, then implement the guard in
mt_cmdbox.c if needed (answer(0,...) must be -1 even if a hypothetical
slot held seq 0; a one-line seq==0 early return makes it structural).
Also add mt_cmd_forward_payload (shares mt_cmd_forward's whole body
via a common static helper taking an optional-payload flag; only the
snprintf differs). Green.
- [ ] **Step 2:** mt_cmd_notify in mt_at.c: s_at_up guard (silent drop
when link down, matching mt_cmd_forward's fail-closed philosophy),
snprintf the seq-0 line, mt_at_urc(). No FreeRTOS objects touched.
cmd_mtcmdresp needs no change if Step 1's structural guard is in
mt_cmdbox (verify: MTCMDRESP=0,1 must answer +MTERR:1; add the
handler-side seq==0 check only if the mailbox guard alone cannot
guarantee it).
- [ ] **Step 3:** AT_MT_SPEC 3.17: the notify-only paragraph (seq 0
reserved, no answer expected or accepted, first consumer
SelfTestRequest).
- [ ] **Step 4:** build_b4 green, patch-check x2, host suite green,
commit.

### Task C2: Water Valve (row 31, AT+MTVALVE)

**Files:**
- Modify: `main/main.cpp` (delegate class + static pool + bridge),
  `main/include/mt_matter.h` (bridge + +MTCMD consumer note),
  `main/mt_devtypes.cpp` (thunk + row), `main/mt_at.c` (cmd_mtvalve),
  `sdkconfig.defaults` (VALVE lift), `docs/AT_MT_SPEC.md` (3.18+
  section), `docs/TESTING.md` (grammar rows)

**Interfaces:**
- Consumes: `bool mt_cmd_forward(...)` (answered form) from the
  forwarding round.
- Produces: `int mt_matter_valve_state_set(uint16_t ep, uint8_t state,
  int level);` (level -1 = absent; MT_ATTR_ERR_* returns); the
  HearthValveDelegate class pattern later tasks copy (pool of
  MT_COMP_MAX_ENDPOINTS delegate objects handed out by the thunk).

- [ ] **Step 1: the delegate.** HearthValveDelegate : ValveConfigurationAndControl::Delegate
in main.cpp; HandleOpenValve forwards via
mt_cmd_forward(ep, ValveConfigurationAndControl::Id, Open command id)
and returns the level argument unchanged on allow / DataModel::NullNullable
on deny (F1: the return cannot fail the command; comment this);
HandleCloseValve forwards Close and returns CHIP_NO_ERROR either way
(same comment); HandleRemainingDurationTick is an empty body with the
why-comment (one tick per second, nothing to forward). The delegate
must know its endpoint: store it at pool-handout time (the CHIP
Delegate base has no endpoint member: verify and quote; add a member).
- [ ] **Step 2: thunk + row.** mk_water_valve takes a delegate from a
static pool (index by handout order, bounds-checked; on exhaustion
return nullptr so the boot rebuild aborts loudly per policy), sets
config.valve_configuration_and_control.delegate, creates. Row 31,
max_variant 0, ID via get_device_type_id(). Kconfig lift with the
materialized-file procedure.
- [ ] **Step 3: bridge + handler.** mt_matter_valve_state_set:
ChipStackLock; ep/cluster lookups (ERR_ENDPOINT/ERR_CLUSTER); state
via UpdateCurrentState((ValveStateEnum)state); level >= 0 also
UpdateCurrentLevel(level); CHIP_ERROR to MT_ATTR_ERR_FAILED.
cmd_mtvalve: set-only, state 0..2 else +MTERR:1, level optional
0..100 else +MTERR:1, route through attr_err_to_mterr. Register in
s_cmds. +MTCMD consumer ids (Open/Close) verified from the pinned
header and quoted in the report.
- [ ] **Step 4:** docs (AT_MT_SPEC section per the spec's wording
incl the verdict-gates-actuation-only caveat; TESTING grammar rows);
all three images green; patch-check x2; host suite green; commit.

### Task C3: Mode Select (row 32, AT+MTMODES)

**Files:**
- Modify: `main/main.cpp` (HearthSupportedModesManager global + store
  + bridge mt_matter_modes_set), `main/include/mt_matter.h`,
  `main/mt_devtypes.cpp` (thunk + row), `main/mt_at.c` (cmd_mtmodes
  with quote parsing), `sdkconfig.defaults` (MODE_SELECT lift),
  `docs/AT_MT_SPEC.md`, `docs/TESTING.md`

**Interfaces:**
- Consumes: the MTTEMPLEVELS handler's quote-parsing idiom (mt_at.c,
  cabinet round) as the grammar model.
- Produces: `int mt_matter_modes_set(uint16_t ep, const uint8_t
  *modes, const char *const *labels, uint8_t count);` (MT_ATTR_ERR_*;
  ERR_CLUSTER when the endpoint lacks ModeSelect).

- [ ] **Step 1: the manager + store.** One global
HearthSupportedModesManager implementing SupportedModesManager
(getModeOptionsProvider / getModeOptionByMode dispatch on endpoint id
per F2), backed by a static per-endpoint store: up to
MT_COMP_MAX_ENDPOINTS entries x 8 modes x (u8 value + 33-byte label +
a ModeOptionStructType with empty SemanticTags list). The
ModeOptionStructType array per endpoint must stay contiguous and
alive (F2: the provider is two raw pointers); rebuild the structs in
place on every store update, under ChipStackLock. Empty store =
provider with begin==end.
- [ ] **Step 2: thunk + row.** mk_mode_select sets
config.mode_select.delegate = &the global manager (the
ModeSelectDelegateInitCB re-sets the same global every time: harmless
per F2, comment it). Row 32. Kconfig lift.
- [ ] **Step 3: bridge + handler.** mt_matter_modes_set validates
endpoint + cluster presence, copies into the store, rebuilds the
struct array, marks SupportedModes dirty
(MatterReportingAttributeChangeCallback, the cabinet-round call).
cmd_mtmodes: set-only; parse pairs <mode>,"<label>" with the
handler-local quote parser (commas inside labels legal); 1..8 pairs;
mode u8, duplicate mode in the list +MTERR:1; labels 1..32 printable
ASCII no double quote; route bridge result. Register in s_cmds.
- [ ] **Step 4:** docs; note in the spec section that CurrentMode is
plain (readable/writable over AT+MTATTR, controller changes URC out);
all three images green; patch-check x2; host green; commit.

### Task C4: OperationalState trio (rows 33-35, AT+MTOPSTATE)

**Files:**
- Modify: `main/main.cpp` (HearthOpStateDelegate class + pool +
  bridge), `main/include/mt_matter.h`, `main/mt_devtypes.cpp` (three
  thunks + rows), `main/mt_at.c` (cmd_mtopstate),
  `sdkconfig.defaults` (OPERATIONAL_STATE lift), `docs/AT_MT_SPEC.md`,
  `docs/TESTING.md`

**Interfaces:**
- Consumes: mt_cmd_forward (answered form).
- Produces: `int mt_matter_opstate_set(uint16_t ep, uint8_t state);`
  routed to the endpoint's Instance (found via esp-matter's
  per-endpoint map or our own registry: F7's DelegateInitCB news the
  Instance into s_operational_state_instances; check whether esp-matter
  exposes a getter for it; if not, keep our own parallel registry
  filled at thunk time, and say which in the report).

- [ ] **Step 1: the delegate class.** HearthOpStateDelegate :
OperationalState::Delegate. GetCountdownTime returns NullNullable;
GetOperationalStateAtIndex serves the four standard states
(kStopped/kRunning/kPaused/kError as GenericOperationalState entries,
index 0..3, else CHIP_ERROR_NOT_FOUND: verify the exact expected
list contents against another in-tree delegate implementation and
quote); GetOperationalPhaseAtIndex returns CHIP_ERROR_NOT_FOUND at
index 0 (PhaseList null, F7); the four Handle*StateCallback forward
via mt_cmd_forward(ep, OperationalState::Id, <command id>) and fill
err with kNoError on allow / kUnableToCompleteOperation on deny
(verify the error enum name in the header, quote). Per-endpoint
objects from a static pool (F7 VerifyOrDie: one object per Instance),
endpoint stored at handout.
- [ ] **Step 2: three thunks + rows.** mk_dish_washer,
mk_laundry_washer, mk_laundry_dryer: identical bodies except the
namespace (per F7 all three wire base cluster 0x0060): pool handout,
config.operational_state.delegate, create. Rows 33-35. One Kconfig
lift covers all three.
- [ ] **Step 3: bridge + handler.** mt_matter_opstate_set: lock;
lookup; state 3 is rejected in the HANDLER (+MTERR:1, kError reserved,
F7) so the bridge only sees 0..2; Instance::SetOperationalState;
CHIP_ERROR to FAILED. cmd_mtopstate set-only, state 0..2. Register.
- [ ] **Step 4:** docs (incl the PhaseList-null and v2-parking notes);
three images; patch-check; host green; commit.

### Task C5: Smoke/CO Alarm + Power Source (rows 36-37, AT+MTALARM)

**Files:**
- Modify: `main/main.cpp` (emberAfPluginSmokeCoAlarmSelfTestRequestCommand
  + bridge mt_matter_alarm_set + field validation accessors),
  `main/include/mt_matter.h`, `main/mt_devtypes.cpp` (two thunks +
  rows), `main/mt_at.c` (cmd_mtalarm), `sdkconfig.defaults`
  (SMOKE_CO_ALARM + POWER_SOURCE lifts), `docs/AT_MT_SPEC.md`,
  `docs/TESTING.md`

**Interfaces:**
- Consumes: `mt_cmd_notify` from C1.
- Produces: `int mt_matter_alarm_set(uint16_t ep, uint8_t field,
  uint8_t value);` field 1..11 mapping to the eleven
  SmokeCoAlarmServer setters per the spec's table; per-field value
  validation happens IN THE BRIDGE against the header enums (C has no
  access to them); out-of-range value returns MT_ATTR_ERR_VALUE
  (maps +MTERR:1, the C1b precedent).

- [ ] **Step 1: the callback + thunks.**
emberAfPluginSmokeCoAlarmSelfTestRequestCommand: one line,
mt_cmd_notify(endpointId, SmokeCoAlarm::Id, SelfTestRequest id)
(quote the command id header line). mk_smoke_co_alarm: feature_flags =
smoke_alarm | co_alarm (trap comment citing esp_matter_cluster.cpp:2019,
trap number seven in the house series). mk_power_source: feature_flags
= battery (EXACT_ONE trap, cite :982, trap eight); after create,
cluster::get + cluster::power_source::attribute::create_bat_percent_remaining(cl, nullable default null)
per F5 (quote the creator signature). Rows 36-37. Two Kconfig lifts.
- [ ] **Step 2: bridge + handler.** mt_matter_alarm_set: lock; ep +
SmokeCoAlarm cluster lookups; switch(field) to the eleven setters
(SetSmokeState..SetSmokeSensitivityLevel per the spec's field table),
casting value to the right enum AFTER validating it against the
enum's kUnknownEnumValue bound (per-field; quote each enum's range
source); setter bool false maps to FAILED. cmd_mtalarm: set-only,
three decimal fields, field 1..11 else +MTERR:1, value forwarded.
Register.
- [ ] **Step 3:** docs: AT_MT_SPEC section with the field table, the
self-test lifecycle (notify URC, host completes with field 5 value 0,
SelfTestComplete emits), and the power-source row note (flat sibling
satisfies the device-type mandate); TESTING grammar rows + the
self-test bench case; three images; patch-check; host green; commit.

### Task C6: Chime (row 38), docs sweep, 0.5.0

**Files:**
- Modify: `main/main.cpp` (HearthChimeDelegate + pool + sounds store +
  manual integration init in the registration window + bridge
  mt_matter_chime_set), `main/include/mt_matter.h`,
  `main/mt_devtypes.cpp` (thunk + row), `main/mt_at.c` (cmd_mtchime:
  see below), `sdkconfig.defaults` (CHIME lift),
  `docs/AT_MT_SPEC.md` (chime section + the 3.9 table to 38 rows),
  `docs/ARCHITECTURE.md` (the round's decision record: valve
  return-ignored, chime SDK gap + I90 note, delegate pools),
  `README.md` (38), `main/include/mt_at_config.h` ("0.5.0")

**Interfaces:**
- Consumes: mt_cmd_forward; the B139 registration window in app_main.
- Produces: `int mt_matter_chime_set(uint16_t ep, uint8_t what,
  uint8_t value);` what 0 = SelectedChime (via
  ChimeCluster::SetSelectedChime), 1 = Enabled; and the sounds store
  fed by `int mt_matter_chime_sounds_set(uint16_t ep, const uint8_t
  *ids, const char *const *names, uint8_t count);`.

- [ ] **Step 1: delegate + store.** HearthChimeDelegate:
GetChimeSoundByIndex/GetChimeIDByIndex serve a per-endpoint host-fed
store (up to 8 sounds x u8 id + 33-byte name, temp-levels discipline,
CHIP_ERROR_NOT_FOUND past the end; call
ReportInstalledChimeSoundsChange on update per F3's doc quote);
PlayChimeSound forwards via mt_cmd_forward_payload(ep, Chime::Id,
PlayChimeSound id, chimeID) so the host sees WHICH chime to play, and
returns Status::Success on allow / Status::Failure on deny (the ONE
verdict visible on the wire: comment it). Pool per endpoint; thunk passes the delegate
(create aborts on null: the trap, F3).
- [ ] **Step 2: the SDK-gap workaround.** In the pre-start window
(where the temp-levels delegate and AirQuality instances register):
for each endpoint with a Chime cluster, call
ESPMatterChimeClusterServerInitCallback(endpoint_id) (F6: no SDK call
site exists; comment with the I90 note and the chime_integration.cpp
evidence). Verify the callback's exact signature and header first.
- [ ] **Step 3: wire surface.** Extend the spec's command set as
designed: AT+MTCHIMESOUNDS=<ep>,<id>,"<name>"[,...] (1..8 pairs, the
MTMODES grammar with ids) feeding mt_matter_chime_sounds_set, and
AT+MTCHIME=<ep>,<what>,<value> (what 0 selected / 1 enabled) feeding
mt_matter_chime_set. Both set-only, registered, documented. (The spec
named the store family without fixing these two grammars; this plan
fixes them; the spec's conventions govern their error codes.)
- [ ] **Step 4: docs sweep + version.** AT_MT_SPEC to 38 rows with
all new sections cross-linked; ARCHITECTURE record; README 38;
MT_FW_VERSION "0.5.0"; strings check. Three images; patch-check;
host green; commit.

### Task C7: Library classes I (valve, mode select, chime)

**Files (branch seven):**
- Create: `src/MatterEndpoints/MatterWaterValve.{h,cpp}`,
  `MatterModeSelect.{h,cpp}`, `MatterChime.{h,cpp}`
- Test: `test/host/test_watervalve.cpp`, `test_modeselect.cpp`,
  `test_chime.cpp`; Makefile + .gitignore

**Interfaces:**
- Consumes: the wire contracts from C1-C6 (grammars in Global
  Constraints; +MTCMD dispatch via hearthOnForwardedCommand, the
  doorlock-round mechanism; notify-only = dispatch without reply).
- Produces:

```cpp
/* MatterWaterValve, devtype 0x0042, cluster 0x0081 (129) */
bool begin();
void onOpen(std::function<bool()> cb);   /* verdict gates actuation only; doc why */
void onClose(std::function<bool()> cb);
bool setValveState(uint8_t state);                 /* AT+MTVALVE=<ep>,<state> */
bool setValveState(uint8_t state, uint8_t level);  /* ...,<level> */
uint8_t getValveState();                            /* URC-fed cache */

/* MatterModeSelect, devtype 0x0027, cluster 0x0050 (80) */
bool begin();
bool setSupportedModes(const uint8_t *modes, const char *const *labels, uint8_t count); /* AT+MTMODES, resent per reconcile */
bool setCurrentMode(uint8_t m);  uint8_t getCurrentMode();  /* plain ATTR 80/3? verify attr id, quote */
void onChangeMode(std::function<void(uint8_t)> cb);

/* MatterChime, devtype per header, cluster 0x0556 (1366) */
bool begin();
void onPlayChime(std::function<bool(uint8_t chimeID)> cb); /* real wire verdict */
bool setInstalledChimeSounds(const uint8_t *ids, const char *const *names, uint8_t count); /* AT+MTCHIMESOUNDS, resent per reconcile */
bool setSelectedChime(uint8_t id);  uint8_t getSelectedChime();  /* AT+MTCHIME=<ep>,0,<id> */
bool setEnabled(bool on);  bool getEnabled();                    /* AT+MTCHIME=<ep>,1,<v> */
```

- [ ] **Step 1: tests first** per class at the recipe minimum plus:
valve verdict dispatch both ways with the no-wire-failure doc pinned
in a comment; mode/chime label commands pinned with a comma-in-label
case each; reconcile resend hooks (hearthOnReconciled, B120 norm) for
the two label stores; chime's PlayChimeSound dispatch pinned with the
five-field URC form (`+MTCMD:<seq>,<ep>,1366,0,<chimeID>`): the
Hearth core's +MTCMD parser accepts the optional fifth field and
hands it to hearthOnForwardedCommand via a widened signature
(uint32_t cluster, uint32_t command, bool hasPayload, uint32_t
payload: extend the virtual with defaulted-behavior compatibility for
the existing overriders, reviewer checks all existing classes still
compile unchanged or are mechanically updated).
- [ ] **Step 2:** implement, full suite green, commit(s).

### Task C8: Library classes II (alarm, power source, opstate trio)

**Files (branch seven):**
- Create: `src/MatterEndpoints/MatterSmokeCOAlarm.{h,cpp}`,
  `MatterPowerSource.{h,cpp}`, `MatterLaundryWasher.{h,cpp}`,
  `MatterDishwasher.{h,cpp}`, `MatterLaundryDryer.{h,cpp}`
- Test: one binary per class; Makefile + .gitignore

**Interfaces:**
- Produces:

```cpp
/* MatterSmokeCOAlarm, devtype 0x0076, cluster 0x005C (92) */
bool begin();
void onSelfTest(std::function<void()> cb);  /* notify-only dispatch */
bool completeSelfTest();                    /* AT+MTALARM=<ep>,5,0 */
bool setSmokeState(uint8_t s);  bool setCOState(uint8_t s);
bool setBatteryAlert(uint8_t s); bool setDeviceMuted(uint8_t s);
bool setHardwareFaultAlert(bool v); bool setEndOfServiceAlert(uint8_t v);
bool setInterconnectSmokeAlarm(uint8_t s); bool setInterconnectCOAlarm(uint8_t s);
bool setContaminationState(uint8_t s); bool setSmokeSensitivityLevel(uint8_t s);
/* getters cached; ExpressedState read via plain ATTR (derived, read-only) */

/* MatterPowerSource, devtype 0x0011, cluster 0x002F (47) */
bool begin();
bool setBatChargeLevel(uint8_t lvl); bool setBatPercentRemaining(double percent); /* x2 halves on the wire */
bool setBatReplacementNeeded(bool v);

/* The opstate trio share one implementation core; per-class thin types
   (devtypes 0x0073/0x0075/0x007C, cluster 0x0060 (96)) */
bool begin();
void onPause(std::function<bool()> cb); void onResume(std::function<bool()> cb);
void onStart(std::function<bool()> cb); void onStop(std::function<bool()> cb);
bool setOperationalState(uint8_t s);  uint8_t getOperationalState();  /* AT+MTOPSTATE; cache */
```

- [ ] **Step 1: tests first**: alarm field commands pinned (exact
AT+MTALARM lines incl completeSelfTest = field 5 value 0); notify-only
+MTCMD:0 dispatch fires onSelfTest and enqueues NO reply (assert
unexpected().empty() with no MTCMDRESP scripted); opstate verdict
dispatch for all four commands; the trio's shared core proven by one
cross-class test (same wire, different devtype declaration); power
source percent doubling pinned. Genuine red, implement, green.
- [ ] **Step 2:** full suite green, commit(s).

### Task C9: Library integration, FullAPI examples, 0.5.0

**Files:** `src/Hearth.h` (+8 includes), `test_matter_umbrella.cpp` +
Makefile, `keywords.txt`, `README.md` (Hearth originals +8; the
38-type narrative; the valve-verdict and notify-only notes),
`library.properties` (0.5.0),
`examples/FullAPI/<C>/<C>.ino` for all EIGHT new classes
(banner-as-checklist, compile-checked each; cabinet-style #define not
needed anywhere here), plus the FullAPI sweep re-run over all 39
sketches as the integration gate.

- [ ] Umbrella + keywords + README + version + eight examples +
39-sketch sweep green (sizes recorded); daemon hygiene by PID; commit.

### Task C10: Bench verification (scripted power cuts available)

- [ ] Flash rebuilt build_b4 (0.5.0), factory-fresh; compose the risk
set: 0x0042, 0x0027, 0x0146, 0x0076, 0x0011, 0x0073, 0x0100 (seven
endpoints); boot rebuild survives (traps seven and eight plus chime's
null-delegate trap live); +MTEP? exact.
- [ ] Grammar matrices for the five new commands (MTVALVE, MTMODES,
MTOPSTATE, MTALARM, MTCHIME/MTCHIMESOUNDS) per TESTING.md rows;
+MTCMDRESP=0 rejection pin; regression pins (MTTEMPLEVELS, MTLOCK,
answered +MTCMD).
- [ ] Commission; chip-tool: SupportedModes and InstalledChimeSounds
read back verbatim (comma labels); ChangeToMode round trip +
CurrentMode URC; valve Open (allow) with AT+MTVALVE actuation report
and ValveStateChanged event read; chime PlayChimeSound allow AND deny
(wire statuses differ); opstate Pause/Start/Stop/Resume allow + deny;
smoke self-test end to end (notify URC, complete via MTALARM,
SelfTestComplete event read); MTALARM SmokeState Warning with the
SmokeAlarm event read; power source BatPercentRemaining read.
- [ ] Library smoke: two representative FullAPI sketches (smoke alarm
incl the self-test flow; one opstate class incl a deny).
- [ ] Restore bench (factory-fresh, 0x0100, espnow bridge), verbatim
evidence, PSK hygiene.
