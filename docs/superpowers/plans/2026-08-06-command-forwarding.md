# Command Forwarding + Door Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** App-adjudicated Matter commands over the AT link (generic
+MTCMD/AT+MTCMDRESP frame with default-deny), with the Door Lock device
type as first consumer and MatterDoorLock as the library's first
Hearth-original class.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-06-command-forwarding-design.md`),
whose section 2 SDK facts (F1-F6) are binding. Single-slot verdict
mailbox; the CHIP task blocks at most 1000 ms; the responder never
takes the CHIP stack lock; the host owns actuation and reports it via
AT+MTLOCK, which drives the event-emitting SetLockState.

**Tech Stack:** C mailbox core (host-testable pure part), C/C++
firmware on esp-matter 21aa3d1 / CHIP b87051a9 pinned, MockStream
library tests on arduino-pico.

## Global Constraints

- **No em dashes** anywhere. mt_at.c stays C with no SDK headers;
  bridges hold ChipStackLock; **exception by design: the AT+MTCMDRESP
  handler must NEVER take ChipStackLock** (the CHIP task is blocked
  inside the cluster callback; taking it deadlocks the window).
- Verdict deadline exactly 1000 ms; timeout, link-down, unknown or
  stale seq, unregistered host callback: all DENY
  (OperationErrorEnum::kUnspecified). A lock fails closed.
- Wire contracts, exact: URC `+MTCMD:<seq>,<ep>,<cluster>,<command>`
  (decimal); timeout URC `+MTCMDTO:<seq>`;
  `AT+MTCMDRESP=<seq>,<verdict>` set-only, verdict 1 allow / 0 deny,
  `+MTERR:1` on bad grammar, bad verdict, or unknown/stale seq;
  `AT+MTLOCK=<ep>,<state>[,<source>]` set-only, state 0..2
  (DlLockState protocol values NotFullyLocked/Locked/Unlocked),
  +MTERR:1 bad values, +MTERR:2 unknown endpoint, +MTERR:3 endpoint
  without a DoorLock cluster, bare ERROR on internal failure.
- Firmware never transcribes SDK enum values: kManual and the source
  upper bound cross into C through the two accessors Task C2 defines.
  The library (which has no SDK) transcribes protocol constants but
  the implementer quotes the pinned-header lines in the report, as
  for every existing class.
- The firmware never calls SetLockState after an allowed verdict:
  actuation timing belongs to the host (spec F4).
- Feature map stays 0 (spec F3): no PIN/USER/COTA features this round.
- Door lock device type ID via `door_lock::get_device_type_id()`,
  never a literal; no abort trap exists for this cluster (spec F5) and
  none is to be invented.
- All three firmware images green at firmware task ends;
  scripts/apply-sdk-patches.sh --check "applied" x2 around builds;
  host suites green in the touched repo at every boundary.
- Library work on branch `doorlock` in
  /home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth (Dropbox:
  in place, never symlink). Standing library rules: TDD with genuine
  red excerpts; decimal IDs in expect() strings; no binaries in
  commits (.gitignore lines); NEW commits never amends; 2-space
  /* */ style.
- Commit messages explain why and end exactly with:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task C1: The verdict mailbox and AT+MTCMDRESP

**Files:**
- Create: `main/mt_cmdbox.c`, `main/include/mt_cmdbox.h` (pure C slot
  state machine, no IDF headers)
- Modify: `main/mt_at.c` (FreeRTOS glue + handler + URC formatting),
  `main/include/mt_at.h` (C-linkage `mt_cmd_forward`)
- Test: `test/host/test_mt_cmdbox.c` + `test/host/Makefile`

**Interfaces:**
- Produces (mt_cmdbox.h, pure, host-tested):

```c
typedef enum { MT_CMDBOX_IDLE, MT_CMDBOX_PENDING, MT_CMDBOX_ANSWERED } mt_cmdbox_state_t;
void mt_cmdbox_init(void);
uint32_t mt_cmdbox_open(uint16_t ep, uint32_t cluster, uint32_t command); /* returns new seq */
/* 0 accepted; -1 wrong/stale seq or not pending; verdict must be 0/1 */
int  mt_cmdbox_answer(uint32_t seq, int verdict);
/* -1 if not ANSWERED for this seq; else verdict, and slot returns to IDLE */
int  mt_cmdbox_take(uint32_t seq);
void mt_cmdbox_expire(uint32_t seq); /* PENDING -> IDLE, for the timeout path */
```

- Produces (mt_at.h): `bool mt_cmd_forward(uint16_t ep, uint32_t cluster, uint32_t command);`
  which raises the URC, blocks up to 1000 ms on a FreeRTOS binary
  semaphore, and returns the verdict (false on any deny path). Task C2
  calls it from the ember callbacks.

- [ ] **Step 1: tests first** (test_mt_cmdbox.c in the house assert
style): open returns monotonically increasing seqs; answer with the
pending seq stores the verdict and take returns it once (second take
-1); answer with a stale seq, a future seq, or in IDLE returns -1 and
changes nothing; answer with verdict 2 or -1 rejected; expire drops a
PENDING slot so a late answer for that seq returns -1; reopen after
expire issues a fresh seq. Run red, implement, run green
(`make -C test/host run`).
- [ ] **Step 2: the glue in mt_at.c.** `mt_cmd_forward()`: if
`!s_at_up` return false immediately (fail closed, no URC); else
`mt_cmdbox_open`, format and raise `+MTCMD:%lu,%u,%lu,%lu` via
`mt_at_urc()`, `xSemaphoreTake(s_cmd_sem, pdMS_TO_TICKS(1000))`; on
take-success return `mt_cmdbox_take(seq) == 1`; on timeout
`mt_cmdbox_expire(seq)`, raise `+MTCMDTO:%lu`, return false. Mailbox
calls from both tasks wrap in `taskENTER_CRITICAL`/`EXIT` on a static
`portMUX_TYPE`. The semaphore is created in `mt_at_start()` alongside
the existing init.
- [ ] **Step 3: cmd_mtcmdresp handler.** Set-only (bare/query answer
plain ERROR, the AT+MTSWITCH pattern); parse two decimal u32/u1
fields; anything malformed or verdict not 0/1 answers +MTERR:1;
`mt_cmdbox_answer` result -1 answers +MTERR:1; on success give
`s_cmd_sem` and answer OK. NO ChipStackLock anywhere in this handler;
put the spec's deadlock rationale in a comment. Register in s_cmds.
- [ ] **Step 4:** build_b4 green, patch-check x2, host suite green,
commit.

### Task C2: Door lock endpoint, ember callbacks, AT+MTLOCK, docs, 0.3.3

**Files:**
- Modify: `main/mt_devtypes.cpp` (row 20), `main/main.cpp` (ember
  callbacks + bridge + init callback), `main/include/mt_matter.h`
  (bridge + accessors), `main/mt_at.c` (cmd_mtlock),
  `docs/AT_MT_SPEC.md` (3.17 command forwarding incl. +MTCMD/+MTCMDTO/
  AT+MTCMDRESP; 3.18 AT+MTLOCK; 3.9 row 20 + door-lock note),
  `docs/ARCHITECTURE.md` (mailbox decision record, lock-free responder,
  default-deny), `README.md` (20-row table gains door lock; forwarding
  paragraph), `main/include/mt_at_config.h` (MT_FW_VERSION "0.3.3")

**Interfaces:**
- Consumes: `mt_cmd_forward(ep, cluster, command)` from C1.
- Produces (mt_matter.h):

```c
int mt_matter_lock_state_set(uint16_t ep, uint8_t state, uint8_t source);
uint8_t mt_matter_lock_source_manual(void); /* OperationSourceEnum::kManual, read not transcribed */
uint8_t mt_matter_lock_source_max(void);    /* highest valid OperationSourceEnum value */
```

- [ ] **Step 1: devtype row.** `{ door_lock::get_device_type_id(),
mk_door_lock, "door_lock", 0 }`; thunk is plain (default config_t, no
feature work, spec F5); verify the namespace at
esp_matter_endpoint.h:543-555 and quote in the report.
- [ ] **Step 2: ember callbacks in main.cpp.** Implement
`emberAfPluginDoorLockOnDoorLockCommand` and
`...OnDoorUnlockCommand` (signatures verbatim from
door-lock-server.h:1112-1146, quote them): both call one static helper
that maps to `mt_cmd_forward(endpointId, DoorLock::Id,
to_underlying(<LockDoor|UnlockDoor>::Id))` and on false sets
`err = OperationErrorEnum::kUnspecified`. Also implement
`emberAfDoorLockClusterInitCallback` per the esp-matter example
(door_lock_callbacks.cpp:20-24: `DoorLockServer::Instance().InitServer
(endpoint)`); check whether a default exists first and report what the
linker demanded. Do NOT take ChipStackLock in these callbacks (they
run on the CHIP task).
- [ ] **Step 3: the bridge.** `mt_matter_lock_state_set`: ChipStackLock;
endpoint lookup MT_ATTR_ERR_ENDPOINT; DoorLock cluster presence
MT_ATTR_ERR_CLUSTER; call the 6-arg
`DoorLockServer::Instance().SetLockState(ep, (DlLockState)state,
(OperationSourceEnum)source)` (nullable tail defaulted); false maps to
MT_ATTR_ERR_FAILED. The two accessors return values read from the
enum in the pinned header (quote the enum lines in the report).
- [ ] **Step 4: cmd_mtlock in mt_at.c.** Set-only; `<ep>` u16,
`<state>` 0..2 else +MTERR:1, `<source>` optional defaulting to
`mt_matter_lock_source_manual()`, validated against
`mt_matter_lock_source_max()` else +MTERR:1; route the bridge result
through `attr_err_to_mterr()`. Register in s_cmds.
- [ ] **Step 5: docs + version.** Spec sections mirror the design doc's
section 4 wording (default-deny, the 1 s window, the queuing
consequence for concurrent bridge commands); README table becomes 20
device types; MT_FW_VERSION "0.3.3"; strings check on the built
binary. All three images green, patch-check x2, host suite green,
commit.

### Task C3: MatterDoorLock class and +MTCMD dispatch (library)

**Files (branch doorlock):**
- Modify: `src/Hearth.cpp`/`src/Hearth.h` (+MTCMD/+MTCMDTO URC parsing,
  dispatch, inline AT+MTCMDRESP reply, HEARTH_CMD_TIMEOUT link event),
  `src/MatterEndPoint.h`/`.cpp` (virtual
  `bool hearthOnForwardedCommand(uint32_t cluster, uint32_t command)`
  default returning false)
- Create: `src/MatterEndpoints/MatterDoorLock.{h,cpp}`
- Test: `test/host/test_doorlock.cpp`, additions to
  `test/host/test_reconcile.cpp`; `test/host/Makefile` + `.gitignore`

**Interfaces:**
- Consumes: firmware wire contract from Global Constraints.
- Produces:

```cpp
/* MatterEndPoint.h */
virtual bool hearthOnForwardedCommand(uint32_t cluster_id, uint32_t command_id); /* default false */

/* MatterDoorLock.h; kManual/kKeypad etc as class constants whose
   values the implementer verifies against the pinned CHIP header and
   quotes in the report */
bool begin(bool locked = true);
void onLock(std::function<bool()> cb);
void onUnlock(std::function<bool()> cb);
bool setLockState(uint8_t state, uint8_t source /* = kSourceManual */);
bool lock();    /* setLockState(kStateLocked, kSourceManual) */
bool unlock();  /* setLockState(kStateUnlocked, kSourceManual) */
uint8_t getLockState();
```

- [ ] **Step 1: dispatch tests first** (test_doorlock.cpp): a delivered
`+MTCMD:7,1,257,0` with an onLock callback returning true produces
exactly `AT+MTCMDRESP=7,1`; callback returning false produces `...,0`;
no callback registered produces `...,0` (deny, fail closed); command 1
routes to onUnlock; a +MTCMD for an endpoint with no object still
answers deny (Hearth core replies, endpoint not found); `+MTCMDTO:7`
raises HEARTH_CMD_TIMEOUT through onLinkEvent. Red, implement
(Hearth.cpp parses both URCs in hearthOnURCLine; reply sent through the
normal command path), green.
- [ ] **Step 2: class tests.** begin declares 0x000A (two-arg
hearthDeclare, variant 0); re-begin refused (+MTERR:10 convention);
setLockState sends `AT+MTLOCK=1,1,<kSourceManual>` exact wire pin and
updates the cache only on OK; failed write leaves cache (house
discipline); lock()/unlock() pins; controller +MTATTR URC for
LockState (cluster 257 attr 0) updates getLockState and dispatches
attributeChangeCB; reconcile hook pushes the begin state via AT+MTLOCK
exactly once per reconcile (B120 norm; test both adopt and rebuild
paths in test_reconcile.cpp). Red, implement, green.
- [ ] **Step 3:** full suite green (all binaries), commit(s).

### Task C4: Library integration and 0.3.3

**Files:** `src/Hearth.h` umbrella include,
`test/host/test_matter_umbrella.cpp` + Makefile, `keywords.txt`,
`README.md` (new "Hearth originals" section distinct from the parity
table: MatterDoorLock documented incl. the poll-latency warning: the
1000 ms verdict window includes Hearth.poll() latency, a sketch that
blocks misses verdicts and the lock fails closed, +MTCMDTO is the
diagnostic), `library.properties` (0.3.3),
`examples/MatterDoorLockAdjudicated/MatterDoorLockAdjudicated.ino`
(Hearth-original example: onLock/onUnlock verdicts from a serial
prompt or pin, setLockState on actuation; no upstream example exists
to copy, state that in the sketch header).

- [ ] Umbrella + umbrella test + keywords + README + version; the
example compiles for `pico:rp2040:challenger_2350_wifi6_ble5`
(arduino-cli; kill discovery daemons afterwards and verify with
pgrep); full suite green; commit.

### Task C5: Bench verification (operator gate)

- [ ] Flash rebuilt build_b4 (0.3.3); compose 0x000A; +MTEP? shows the
row; commission.
- [ ] Raw AT pins: controller LockDoor with a scripted
AT+MTCMDRESP allow answers Status SUCCESS controller-side; deny
answers Status FAILURE and chip-tool reads the LockOperationError
event; silent host proves default-deny after ~1 s and +MTCMDTO
arrives; stale MTCMDRESP after the timeout answers +MTERR:1;
AT+MTLOCK=<ep>,1 flips LockState (read via chip-tool AND AT+MTATTR)
and emits a LockOperation event with the manual source.
- [ ] Library sketch (the C4 example): controller lock/unlock reaching
onLock/onUnlock with both verdicts; setLockState from the sketch
observed controller-side; verdict-window behavior with a deliberately
blocking loop() documented as observed.
- [ ] Restore bench (factory-fresh, 0x0100, espnow bridge), report with
verbatim evidence, PSK hygiene.
