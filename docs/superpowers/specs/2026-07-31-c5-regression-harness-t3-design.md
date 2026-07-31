# C5 regression harness, stage T3: Phase 2.6-2.12 design

Date: 2026-07-31. Follows the T2 design
(`2026-07-30-c5-regression-harness-t2-design.md`); T2 shipped the chip-tool
lifecycle (2.1-2.5 plus cleanup), hardware-verified with the
`wifi-lifecycle.json` baseline. T3 adds TESTING.md 2.6 to 2.12: the
root-endpoint URC sweep, the second fabric, both reboot-persistence tests, the
window-expiry test, the two-resets distinction, and the rig-restoring factory
reset. It also retires the T2 review carry-overs (graph note N41).

## 1. Decisions taken with the user (2026-07-31)

- **Approach A: extend the existing Phase 2 chain.** The T3 tests are steps
  appended to the same sequential runner. One `--phase 2` run covers 2.1
  through 2.12. A separate phase 3 was rejected: 2.6 is defined across the
  whole of Phase 2, and 2.7 onward need the commissioned state 2.3 built, so
  a separate phase would either re-commission needlessly or depend on phase 2
  having just run, which is the same chain with an extra seam.
- **2.9 uses an operator prompt** behind `--include-manual`. uhubctl VBUS
  automation was deferred until the bench has a PPPS-capable hub; deferring
  the test entirely was rejected because the bug it guards (the B4.3
  boot-loop) needs exactly this test to stay dead.
- **Separate gates**: `--include-slow` for 2.10 (unattended, ~200 s),
  `--include-manual` for 2.9 (needs a human). An unattended run can take the
  slow test without stalling at a prompt.
- **Runs end with the composition re-declared.** 2.11/2.12 end in a true
  post-`AT+MTFRESET` state (no composition). The cleanup captures `AT+MTEP?`
  at phase start and replays it after the final factory reset, so the bench
  keeps ending in the state every tool and baseline documents: factory-fresh,
  composition declared, window open. This also self-repairs a bench someone
  factory-reset by hand.

## 2. Components (all in `test/mt_regression.py`, no new dependencies)

**ATLink URC history.** A new append-only `urc_history` list of
`(timestamp, line)` records every URC the reader thread ever queues.
`drain()` keeps clearing the pending queue; the history is never cleared.
2.6 reads the history; nothing else changes for the await helpers. This also
retires the N41 drain-visibility complaints: FakeLink mirrors the same
two-container semantics, so a step that skips its `drain()` or a drain that
stops clearing becomes observable in self-tests.

**Warm-reset helper.** `swd_reset()` shells out to
`openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "init; reset run;
shutdown"` with an injectable runner. Resetting the RP2350 re-runs the bridge
sketch's `setup()`, which pulses `PIN_ESP_RST`, giving the C6 a hardware
reset with NVS intact: exactly the RP2350-driven warm reboot TESTING.md 2.8
names. The harness closes the serial port first, waits for the by-id path to
come back, reopens, and allows the one documented settling retry (N22). The
phase 2 gate gains a check that the openocd binary exists.

**Operator power-cycle helper.** Used only by 2.9 under `--include-manual`:
prints unplug/replug instructions, then *verifies the cycle actually
happened* by watching the by-id device path disappear and reappear before
waiting for `+MTREADY`. A prompt that trusts the operator without observing
the disappearance would pass with no power cycle at all.

**Second controller identity.** 2.7 uses a second `ChipTool` instance with
storage `<storage>-f2` and node id `<node_id>+1`. Both storages are wiped at
phase start and by the cleanup. The one-process rule is unchanged: the two
ChipTool instances never run concurrently, and neither runs while a
Subscriber lives.

**Composition capture and replay.** At phase start the runner captures the
`+MTEP:` lines from `AT+MTEP?`. The cleanup replays them after the final
`AT+MTFRESET` with the section 3.9 staging grammar: `AT+MTEPCLEAR`, one
`AT+MTEP=<device_type>` per captured endpoint in index order, then
`AT+MTEPAPPLY`, which persists and reboots on its own. After the reboot the
`AT+MTEP?` readback must match the capture (endpoint ids are reproducible by
design, the boot rebuild allocates from the same base).

## 3. Sequencing, gates, and the exit contract

Step order: 2.1-2.5 (unchanged), 2.7, 2.8, 2.9 (gated), 2.10 (gated), 2.11,
2.12+cleanup, then 2.6 scored last so its sweep covers every URC of the whole
run including the cleanup reboots.

Gate-skips are a new, distinct outcome. T2's rule "any skip exits nonzero"
exists so a truncated run cannot masquerade as clean; a deliberately gated-out
step is not truncation. So:

- A step skipped because its flag is absent prints
  `[SKIP] <name> (gated: --include-slow)` and does **not** affect the exit
  code.
- A step skipped because an earlier step aborted keeps the T2 behavior:
  nonzero exit.
- The summary distinguishes them: `N passed, M failed, K skipped, G gated`.

`--baseline` refuses to run unless both include flags are present: a committed
baseline must never contain gated-out entries. The baseline stays
`test/baselines/wifi-lifecycle.json`, growing from 38 to the full T3 set; T1
phase-1 baselines remain untouched.

## 4. Test inventory

Assertions here are the authoritative T3 set; extensions over TESTING.md are
called out.

**2.7 Additional window, second fabric.** Precondition: commissioned from
2.5. `AT+MTCOMMISSION=180` returns OK; `+MTEVT:0`; `AT+MTSTATE?` is 1.
Second controller commissions through the open window (verb pinned in Task 1:
expected `pairing onnetwork-long <node2> <passcode> <long-discriminator>`
since the device is already on the network; `ble-wifi` is the fallback).
Assert: chip-tool exits 0; `+MTEVT:1` then `+MTEVT:3`; exactly one `+MTEVT:4`
after `+MTEVT:3` with none in a 10 s watch (the DE24 pair holds for
host-opened windows too, an extension); `AT+MTFABRICS?` is 2; `AT+MTSTATE?`
is 2. Then the second controller removes its own fabric
(`operationalcredentials remove-fabric <index>`, index source pinned in Task
1); assert `AT+MTFABRICS?` returns to 1.

**2.8 Warm reboot persistence.** Set the light on and read it back. Close the
port, `swd_reset()`, reopen, `+MTREADY` within 15 s. Assert
`AT+MTFABRICS?` is 1, `AT+MTATTR=1,6,0` reads 1, `AT+MTSTATE?` is 2, and no
`+MTEVT:0` arrives in a 5 s watch (a commissioned device must not open a boot
window; an extension of TESTING.md).

**2.9 Cold boot with a state change at init (gated `--include-manual`).**
Set the light on and read it back. Operator prompt; verified power cycle;
`+MTREADY` within 15 s of re-enumeration. Assert `AT+MTFABRICS?` is 1 and
`AT+MTATTR=1,6,0` reads 0 (StartUpOnOff toggled at init, proving the URC path
survived the pre-`mt_at_start()` window). A read of 1 means no state change
occurred and the run proved nothing: scored as a failure whose message says
"inconclusive, no state change at init", per TESTING.md's instruction that it
must not pass.

**2.10 Window expiry (gated `--include-slow`).** `AT+MTCOMMISSION=180` with no
controller: OK and `+MTEVT:0`. Wait up to 200 s for the window-end report.
Which URC ends a never-attached window (`+MTEVT:5`, `+MTEVT:4`, or both, and
in what order) is pinned on hardware during implementation and then asserted
exactly; TESTING.md and `AT_MT_SPEC.md` are updated in the same commit.
Afterwards: `AT+MTSTATE?` is 2 and no further window URC in a 10 s watch.

**2.11 The two resets differ in exactly one respect.** Capture `AT+MTEP?`.
`AT+MTRESET`, `+MTREADY`, assert `AT+MTFABRICS?` is 0 **and** `AT+MTEP?`
returns the identical lines. Wipe the first controller's storage (its fabric
died with the reset), re-commission over BLE + WiFi, assert `AT+MTFABRICS?`
is 1. Then `AT+MTFRESET`, `+MTREADY`, assert `AT+MTFABRICS?` is 0 **and**
`AT+MTEP?` returns zero lines.

**2.12 Factory reset returns the rig to a known state, plus cleanup.**
After the `AT+MTFRESET`: the old node is unreachable (`onoff read` against
the stale node id exits nonzero within a bounded timeout); the device
advertises commissionable again (`AT+MTSTATE?` is 1 and a fresh `+MTEVT:0`
was raised after the reset, the unprovisioned auto-window). Cleanup: replay
the captured composition, `AT+MTRESET`, verify `AT+MTEP?` matches the
capture, `AT+MTFABRICS?` is 0, `AT+MTSTATE?` is 1, wipe both chip-tool
storages. The bench ends exactly as T2 left it.

**2.6 Root-endpoint URCs stay off the link.** Scored last, over the full
`urc_history` of the run: no line matches `+MTATTR:0,`. Every reboot in the
run (four resets plus the reboots inside commissioning) widens the net this
sweep casts.

## 5. T2 carry-overs retired (N41)

- Fallback discriminator regex in `parse_setup_payload`: gains a fixture-less
  unit test.
- FakeLink honesty: the `urc_history`/pending split lands in FakeLink too;
  the drain tests seed a stale `+MTREADY` so non-clearing drain semantics
  become observable, and TestStep23/25's URC release is keyed to runner
  activity where the step under test makes that meaningful.
- `-k` under `--phase 2`: now filters phase 2 steps by name, with the
  constraint that deselecting a step every later step depends on turns the
  later steps into abort-skips (the dependency chain is explicit in the step
  table).
- Cleanup-after-abort: an abort in any step after 2.3 now runs a best-effort
  `AT+MTRESET` recovery (not scored) unless the abort reason is link death,
  so a failed run does not strand a commissioned device.

## 6. Configuration and CLI

- `--include-slow` becomes effective (2.10); `--include-manual` is new (2.9).
- The second storage directory is derived (`<storage>-f2`), not a new flag;
  the second node id is `<node_id>+1`.
- `--baseline` requires both include flags when `--phase 2` is selected.
- Environment and credential handling are unchanged from T2 (`MT_SSID` /
  `MT_PSK` at run time, PSK never stored anywhere).

## 7. Self-tests

Hardware-free, same pattern as T1/T2:

- `urc_history` semantics: appended on every URC, survives `drain()`, FakeLink
  mirrors both containers.
- Gate-skip vs abort-skip: exit-code rule (gated is zero, abort is nonzero),
  summary line shape, baseline refusal without both flags.
- `swd_reset` with an injected runner: argv shape, failure propagation.
- The power-cycle verifier with an injected path-watcher: pass, no-disappear
  (operator did nothing), no-reappear (timeout).
- Steps 2.7-2.12 against FakeLink/FakeChipRunner scripts, including 2.9's
  inconclusive-read failure, 2.11's identical-composition comparison, and the
  2.6 sweep in both the clean and the poisoned case.
- The N41 items' tests as listed in section 5.

## 8. Hardware verification procedure

1. **Task 1 bench preflight pins the unknowns before implementation is
   trusted:** the SWD reset round-trip (openocd, port re-enumeration, one
   settling retry, `+MTREADY`); the second-fabric verb on this chip-tool
   binary and where the fabric index for `remove-fabric` comes from; the
   window-expiry URC contract via one scripted 200 s observation run
   (`+MTEVT:5` vs `+MTEVT:4`); and the `AT+MTEP=` declaration grammar
   round-trip (declare, reboot, readback) on a scratch entry.
2. Implementation tasks follow, all hardware-free.
3. Full verification: two complete `--phase 2 --include-slow --include-manual`
   runs (operator present for the two prompts), byte-identical check lines,
   then a third recording the extended `wifi-lifecycle.json`, committed.
4. A final default run (no flags) proves the gated steps skip cleanly with
   exit 0.

## 9. Documentation ride-alongs

- TESTING.md 2.10 gets the pinned expiry-URC contract; `AT_MT_SPEC.md`
  section 3.11 gets the same fact if it is not already implied.
- TESTING.md 2.12 gains a note that the harness restores the composition
  after the factory reset, so the rig convention (factory-fresh with
  composition) is maintained by the run itself.
- TESTING.md section 7 preamble notes the gate flags and the summary's
  gated-skip category.

## 10. Out of scope, recorded so they are not rediscovered

- Thread-image Phase 2 (needs `ble-thread` and an OTBR on the bench).
- uhubctl VBUS automation for 2.9 (revisit if the bench gains a
  PPPS-capable hub).
- Baseline comparison inside the harness (still `diff`).
- Multi-endpoint compositions; the sweep and replay assume the single
  `on_off_light` bench composition but are written against whatever
  `AT+MTEP?` returns, so they do not preclude more.
