# Command forwarding design: app-adjudicated commands and the door lock

Date: 2026-08-06. Takes on the challenge parked in the cabinet round
(graph I110): lock-class commands demand a synchronous app verdict,
and the app lives on the other side of a UART. First consumer: the
Matter Door Lock device type (0x000A), Hearth's first class beyond
arduino-esp32 parity (upstream has no door lock class or example;
confirmed against 3.3.8).

## 1. Decisions taken with the user (2026-08-06)

- **The host adjudicates.** The C6 forwards lock-class commands over a
  URC and waits for the sketch's allow/deny. All permission logic lives
  in the sketch. Rejected: storing Matter credentials on the C6 this
  round (see section 7).
- **Generic frame.** One URC/response pair (`+MTCMD` /
  `AT+MTCMDRESP`) carries any future app-adjudicated command; the door
  lock is the first registered consumer, not the protocol's shape.
- **Featureless lock v1** (feature map 0, no PIN/USER/COTA features).
  Forced by SDK fact F3 below and accepted: controllers send bare
  LockDoor/UnlockDoor; host-side PIN entry remains possible via the
  sketch's own keypad logic, outside Matter.

## 2. SDK facts this design is built on (explorer evidence, 2026-08-06)

- **F1: the verdict is synchronous, with no async path.**
  `emberAfPluginDoorLockOnDoorLockCommand` /
  `...OnDoorUnlockCommand` return bool inline;
  `HandleRemoteLockOperation` writes the command status in the same
  call frame (door-lock-server.cpp:3711, :3726). No CommandHandler is
  retained for deferred completion. UnlockWithTimeout reuses the
  unlock callback; esp-matter's cluster create() registers only
  LockDoor and UnlockDoor commands anyway.
- **F2: the callback runs on the CHIP event-loop task.** Blocking it
  stalls MRP retransmits, subscription reports, and all other exchange
  processing until it returns. The controller-side invoke budget is
  kExpectedIMProcessingTime (2 s) plus MRP margins
  (InteractionModelTimeout.h:27, CommandSender.cpp:138).
- **F3: a supplied PIN is validated against the device's own stored
  credentials BEFORE the app callback** (door-lock-server.cpp:
  3653-3708); an unknown PIN is rejected as kInvalidCredential and the
  app is never consulted. With feature map 0 (esp-matter's door lock
  default; the config_t has no feature_flags field and create()
  hard-codes feature map 0), bare commands always reach the app and
  PIN-carrying commands are refused by the server.
- **F4: the app owns the state change.** The server deliberately does
  not set LockState on a successful command ("The app should trigger
  the lock state change as it may take a while",
  door-lock-server.cpp:3712 comment). Only the 6-arg
  `DoorLockServer::SetLockState(ep, state, opSource, ...)` emits the
  LockOperation / LockOperationError events controllers expect; the
  2-arg overload silently writes the attribute.
- **F5: no fifth-trap analogue.** `cluster::door_lock::create()` has no
  VALIDATE_FEATURES macro and does not abort on a default config
  (esp_matter_cluster.cpp:2054-2098). The endpoint thunk is plain.
- **F6: LockOperationError on deny and DoorLockAlarm on wrong-code
  lockout are emitted by the server itself**; the app emits other
  alarms explicitly (SendLockAlarmEvent). v1 exposes no alarm surface.

## 3. The forwarding mechanism (firmware)

- **One pending-verdict mailbox.** Matter serializes command invokes on
  the CHIP task, so exactly one adjudication can be in flight; the
  mailbox is a single slot holding {seq, ep, cluster, command, verdict,
  state} plus a FreeRTOS binary semaphore.
- Flow: the ember callback (C++, main.cpp) calls the C-linkage
  `mt_cmd_forward(ep, cluster, command)` (mt_at.c), which assigns the
  next sequence number, raises `+MTCMD:<seq>,<ep>,<cluster>,<command>`
  through `mt_at_urc()` (the one URC path, s_at_up-gated as always),
  and blocks on the semaphore with a **1000 ms deadline**.
- `AT+MTCMDRESP=<seq>,<verdict>` runs on the AT parser task. It
  validates seq against the pending slot, stores the verdict, gives
  the semaphore. **It must not take the ChipStackLock**: the CHIP task
  is blocked inside a cluster callback, and taking the lock here
  deadlocks the 1 s window. Mailbox access uses a critical section
  (taskENTER_CRITICAL), nothing else.
- **Default-deny.** Timeout, link down (`s_at_up` false), or no
  pending seq match: the callback returns false with
  OperationErrorEnum::kUnspecified; the controller sees
  Status::Failure and the server-emitted LockOperationError (F6). A
  late `AT+MTCMDRESP` for a seq that already timed out answers
  `+MTERR:1`; the firmware additionally raises `+MTCMDTO:<seq>` at the
  moment of timeout so the host learns its answer window was missed.
- Accepted consequence, documented in the spec and AT_MT_SPEC: while
  the CHIP task waits (worst case 1 s), other AT bridge commands
  (AT+MTATTR and friends) queue behind the stack lock. Bounded by the
  deadline; the AT link itself stays responsive for non-bridge
  commands, including AT+MTCMDRESP itself.

## 4. AT surface

- `+MTCMD:<seq>,<ep>,<cluster>,<command>` URC. Decimal fields, like
  +MTATTR. Door lock registers cluster 257 (0x0101) commands 0
  (LockDoor) and 1 (UnlockDoor). No payload field in v1: the bare
  commands carry none that the host may see (F3 strips PINs by
  refusing them earlier). A future consumer that needs a payload adds
  a fifth field without breaking existing parsers.
- `AT+MTCMDRESP=<seq>,<verdict>`: set-only; verdict 1 allow, 0 deny;
  anything else `+MTERR:1`. Unknown or stale seq `+MTERR:1`. Bare and
  query forms: plain ERROR (set-only convention, as AT+MTSWITCH).
- `AT+MTLOCK=<ep>,<state>[,<source>]`: drives the 6-arg
  `SetLockState` through a new ChipStackLock bridge
  (`mt_matter_lock_state_set`), so LockOperation events emit (F4).
  `<state>`: 0 NotFullyLocked, 1 Locked, 2 Unlocked (DlLockState
  values). `<source>` defaults to kManual (numeric value read from the pinned
  header, never transcribed); accepted values are
  OperationSourceEnum's, with the upper bound read from the pinned
  CHIP header at implementation time (never transcribed), `+MTERR:1`
  outside it. Lookup
  errors follow house semantics: `+MTERR:2` unknown endpoint,
  `+MTERR:3` endpoint without a DoorLock cluster. The firmware never
  calls SetLockState on its own after an allowed verdict: actuation
  timing belongs to the host (F4); LockState reads keep working over
  AT+MTATTR unchanged.
- Event mask: no new +MTEVT codes. The forward is its own URC, not a
  platform event.

## 5. Firmware pieces

- Devtype table row 20: `door_lock` (0x000A), max_variant 0, plain
  thunk (F5), feature map left at 0 (F3).
- The two ember callbacks in main.cpp forward and return the verdict;
  both funnel through one helper (they differ only in the command id
  logged). UnlockWithTimeout arrives at the unlock callback if a
  controller ever sends it (F1) and is adjudicated identically.
- mt_at.c owns the mailbox, sequence counter, `+MTCMD`/`+MTCMDTO`
  formatting, and the `AT+MTCMDRESP` handler; C-linkage
  `mt_cmd_forward()` is declared in mt_at.h for main.cpp. The bridge
  `mt_matter_lock_state_set(ep, state, source)` follows the existing
  mt_matter.h pattern (ChipStackLock, MT_ATTR_ERR_* returns routed
  through attr_err_to_mterr).
- MT_FW_VERSION "0.3.3". Docs: AT_MT_SPEC sections for the three new
  surfaces, the 20-row device table, ARCHITECTURE decision record for
  the mailbox and the lock-free responder.

## 6. Library: MatterDoorLock (Hearth-original)

- First class beyond upstream parity; the header says so and the README
  gets a "Hearth originals" section distinct from the parity table.
- API, house conventions throughout: `begin(bool locked = true)`;
  `onLock(cb)` / `onUnlock(cb)` with `bool cb(void)` returning the
  verdict; `setLockState(uint8_t state, uint8_t source = kManual)` (the
  enum value from the pinned header, exposed as a library constant)
  mapping
  to AT+MTLOCK; `getLockState()` (cache fed by +MTATTR URCs);
  `lock()` / `unlock()` conveniences calling setLockState with
  kManual. Unregistered callback: Hearth.poll() answers deny
  immediately (fail closed) rather than leaving the firmware to time
  out.
- `Hearth.poll()` dispatches `+MTCMD` to the endpoint's callback and
  sends `AT+MTCMDRESP` inline. **The 1000 ms deadline therefore
  includes host poll latency**: a sketch that blocks misses verdicts
  and the lock fails closed. Documented in the same README section as
  the poll() rule, with the +MTCMDTO URC as the diagnostic.
- Reconcile: begin(locked) declares 0x000A variant 0; the reconcile
  hook pushes the initial LockState via AT+MTLOCK (source kManual)
  exactly once per reconcile, the cabinet-round pattern (cache must
  mirror device; bug B120's lesson is the norm now).
- library.properties 0.3.3.

## 7. Out of scope, recorded

- **Matter credential storage** (controllers programming PINs/users
  into the device over COTA): requires the credential-database
  callback surface (GetUser/SetUser/GetCredential/SetCredential and
  persistence) bridged to or synced with the host, and F3 then makes
  PIN-carrying commands meaningful. Parked as the recorded phase 2;
  the sync-ownership question (who stores what) is its own design.
- UnlockWithTimeout/UnboltDoor command registration, auto-relock,
  alarm surface (SendLockAlarmEvent), DoorSense (door state attribute):
  none are needed for v1 adjudication; each is a later increment.
- Forwarding payloads in +MTCMD: the fifth field is reserved by
  design but v1 defines none.

## 8. Verification

- Firmware host tests: mailbox semantics are pure C where practical
  (seq matching, stale rejection); handler grammar tests per house
  style.
- Library host tests: callback dispatch and MTCMDRESP wire pins
  (exact strings, decimal IDs), deny-when-unregistered, MTLOCK pins
  including the source default, reconcile initial-state push, cache
  discipline.
- Bench (operator session): commission a lock; controller lock/unlock
  with the sketch allowing (Status::Success + LockOperation after the
  sketch's setLockState) and denying (Status::Failure +
  LockOperationError); verdict timeout with a deliberately silent
  sketch (default-deny + +MTCMDTO); host-local setLockState with
  kManual observed as a LockOperation event and a subscription update
  controller-side; +MTEP? shows the 0x000A row; regression pins for
  neighbouring surfaces stay green.
