# C5 Regression Harness T3 (Phase 2.6-2.12) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the Phase 2 chain in `test/mt_regression.py` with TESTING.md
2.6-2.12: the root-endpoint URC sweep, second fabric, warm and cold reboot
persistence, window expiry, the two-resets distinction, and a cleanup that
restores the bench composition after the factory reset.

**Architecture:** Approach A per the design spec
(`docs/superpowers/specs/2026-07-31-c5-regression-harness-t3-design.md`): the
new tests are steps appended to the existing sequential runner. New machinery:
an append-only URC history on ATLink, gate-skips as a distinct non-failing
outcome, an SWD reset helper (openocd resets the RP2350, whose bridge setup
pulses the C6 reset), an observed operator power-cycle, and a second ChipTool
identity. Step order: 2.1-2.5, 2.7, 2.8, 2.9 (gated), 2.10 (gated), 2.11,
2.12+cleanup, 2.6 scored last.

**Tech Stack:** Python 3 stdlib + pyserial (already required). chip-tool as a
subprocess. openocd (already on the bench) for the SWD reset. No new
dependencies.

## Global Constraints

- **No em dashes** anywhere: code, comments, docs, commit messages. Colon,
  comma, parentheses or a full stop instead.
- **No new Python dependencies.** stdlib + pyserial only.
- **One chip-tool process at a time.** Never a `ChipTool.run()` (either
  instance) while a Subscriber lives; the two ChipTool instances never run
  concurrently.
- **MT_PSK must never appear** in any file, fixture, baseline, report,
  commit message, or log excerpt copied into a report.
- **Expect-prefix rule** (module docstring): no `AT+MTATTR` command in
  flight while a controller-driven `+MTATTR` URC is expected.
- **`ATLink.drain()` clears the pending URC queue.** Never read `link.urcs`
  after a drain expecting history; that is what `urc_history` (Task 2) is
  for.
- Hardware is touched ONLY in Tasks 1 and 11. Every other task must run
  `python3 test/test_mt_regression.py` green with no device attached.
- Commit messages explain why, and end with exactly:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

- The harness file is `test/mt_regression.py`, self-tests are
  `test/test_mt_regression.py` (run from `test/`). 4-space indent, match the
  file's existing style and docstring voice.
- Node ids: primary `0x4845` (existing default), second controller uses
  `node_id + 1` (`0x4846`). Second storage dir is `<storage>-f2`.
- Timeouts: 120 s subprocess timeout for pairing calls, 30 s for reads,
  90 s awaits for `+MTEVT:1`/`+MTEVT:3`, 15 s for `+MTREADY`, 10 s
  duplicate-`+MTEVT:4` watch, 200 s expiry wait in 2.10.
- Bench commands in Tasks 1 and 11 resolve the port via
  `/dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00` and pass that
  by-id path (not the ttyACM name) as `--port`, so reopen-after-reset finds
  the device again.

---

### Task 1: Bench preflight: pin the four unknowns

Bench-facing. Requires `MT_SSID`/`MT_PSK` (load with `set -a; . ./.env;
set +a` in the repo root; if absent, STOP and ask). The bench starts
factory-fresh with the composition declared and the window open (T2's end
state). Do not modify any production or test code in this task. Record every
finding in `.superpowers/sdd/2026-07-31-c5-regression-harness-t3/task-1-findings.md`
with verbatim command output (minus any line containing the PSK).

**Files:**
- Create: `test/fixtures/chiptool_read_fabrics.txt` (real capture)
- Create: `.superpowers/sdd/2026-07-31-c5-regression-harness-t3/task-1-findings.md`

**Interfaces:**
- Produces: the findings file with four pinned facts that Tasks 5, 6, 8 and
  10 transcribe: (a) SWD reset round-trip behavior, (b) the second-fabric
  pairing verb and the remove-fabric argv, (c) the window-expiry URC
  sequence, (d) the MTEP staging round-trip including whether endpoint ids
  reproduce. Plus the committed read-fabrics fixture Task 5's parser is
  validated against.

- [ ] **Step 1: SWD reset round-trip**

```bash
PORT=$(readlink -f /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)
BYID=$(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)
# Confirm the device answers first (phase 0):
python3 test/mt_regression.py --port $BYID --phase 0
# Reset the RP2350 over SWD; the bridge setup() pulses the C6 reset:
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "init; reset run; shutdown"
# Watch the by-id path disappear and reappear, then immediately listen:
python3 - "$BYID" <<'EOF'
import sys, time, os, serial
p = sys.argv[1]
t0 = time.monotonic()
while os.path.exists(p) and time.monotonic() - t0 < 10: time.sleep(0.05)
gone = time.monotonic() - t0
while not os.path.exists(p) and time.monotonic() - t0 < 30: time.sleep(0.05)
back = time.monotonic() - t0
time.sleep(0.3)
s = serial.Serial(p, 115200, timeout=0.2)
t1 = time.monotonic(); buf = b""
while time.monotonic() - t1 < 20:
    buf += s.read(256)
    if b"+MTREADY" in buf: break
print("gone at %.2fs, back at %.2fs, MTREADY seen: %s (%.2fs after reopen)"
      % (gone, back, b"+MTREADY" in buf, time.monotonic() - t1))
EOF
```

Record: whether the path actually disappears (it may not if openocd is
faster than the kernel notices; then record that), how long re-enumeration
takes, and whether `+MTREADY` is captured after reopening. If `+MTREADY` is
NOT captured (lost in the enumeration race), record that clearly: Task 6
then probes with `AT` + `AT+MTSTATE?` instead of awaiting `+MTREADY`, and
the findings file must say which variant Task 6 must implement.

- [ ] **Step 2: commission and pin the second-fabric verb**

```bash
set -a; . ./.env; set +a
BYID=$(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)
BIN=~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool
S1=/tmp/mt-t3-pre1; S2=/tmp/mt-t3-pre2
rm -rf $S1 $S2; mkdir -p $S1 $S2
# Passcode/discriminator from the device codes (AT+MTCODES? via a quick probe,
# then parse-setup-payload). Commission fabric 1:
$BIN pairing ble-wifi 0x4845 "$MT_SSID" "$MT_PSK" <passcode> <long-discriminator> --storage-directory $S1
# Open an additional window (via a probe script or manually over the AT link):
#   AT+MTCOMMISSION=180 -> OK, +MTEVT:0
# Second controller, over the network (the device is already on WiFi):
$BIN pairing onnetwork-long 0x4846 <passcode> <long-discriminator> --storage-directory $S2
```

If `onnetwork-long` fails, try `ble-wifi` through the same window and record
which verb works, its exit code, and the `+MTEVT` sequence observed on the AT
link (expected: 1, 3, then exactly one 4). Record the verb and full argv in
the findings file; Task 5 transcribes it verbatim.

- [ ] **Step 3: capture read-fabrics and pin remove-fabric**

```bash
$BIN operationalcredentials read fabrics 0x4846 0 --storage-directory $S2 \
  | tee /tmp/read_fabrics_raw.txt
# Strip nothing: commit the raw capture (it contains no secrets):
cp /tmp/read_fabrics_raw.txt test/fixtures/chiptool_read_fabrics.txt
# Identify the FabricIndex of the 0x4846 entry, then:
$BIN operationalcredentials remove-fabric <index> 0x4846 0 --storage-directory $S2
# Verify on the AT link: AT+MTFABRICS? -> +MTFABRICS:1
```

Record the exact remove-fabric argv that worked and where the index came
from in the output. Then commit the fixture:

```bash
git add test/fixtures/chiptool_read_fabrics.txt
git commit -m "test: capture real read-fabrics output for the T3 index parser

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

- [ ] **Step 4: window-expiry URC contract (~200 s)**

With the device still commissioned (fabric 1 present): open
`AT+MTCOMMISSION=180`, touch nothing, and capture every URC with timestamps
for 200 s (a small pyserial script like Step 1's, logging all `+` lines).
Record: which events arrive (`+MTEVT:5` fail-safe, `+MTEVT:4` window
closed, both, neither), their order and timing, and whether `AT+MTSTATE?`
returns to `2,1` afterwards. This is the contract Task 8 asserts and
documents; the findings file must state it as one unambiguous sentence.

- [ ] **Step 5: MTEP staging round-trip**

On the AT link (probe script): capture `AT+MTEP?` (expect one line,
`+MTEP:0,1,0x0100` or similar). Then `AT+MTEPCLEAR` -> OK,
`AT+MTEP=<the captured devtype>` -> OK, `AT+MTEPAPPLY` -> OK, wait
`+MTREADY`, `AT+MTEP?` again. Record whether the readback is byte-identical
to the capture (endpoint id reproduced). Then restore the bench:
`AT+MTRESET` (erases the preflight fabric), verify `+MTREADY`,
`AT+MTFABRICS?` 0, `AT+MTSTATE?` 1,0. Wipe `$S1` and `$S2`.

- [ ] **Step 6: write the findings file**

All four facts, each with verbatim evidence and the one-sentence conclusion
Tasks 5, 6, 8 and 10 will transcribe. State the bench end state (factory
fresh, composition intact, storages wiped). The findings file is gitignored
(`.superpowers/`), so no commit for it.

### Task 2: ATLink URC history and FakeLink honesty

**Files:**
- Modify: `test/mt_regression.py` (ATLink `__init__`, `_collect`,
  `_pump_one`; new `_queue_urc`)
- Modify: `test/test_mt_regression.py` (FakeLink, new tests)

**Interfaces:**
- Produces: `ATLink.urc_history`: list of `(timestamp, line)`, append-only,
  every URC ever queued, never cleared by `drain()`. `FakeLink.urc_history`
  with identical semantics. Task 10's `step_2_6_root_urc_sweep` reads
  `ctx.link.urc_history`.

- [ ] **Step 1: write the failing tests**

Add to `test/test_mt_regression.py` (a new class near TestAwaitUrcTs):

```python
class TestUrcHistory(unittest.TestCase):
    def test_history_survives_drain(self):
        link, _ = link_with_reply(b"+MTEVT:0\r\n+MTATTR:0,40,2,1\r\nOK\r\n")
        link.command("AT")
        link.drain(0.05)
        self.assertEqual(link.urcs, [])
        self.assertEqual([u for _, u in link.urc_history],
                         ["+MTEVT:0", "+MTATTR:0,40,2,1"])

    def test_history_accumulates_across_commands(self):
        link, t = link_with_reply(b"+MTEVT:1\r\nOK\r\n")
        link.command("AT")
        t.rx += b"+MTEVT:3\r\nOK\r\n"
        link.command("AT")
        self.assertEqual([u for _, u in link.urc_history],
                         ["+MTEVT:1", "+MTEVT:3"])

    def test_fake_link_mirrors_history(self):
        link = FakeLink(commands={"AT+MTRESET": (0, [])},
                        stale_urcs=["+MTREADY"])
        link.drain()
        link.command("AT+MTRESET")
        self.assertIn("+MTREADY", [u for _, u in link.urc_history])
        self.assertEqual(link.urcs, [])
```

(`link_with_reply` already exists; check its exact return shape before
using, and adapt the second test if the fake transport attribute is not
named `rx`. The intent of each assertion is fixed; the plumbing follows the
file's existing helpers.)

- [ ] **Step 2: run, expect FAIL** (`AttributeError: urc_history`)

Run: `cd test && python3 test_mt_regression.py TestUrcHistory`

- [ ] **Step 3: implement in ATLink**

In `__init__`: add `self.urc_history = []`. Add a helper and use it at BOTH
queueing sites (`_collect`'s stray-`+` branch, `_pump_one`):

```python
    def _queue_urc(self, line):
        entry = (time.monotonic(), line)
        self.urcs.append(entry)
        self.urc_history.append(entry)
```

Replace the two `self.urcs.append((time.monotonic(), line))` calls with
`self._queue_urc(line)`. `drain()` is untouched: it clears `self.urcs`
only. Extend the class docstring's last sentence: "and mirrored into
urc_history, which drain() never clears (2.6 sweeps it)".

- [ ] **Step 4: mirror in FakeLink**

In `test/test_mt_regression.py`, FakeLink: wherever a URC enters `urc_queue`
or `urcs` (construction seeding, `no_reset` seeding, post-reset release,
`urcs_after_drain` release), also append the same `(ts, line)` entry to a
new `self.urc_history` list initialized in `__init__`. `drain()` keeps
clearing only the pending queue. Read the current FakeLink implementation
first; there are four distinct seeding paths (stale_urcs, no_reset,
commands-triggered, urcs_after_drain) and every one must mirror.

- [ ] **Step 5: run the new tests, then the whole file**

Run: `cd test && python3 test_mt_regression.py`
Expected: all pass (69 existing + 3 new).

- [ ] **Step 6: add the stale-`+MTREADY` drain-honesty test (N41 item 3)**

```python
    def test_drain_clears_stale_ready_that_would_satisfy_await(self):
        """A gutted drain() must fail this: the stale +MTREADY would
        satisfy the await that follows it."""
        link = FakeLink(commands={"AT+MTRESET": (0, [])},
                        stale_urcs=["+MTREADY"])
        link.drain()
        self.assertIsNone(link.await_urc(r"\+MTREADY$", timeout=0.05))
```

Verify it bites: comment out the queue-clear in FakeLink.drain, run, watch
this test FAIL, restore, run green.

- [ ] **Step 7: add the fallback discriminator regex test (N41 item 1)**

```python
    def test_parse_setup_payload_short_discriminator_fallback(self):
        text = "Passcode: 20202021\nDiscriminator value: 3840\n"
        self.assertEqual(parse_setup_payload(text), (20202021, 3840))
```

- [ ] **Step 8: full run and commit**

Run: `cd test && python3 test_mt_regression.py` -> OK.

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: ATLink urc_history, drain-proof, mirrored honestly in FakeLink

2.6 needs every URC of the whole run; drain() clears the pending queue
by design, so assertions built on link.urcs after a drain see nothing
(three separate incidents during D16). The history is a second
append-only container both containers' semantics are now pinned by
tests, including a stale +MTREADY probe that fails if drain stops
clearing. Rides along: the fallback discriminator regex gets its first
test (T2 review carry-over N41).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

### Task 3: Gate-skips, the step table, -k for phase 2, abort recovery

**Files:**
- Modify: `test/mt_regression.py` (Suite, PHASE2_STEPS shape, run_phase2,
  main argparse)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: existing `Suite`, `StepAbort`, `PHASE2_STEPS`, `run_phase2`.
- Produces: `Suite.gate_skip(name, flag)` and `Suite.gated` (list);
  PHASE2_STEPS entries become dicts
  `{"name": str, "fn": callable, "gate": str|absent, "requires": [names]|absent}`;
  `run_phase2(ctx)` handling gates, `-k`, requires, and calling
  `recover_after_abort(ctx, reason)`; argparse gains `--include-manual`;
  `--baseline` with `--phase 2` requires both include flags. Exit code
  formula unchanged (`gated` never affects it).

- [ ] **Step 1: write the failing tests**

```python
class TestGateSkips(unittest.TestCase):
    def _ctx_with_steps(self, steps, **optkw):
        # fresh_ctx() exists; read it first and reuse its FakeLink/opts
        # construction, then override PHASE2_STEPS around run_phase2.
        ...

    def test_gated_step_skips_without_failing_exit(self):
        s = Suite()
        s.check("a", True)
        s.gate_skip("2.10 window expiry", "include_slow")
        self.assertEqual(s.failed, 0)
        self.assertEqual(s.skipped, [])
        self.assertEqual(len(s.gated), 1)

    def test_summary_shows_gated_count(self):
        s = Suite()
        s.check("a", True)
        s.gate_skip("2.9 cold boot", "include_manual")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            s.summary()
        self.assertIn("1 gated", buf.getvalue())

    def test_run_phase2_gates_and_requires(self):
        calls = []
        steps = [
            {"name": "one", "fn": lambda ctx: calls.append("one")},
            {"name": "slow", "fn": lambda ctx: calls.append("slow"),
             "gate": "include_slow"},
            {"name": "needs-one", "fn": lambda ctx: calls.append("dep"),
             "requires": ["one"]},
            {"name": "needs-slow", "fn": lambda ctx: calls.append("dep2"),
             "requires": ["slow"]},
        ]
        ctx = fresh_ctx()          # opts has include_slow=False
        with patched_steps(steps): # helper below
            run_phase2(ctx)
        self.assertEqual(calls, ["one", "dep"])
        self.assertEqual([n for n, _ in ctx.suite.gated],
                         ["slow"])
        self.assertEqual([n for n, _ in ctx.suite.skipped],
                         ["needs-slow"])

    def test_keyword_deselects_by_substring(self):
        calls = []
        steps = [
            {"name": "2.4 host to controller",
             "fn": lambda ctx: calls.append("a")},
            {"name": "2.5 controller to host",
             "fn": lambda ctx: calls.append("b")},
        ]
        ctx = fresh_ctx()
        ctx.opts.keyword = "2.5"
        with patched_steps(steps):
            run_phase2(ctx)
        self.assertEqual(calls, ["b"])

    def test_abort_triggers_recovery(self):
        def boom(ctx):
            ctx.suite.check("boom", False, tag="P2")
            raise StepAbort("dead")
        steps = [{"name": "boom", "fn": boom},
                 {"name": "after", "fn": lambda ctx: None}]
        ctx = fresh_ctx()   # its FakeLink accepts AT+MTRESET
        with patched_steps(steps):
            run_phase2(ctx)
        self.assertIn("AT+MTRESET", ctx.link.sent)  # recovery reset issued
        self.assertEqual([n for n, _ in ctx.suite.skipped], ["after"])
```

Add the small helper (contextmanager) `patched_steps(steps)` that swaps
`mt_regression.PHASE2_STEPS[:]` and restores it. Read `fresh_ctx()` and
FakeLink first: reuse whatever attribute records sent commands (add a
`sent` list to FakeLink.command if none exists). Adjust `ctx.opts` to a
namespace that has `include_slow`, `include_manual`, `keyword` attributes
(update the existing opts construction).

- [ ] **Step 2: run, expect failures** (`gate_skip` missing, dict steps
unsupported)

- [ ] **Step 3: implement Suite.gate_skip and summary**

```python
    def gate_skip(self, name, flag):
        """A step deliberately gated out (--include-slow, --include-manual,
        -k). Unlike skip(), never fails the run: gating is operator
        intent, not truncation."""
        self.gated.append((name, flag))
        print("  [SKIP] %s (gated: --%s)" % (name, flag.replace("_", "-")))
```

`__init__` gains `self.gated = []`. `summary()` builds the tail from parts:

```python
    def summary(self):
        passed = sum(1 for _, ok, _ in self.results if ok)
        parts = ["%d passed" % passed, "%d failed" % self.failed]
        if self.skipped:
            parts.append("%d skipped" % len(self.skipped))
        if self.gated:
            parts.append("%d gated" % len(self.gated))
        print("===== RESULT: %s =====" % ", ".join(parts))
```

- [ ] **Step 4: implement the dict step table and run_phase2**

Convert `PHASE2_STEPS[:]` at the bottom of the module to dicts (existing
six entries get just `name` and `fn`; later tasks add entries with `gate`
and `requires`). New runner:

```python
def recover_after_abort(ctx, reason):
    """Best-effort bench recovery after a chain abort: do not strand a
    commissioned device or dirty storage for the next run. Unscored;
    link death never reaches here (it raises SerialException past
    run_phase2, and a dead link would fail these commands harmlessly)."""
    print("  (recovery after abort: %s)" % reason)
    res, _ = cmd_retry(ctx.link, "AT+MTRESET", timeout=5.0)
    if res == 0:
        ctx.link.await_urc(r"\+MTREADY$", timeout=15.0)
    for c in (ctx.chip, getattr(ctx, "chip2", None)):
        if c is not None:
            c.wipe_storage()


def run_phase2(ctx):
    """Ordered execution with abort/skip semantics (T2) plus gates and
    -k deselection (T3): a gated-out step is operator intent and exits
    zero; a step whose prerequisite did not run is an abort-skip, so a
    truncated chain still fails loudly."""
    abort_reason = None
    ran = set()
    kw = getattr(ctx.opts, "keyword", None)
    for step in PHASE2_STEPS:
        name, fn = step["name"], step["fn"]
        if abort_reason is not None:
            ctx.suite.skip(name, abort_reason)
            continue
        gate = step.get("gate")
        if gate and not getattr(ctx.opts, gate, False):
            ctx.suite.gate_skip(name, gate)
            continue
        if kw and kw not in name:
            ctx.suite.gate_skip(name, "k=%s" % kw)
            continue
        missing = [r for r in step.get("requires", ()) if r not in ran]
        if missing:
            ctx.suite.skip(name, "requires step that did not run: %s"
                           % missing[0])
            continue
        try:
            fn(ctx)
            ran.add(name)
        except StepAbort as exc:
            abort_reason = str(exc)
            recover_after_abort(ctx, abort_reason)
```

Note the gate_skip flag for `-k` prints as `--k=...` via the replace; that
is acceptable and the test only checks selection behavior.

- [ ] **Step 5: argparse and the baseline refusal in main()**

Replace the `--include-slow` help (it becomes real) and add:

```python
    ap.add_argument("--include-slow", action="store_true",
                    help="include the ~200 s window-expiry test (2.10)")
    ap.add_argument("--include-manual", action="store_true",
                    help="include tests needing an operator at the bench "
                         "(2.9 cold boot)")
```

After `args = ap.parse_args(argv)`:

```python
    if args.baseline and args.phase == 2 and not (
            args.include_slow and args.include_manual):
        ap.error("--baseline with --phase 2 requires --include-slow and "
                 "--include-manual: a committed baseline must not contain "
                 "gated-out entries")
```

- [ ] **Step 6: run all self-tests** -> OK.

- [ ] **Step 7: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: gate-skips, dict step table, -k for phase 2, abort recovery

Gating is operator intent, not truncation, so it must not trip the
exit-nonzero-on-skip contract that exists to keep truncated runs
visible; the two outcomes get separate accounting and the summary
distinguishes them. The step table grows gate and requires fields so a
deselected prerequisite turns dependents into loud abort-skips instead
of silent misruns, and an abort now leaves the bench recovered (reset
plus storage wipe) rather than stranded commissioned (N41 item 4).
Baselines refuse to record with gates off so a committed baseline can
never contain gated-out entries.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

### Task 4: swd_reset, relink, and the observed power cycle

**Files:**
- Modify: `test/mt_regression.py` (new helpers after `cmd_retry`; Phase2Context
  seams; `phase2_gate` openocd check; `main()` relink wiring and the finally
  clause)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: `Phase2Context`, `phase2_gate`, `main`.
- Produces:
  - `swd_reset(runner=None) -> (ok: bool, detail: str)`
  - `make_relink(link, port_path) -> callable`; the callable signature is
    `relink(action) -> (ok: bool, detail: str)` where `action() -> (bool, str)`
    runs while the port is closed.
  - `operator_power_cycle(port_path, printer=print, path_exists=os.path.exists,
    sleep=time.sleep, unplug_timeout=60.0) -> (ok, detail)`
  - `Phase2Context` gains `self.relink = None`, `self.swd_runner = None`,
    `self.power_cycler = None` (test seams; `main()` installs the real ones).
  - `phase2_gate` returns an abort message when `openocd` is not on PATH.

- [ ] **Step 1: write the failing tests**

```python
class TestSwdReset(unittest.TestCase):
    def test_argv_and_success(self):
        calls = []
        def runner(argv, capture_output, text, timeout):
            calls.append(argv)
            return types.SimpleNamespace(returncode=0, stdout="", stderr="")
        ok, _ = swd_reset(runner=runner)
        self.assertTrue(ok)
        self.assertEqual(calls[0][:2], ["openocd", "-f"])
        self.assertIn("init; reset run; shutdown", calls[0])

    def test_failure_and_exception_are_reported(self):
        def bad(argv, **kw):
            return types.SimpleNamespace(returncode=1, stdout="x", stderr="y")
        ok, detail = swd_reset(runner=bad)
        self.assertFalse(ok)
        self.assertIn("x", detail)
        def raiser(argv, **kw):
            raise OSError("no probe")
        ok, detail = swd_reset(runner=raiser)
        self.assertFalse(ok)
        self.assertIn("no probe", detail)


class TestOperatorPowerCycle(unittest.TestCase):
    def _run(self, presence):
        """presence: list of booleans path_exists returns in order."""
        seq = iter(presence)
        return operator_power_cycle(
            "/dev/fake", printer=lambda *a: None,
            path_exists=lambda p: next(seq, presence[-1]),
            sleep=lambda s: None, unplug_timeout=1.0)

    def test_observed_cycle_passes(self):
        ok, _ = self._run([True, True, False])
        self.assertTrue(ok)

    def test_never_unplugged_fails(self):
        ok, detail = self._run([True] * 10000)
        self.assertFalse(ok)
        self.assertIn("not observed", detail)
```

(`types` and the timeout-free fake sleep keep these instant. The
sleep=lambda both silences waiting and lets the unplug_timeout loop run on
a monotonic deadline; make the implementation count attempts rather than
wall time when `sleep` is injected... simpler and deterministic: implement
the loop on `time.monotonic()` but pass `unplug_timeout=1.0` in tests and
accept up to a second, or better, give `operator_power_cycle` an injectable
`clock=time.monotonic`. Choose the injectable clock; the test then drives
a fake clock list. Implementer's choice as long as tests are instant and
deterministic.)

- [ ] **Step 2: run, expect NameError/ImportError failures**

- [ ] **Step 3: implement the three helpers**

```python
OPENOCD_ARGV = ["openocd", "-f", "interface/cmsis-dap.cfg",
                "-f", "target/rp2350.cfg",
                "-c", "init; reset run; shutdown"]


def swd_reset(runner=None):
    """Reset the RP2350 over SWD (graph N22). The bridge sketch's setup()
    then pulses PIN_ESP_RST, so this is the RP2350-driven warm C6 reboot
    TESTING.md 2.8 names. The 1200-baud touch reset is NOT equivalent:
    it drops the bridge into BOOTSEL."""
    runner = runner or subprocess.run
    try:
        proc = runner(OPENOCD_ARGV, capture_output=True, text=True,
                      timeout=30)
    except (OSError, subprocess.SubprocessError) as exc:
        return False, str(exc)
    if proc.returncode != 0:
        return False, (proc.stdout or "") + (proc.stderr or "")
    return True, ""


def make_relink(link, port_path):
    """Close the port, run action() while it is closed, wait for the
    device path to come back (SWD reset and power cycles re-enumerate
    USB), reopen and swap the transport in place. Use the by-id path as
    --port or the reopen may chase a renumbered ttyACM."""
    import serial

    def relink(action):
        try:
            link.t.close()
        except Exception:
            pass
        ok, detail = action()
        if not ok:
            return False, detail
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            if os.path.exists(port_path):
                try:
                    link.t = serial.Serial(port_path, 115200, timeout=0.05)
                    link._buf = b""
                    return True, ""
                except serial.SerialException:
                    pass
            time.sleep(0.2)
        return False, "port did not come back: %s" % port_path
    return relink


def operator_power_cycle(port_path, printer=print,
                         path_exists=os.path.exists, sleep=time.sleep,
                         unplug_timeout=60.0, clock=time.monotonic):
    """2.9's power cycle, observed rather than trusted: the device path
    must actually disappear before the step counts the cycle as having
    happened. A prompt alone would pass with no power cycle at all."""
    printer("")
    printer("  *** OPERATOR: unplug the Challenger's USB cable now. ***")
    deadline = clock() + unplug_timeout
    while path_exists(port_path):
        if clock() > deadline:
            return False, "power cycle not observed (device never vanished)"
        sleep(0.2)
    printer("  *** Power-off confirmed. Wait 5 seconds, then replug. ***")
    return True, ""
```

- [ ] **Step 4: wire the seams**

`Phase2Context.__init__` gains:

```python
        self.relink = None        # installed by main(); steps 2.8/2.9 need it
        self.swd_runner = None    # test seam for swd_reset
        self.power_cycler = None  # test seam for operator_power_cycle
```

`phase2_gate` gains (before the credential check; add `import shutil` to the
module imports):

```python
    if shutil.which("openocd") is None:
        return ("openocd not on PATH: the 2.8 warm reboot resets the "
                "RP2350 over SWD (see graph N22)")
```

`main()`, inside the `args.phase == 2` branch after `ctx` is built:

```python
            ctx.relink = make_relink(link, args.port)
```

and change the `finally: port.close()` to:

```python
    finally:
        try:
            link.t.close()
        except Exception:
            pass
```

(after a relink, `port` is a stale object; `link.t` is the live one).

- [ ] **Step 5: update the gate self-test**

The existing phase2_gate self-tests construct argv/environments; they now
need `openocd` findable. Monkeypatch `shutil.which` in those tests to
return `"/usr/local/bin/openocd"` (and add one asserting the message when
it returns None).

- [ ] **Step 6: run all self-tests** -> OK. Commit:

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: swd_reset, relink and the observed operator power cycle

2.8 and 2.9 both survive a USB re-enumeration mid-step, so the port
handling moves from main's open-once model to a relink seam that swaps
the transport in place. The power cycle is observed (device path must
vanish), not trusted: a prompt alone would pass with the operator
doing nothing, which is exactly the vacuous-check trap the bench rules
warn about. The phase 2 gate now refuses without openocd rather than
failing mid-chain at 2.8.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

### Task 5: parse_fabric_index, the second controller, step 2.7

Read `.superpowers/sdd/2026-07-31-c5-regression-harness-t3/task-1-findings.md`
FIRST. It pins the pairing verb and the remove-fabric argv; transcribe them,
do not trust this plan's guesses where the findings differ. The code below
assumes `onnetwork-long` and `remove-fabric <index> <node> 0`; adjust to the
findings and say so in your report if they differ.

**Files:**
- Modify: `test/mt_regression.py` (parse_fabric_index near the other
  parsers; step_2_7_second_fabric after step_2_5; Phase2Context: `chip2`,
  `passcode`, `discriminator`; step_2_3 stores the parsed pair; main wires
  chip2; PHASE2_STEPS entry)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: `ChipTool`, `parse_setup_payload`, Task 1's fixture
  `test/fixtures/chiptool_read_fabrics.txt`, Task 3's dict step table.
- Produces: `parse_fabric_index(text, node_id) -> int|None`;
  `Phase2Context.chip2` (None seam), `ctx.passcode`/`ctx.discriminator` set
  by step_2_3; `step_2_7_second_fabric(ctx)`; table entry
  `{"name": "2.7 second fabric", "fn": step_2_7_second_fabric,
  "requires": ["2.3 commission ble-wifi"]}` inserted after 2.5's entry
  (before the cleanup entry).

- [ ] **Step 1: failing parser test against the real fixture**

```python
class TestParseFabricIndex(unittest.TestCase):
    def test_finds_index_for_node(self):
        text = fixture("chiptool_read_fabrics.txt")
        idx = parse_fabric_index(text, 0x4846)
        self.assertIsInstance(idx, int)   # exact value: assert what the
        self.assertGreater(idx, 0)        # fixture actually contains

    def test_absent_node_is_none(self):
        text = fixture("chiptool_read_fabrics.txt")
        self.assertIsNone(parse_fabric_index(text, 0xDEAD))
```

After reading the fixture, tighten the first assertion to the literal index
the capture contains.

- [ ] **Step 2: run, expect NameError**

- [ ] **Step 3: implement the parser**

Shape it against the fixture; the expected chip-tool TLV dump interleaves
`FabricIndex:` and `NodeID:`/`NodeId:` lines per entry:

```python
def parse_fabric_index(text, node_id):
    """FabricIndex of the fabrics-list entry whose node id matches, from
    `operationalcredentials read fabrics` output. Shape validated
    against the Task 1 fixture. None when the node is not present."""
    last_index = None
    for m in re.finditer(
            r"FabricIndex:\s*(\d+)|Node ?ID:\s*(?:0x)?([0-9A-Fa-f]+)",
            text, re.IGNORECASE):
        if m.group(1) is not None:
            last_index = int(m.group(1))
        elif last_index is not None and int(m.group(2), 16) == node_id:
            return last_index
    return None
```

Adjust the regex to the fixture's actual field names (that is the point of
capturing it); node ids in chip-tool dumps print as hex without `0x` in
some revisions and decimal in others, and the fixture decides.

- [ ] **Step 4: step_2_3 stores the parsed pair**

In `step_2_3_commission`, after `passcode, discriminator = parsed`, add:

```python
    ctx.passcode, ctx.discriminator = passcode, discriminator
```

and in `Phase2Context.__init__`:

```python
        self.passcode = None
        self.discriminator = None
        self.chip2 = None   # second controller identity (T3 2.7)
```

- [ ] **Step 5: write the failing step test**

Follow TestStep23's existing pattern exactly (FakeLink with scripted
commands and URC releases, FakeChipRunner scripts). The step under test:

```python
class TestStep27(unittest.TestCase):
    def _ctx(self):
        link = FakeLink(commands={
            "AT+MTCOMMISSION=180": (0, []),
            "AT+MTSTATE?": [(0, ["+MTSTATE:1,1"]),
                            (0, ["+MTSTATE:2,2"])],
            "AT+MTFABRICS?": [(0, ["+MTFABRICS:2"]),
                              (0, ["+MTFABRICS:1"])],
        }, no_reset=True,
           stale_urcs=[])  # adapt seeding to FakeLink's real API
        # URCs 0,1,3,4 must be visible to the awaits; use the same
        # release mechanism TestStep23 uses.
        ...
    def test_happy_path_counts(self): ...
    def test_pairing_failure_aborts(self): ...
    def test_missing_index_scores_fail_but_continues(self): ...
```

Assert: check names scored, the abort on pairing rc != 0, and that a None
index fails "2.7 fabric index found" without raising. Reuse the file's
existing helpers; the assertions above are the contract, the double
plumbing follows TestStep23, with one deliberate upgrade (N41 item 2, spec
section 5): the commissioning URCs (1, 3, 4) must be released by a
FakeChipRunner `on_call` hook when the `pairing` invocation happens, not
pre-seeded at construction. Give FakeChipRunner an optional
`on_call=None` callable invoked with each argv; the test wires it to push
the URCs into the FakeLink. Deleting the step's `chip2.run` pairing call
must then fail the URC assertions, which pre-seeded doubles cannot detect.

- [ ] **Step 6: implement step_2_7_second_fabric**

```python
def step_2_7_second_fabric(ctx):
    """TESTING.md 2.7: fabric accounting through an additional window.
    The second controller pairs over the network (the device is already
    on WiFi; verb pinned by T3 Task 1), and the DE24 pair (one +MTEVT:4
    per +MTEVT:0, after complete) must hold for host-opened windows
    exactly as it does for the boot window."""
    link, s, chip2 = ctx.link, ctx.suite, ctx.chip2
    res, _ = link.command("AT+MTCOMMISSION=180")
    okw = s.check("2.7 MTCOMMISSION=180 -> OK", res == 0, tag="P2")
    s.check("2.7 +MTEVT:0 window opened",
            link.await_urc(r"\+MTEVT:0$", 5.0) is not None, tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.7 state 1 while window open",
            res == 0 and lines == ["+MTSTATE:1,1"], tag="P2")
    if not okw:
        raise StepAbort("could not open an additional window")
    node2 = ctx.node_id + 1
    rc, out = chip2.run(["pairing", "onnetwork-long", "0x%X" % node2,
                         str(ctx.passcode), str(ctx.discriminator)],
                        timeout=120)
    paired = s.check("2.7 second controller pairs", rc == 0, tag="P2")
    s.check("2.7 +MTEVT:1 session started",
            link.await_urc(r"\+MTEVT:1$", 90.0) is not None, tag="P2")
    got3 = link.await_urc_ts(r"\+MTEVT:3$", 90.0)
    s.check("2.7 +MTEVT:3 complete", got3 is not None, tag="P2")
    got4 = link.await_urc_ts(r"\+MTEVT:4$", 15.0)
    s.check("2.7 exactly one +MTEVT:4, after complete",
            got3 is not None and got4 is not None and got4[0] > got3[0]
            and link.assert_no_urc(r"\+MTEVT:4$", 10.0), tag="P2")
    if not (paired and got3 is not None):
        raise StepAbort("second commissioning failed")
    res, lines = link.command("AT+MTFABRICS?")
    s.check("2.7 fabrics 2", res == 0 and lines == ["+MTFABRICS:2"],
            tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.7 state 2 after window",
            res == 0 and lines == ["+MTSTATE:2,2"], tag="P2")
    rc, out = chip2.run(["operationalcredentials", "read", "fabrics",
                         "0x%X" % node2, "0"], timeout=30)
    idx = parse_fabric_index(out, node2) if rc == 0 else None
    if s.check("2.7 fabric index found", idx is not None, tag="P2"):
        rc, _ = chip2.run(["operationalcredentials", "remove-fabric",
                           str(idx), "0x%X" % node2, "0"], timeout=30)
        s.check("2.7 remove-fabric exits 0", rc == 0, tag="P2")
    res, lines = link.command("AT+MTFABRICS?")
    s.check("2.7 fabrics back to 1",
            res == 0 and lines == ["+MTFABRICS:1"], tag="P2")
    link.drain(0.5)
```

Table entry after "2.5 controller to host":

```python
    {"name": "2.7 second fabric", "fn": step_2_7_second_fabric,
     "requires": ["2.3 commission ble-wifi"]},
```

- [ ] **Step 7: wire chip2 in main()**

In the `args.phase == 2` branch, after `chip.wipe_storage()`:

```python
            ctx.chip2 = ChipTool(args.chip_tool, args.storage + "-f2")
            ctx.chip2.wipe_storage()
```

(Construct after `ctx`; `Phase2Context` takes `chip` as before.)

- [ ] **Step 8: run all self-tests -> OK, commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: step 2.7, second fabric through an additional window

Fabric accounting is the one thing nothing else covers: MTSTATE and
MTFABRICS both count fabrics, and only adding then removing a second
one exercises the counts moving in both directions. The second
controller gets its own storage directory (chip-tool ini storage is
single-process, and a shared directory would corrupt fabric state) and
pairs over the network per the Task 1 preflight findings. The DE24
window-close pairing is asserted for a host-opened window for the
first time.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

### Task 6: Step 2.8, warm reboot persistence

Read the Task 1 findings first: if `+MTREADY` was lost to the enumeration
race on the bench, replace the await below with the probe variant the
findings prescribe (`cmd_retry(link, "AT")` then `AT+MTSTATE?`), and say so
in your report.

**Files:**
- Modify: `test/mt_regression.py` (step_2_8_warm_reboot after step_2_7;
  table entry)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: `ctx.relink`, `swd_reset`, `ctx.swd_runner`, `cmd_retry`.
- Produces: `step_2_8_warm_reboot(ctx)`; table entry
  `{"name": "2.8 warm reboot", "fn": step_2_8_warm_reboot,
  "requires": ["2.3 commission ble-wifi"]}` after 2.7's entry.

- [ ] **Step 1: failing tests** (TestStep28, same double pattern): happy
path scores all checks PASS with a fake relink that returns `(True, "")`
and a FakeLink releasing `+MTREADY` after the relink call; relink failure
raises StepAbort after scoring its FAIL; a `+MTEVT:0` seeded after reboot
makes "no boot window" FAIL.

```python
class TestStep28(unittest.TestCase):
    def test_happy_path(self):
        # FakeLink: AT+MTATTR=1,6,0,1 -> OK; AT+MTATTR=1,6,0 -> +MTATTR:1,6,0,1
        # (twice); AT+MTFABRICS? -> +MTFABRICS:1; AT+MTSTATE? -> +MTSTATE:2,1
        # relink releases +MTREADY into the link, returns (True, "")
        ...
        self.assertEqual(ctx.suite.failed, 0)
        self.assertTrue(relink_called)

    def test_relink_failure_aborts(self): ...
    def test_boot_window_urc_fails_the_guard(self): ...
```

- [ ] **Step 2: run, expect NameError**

- [ ] **Step 3: implement**

```python
def step_2_8_warm_reboot(ctx):
    """TESTING.md 2.8. The reset is RP2350-driven over SWD: openocd
    resets the bridge, whose setup() pulses the C6 reset, so the C6
    reboots with NVS intact. Extension over TESTING.md: a commissioned
    device must not open a boot window, so a +MTEVT:0 here is a fail."""
    link, s = ctx.link, ctx.suite
    res, _ = link.command("AT+MTATTR=1,6,0,1")
    s.check("2.8 precondition write 1 -> OK", res == 0, tag="P2")
    res, lines = link.command("AT+MTATTR=1,6,0")
    if not s.check("2.8 precondition reads 1",
                   res == 0 and lines == ["+MTATTR:1,6,0,1"], tag="P2"):
        raise StepAbort("could not establish the pre-reboot state")
    link.drain(0.3)
    ok, detail = ctx.relink(lambda: swd_reset(ctx.swd_runner))
    if not s.check("2.8 SWD reset, port back", ok, tag="P2"):
        raise StepAbort("bridge did not come back after SWD reset: %s"
                        % detail)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("2.8 +MTREADY within 15 s", ready is not None, tag="P2")
    if ready is None:
        raise StepAbort("device not ready after warm reboot")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("2.8 fabric survived",
            res == 0 and lines == ["+MTFABRICS:1"], tag="P2")
    res, lines = cmd_retry(link, "AT+MTATTR=1,6,0")
    s.check("2.8 attribute value survived",
            res == 0 and lines == ["+MTATTR:1,6,0,1"], tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.8 state 2 (operational)",
            res == 0 and lines == ["+MTSTATE:2,1"], tag="P2")
    s.check("2.8 no boot window on commissioned device",
            link.assert_no_urc(r"\+MTEVT:0$", 5.0), tag="P2")
```

- [ ] **Step 4: tests green, full run, commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: step 2.8, warm reboot persistence over the SWD reset path

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

(Expand the message body with the why: the SWD path is the RP2350-driven
reset TESTING.md names, the touch reset is not equivalent, and the no-boot-
window guard is the extension.)

### Task 7: Step 2.9, cold boot with a state change at init

**Files:**
- Modify: `test/mt_regression.py` (step_2_9_cold_boot; table entry with
  `"gate": "include_manual"`)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: `ctx.relink`, `ctx.power_cycler`, `operator_power_cycle`,
  `cmd_retry`.
- Produces: `step_2_9_cold_boot(ctx)`; table entry
  `{"name": "2.9 cold boot", "fn": step_2_9_cold_boot,
  "gate": "include_manual", "requires": ["2.3 commission ble-wifi"]}`.

- [ ] **Step 1: failing tests** (TestStep29): happy path (post-boot read
returns 0) all PASS; the inconclusive path (post-boot read returns 1) FAILS
the StartUpOnOff check and the printed line contains "inconclusive"; cycle
not observed aborts.

- [ ] **Step 2: run, expect NameError**

- [ ] **Step 3: implement**

```python
def step_2_9_cold_boot(ctx):
    """TESTING.md 2.9, the B4.3 boot-loop regression: a commissioned
    device whose OnOff state changes at init fires a URC during
    esp_matter::start(), before the AT UART exists. Only a cold boot
    with a pending state change reaches the path; a warm reset (2.8)
    preserves state and proves nothing here. A post-boot read of 1
    means no state change happened, and TESTING.md forbids calling
    that a pass: it scores FAIL with an inconclusive note."""
    link, s = ctx.link, ctx.suite
    res, _ = link.command("AT+MTATTR=1,6,0,1")
    okw = res == 0
    res, lines = link.command("AT+MTATTR=1,6,0")
    if not s.check("2.9 precondition: light on before power-off",
                   okw and res == 0 and lines == ["+MTATTR:1,6,0,1"],
                   tag="P2"):
        raise StepAbort("could not establish the pre-power-off state")
    link.drain(0.3)
    cycler = ctx.power_cycler or (
        lambda: operator_power_cycle(ctx.opts.port))
    ok, detail = ctx.relink(cycler)
    if not s.check("2.9 observed power cycle, port back", ok, tag="P2"):
        raise StepAbort("power cycle not completed: %s" % detail)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("2.9 +MTREADY within 15 s", ready is not None, tag="P2")
    if ready is None:
        raise StepAbort("device not ready after cold boot")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("2.9 fabric survived cold boot",
            res == 0 and lines == ["+MTFABRICS:1"], tag="P2")
    res, lines = cmd_retry(link, "AT+MTATTR=1,6,0")
    toggled = res == 0 and lines == ["+MTATTR:1,6,0,0"]
    s.check("2.9 StartUpOnOff toggled at init", toggled, tag="P2")
    if res == 0 and lines == ["+MTATTR:1,6,0,1"]:
        print("    (inconclusive: no state change at init, the guarded "
              "path was never reached)")
```

- [ ] **Step 4: tests green, full run, commit** (message explains the
observed-cycle rule and the inconclusive mapping).

### Task 8: Step 2.10, window expiry

Read the Task 1 findings FIRST: they pin which URCs end a never-attached
window. The code below asserts the DE24 baseline (exactly one `+MTEVT:4`)
and carries a module constant for whether `+MTEVT:5` precedes it; set the
constant from the findings, and update BOTH docs in this same commit.

**Files:**
- Modify: `test/mt_regression.py` (constant + step_2_10_window_expiry;
  table entry with `"gate": "include_slow"`)
- Modify: `test/test_mt_regression.py`
- Modify: `docs/TESTING.md` (2.10: replace the parenthetical open question
  with the pinned contract)
- Modify: `docs/AT_MT_SPEC.md` (section 3.11 event-pairing paragraph: one
  sentence stating what ends a never-attached window, if not already
  implied)

**Interfaces:**
- Consumes: Task 3's gate machinery.
- Produces: `WINDOW_EXPIRY_RAISES_EVT5: bool` (set from findings);
  `step_2_10_window_expiry(ctx)`; table entry
  `{"name": "2.10 window expiry", "fn": step_2_10_window_expiry,
  "gate": "include_slow", "requires": ["2.3 commission ble-wifi"]}`.

- [ ] **Step 1: failing tests** (TestStep210): with FakeLink releasing the
findings-pinned sequence, all checks PASS; a duplicate `+MTEVT:4` seeded in
the 10 s watch FAILS the exactly-one check; state not returning to `2,1`
FAILS its check. Use a shortened wait via a module-level
`ctx.opts.expiry_wait` test override (add `expiry_wait` handling below) so
the self-test never sleeps 200 s.

- [ ] **Step 2: run, expect NameError**

- [ ] **Step 3: implement**

```python
WINDOW_EXPIRY_RAISES_EVT5 = False  # set from T3 Task 1 findings


def step_2_10_window_expiry(ctx):
    """TESTING.md 2.10, gated behind --include-slow (~200 s). A window
    nobody attaches to must end exactly once on the wire: the DE24 pair
    holds with no PASE session ever established. The +MTEVT:5 half of
    the contract was pinned on hardware in T3 Task 1."""
    link, s = ctx.link, ctx.suite
    wait = getattr(ctx.opts, "expiry_wait", 200.0)
    res, _ = link.command("AT+MTCOMMISSION=180")
    okw = s.check("2.10 MTCOMMISSION=180 -> OK", res == 0, tag="P2")
    s.check("2.10 +MTEVT:0 window opened",
            link.await_urc(r"\+MTEVT:0$", 5.0) is not None, tag="P2")
    if not okw:
        raise StepAbort("could not open the expiry window")
    got4 = link.await_urc_ts(r"\+MTEVT:4$", wait)
    s.check("2.10 window end reported (+MTEVT:4)", got4 is not None,
            tag="P2")
    if WINDOW_EXPIRY_RAISES_EVT5:
        got5 = link.await_urc_ts(r"\+MTEVT:5$", 1.0)
        s.check("2.10 fail-safe expiry (+MTEVT:5) accompanies the close",
                got5 is not None, tag="P2")
    s.check("2.10 no duplicate window-close",
            link.assert_no_urc(r"\+MTEVT:4$", 10.0), tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.10 state back to 2 (operational)",
            res == 0 and lines == ["+MTSTATE:2,1"], tag="P2")
```

- [ ] **Step 4: doc ride-alongs in the same commit**

TESTING.md 2.10: replace "(... Which URC ends a never-attached window,
`+MTEVT:5` or only `+MTEVT:4`, is pinned when T3 implements this test.)"
with the pinned fact from the findings, stated in one sentence.
AT_MT_SPEC.md 3.11: add the same fact to the event-pairing paragraph if the
table's `5` fail-safe row does not already make it unambiguous.

- [ ] **Step 5: tests green, full run, commit** (message records the pinned
contract sentence itself).

### Task 9: Composition capture and step 2.11

**Files:**
- Modify: `test/mt_regression.py` (capture in run_phase2; step_2_11_two_resets;
  Phase2Context.composition; table entry)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: `cmd_retry`, `ctx.passcode`/`ctx.discriminator` (Task 5),
  `ChipTool.wipe_storage`.
- Produces: `Phase2Context.composition` (list of `+MTEP:` lines, captured
  by `run_phase2` before the first step); `step_2_11_two_resets(ctx)`;
  table entry `{"name": "2.11 two resets", "fn": step_2_11_two_resets,
  "requires": ["2.3 commission ble-wifi"]}`. Task 10's cleanup consumes
  `ctx.composition`.

- [ ] **Step 1: failing tests**: run_phase2 populates `ctx.composition`
from a FakeLink whose `AT+MTEP?` returns `(0, ["+MTEP:0,1,0x0100"])`;
TestStep211 happy path (composition identical after MTRESET, empty after
MTFRESET) all PASS; composition changed by MTRESET FAILS "survives" check;
re-commission rc != 0 aborts.

- [ ] **Step 2: run, expect failures**

- [ ] **Step 3: capture in run_phase2** (top of the function, before the
loop):

```python
    res, lines = ctx.link.command("AT+MTEP?")
    ctx.composition = lines if res == 0 else None
```

and `self.composition = None` in `Phase2Context.__init__`.

- [ ] **Step 4: implement step_2_11_two_resets**

```python
def step_2_11_two_resets(ctx):
    """TESTING.md 2.11: MTRESET erases the fabric and keeps the
    composition; MTFRESET erases both. The composition surviving the
    first is the whole distinction (spec 3.10): losing it turns an
    end-user unpair into a device that presents nothing."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    res, before = link.command("AT+MTEP?")
    if not s.check("2.11 composition present before resets",
                   res == 0 and bool(before), tag="P2"):
        raise StepAbort("no composition to compare across resets")
    link.drain(0.3)
    res, _ = cmd_retry(link, "AT+MTRESET", timeout=5.0)
    s.check("2.11 MTRESET -> OK", res == 0, tag="P2")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("2.11 +MTREADY after MTRESET", ready is not None,
                   tag="P2"):
        raise StepAbort("device did not come back from AT+MTRESET")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("2.11 fabrics 0 after MTRESET",
            res == 0 and lines == ["+MTFABRICS:0"], tag="P2")
    res, lines = cmd_retry(link, "AT+MTEP?")
    s.check("2.11 composition survives MTRESET",
            res == 0 and lines == before, tag="P2")
    chip.wipe_storage()  # the old fabric died with the reset
    rc, _ = chip.run(["pairing", "ble-wifi", "0x%X" % ctx.node_id,
                      ctx.opts.ssid, ctx.opts.psk,
                      str(ctx.passcode), str(ctx.discriminator)],
                     timeout=120)
    paired = s.check("2.11 re-commission exits 0", rc == 0, tag="P2")
    got3 = link.await_urc_ts(r"\+MTEVT:3$", 90.0)
    s.check("2.11 +MTEVT:3 on re-commission", got3 is not None, tag="P2")
    if not (paired and got3 is not None):
        raise StepAbort("re-commissioning failed")
    res, lines = link.command("AT+MTFABRICS?")
    s.check("2.11 fabrics 1 again",
            res == 0 and lines == ["+MTFABRICS:1"], tag="P2")
    link.drain(0.5)
    res, _ = cmd_retry(link, "AT+MTFRESET", timeout=5.0)
    s.check("2.11 MTFRESET -> OK", res == 0, tag="P2")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("2.11 +MTREADY after MTFRESET", ready is not None,
                   tag="P2"):
        raise StepAbort("device did not come back from AT+MTFRESET")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("2.11 fabrics 0 after MTFRESET",
            res == 0 and lines == ["+MTFABRICS:0"], tag="P2")
    res, lines = cmd_retry(link, "AT+MTEP?")
    s.check("2.11 composition erased by MTFRESET",
            res == 0 and lines == [], tag="P2")
```

- [ ] **Step 5: tests green, full run, commit** (message: why the
distinction matters, and that re-commissioning wipes controller storage
because the device-side fabric died).

### Task 10: Step 2.12 with the restoring cleanup, and the 2.6 sweep

This task finalizes PHASE2_STEPS: the old `cleanup factory-fresh` entry is
REPLACED by `2.12 rig restore` (which absorbs its duties), and `2.6 sweep`
goes last. Read the Task 1 findings for the MTEP staging round-trip fact
(byte-identical readback) before writing the comparison.

**Files:**
- Modify: `test/mt_regression.py` (step_2_12_rig_restore,
  step_2_6_root_urc_sweep; delete step_cleanup_factory_fresh; final
  PHASE2_STEPS table)
- Modify: `test/test_mt_regression.py` (retire tests bound to the old
  cleanup step; new TestStep212 and TestStep26)
- Modify: `docs/TESTING.md` (2.12 note + section 7 preamble)

**Interfaces:**
- Consumes: `ctx.composition` (Task 9), both chips, `cmd_retry`.
- Produces: the final table:

```python
PHASE2_STEPS[:] = [
    {"name": "2.1 factory-fresh baseline", "fn": step_2_1_factory_fresh},
    {"name": "2.2 onboarding codes stable", "fn": step_2_2_codes_stable},
    {"name": "2.3 commission ble-wifi", "fn": step_2_3_commission},
    {"name": "2.4 host to controller", "fn": step_2_4_host_to_controller,
     "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.5 controller to host", "fn": step_2_5_controller_to_host,
     "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.7 second fabric", "fn": step_2_7_second_fabric,
     "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.8 warm reboot", "fn": step_2_8_warm_reboot,
     "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.9 cold boot", "fn": step_2_9_cold_boot,
     "gate": "include_manual", "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.10 window expiry", "fn": step_2_10_window_expiry,
     "gate": "include_slow", "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.11 two resets", "fn": step_2_11_two_resets,
     "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.12 rig restore", "fn": step_2_12_rig_restore,
     "requires": ["2.11 two resets"]},
    {"name": "2.6 root-endpoint URC sweep", "fn": step_2_6_root_urc_sweep},
]
```

- [ ] **Step 1: failing tests.** TestStep212: happy path (unreachable read
rc != 0, `+MTEVT:0` seen, staging commands all OK, readback equals
`ctx.composition`, storages wiped) all PASS; a reachable old node (rc 0)
FAILS "old node unreachable"; a readback mismatch FAILS "composition
restored". TestStep26: clean history PASSES; a `+MTATTR:0,40,2,1` anywhere
in `urc_history` FAILS. Also update TestRunPhase2/TestMain-family tests
that reference the old `cleanup factory-fresh` name.

- [ ] **Step 2: run, expect failures**

- [ ] **Step 3: implement both steps; delete step_cleanup_factory_fresh**

```python
def step_2_12_rig_restore(ctx):
    """TESTING.md 2.12 plus the T3 cleanup: after the 2.11 MTFRESET the
    old node must be gone and the device commissionable again, and the
    run must hand the next one the documented bench convention:
    factory-fresh WITH the composition declared. The replay uses the
    spec 3.9 staging grammar; MTEPAPPLY persists and reboots on its
    own."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    rc, _ = chip.run(["onoff", "read", "on-off", "0x%X" % ctx.node_id,
                      "1"], timeout=30)
    s.check("2.12 old node unreachable", rc != 0, tag="P2")
    s.check("2.12 +MTEVT:0 after factory reset",
            link.await_urc(r"\+MTEVT:0$", 15.0) is not None, tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.12 state 1 (commissionable again)",
            res == 0 and lines == ["+MTSTATE:1,0"], tag="P2")
    devtypes = []
    for ln in ctx.composition or []:
        m = re.fullmatch(r"\+MTEP:\d+,\d+,(\S+)", ln)
        if m:
            devtypes.append(m.group(1))
    if not s.check("cleanup captured composition parses",
                   bool(devtypes), tag="P2"):
        raise StepAbort("no captured composition to restore")
    ok = link.command("AT+MTEPCLEAR")[0] == 0
    for dt in devtypes:
        ok = link.command("AT+MTEP=%s" % dt)[0] == 0 and ok
    s.check("cleanup composition staged", ok, tag="P2")
    link.drain(0.3)
    res, _ = link.command("AT+MTEPAPPLY", timeout=5.0)
    s.check("cleanup MTEPAPPLY -> OK", res == 0, tag="P2")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("cleanup +MTREADY after apply", ready is not None,
                   tag="P2"):
        raise StepAbort("device did not come back from AT+MTEPAPPLY")
    res, lines = cmd_retry(link, "AT+MTEP?")
    s.check("cleanup composition restored",
            res == 0 and lines == ctx.composition, tag="P2")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("cleanup fabrics 0",
            res == 0 and lines == ["+MTFABRICS:0"], tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("cleanup state 1 (window open)",
            res == 0 and lines == ["+MTSTATE:1,0"], tag="P2")
    for c in (ctx.chip, ctx.chip2):
        if c is not None:
            c.wipe_storage()
    s.check("cleanup storages wiped",
            all(not os.listdir(c.storage_dir)
                for c in (ctx.chip, ctx.chip2) if c is not None),
            tag="P2")


def step_2_6_root_urc_sweep(ctx):
    """TESTING.md 2.6, scored last: across every URC of the whole run,
    endpoint 0 stays silent. Every reboot in the chain (four resets
    plus the commissioning cycles) widens the net; regressing the
    suppression floods the host with boot-time init noise."""
    bad = [u for _, u in ctx.link.urc_history
           if u.startswith("+MTATTR:0,")]
    ctx.suite.check("2.6 no root-endpoint +MTATTR URC in the whole run",
                    not bad, tag="P2")
    if bad:
        print("    offending (first 5): %s" % bad[:5])
```

- [ ] **Step 4: TESTING.md ride-alongs.** 2.12 gains: "The T3 harness
restores the captured composition after this test (staging grammar, spec
3.9), so a full run still ends in the factory-fresh-with-composition state
2.1 expects." Section 7 preamble gains one sentence naming the two gate
flags and the gated-skip summary category.

- [ ] **Step 5: tests green (delete/adjust old cleanup tests), full run,
commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py docs/TESTING.md
git commit -m "test: 2.12 rig restore replaces the cleanup, 2.6 sweeps the whole run

The factory reset is the honest end state and also the reason the old
cleanup step is insufficient: MTFRESET erases the composition, and a
bench left that way breaks every tool documented against the
factory-fresh-with-composition convention. The restore replays the
composition captured at phase start with the spec 3.9 staging grammar
and verifies the readback, which doubles as a live test of endpoint id
reproducibility. 2.6 scores last over the drain-proof URC history so
its silence claim covers all four resets and every commissioning
cycle of the run.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

### Task 11: Hardware verification and the extended baseline

Bench-facing. Requires `MT_SSID`/`MT_PSK` (`set -a; . ./.env; set +a`) and
an operator present for the two 2.9 prompts. If credentials are missing,
STOP and ask.

**Files:**
- Modify: `test/baselines/wifi-lifecycle.json` (regenerated, extended)

**Interfaces:**
- Consumes: everything above, the bench.

- [ ] **Step 1: self-tests green, then phase 0**

```bash
cd test && python3 test_mt_regression.py && cd ..
BYID=$(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)
python3 test/mt_regression.py --port $BYID --phase 0
```

- [ ] **Step 2: two full runs, byte-identical**

```bash
set -a; . ./.env; set +a
python3 test/mt_regression.py --port $BYID --phase 2 \
  --include-slow --include-manual 2>&1 | tee /tmp/t3-run1.log
python3 test/mt_regression.py --port $BYID --phase 2 \
  --include-slow --include-manual 2>&1 | tee /tmp/t3-run2.log
diff <(grep -E "\[(PASS|FAIL|SKIP)\]" /tmp/t3-run1.log) \
     <(grep -E "\[(PASS|FAIL|SKIP)\]" /tmp/t3-run2.log)
```

Expected: all PASS, exit 0 both, empty diff. Budget ~12-15 min per run
(three commissionings, two reboots via reset paths, the 200 s expiry, and
the operator power cycle). Give each run command a 900000 ms... the Bash
tool caps at 600000 ms: run each with `run_in_background` semantics or in
chunks per your harness rules; do NOT lower the harness's own timeouts.
If a step fails for a real firmware or harness-logic reason, STOP, collect
the log excerpt and URC lines, and report BLOCKED. Do not patch code
outside what a Task 1 findings divergence explicitly prescribes.

- [ ] **Step 3: third run records the extended baseline**

```bash
python3 test/mt_regression.py --port $BYID --phase 2 \
  --include-slow --include-manual \
  --baseline test/baselines/wifi-lifecycle.json
python3 -c "import json; d=json.load(open('test/baselines/wifi-lifecycle.json')); print(len(d['results']), sorted(set(d['results'].values())))"
```

Expected: exit 0; every result PASS; the count grew from 38 to the full
2.1-2.12 set.

- [ ] **Step 4: default run proves the gates**

```bash
python3 test/mt_regression.py --port $BYID --phase 2 2>&1 | tee /tmp/t3-run4.log
echo "exit: $?"
grep "gated" /tmp/t3-run4.log
```

Expected: exit 0, `[SKIP] 2.9 cold boot (gated: --include-manual)` and
`[SKIP] 2.10 window expiry (gated: --include-slow)` present, summary shows
`2 gated`.

- [ ] **Step 5: baseline-refusal spot check**

```bash
python3 test/mt_regression.py --port $BYID --phase 2 --baseline /tmp/x.json; echo "exit: $?"
```

Expected: argparse error naming both flags, exit 2, device untouched.

- [ ] **Step 6: commit and document the bench**

```bash
git add test/baselines/wifi-lifecycle.json
git commit -m "test: extended WiFi lifecycle baseline, phases 2.1-2.12

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

(Expand the message body: three runs, idempotency diff empty, gates proven,
which port.) Task report records: run wall-clocks, the 2.9 prompt flow
working, the pinned expiry contract observed live, bench end state
(factory-fresh, composition restored, both storages wiped, port).
