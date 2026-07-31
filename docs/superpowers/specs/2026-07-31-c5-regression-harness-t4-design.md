# C5 regression harness, stage T4: Thread-image Phase 2 design

Date: 2026-07-31. Follows the T3 design
(`2026-07-31-c5-regression-harness-t3-design.md`); T3 completed the full
Phase 2 chain (2.1-2.12) on the WiFi image, and the B63 fix work re-pinned
2.8/2.9 as persistence guards. T4 makes the same chain run against the
Thread image (`build_thread`), closing the transport asymmetry recorded as
out of scope in the T2 and T3 designs. C5 then covers both images end to
end.

## 1. Decisions taken with the user (2026-07-31)

- **Approach A: parameterize the existing chain by transport.** The gate
  detects the transport from `AT+MTNET?` and branches its preconditions;
  the only step code that branches is the pairing verb in 2.3 and 2.11.
  A Thread-specific step list was rejected: the chain is already
  transport-neutral everywhere else, proven by the 2026-07-29 hardware
  verification where chip-tool drove the device over the OMR route.
- **The gate checks OTBR, it does not manage it.** Starting `otbr-agent`
  needs sudo and the D-Bus policy file (graph F36); that stays a manual,
  documented bench step. The gate verifies liveness and aborts with a
  one-line remedy, matching how the WiFi gate treats missing credentials.
- **The Thread dataset is fetched live via `ot-ctl dataset active -x`**,
  so pairing always joins the network that actually exists;
  `MT_DATASET`/`--dataset` remain as overrides. The dataset is bench-local
  and carries no account secret, so unlike the WiFi PSK it may appear in
  logs, though the baseline still records none of it.
- **The bench ends on the WiFi image.** The hardware verification reflashes
  `build_b4` at the end and confirms the factory-fresh-with-composition
  convention, so every other tool keeps meeting the documented bench state.

## 2. Components (all in `test/mt_regression.py`, no new dependencies)

**Transport detection.** `phase2_gate` gains a first step: query
`AT+MTNET?` over a short-lived probe (the gate currently runs before the
main serial open; detection moves the port open earlier or the gate gains a
link argument, implementation's choice, but the gate must still refuse
before anything destructive). The transport lands in `ctx.transport`
(`"WIFI"` or `"THREAD"`); `--transport` overrides detection for the
self-tests and for forcing a mismatch error message.

**Gate branches.**
- WIFI: unchanged (credentials, chip-tool, openocd).
- THREAD: chip-tool and openocd checks unchanged, credentials replaced by:
  `otbr-agent` process alive; `ot-ctl state` answers with an active role
  (leader, router or child); `ot-ctl dataset active -x` yields a hex
  dataset, stored for the pairing steps. Each failure aborts with a
  one-line remedy naming the bring-up command.

**OtCtl runner.** A small injectable wrapper (`OTCTL` default path from the
ot-br-posix checkout, `MT_OTCTL`/`--ot-ctl` overrides; whether sudo is
required is pinned by the Task 1 preflight and encoded once). Self-tests
inject a fake runner; a captured real `dataset active -x` output becomes a
fixture the parser is validated against.

**Pairing verb selection.** 2.3 and 2.11 call a helper that builds the
pairing argv from `ctx.transport`: `ble-wifi <node> <ssid> <psk> ...` or
`ble-thread <node> hex:<dataset> <passcode> <discriminator>`. Everything
else in both steps is untouched.

## 3. What does NOT change

The step table, the gates (`--include-slow`/`--include-manual`), the exit
contract, the URC history sweep, the SWD warm reboot, the observed power
cycle, the composition capture/replay, the B63 guards, and the one-process
chip-tool rule are all transport-neutral and stay byte-identical. The
`+MTNET` phase-1 regex already accepts both transports and the mismatch
flag, so the transport-mismatch boot that follows reflashing between images
(`+MTEVT:27`, P2 semantics) needs no harness change: the chain's own 2.1
factory reset normalizes it, and the gate must merely not choke on the
mismatch state (it queries `AT+MTNET?`, which reports it fine).

## 4. Baseline

`test/baselines/thread-lifecycle.json`, recorded by an explicit
`--baseline` run with both include flags (the T3 refusal rule applies
unchanged). Header gains `transport: THREAD` naturally via capture_header;
`ssid` stays null on Thread runs, and the dataset is deliberately NOT
recorded in the header (it is reproducible from the live OTBR and has no
place in the repo). The WiFi baselines are untouched.

## 5. Bench unknowns pinned by the Task 1 preflight, not guessed

1. Whether `ot-ctl` reaches the agent socket as the normal user or needs
   sudo; the invocation that works is encoded in the OtCtl default and a
   real `dataset active -x` capture is committed as the parser fixture.
2. Whether the 2026-07-29 network's dataset auto-restores when
   `otbr-agent` restarts, or the network must be re-formed (and if so, the
   re-form one-liner goes into the docs).
3. The `ble-thread` pairing argv on this chip-tool binary (expected
   `pairing ble-thread <node> hex:<dataset> <passcode> <discriminator>`).
4. The transport-mismatch boot sequence when `build_thread` first boots
   over WiFi-era NVS: confirm `+MTEVT:27` appears, `AT+MTNET?` reports the
   mismatch flag, and 2.1's reset clears it, so the run needs no special
   handling.

## 6. Self-tests

Hardware-free, same pattern: gate transport branches (WIFI requires
credentials, THREAD requires a live OtCtl fake and a parseable dataset,
each failure message named); dataset parser against the real fixture;
pairing argv helper for both transports (FakeChipRunner argv assertions in
the existing TestStep23/TestStep211 style); `--transport` override plumbing.

## 7. Hardware verification procedure

1. Task 1 preflight pins the section 5 unknowns and commits the ot-ctl
   fixture. OTBR bring-up is manual, documented in the findings with the
   F36 gotchas (run otbr-agent directly, D-Bus policy file mandatory).
2. Implementation tasks follow, hardware-free.
3. Full verification: flash `build_thread` (with the SDKCONFIG redirect
   rule from CLAUDE.md), OTBR up, two full-flag runs (operator present for
   the 2.9 power cycles), byte-identical check lines, a third run recording
   `thread-lifecycle.json`, a default run proving the gates, then reflash
   `build_b4`, confirm factory-fresh-with-composition, and leave OTBR
   stopped (the bench default).

## 8. Documentation ride-alongs

- TESTING.md section 7 preamble: one paragraph on transport
  parameterization (detection, the Thread gate preconditions, the pairing
  verb difference) and the second committed baseline.
- TESTING.md section 9 (shared at_core, run both suites) gains the Thread
  phase 2 run command next to the WiFi one.

## 9. Out of scope, recorded so they are not rediscovered

- The combined WiFi+Thread single image (DE35): its own design cycle.
- OTBR lifecycle management by the harness, and unattended OTBR health
  monitoring.
- Thread-only commissioning paths that bypass BLE (network-layer joining):
  the firmware commissions over BLE on both transports by design.
- Cross-transport reflash tests (WiFi image commissioned, reflash to
  Thread, assert the P2 mismatch contract end to end): worth a dedicated
  test some day; today the P2 behavior is covered by its 2026-07-29
  hardware verification only.
