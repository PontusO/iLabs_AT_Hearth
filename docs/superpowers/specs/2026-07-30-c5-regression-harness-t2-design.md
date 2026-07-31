# C5 regression harness, stage T2: Phase 2.1-2.5 design

Date: 2026-07-30. Follows the T1 design
(`2026-07-30-c5-regression-harness-t1-design.md`); T1 shipped the Phase 0 gate
and the 67 Phase 1 checks, hardware-verified on both images. T2 adds the
chip-tool driven Matter lifecycle, TESTING.md sections 2.1 to 2.5, plus the
run-end cleanup. 2.6 to 2.12 remain T3.

## 1. Decisions taken with the user (2026-07-30)

- **WiFi only.** T2 designs and verifies against `build_b4` with
  `chip-tool pairing ble-wifi`. Thread reuses the same checks later with a
  `ble-thread` pairing step and an OTBR dependency; nothing in this design may
  preclude that, but nothing implements it either.
- **Credentials via environment.** `MT_SSID` / `MT_PSK` are exported by the
  operator at run time. Nothing is stored in the repo; the baseline records the
  SSID, never the PSK.
- **Runs end factory-fresh.** The final step is `AT+MTRESET` plus a wipe of the
  chip-tool storage directory: fabric gone, composition kept, boot window open.
  A T2 run is idempotent and leaves the bench in the state every other tool
  documented against.
- **Approach A** for controller-side observation: one-shot chip-tool
  subprocesses for actions, one background `chip-tool onoff subscribe` process
  for report observation. A persistent `interactive start` REPL was rejected as
  a fragile parsing dependency; reads-only (no subscription) was rejected
  because only report observation catches a mode-0-notifies regression
  (TESTING.md 2.4). Hardware overturned the first rejection for the
  subscription only: see section 2 and the section 8.1 fallback.

## 2. Components

Everything lives in `test/mt_regression.py`, as in T1. No new dependencies:
chip-tool is a subprocess, parsed from stdout.

**`ChipTool`** wraps one-shot invocations.

- `run(args, timeout=60)` returns `(exit_code, stdout_text)`. Every call
  passes `--storage-directory <dir>`, except the `payload` subcommand
  family: those are pure parsers taking exactly one positional argument
  and no options, and the flag makes the real binary fail with "Wrong
  arguments number" (found during Task 11 hardware verification).
- Binary resolution: `--chip-tool` argument, else `MT_CHIPTOOL`, else the
  esp-matter build product
  `~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool`.
- Storage: `--storage` argument, else `MT_CHIPTOOL_STORAGE`, else
  `/tmp/mt-regression`. Wiped at Phase 2 start and again by the cleanup step.
- Constructor takes an injectable runner callable (defaults to
  `subprocess.run`) so self-tests feed canned stdout without spawning.

**`Subscriber`** owns the background report watcher.

- Runs `chip-tool interactive start` with
  `onoff subscribe on-off <min> <max> <node> <ep>` written to its stdin and
  stdout captured to a file; `reports()` parses the value reports seen so
  far; `stop()` sends `quit()` and escalates to terminate/kill. This is the
  section 8.1 fallback, forced during Task 11 hardware verification: the
  real binary's one-shot subscribe exits about 3 seconds after the priming
  report, so it can never observe a change-triggered report.
- **Concurrency rule, enforced by sequencing:** chip-tool's ini-file storage is
  not safe for concurrent processes. No one-shot `ChipTool.run()` happens while
  the Subscriber is alive. 2.4 therefore orders: subscribe, AT-side writes,
  teardown, controller reads.

**Phase 2 runner:** an ordered list of steps, each a function returning scored
checks through the existing `Suite`. Steps map one-to-one onto TESTING.md
2.1-2.5 plus the cleanup step.

## 3. Sequential semantics and exit codes

Phase 1 checks are independent and state-safe; Phase 2 steps chain, each
step's preconditions established by the one before. Consequences:

- A step whose failure invalidates its successors (reset fails, commissioning
  fails, subscriber cannot start) **aborts the phase**. Remaining steps are
  not scored; each prints `[SKIP] <name>: <reason>`.
- The summary becomes `N passed, M failed, K skipped`. Phase-1-only runs are
  unchanged (no skip line when K is 0).
- Exit code is non-zero on any fail **or** skip. A truncated run can no longer
  masquerade as a clean one, which closes the third N23 concern.

Phase 2 never runs by default. It requires an explicit `--phase 2`. The Phase
0 gate always runs first and gains three Phase-2-only checks: the chip-tool
binary exists and runs as the invoking user (regression guard for the
root-owned `/tmp/chip_counters.ini` incident of 2026-07-29), and `MT_SSID` /
`MT_PSK` are set. A missing precondition aborts at the gate with a
one-line remedy, before anything destructive happens.

## 4. Test inventory

Step numbering follows TESTING.md section 7. Assertions listed here are the
authoritative T2 set; where they extend TESTING.md, the extension is called
out.

**2.1 Factory-fresh baseline.** Capture `AT+MTCODES?` first (2.2 needs the
pre-reset value). `AT+MTRESET` returns OK; `+MTREADY` arrives within 15 s; the
first command after reboot gets one retry (the documented settling quirk,
graph N22); `+MTEVT:0` is observed after the reboot; `AT+MTFABRICS?` is 0;
`AT+MTSTATE?` reports state 1 (window open, no fabric).

**2.2 Onboarding codes are usable.** `AT+MTCODES?` after the reset equals the
pre-reset capture. Machine-usability is proven in 2.3 by parsing them.

**2.3 Commission over BLE + WiFi.** The passcode and discriminator are
extracted from `chip-tool payload parse-setup-payload <qr>` on the QR payload
read in 2.1, not hardcoded: if the codes stop being parseable, 2.3 fails at
the parse, which is 2.2's point made executable. Then
`pairing ble-wifi <node> <ssid> <psk> <passcode> <discriminator>`. Assert:

- chip-tool exits 0;
- `+MTEVT:1` (session started) then `+MTEVT:3` (complete) arrive on the AT
  link, in that order, within a 90 s commissioning budget;
- **exactly one `+MTEVT:4`, arriving after `+MTEVT:3`** (the DE24 pairing
  contract, commit eb15d0f: the window truly ends at completion; the
  session-establishment advertising stop must not leak), with no second
  `+MTEVT:4` in a 10 s watch after the first. This assertion extends
  TESTING.md and is the regression guard for the D16 double-fire;
- `AT+MTFABRICS?` is 1 and `AT+MTSTATE?` reports state 2.

**2.4 Attribute round-trip, host to controller.**

1. `AT+MTATTR=1,6,0,1` returns OK (its `+MTATTR:` echo is an intermediate
   response line, not a URC); `chip-tool onoff read on-off <node> 1` reports 1.
   Repeat for 0.
2. Subscriber up (min 0 s, max 5 s). `AT+MTATTR=1,6,0,1,1` (explicit notify):
   a new report with value 1 within 5 s. `AT+MTATTR=1,6,0,0,0` (local only):
   **no new report** within a 5 s settle window, while `AT+MTATTR=1,6,0` reads
   0. Subscriber down. `chip-tool onoff read` afterwards shows 0: the value
   changed, the report was suppressed. This is the only check in the suite
   that catches a mode-0-notifies regression.

**2.5 Attribute round-trip, controller to host.** Set the value to 1 via
`AT+MTATTR=1,6,0,1` first, so the controller's `off` is a real transition (a
no-change write may fire no callback and prove nothing). Then
`chip-tool onoff off <node> 1`, `on`, `toggle`: each must produce an
unprompted `+MTATTR:1,6,0,<v>` URC within 2 s and an agreeing
`AT+MTATTR=1,6,0` read afterwards.

**Cleanup (T2 addition).** `AT+MTRESET`, `+MTREADY` within 15 s,
`AT+MTFABRICS?` is 0, storage directory wiped. Scored, not just performed:
a cleanup that fails leaves a bench that lies to the next run.

## 5. ATLink changes

- **Expect-prefix ambiguity (N23 item 1) is resolved by sequencing, not by
  restructuring ATLink.** The standing rule: no `AT+MTATTR` command may be in
  flight while a controller-driven `+MTATTR` URC is expected. T2's steps obey
  it by construction (2.5 awaits the URC before issuing the read). The rule is
  documented in the module docstring for future phases.
- **Link-lost narrows (N23 item 2).** `main()`'s broad `except Exception`
  becomes `except (serial.SerialException, OSError)`. A harness-internal bug
  now raises a traceback instead of being mislabeled "link lost".

## 6. Configuration and baseline

- Environment: `MT_CHIPTOOL`, `MT_CHIPTOOL_STORAGE`, `MT_SSID`, `MT_PSK`.
  CLI overrides: `--chip-tool`, `--storage`, `--ssid`, `--psk`, and
  `--node-id` (default `0x4845`, recorded in the baseline header).
- `--phase` gains choice `2`. Default run remains phases 0+1.
- Phase 2 results are written by `--baseline` to a **separate committed
  file**, `test/baselines/wifi-lifecycle.json`, same format as T1 baselines
  with `node_id` and `ssid` added to the header. The T1 phase-1 baselines are
  untouched by T2 runs.

## 7. Self-tests

Hardware-free, in `test/test_mt_regression.py`, same pattern as T1:

- `ChipTool` with an injected fake runner: exit-code propagation, storage
  argument always present, read-value parsing (`onoff read` output),
  `parse-setup-payload` passcode/discriminator extraction, pairing
  success and failure stdout.
- `Subscriber.reports()` against a captured real `subscribe` output file:
  report counting, value extraction, and the no-new-report window logic.
- The sequential runner with scripted step outcomes: abort-on-broken-
  precondition, `[SKIP]` accounting, and the exit-code rule (fail or skip is
  non-zero).

## 8. Hardware verification procedure

1. **Bench preflight, before implementation is trusted:** chip-tool runs as
   the normal user and `payload parse-setup-payload` works on the baseline QR
   (also proves the counters file is writable by the user). Verify
   `chip-tool onoff subscribe` works non-interactively with a timeout on this
   binary. If it turns out interactive-only, the fallback replaces only the
   `Subscriber` internals with a minimal `interactive start` driver whose sole
   job is one subscription; the rest of the design stands.
2. Reflash `build_b4` (WiFi), `AT+MTRESET` to the documented start state.
3. Two consecutive clean `--phase 2` runs prove the cleanup step makes the
   phase idempotent.
4. A third run records `test/baselines/wifi-lifecycle.json`, committed.

## 9. Documentation ride-alongs

- TESTING.md 2.7 (`AT+MTCOMMISSION=60`) and 2.10 (`AT+MTCOMMISSION=30`) are
  below the 180 s floor shipped in eb15d0f and must be corrected to `=180`
  (2.10's wall-clock note changes accordingly). Both tests are T3 scope; the
  doc fix rides with T2 so the document never contradicts the firmware.
- TESTING.md section 7 gains the DE24 window-event contract note where 2.3 is
  described.

## 10. Out of scope, recorded so they are not rediscovered

- 2.6 to 2.12 (T3): root-endpoint URC suppression sweep, second fabric,
  warm-reboot and cold-boot persistence, window expiry (slow), the two-resets
  distinction, and the rig-restoring factory reset.
- Thread-image Phase 2 (needs `ble-thread` pairing and a running OTBR).
- Baseline comparison inside the harness (still `diff`).
- Multi-endpoint compositions: Phase 2 assumes the single `on_off_light`
  composition the bench carries.
