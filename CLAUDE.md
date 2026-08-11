# Task graph (copy this section into your project's CLAUDE.md)

This project uses the `claude-task-graph` MCP server. Maintain the graph
continuously — it is the user's primary overview of the work.

## Rules

1. At the start of a session, and again after context compaction, call
   `get_graph` to re-orient yourself.
2. When the user asks for something new, create a `task` node before starting.
3. Break work down: create `subtask` nodes with `parent_id` set to the task.
4. When the user branches to a new topic mid-discussion, do NOT lose it:
   create an `idea` or `discussion` node with `parent_id` pointing at the
   node where the branch happened, status `parked` if it is not being
   worked on right now.
5. Update status as you go: `in-progress` when you start, `done` when
   finished, `blocked` (with a note explaining why) when stuck.
6. Record what you learn as nodes, not just as chat output. Attach each
   with `parent_id` to the task or subtask where it came up:
   - `finding`: something discovered while working (root cause, constraint,
     gotcha, how a subsystem actually behaves). Detail in the description.
   - `decision`: a significant decision, with the reasoning and the
     alternatives that were rejected in the description.
   - `bug`: a bug that was found. `in-progress` while fixing, `done` when
     fixed, with a note describing the fix. A bug that will not be fixed
     now stays `parked`.
   Record these at the moment they happen, not in a batch when the task
   ends. When work is delegated to subagents, record what they found and
   decided yourself; subagents do not maintain the graph.
7. If something is important enough to write down, it is important enough
   to be visible in the graph: create a `note` node with `parent_id` on
   the relevant node. Reserve `update_node`'s note log for the status
   trail of the node itself (why it is blocked, what the fix was), not
   for standalone information.
8. When a node corresponds to a section of a written spec or plan (a
   markdown file in the project), set its `doc` field to
   `"path/to/spec.md#heading"` so the user can jump from the node
   straight to the document. Keep it updated if the document moves.
9. Use `link_nodes` with `depends-on` when one task cannot start before
   another finishes.
10. Keep node titles short (a few words). Put detail in the description.
11. Never delete nodes unless the user explicitly asks. The user also
    removes nodes directly in the viewer; a "does not exist" error on
    update means they did. Do not recreate the node.
12. Plan-execution stages (SDD or similar) are recorded as a CHAIN, not a
    fan-out: each task's node is parented to the PREVIOUS task's node (the
    first task parents to the stage node), created in-progress at dispatch
    and set done with the RESULT in the description (what was built or
    found, commit hash, test count, review outcome) when its review
    closes. When the stage completes, append an end-result `note` leaf to
    the chain tail: outcome, evidence, artifact references with commit
    hashes, commit range, bugs found, remaining scope, cross-linked
    related-to the stage node and any bug nodes. Do all of this
    unprompted; the chain head shows the live position.

The user watches the graph live at http://localhost:7300 — treat it as the
summary they actually read, and keep chat output brief since the graph
carries the structure.

## Bootstrapping from an existing project

If a memory plugin (e.g. claude-mem) has injected history for this project
and the graph is empty, offer to reconstruct it: distill the history into
tasks, subtasks, parked ideas and open discussions, then create them in one
call with `import_graph`. Ask the user to confirm the distilled list first.

# iLabs AT Hearth

ESP32-C6 smart-home co-processor. A host MCU (RP2350 on the Challenger board)
drives it over a UART AT interface. Sibling to `iLabs_AT_ESP-now`, which exposes
ESP-NOW over `AT+EN`; the two are single-purpose images sharing one engine, and
the host reflashes the C6 to switch personality.

## Naming and legal constraint

**The firmware is called Hearth. Never name it, the repo, or the artifact
"Matter".** The Matter word mark belongs to the Connectivity Standards Alliance
and requires paid adopter membership. Matter appears throughout the docs as the
*protocol being spoken*, which is descriptive use and fine; it must not appear
as the name of this product.

The `AT+MT` command namespace was **deliberately not renamed**. MT is two
letters that need not stand for anything, and renaming a published protocol
surface buys nothing legally. Do not "fix" this.

Apache-2.0 on connectedhomeip grants rights to the *code* only. It grants no
trademark rights and implies no certification. This firmware is uncertified and
uses development credentials (VID `0xFFF1`).

## Build

Two SDKs must be sourced, in this order. IDF **v5.4.1**, not the ESP-NOW
firmware's v5.5.4: esp-matter `release/v1.5` fails to build on 5.5.4 at
`chip_gn`.

```sh
source ~/esp/esp-idf-v5.4.1/export.sh
source ~/esp/esp-matter/export.sh
idf.py -B build_b4 build
```

`build_b4` is the live build directory. `build`, `build_541` and `build_b2` are
stale leftovers with old absolute paths in their CMake caches.

Flash. With `--port` given (or exactly one `ttyACM` present) **no BOOTSEL press
is needed**: a 1200-baud open touch-resets the RP2350 into mass storage, so the
whole cycle is automatable. `--bridge espnow` additionally leaves a bridge that
forwards the C6 console to GP12/13 at 921600, which is the difference between
debugging with logs and without.

```sh
python3 fw/flash.py --build-dir build_b4 --port /dev/ttyACM0 --bridge espnow
```

`--bridge-only` swaps the bridge without touching the C6; `--no-auto-bootsel`
restores the wait-for-the-button behaviour. Passing a build directory is not
optional in practice: `flash.py` refuses one whose app is not
`ilabs_at_hearth.bin` at 0x20000, because the default `build/` is a stale
leftover whose app sits at 0x10000, which is now `nvs`.

Host unit tests, no hardware or IDF needed:

```sh
make -C test/host run
```

## Layout and the language split

```
main/main.cpp          C++: esp_matter runtime, callbacks, app_main, and the
                       mt_matter_* bridge functions
main/mt_at.c           C: AT command handlers, event mask, +MTERR code space
main/mt_composition.c  C: composition codec, pure, host-testable, no IDF
main/mt_comp_store.c   C: NVS persistence for the composition
main/mt_devtypes.cpp   C++: device type ID -> esp_matter create thunk table
main/include/mt_matter.h   the C-linkage bridge contract
test/host/             gcc unit tests for the pure-C parts
```

**`mt_at.c` is C and must stay C.** Anything touching `esp_matter` or CHIP goes
in a `.cpp` file behind an `extern "C"` bridge declared in `mt_matter.h`. No
esp_matter header may enter a C translation unit.

`at_core` (the AT engine, UART transport, link_mgr) lives in
`../iLabs_AT_ESP-now/components/at_core` and is pulled in cross-repo via
`EXTRA_COMPONENT_DIRS`. It is shared, so **a change there needs both firmwares
retested**, including the ESP-NOW RegressionSuite on its two-board rig.

## Things that will bite you

Each of these cost real debugging time. They are not obvious from the code.

**Build the endpoint composition before `esp_matter::start()`.** esp-matter
persists its `min_unused_endpoint_id` counter, but only for endpoints created
*after* start (`esp_matter_data_model.cpp`, store and read both gated on
`is_started()`). Creating before start keeps the counter in RAM, so every boot
allocates from the same base and endpoint IDs are reproducible. Move it after
start and IDs climb on every change, breaking the whole NVS scheme.

**A failed `endpoint::create()` consumes no endpoint ID.** It assigns the ID at
line 1668 and every failure path returns before that, so an endpoint after a
failed one slides down into its place. The boot rebuild therefore **aborts the
whole composition** on any single failure rather than skipping the entry.
Skipping would hand a commissioned device a silently wrong data model.

**`mt_at_event()` and `mt_at_urc()` are ONE path, and must stay one.**
`mt_at_event()` formats its line and hands it to `mt_at_urc()`; it must never
write to `at_uart` itself. It used to, which made it a second URC path with no
`s_at_up` gate, and it panicked the device with the very assert below. It hid
for months because the commissioning window opened *after* `mt_at_start()`;
keeping BLE resident (defect D1) made CHIP advertise during `Server::Init`,
early enough to hit it. Any future URC helper goes through `mt_at_urc()` too.

**Never raise a URC before `mt_at_start()` has run.** `app_main` runs
`esp_matter::start()` first, and CHIP callbacks fire during it. At that point
`at_uart`'s TX mutex does not exist and writing asserts in
`xSemaphoreTake(NULL)`, panics, and boot-loops forever. `mt_at_urc()` is guarded
by `s_at_up`; keep any new URC path behind the same guard.

**`CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` must be at least
`MT_COMP_MAX_ENDPOINTS`.** It was 2, inherited from the esp-matter light
example, which silently refused every endpoint past the first. Now 16, costing
about 404 bytes of `.bss` per slot.

**A cluster's `create()` populating attributes and events is not evidence its
commands were wired.** esp-matter ships command creation helpers with no call
site for some clusters: `operational_state::create()` and
`smoke_co_alarm::create()` add no command entries (unlike
`valve_configuration_and_control::create()`, which adds its own), so their
`AcceptedCommandList` is empty and every controller invoke answers 0x81
UNSUPPORTED_COMMAND. Builds and host suites cannot see this; it is only
observable on the wire. The chime cluster has the same disease in a different
organ (`ESPMatterChimeClusterServerInitCallback` exists with no caller, and
calling it without a prior `SetDelegate()` dereferences null). Before adding a
device type, read the cluster's `create()` body and grep the SDK for callers of
its `command::create_*` and init helpers; hand-add in the thunk what the SDK
declares but never calls (see `mt_opstate_add_commands` and
`mt_chime_register_all` in this repo, and ARCHITECTURE.md §8.6).

**Read device type IDs from `<ns>::get_device_type_id()`, never transcribe
them.** arduino-esp32 3.3.8 bundles esp_matter **1.4.1** while this firmware
pins **v1.5.1**, and namespaces were renamed between them
(`on_off_plugin_unit` to `on_off_plug_in_unit`, `window_covering_device` to
`window_covering`). Three arduino-esp32 classes call namespaces present in
neither revision. Check
`~/esp/esp-matter/components/esp_matter/data_model/esp_matter_endpoint.h`
before adding a device type.

**`app_main` needs an 8192-byte stack, not IDF's 3584 default.** It calls into
CHIP, and opening a commissioning window from `mt_boot_window_policy()` drags
in mDNS advertising and overflows the default outright (`Stack protection
fault ... task "main"`). The identical call via `AT+MTCOMMISSION` always worked
because that path runs on the AT parser task at 6144. Same function, two
stacks. `CONFIG_ESP_MAIN_TASK_STACK_SIZE` in `sdkconfig.defaults`; do not trim
it back.

**Every `mt_matter_*` bridge function must hold the CHIP stack lock.** They run
on the AT parser task, not the CHIP event loop. `ChipStackLock` in `main.cpp`
is the guard; use it in any new one. Without it `AT+MTCOMMISSION` aborted the
device outright (`Chip stack locking error at SystemLayerImplFreeRTOS.cpp:55`,
then `chipDie`), because opening a window arms a timer and the system layer
asserts the lock. The read-only accessors did not crash, which is worse: they
raced silently. Do NOT take the lock in `app_event_cb()`, which already runs on
the CHIP task.

**`CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING` must stay `n`.** CHIP defaults it to
`y`, which releases the BLE controller memory once commissioned, so
`AT+MTCOMMISSION` opens a window in CHIP's bookkeeping and advertises to
nobody: it returns `OK`, raises `+MTEVT:0`, and lies. That was defect D1
(`AT_MT_SPEC.md` §3.5), fixed 2026-07-29. Measured cost of keeping BLE
resident: 35,800 bytes, leaving 173 KB free heap; the firmware logs both
figures every boot so an SDK bump cannot quietly change the trade.
Diagnostic tell if it ever regresses: CHIP logs `bleAdv Timeout : Start slow
advertisement` while NimBLE logs nothing, and NimBLE announces every GAP
procedure it really runs. Absence of the NimBLE line is the evidence.

**`+MTREADY` must be the first line of a session.** `mt_at_start()` writes the
marker *before* setting `s_at_up`. Reversed, the CHIP task can slip a URC into
the gap: observed as `+MTEVT:0` preceding `+MTREADY` on the boot after
`AT+MTFRESET`, when an unprovisioned device auto-opens a window.

**The composition survives `AT+MTRESET` and is erased by `AT+MTFRESET`.**
`esp_matter::factory_reset()` only clears its own NVS namespace; the composition
lives in `mt_ep` in the default partition. This is deliberate: the composition
is a product definition, not user data.

**`AT+MTEVT?` answers `+MTEVTMASK`, not `+MTEVT`.** URCs can arrive between a
command and its terminal response, so a `+MTEVT:<n>` reply would be
indistinguishable from an event landing at that instant.

**The attribute surface is integers and booleans.** A census of all 20
arduino-esp32 endpoint classes found `u8`, `u16`, `bool`, `i16`, `u32` and
exactly one array. `AT+MTATTR` therefore covers essentially the whole parity
surface; `AT+MTATTRX` for opaque types is specified but unimplemented.

**No C6 board wires RTS/CTS, so `AT+MTFLOW` accepts only mode 0.** `at_core`
can drive them (`AT_UART_RTS_PIN` GPIO19, `AT_UART_CTS_PIN` GPIO18) and
`AT+ENFLOW` on the ESP-NOW image accepts all four modes, which makes this
look like an oversight rather than a hardware fact. It is a hardware fact:
the netlists of Challenger RP2350 WiFi6/BLE5 V0.2 and Slice RP2350 WiFi6 both
show the ESP32-C6-MINI-1-H4 using only `RXD0`/`TXD0`, the esp-hosted SPI
(IO2/3/7/14/15), IO9 boot, IO8 strapping, IO12/13 USB and `EN`. GPIO18 and 19
go nowhere. The pair *is* routed on the older ESP32-C3 Connectivity board,
which is where at_core's assignment came from. `MT_UART_FLOWCTRL_WIRED` in
`mt_at_config.h` is the single macro a board revision that routes them flips.
Enabling CTS against an unbonded pin does not degrade: it gates the C6's
transmitter and the link stops until reset.

**The `AT+MTBAUD` rate is not persisted, deliberately.** It lives in RAM, so
any reset returns the link to 115200. That is what gives a host that loses
sync a guaranteed way back. Do not "improve" this by storing it in NVS: one
bad switch would then need a serial-download reflash to undo.

## Debugging on hardware

Two UARTs, and confusing them wastes time:

- **GPIO16/17**: the AT link, and also where the **ROM** prints, since the boot
  ROM knows nothing about the custom console pin. A trace showing only
  `ESP-ROM:`, `load:`, `entry` and nothing else means you are reading this one.
- **GPIO2**: the console. Bootloader and application logs, and where a panic
  backtrace appears.

The RP2350 bridge sketch carries both. **Keep them as separate streams**: merged,
they interleave character by character and every URC assertion becomes flaky.

## Conventions

- **No em dashes** anywhere: prose, comments, commit messages. Colon, comma,
  parentheses or a full stop instead.
- C style: 4-space indent, `/* */` comments, `snake_case`.
- Commit messages explain *why*, and name the thing that would otherwise be
  rediscovered.
- `AT+MT` grammar: a bare `ERROR` means the command **form** was wrong. Bad
  parameter values carry `+MTERR:1`; failed data-model lookups carry `2` to `5`.
  Keep that division; it is what lets a host tell "you asked the wrong way" from
  "you asked for something that is not there".

## Documentation

| File | What |
|---|---|
| `docs/AT_MT_SPEC.md` | The host contract. Authoritative for command behaviour. |
| `docs/ARCHITECTURE.md` | Decision record. |
| `docs/TESTING.md` | Regression plan. The harness is not written yet. |
| `docs/FIRMWARE_UPDATE_SPEC.md` | Host-driven serial flash OTA design. |
| `docs/hearth-integration-plan.md` | Original Phase A/B plan, historical. |
| `docs/superpowers/specs/` | Design specs. The C-phase API design lives here. |
| `docs/superpowers/plans/` | Implementation plans. |

## State

Phases A and B complete. Phase C is the work to support an
**arduino-esp32-parity host library** on arduino-pico, so an unmodified
arduino-esp32 Matter sketch runs on a Challenger.

- **C1 done**, hardware-verified: host-declared endpoint composition over
  `AT+MTEP`, persisted in NVS, rebuilt at boot.
- **C2 done**, hardware-verified: `AT+MTATTR` write modes and the `+MTERR` code
  retrofit.
- **C3 done**, hardware-verified: event mask and `+MTEVT`, `+MTIDENT`,
  `AT+MTNET?`, and the removal of `+MTCOMMISSION:*` URCs.
- **C4 done, hardware-verified end to end on 2026-07-28**: an unmodified
  upstream `MatterOnOffLight` sketch on a Challenger RP2350 WiFi6/BLE5 was
  commissioned onto a fabric and drives a light from a controller. That one
  run exercised the whole chain: variant-driven link bring-up, the C6 reset
  and `+MTREADY` sync, the composition rebuilt from NVS, `AT+MTCODES?`,
  `Matter.isDeviceCommissioned()`, BLE commissioning with the C6 joining WiFi
  on credentials it received, and `AT+MTATTR` writes reaching the endpoint.
  The `iLabs_Hearth` host
  library on arduino-pico, implemented across plan tasks 1 to 9: the AT line
  protocol client, the attribute-value compat layer, `MatterEndPoint` and its
  registry, the `Hearth` global, `ArduinoMatter` and the composition reconcile,
  and the four endpoint classes, with 478 host unit tests passing. Lives at
  `Dropbox/Arduino/libraries/iLabs_Hearth`, branch `c4`. **All three upstream
  examples compile and link with no build flags**, still byte-identical to
  upstream. Getting there closed the two external blockers by filling the
  core's gaps under the sketches:
  - The link is variant-driven now (`ESP_SERIAL_PORT`, i.e. `Serial2` on the
    Challenger, not `Serial1`), with the ESP-NOW library's reset sequence on
    `PIN_ESP_MODE`/`PIN_ESP_RST` and a wait for `+MTREADY`, on first use.
  - `src/Preferences.{h,cpp}`: the arduino-esp32 Preferences API over
    arduino-pico's EEPROM. EEPROM and not LittleFS because every Challenger's
    default flash option is "no FS", so a filesystem store would silently
    return defaults on a stock install.
  - `src/HearthBootPin.cpp`: `BOOT_PIN` routed to BOOTSEL by *defining*
    `pinMode`/`digitalRead`, which arduino-pico declares as weak aliases.
    Verified at link time, not assumed: `nm` shows `digitalRead` and
    `__digitalRead` at different addresses.
  - `CONFIG_ENABLE_CHIPOBLE` set to 1 in `HearthCompat.h`. **The host can
    never have WiFi while the C6 runs Hearth**: arduino-pico's `WiFi` on this
    board drives the C6 over esp-at or esp-hosted, and one co-processor holds
    one personality. Left unset, upstream's `#if !CONFIG_ENABLE_CHIPOBLE`
    evaluates true and every example spins in `while (WiFi.status() !=
    WL_CONNECTED)` forever, never reaching `Matter.begin()`. Setting it is
    factual, not a workaround: the firmware has `CONFIG_ENABLE_CHIPOBLE=y`
    and `CONFIG_ENABLE_WIFI_STATION=y`, so the C6 is handed credentials
    during BLE commissioning and joins on its own.
- **Seven-type batch (0.5.0) shipped 2026-08-08**: device type rows 31-38
  (Water Valve, Mode Select, Chime, Smoke/CO Alarm, Power Source, and the
  laundry washer / dishwasher / laundry dryer trio on one OperationalState
  core), the `+MTCMD` notify-only form (seq 0) with a fifth payload field,
  and `AT+MTVALVE`/`MTMODES`/`MTOPSTATE`/`MTALARM`/`MTCHIME`/`MTCHIMESOUNDS`.
  Firmware tag `0.5.0` at `16b6cd7`; library tag `0.5.0` at merge `ce577bf`
  with eight new classes and FullAPI examples (39 sketches). Everything was
  hardware-verified end to end; the bench found five defects no build or host
  suite could see (SDK command entries missing, the ExpressedState self-test
  wedge, the arduino-pico 31-byte RX buffer dropping URC-burst tails, example
  config ordered before `Matter.begin()`, chime first-write suppression), all
  fixed in-range. Bench evidence: `.superpowers/sdd/2026-08-07-seven-type-batch/`.
- **RVC + Microwave batch (0.6.0) shipped 2026-08-09**: device types 0x0074
  Robotic Vacuum Cleaner and 0x0079 Microwave Oven across the whole stack.
  Firmware: cluster-aware `AT+MTMODES` (mandatory per-mode tag, 0 = per-cluster
  conformance default), `AT+MTOPSTATE` serving the RVC opstate cluster
  (states 0x40-0x42), `+MTCMD` documented-arity payload (up to four fields),
  the RVC opstate and AddMoreTime commands hand-wired (the 8.6 disease twice
  more, ARCHITECTURE.md 8.7). Library: MatterRoboticVacuum and
  MatterMicrowaveOven on a multi-field +MTCMD dispatch layer (41 classes,
  2748 host checks), merged 90f3d57, tag 0.6.0. Phase 3 now 13 slots / 239
  checks with all four baselines re-recorded on both transports. Key facts
  proven on hardware: Instance-owned attributes (ModeBase CurrentMode, opstate
  states, CookTime/PowerSetting) NEVER raise +MTATTR URCs, so host-side
  caches update on allow verdicts only; ModeBase short-circuits a same-mode
  ChangeToMode to success before the delegate (bit the harness once, B196);
  a stale build_thread/sdkconfig silently overrides sdkconfig.defaults
  (TESTING.md 9 runbook row); arduino-cli cannot upload to RP2350 boards
  (drive-name scan, same runbook). Firmware tag 0.6.0 at 3a9344b.
- **Composed-appliance round (0.7.0) shipped 2026-08-11**: Refrigerator
  0x0070, Oven 0x007B and Cook Surface 0x0077 on parent-endpoint machinery
  across the whole stack. Firmware: composition blob v3 with a per-entry
  parent index (earlier-index rule, so validation happens at append and the
  rebuild stays single-pass via `set_parent_endpoint()`),
  `AT+MTEP=<devtype>[,<variant>[,<parent_idx>]]` with five-field query lines
  when parented (unparented lines byte-identical), endpoint cap 24, cabinet
  conditional clusters derived from the parent type (Cooler under fridge,
  Heater under oven, never encoded in the variant), cluster-aware `AT+MTALARM`
  (fridge bit form; two Phase 1 rows deliberately migrated), and hand-rolled
  OvenMode/OvenCavityOperationalState shells because esp-matter ships no
  helpers for either (the 8.6 disease at its most severe grade; Stop/Start
  only, Pause/Resume are disallowConform). ARCHITECTURE.md 8.9 is the
  decision record. Library: `composed-trio` merged at `2c80c47`, tag `0.7.0`;
  owner classes with typed children (`MatterRefrigerator::addCabinet()`,
  `MatterOven::addCavity()`, `MatterCooktop::addSurface()`), owned children
  never self-declare, `HEARTH_MAX_ENDPOINTS` 24, suite 3187/0. Phase 3 now
  20 endpoints / 306 checks, baselines re-recorded on both transports with
  identical verdicts. The bench found one defect nothing else could see:
  B216, `cook_surface::add()` OVERWRITES the temperature-control feature
  flags (unlike the cabinet's pass-through and room AC's OR), fixed by
  destroy-and-recreate in the thunk (`1847d74`). Zero harness defects, a
  first. Two config landmines worth remembering: the stale-override file for
  the default `build_b4` build is the REPO-ROOT `sdkconfig` (not
  `build_b4/sdkconfig`), and `sdkconfig.defaults` had refrigerator cluster
  Kconfigs explicitly `=n` that had to be removed. Firmware tag 0.7.0 at
  6d0011a.
- **Energy Round A (0.8.0) shipped 2026-08-11**: Electrical Sensor 0x0510 and
  Electrical Meter 0x0514 across the whole stack, the first Tier 3 energy
  round (DE220 split; rounds B and C remain). Firmware: the 64-bit `AT+MTATTR`
  value pipeline (`attr_val_to_i64`/`i64_to_attr_val` with per-arm width
  gates, `parse_i64`/`parse_u64`, full-width `%lld`/`%llu` formatting; closed
  a silent truncation defect on the RISC-V target), `AT+MTMEAS` set-only push
  (1-7 field/value pairs, per-field signedness table), an EPM pull-model
  delegate pool plus EEM push via `NotifyCumulativeEnergyMeasured`, and the
  8.6 disease found AGAIN in a new form: EEM's AttrAccess is declared but
  never registered by the SDK, needing a custom init CB (ARCHITECTURE.md
  8.10). Variant 0 carries both measurement clusters, variant 1 is
  power-only; 1.5.1 marks EEM mandatory on the meter, so the CONFORMANT
  power-only declaration is the sensor and the variant-1 meter is disclosed
  as permissive-beyond-conformance. Library: MatterElectricalSensor and
  MatterElectricalMeter (meter subclasses sensor, washer precedent), merged
  at `51ad7b1`, tag `0.8.0`, suite 3383/0. The review (not the bench) found
  the round's one defect, B229: the setters' unchanged-value no-op guard
  survived a C6 reboot so a post-reboot same-value setter never reached the
  fabric; fixed by a `hearthOnReconciled()` override that clears the
  wire-pushed memory without re-pushing (16c330c). Key semantic: measurement
  pushes are never re-sent on reconcile (volatile readings), and
  Instance-owned measurement attributes never raise `+MTATTR` URCs, so the
  host cache updates on successful pushes only. Harness: Phase 1 164 /
  Phase 3 341 (22 endpoints; slot 21 sensor variant 1, slot 22 meter variant
  0, the conformance-driven flip), verdict-identical on both transports,
  zero bench defects (second clean round in a row). B98 closed on live
  nullable evidence. Heap standing figures now WiFi 118580 / Thread 188312
  (about -12.7 KB each, the newly linked measurement cluster servers).
  Firmware tag 0.8.0 at ed361f4.
- **C5, the regression harness, is COMPLETE through T5 (2026-08-09).** T1-T4
  built `test/mt_regression.py`'s Phases 0-2 (link preflight, raw AT protocol
  conformance, Matter lifecycle) with committed WiFi and Thread baselines.
  T5 added Phase 3 (design spec
  `docs/superpowers/specs/2026-08-08-c5-regression-harness-t5-design.md`):
  the post-August-1 device type surface on an 11-endpoint composition, the
  grammar completion for the ten command families, both ready-made cases
  (the B165 wedge repro and the opstate in-state guard), and a CmdResponder
  for adjudicated `+MTCMD` forwards. Live-validated on BOTH transports:
  `test/baselines/wifi-devicetypes.json` (5a8e159) and
  `thread-devicetypes.json` (38fea8d), 165/165 each; suite at 216. The
  first live run found 13 harness defects (zero firmware defects), the
  big one being that chip-tool colours piped output and end-anchored
  parsers break on the trailing escape; all fixed in-range (7d5c5c7).
  `docs/TESTING.md` §8 describes Phase 3. Two residues: the WiFi
  operational-discovery transient (2 of 5 commissioning attempts, cause
  narrowed to the WiFi-side mDNS path, watch item), and B181: the T4
  fixture had committed the live Thread dataset (network key + PSKc) to
  the public repo; remediated 2026-08-09 by rotating the rig's dataset
  and replacing the fixture with a synthetic one (9fc1476). Treat Thread
  dataset captures as credentials everywhere, same as the PSK.

Open questions, both recorded in the design spec §12.1:

- **P2: RESOLVED, hardware-verified 2026-07-29.** See `AT_MT_SPEC.md` §3.12.1.
  The question ("can the boot window be suppressed on an unconfigured device?")
  was the wrong way round: the bug is that **"configured" was the wrong
  predicate**. It must mean *has a fabric usable on this transport*, not merely
  *has a fabric*. A device reflashed between the WiFi and Thread images keeps
  its fabric (NVS is untouched by design), reports itself commissioned, and is
  reachable by no route at all.
  Decided behaviour: erase nothing, treat a transport mismatch as
  not-commissioned for the boot window, raise `+MTEVT:27`, and report
  `<mismatch>` on `AT+MTNET?`. Erasing was rejected because it destroys a
  working fabric on any transport flip, including a brief test one.
  `mt_boot_window_policy()` is where it lands, as intended.
  Detected by asking CHIP whether this transport is provisioned
  (`IsWiFiStationProvisioned()` / `IsThreadProvisioned()`), **not** by a stored
  marker. A marker was written first and replaced after hardware showed it
  answers the wrong question: a device commissioned over Thread, reflashed to
  the WiFi image, joined WiFi on credentials left by an earlier commissioning
  and served its Thread-era fabric happily. Fabric credentials are
  transport-independent; provisioning is not. The condition is
  `fabric_count > 0 && !provisioned`, cleared on `kCommissioningComplete`.
  Note it needs the *fabric* count, not `mt_boot_window_policy()`'s
  `configured` argument, which reports whether a composition is declared. Those
  are independent.
  One ordering trap this codebase's own rules predict: `+MTEVT:27` must be
  raised in `app_main` after `mt_at_start()`, not from the policy function, or
  the `s_at_up` guard drops it and it would precede `+MTREADY`.
**Single-app partition switch: done 2026-07-28.** `partitions.csv` is one
`factory` slot at 0x20000, 3840 KB, and `CONFIG_ENABLE_OTA_REQUESTOR=n`. Free
space went from 301 KB (15%) to 2.31 MB (59%), and dropping the OTA Requestor
shrank the app by ~45 KB on its own. Thread's +164 KB now fits with ~2.1 MB
spare. `nvs` and `nvs_keys` do not move, and this was **verified on hardware**:
the device commissioned before the switch came back on the same fabric after
being reflashed with the new table, still controllable from the app. A
repartition is therefore not a recommissioning event, which is worth keeping
true, since it is what makes the layout safe to change again later.

Thread is a **build-time** variant, **hardware-verified 2026-07-29**: an
unmodified `MatterOnOffLight` composition was commissioned over BLE onto a
Thread network and driven from a controller, with the Matter traffic running
over 802.15.4 (the device's CASE session addressed chip-tool at the border
router's OMR address, so no fallback path was involved). Border router was an
OpenThread `otbr-agent` on the dev box with a Nabu Casa ZBT-2 reflashed from
Zigbee to `ot-rcp`.

```sh
idf.py -B build_thread -D SDKCONFIG=build_thread/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.thread" \
  build
```

**`SDKCONFIG` must be redirected into the build directory.** Left at its
default, the Thread configuration is written to `./sdkconfig` and silently
takes over the WiFi build too.

**Thread-only is 100,416 bytes SMALLER than WiFi** (1,517,936 against
1,618,352), because OpenThread costs less than the WiFi stack it replaces. The
`+164 KB` in ARCHITECTURE.md is for WiFi *plus* Thread, which is a different
image and not what this firmware builds.

**Do not model the Thread variant on esp-matter's `c6_wifi_thread`.** It adds a
secondary network commissioning endpoint (`THREAD_NETWORK_ENDPOINT_ID=2`,
`WIFI_NETWORK_ENDPOINT_ID=0`) and pins `ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=3`.
Both wreck C1: the host owns the endpoint space above 0 and caches ids, so an
endpoint it never declared desynchronises all of them. `c6_thread` is the right
model. Dual-transport in one image is a composition redesign, not a config flip.
