# C5 regression harness, stage T1: design

Date: 2026-07-30
Status: approved
Scope: T1 of the build-out order in `docs/TESTING.md` §10, i.e. Phase 0 (link
preflight) and Phase 1 (raw AT protocol conformance). Phases 2.x (T2/T3) are
follow-on work and are out of scope here, as is T4.

`docs/TESTING.md` remains **authoritative for test content**: every command,
expected response and rationale in its §5, §6.1, §6.2 and §6.3 is the
inventory this harness implements, and is not restated in this document. This
document records the decisions T1 needed that TESTING.md left open.

## 1. Decisions taken with the user (2026-07-30)

- **Scope**: build and hardware-verify T1 as its own deliverable, then continue
  into T2/T3 as follow-on work under the same C5 task. C5 is not done at T1.
- **Hardware verification target**: both images. First the Thread image
  currently on the bench (commissioned, which Phase 1 must not disturb), then
  `build_b4` (WiFi) after a reflash with `fw/flash.py`.
- **Structure**: a single file, `test/mt_regression.py`, with the bespoke
  `check()` runner from TESTING.md §4. Not pytest (report format would diverge
  from the ESP-NOW RegressionSuite and ordered stateful phases fit it badly),
  and not a package yet (T1 does not need one; splitting later is mechanical).
- **The +MTEVT:4 double-fire question stays open.** It only affects Phase 2
  assertions (window lifecycle), so it blocks T2, not T1. Parked on the task
  graph as D16.

## 2. Architecture

One program, Python 3, standard library plus `pyserial`. No CHIP bindings, no
chip-tool dependency in T1.

```
test/mt_regression.py       the harness (this deliverable)
test/test_mt_regression.py  PC-only self-test of ATLink (no hardware)
test/baselines/thread.json  committed baseline, Thread image
test/baselines/wifi.json    committed baseline, WiFi image
```

### CLI

| Flag | Meaning |
|---|---|
| `--port <dev>` | Serial port. Default `$MT_PORT`, else `/dev/ttyACM0`. |
| `--phase 0\|1` | Run only that phase. Default: all implemented phases. |
| `-k <substring>` | Run only tests whose name contains the substring. Phase 0 always runs: it is a gate, not a test. |
| `--baseline <out.json>` | Record this run as a JSON baseline. |
| `--include-slow` | Accepted now, no effect until Phase 2's ~90 s window-expiry test exists. Kept so the CLI is stable across T1/T2/T3. |

Exit codes: `0` all checks passed, `1` at least one failure, `2` Phase 0
preflight abort (report is noise, nothing was scored).

## 3. ATLink

`ATLink` implements the result mapping of TESTING.md §4 (`0` for `OK`, `n > 0`
for `+MTERR:<n>` then `ERROR`, `-1` for bare `ERROR`, `-2` for timeout) and the
timestamped URC queue.

**Transport injection is the one structural addition.** `ATLink` talks to a
stream-like transport object (`read`/`write`/timeout), not to `serial.Serial`
directly. The real runner wraps pyserial; the self-test injects a scripted fake.
This is what makes the result mapping, echo-aware reading (`ATE1`/`ATE0`), URC
queueing and line assembly testable on the PC with no board, in the same spirit
as `test/host` for the composition codec.

URC semantics:

- Any `+MT…` line that is not the current command's own intermediate response is
  queued with a timestamp, never dropped, never matched against the command.
- `await_urc(regex, timeout)` consumes from the queue.
- `assert_no_urc(regex, window)` proves absence; it is how the four reset-form
  negatives assert "the device did not reboot" (no `+MTREADY` within 3 s).
- The queue is drained and logged between tests so one test's stray URC cannot
  satisfy the next test's expectation.

Non-`+MT` lines that are not command responses (console noise cannot occur on
this stream, but future firmware chatter might) are logged and never asserted
on.

## 4. Test inventory

Exactly the tables of TESTING.md §5, §6.1 and §6.2, with these T1 refinements:

- **Transport awareness**: `AT+MTNET?` is asserted against the format
  `+MTNET:(WIFI|THREAD),[01],[01]` and the answering transport is recorded in
  the report header. Nothing else in Phase 1 branches on transport; that is
  what lets the identical run pass on both images.
- **Onboarding codes**: assert the QR string starts `MT:` and the manual code is
  11 digits, assert stability across two consecutive reads, and record both
  values in the baseline. The baseline, not the harness, carries the expected
  value, so provisioned production units do not fail spuriously (TESTING.md
  §6.1).
- **Event mask**: the set / read-back / restore triple runs exactly as §6.1,
  restoring `0x0800003F` afterwards. It is the only state Phase 1 touches.
- **Exact codes**: every negative asserts the exact `+MTERR` code or the exact
  bare-`ERROR` shape, both directions, per §6.3.
- **State safety proof**: `AT+MTSTATE?` after each `AT+MTCOMMISSION` range
  negative asserts the state is unchanged, per §6.2.

## 5. Report and baseline

Console output matches the ESP-NOW RegressionSuite:

```
  [PASS] [AT+] MTVER? emits +MTVER:
  [FAIL] [AT-] MTCOMMISSION=901 -> +MTERR:1
===== RESULT: N passed, M failed =====
```

The JSON baseline records every test name and result, plus a header with:

- the `AT+CGMR` version string,
- git HEAD of this repo and of `iLabs_AT_ESP-now` (at_core is compiled in
  cross-repo, so both hashes are needed to bisect a regression),
- transport reported by `AT+MTNET?`,
- onboarding codes,
- port and timestamp,
- SSID from `$MT_SSID` when set (WiFi benches; absent on Thread).

Verified baselines are committed under `test/baselines/`, one per image.
Comparing a run against a baseline is a `diff`; the harness does not implement
comparison in T1.

## 6. Error handling

- Phase 0 failure aborts with exit 2 and a pointed message. The
  wrong-personality case (`AT+CGMM` reporting `ESP32-C6 ESP-NOW`) gets its own
  message naming the fix, per TESTING.md §5.
- Serial disconnect mid-run and Ctrl-C both close the port cleanly and print
  the summary of whatever was scored.
- A command timeout is a result (`-2`), not an exception; the bare-CR test
  depends on that.

## 7. Hardware verification procedure

1. Run the self-test (`python3 test/test_mt_regression.py`), no hardware.
2. Run the harness against the bench as it stands (Thread image, commissioned).
   All checks pass, and afterwards `AT+MTFABRICS?` still reports the same count
   and `AT+MTSTATE?` the same state as before the run: Phase 1's state-safety
   claim is itself part of the verification.
3. Record and commit `test/baselines/thread.json`.
4. `python3 fw/flash.py --build-dir build_b4 --port /dev/ttyACM0 --bridge espnow`,
   run again, record and commit `test/baselines/wifi.json`.

## 8. Out of scope, recorded so they are not rediscovered

- Phase 2 (T2/T3): chip-tool wrapper, commissioning, attribute round-trips,
  resets. Blocked in part on the +MTEVT:4 contract decision (graph node D16)
  and on the root-owned `/tmp/chip_counters.ini` issue from 2026-07-29.
- Baseline comparison inside the harness (T1 uses `diff`).
- Folding Phase 1 into an RP2350 sketch against the C4 host library
  (TESTING.md T4).
- TESTING.md §11 notes "Thread is not built"; that is stale since 2026-07-29
  but correcting TESTING.md is a docs task, not part of this harness change.
