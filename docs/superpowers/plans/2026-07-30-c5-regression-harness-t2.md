# C5 Regression Harness T2 (Phase 2.1-2.5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `test/mt_regression.py` with Phase 2 (TESTING.md 2.1-2.5 plus a
factory-fresh cleanup step): chip-tool driven commissioning and attribute
round-trips, hardware-verified on the WiFi image with a committed lifecycle
baseline.

**Architecture:** Approach A from the design spec
(`docs/superpowers/specs/2026-07-30-c5-regression-harness-t2-design.md`):
one-shot `chip-tool` subprocesses for controller actions, one background
`chip-tool onoff subscribe` process for report observation, and a sequential
Phase 2 runner with abort/skip semantics layered on the existing `Suite`.

**Tech Stack:** Python 3 (stdlib + pyserial only), chip-tool from the
esp-matter checkout, the existing T1 harness in `test/mt_regression.py`.

## Global Constraints

- No em dashes anywhere: prose, comments, commit messages. Colon, comma,
  parentheses or a full stop instead.
- No new Python dependencies. chip-tool is a subprocess parsed from stdout.
- Match the existing file's style: PEP8-ish, docstrings that explain *why*,
  `%`-formatting for prints.
- Never run two chip-tool processes concurrently, except that the Subscriber
  may be alive while **AT-side** (serial) traffic flows. No `ChipTool.run()`
  while a Subscriber is alive: chip-tool's ini storage is not concurrency-safe.
- Hardware/bench interaction happens ONLY in Task 1 and Task 11. Tasks 2-10
  are software-only; their tests run with `python3 test/test_mt_regression.py`.
- Commit messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01GYJzvRPu89WWN4FUJLrVgX`
- The default node id is `0x4845`. The commissioning budget is 90 s. The
  duplicate `+MTEVT:4` watch is 10 s. Report windows in 2.4 are 5 s.

---

### Task 1: Bench preflight and payload fixture

Bench-facing sanity per design spec section 8 item 1, before any code is
trusted. No firmware or Python changes.

**Files:**
- Create: `test/fixtures/chiptool_parse_setup_payload.txt`

**Interfaces:**
- Produces: the fixture file consumed by Task 3's self-tests, and a go/no-go
  on the non-interactive `subscribe` subcommand.

- [ ] **Step 1: chip-tool runs as the invoking user**

```bash
BIN=~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool
ls -la /tmp/chip_*.ini 2>&1   # note ownership if present; root-owned is the 2026-07-29 failure mode
$BIN payload parse-setup-payload MT:Y.K9042C00KA0648G00; echo rc=$?
ls -la /tmp/chip_*.ini 2>&1   # must now exist and be owned by $USER
```

Expected: `rc=0`, output contains a passcode line (`20202021`) and a
discriminator line (`3840`), and any `/tmp/chip_*.ini` files are owned by the
current user. If a root-owned file blocks the run, STOP and report; do not
delete other users' files.

- [ ] **Step 2: capture the parse fixture**

```bash
cd /mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/src/git/iLabs_AT_Hearth
mkdir -p test/fixtures
$BIN payload parse-setup-payload MT:Y.K9042C00KA0648G00 > test/fixtures/chiptool_parse_setup_payload.txt 2>&1
grep -iE "passcode|discriminator" test/fixtures/chiptool_parse_setup_payload.txt
```

Expected: the grep shows the passcode and discriminator lines. Record the
exact line shapes in the task report; Task 3's regexes must match them.

- [ ] **Step 3: confirm the subscribe subcommand exists non-interactively**

```bash
$BIN onoff 2>&1 | grep -w "subscribe" || echo "MISSING"
$BIN onoff subscribe 2>&1 | head -5   # usage text for argument order
```

Expected: `subscribe` is listed with arguments `on-off <min-interval>
<max-interval> <destination-id> <endpoint-id>`. If the subcommand is missing,
STOP: the design's fallback (a minimal interactive driver inside `Subscriber`)
needs a human decision first.

- [ ] **Step 4: commit**

```bash
git add test/fixtures/chiptool_parse_setup_payload.txt
git commit -m "test: capture chip-tool parse-setup-payload fixture for T2"
```

---

### Task 2: Suite skip accounting, StepAbort, and the Phase 2 runner

**Files:**
- Modify: `test/mt_regression.py` (class `Suite`, ~line 157; `write_baseline`,
  ~line 348)
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `Suite.skip(name, reason)`, `Suite.skipped` (list of
  `(name, reason)`), `class StepAbort(Exception)`,
  `class Phase2Context(link, chip, suite, opts)` with attributes
  `link, chip, suite, opts, qr, manual, node_id`,
  `PHASE2_STEPS` (module-level list of `(name, fn)`), and
  `run_phase2(ctx)` which iterates `PHASE2_STEPS`, catching `StepAbort` and
  skipping the remainder.
- Consumes: existing `Suite.check`.

- [ ] **Step 1: write the failing tests**

Append to `test/test_mt_regression.py` (imports at top of file already
include `json`, `io`, `contextlib`; add imports to the existing import block
only if missing):

```python
from mt_regression import (Suite, StepAbort, Phase2Context, run_phase2,
                           PHASE2_STEPS, write_baseline)


class TestSuiteSkip(unittest.TestCase):
    def test_skip_is_counted_and_printed(self):
        s = Suite()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            s.check("a", True)
            s.skip("b", "earlier step failed")
            s.summary()
        text = out.getvalue()
        self.assertIn("[SKIP] b: earlier step failed", text)
        self.assertIn("1 passed, 0 failed, 1 skipped", text)
        self.assertEqual(s.skipped, [("b", "earlier step failed")])

    def test_summary_without_skips_is_unchanged(self):
        s = Suite()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            s.check("a", True)
            s.summary()
        self.assertIn("===== RESULT: 1 passed, 0 failed =====",
                      out.getvalue())

    def test_baseline_records_skips(self):
        s = Suite()
        with contextlib.redirect_stdout(io.StringIO()):
            s.check("ran", True)
            s.skip("not run", "aborted")
        with tempfile.NamedTemporaryFile("r", suffix=".json") as f:
            with contextlib.redirect_stdout(io.StringIO()):
                write_baseline(f.name, {"port": "x"}, s)
            data = json.load(open(f.name))
        self.assertEqual(data["results"]["ran"], "PASS")
        self.assertEqual(data["results"]["not run"], "SKIP")


class TestRunPhase2(unittest.TestCase):
    def _ctx(self):
        s = Suite()
        return Phase2Context(link=None, chip=None, suite=s, opts=None), s

    def test_abort_skips_the_rest(self):
        calls = []

        def ok_step(ctx):
            calls.append("ok")

        def bad_step(ctx):
            raise StepAbort("device did not come back")

        def never_step(ctx):
            calls.append("never")

        saved = list(PHASE2_STEPS)
        PHASE2_STEPS[:] = [("one", ok_step), ("two", bad_step),
                           ("three", never_step)]
        try:
            ctx, s = self._ctx()
            with contextlib.redirect_stdout(io.StringIO()):
                run_phase2(ctx)
        finally:
            PHASE2_STEPS[:] = saved
        self.assertEqual(calls, ["ok"])
        self.assertEqual(s.skipped,
                         [("three", "device did not come back")])

    def test_no_abort_runs_all(self):
        calls = []
        saved = list(PHASE2_STEPS)
        PHASE2_STEPS[:] = [("one", lambda ctx: calls.append(1)),
                          ("two", lambda ctx: calls.append(2))]
        try:
            ctx, _ = self._ctx()
            run_phase2(ctx)
        finally:
            PHASE2_STEPS[:] = saved
        self.assertEqual(calls, [1, 2])
```

- [ ] **Step 2: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestSuiteSkip TestRunPhase2 -v 2>&1 | tail -5`
Expected: ImportError (`StepAbort` not defined).

- [ ] **Step 3: implement**

In `test/mt_regression.py`, extend `Suite`:

```python
    def __init__(self):
        self.results = []
        self.skipped = []

    def skip(self, name, reason):
        """A step not run because an earlier step broke its preconditions.
        Counted separately from failures, and never silently: a truncated
        run must not read as a clean one (design spec section 3)."""
        self.skipped.append((name, reason))
        print("  [SKIP] %s: %s" % (name, reason))

    def summary(self):
        passed = sum(1 for _, ok, _ in self.results if ok)
        if self.skipped:
            print("===== RESULT: %d passed, %d failed, %d skipped ====="
                  % (passed, self.failed, len(self.skipped)))
        else:
            print("===== RESULT: %d passed, %d failed ====="
                  % (passed, self.failed))
```

Extend `write_baseline`'s results dict:

```python
        "results": dict(
            [(name, "PASS" if ok else "FAIL")
             for name, ok, _ in suite.results]
            + [(name, "SKIP") for name, _ in suite.skipped]),
```

Add after the `Suite` class:

```python
class StepAbort(Exception):
    """Raised inside a Phase 2 step when its failure invalidates every
    later step's preconditions (reset failed, commissioning failed). The
    step has already scored its own FAIL; this just stops the chain."""


class Phase2Context:
    """Shared state the ordered Phase 2 steps hand each other."""

    def __init__(self, link, chip, suite, opts):
        self.link = link
        self.chip = chip
        self.suite = suite
        self.opts = opts
        self.qr = None
        self.manual = None
        self.node_id = getattr(opts, "node_id", 0x4845)


PHASE2_STEPS = []  # populated bottom-of-module once the steps exist


def run_phase2(ctx):
    """Ordered execution with abort/skip semantics: Phase 1 checks are
    independent, Phase 2 steps chain, so a broken precondition skips the
    remainder instead of producing 20 misleading FAILs."""
    abort_reason = None
    for name, fn in PHASE2_STEPS:
        if abort_reason is not None:
            ctx.suite.skip(name, abort_reason)
            continue
        try:
            fn(ctx)
        except StepAbort as exc:
            abort_reason = str(exc)
```

- [ ] **Step 4: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK`, no failures anywhere in the file.

- [ ] **Step 5: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: Phase 2 runner with abort/skip semantics and skip accounting"
```

---

### Task 3: ChipTool wrapper and output parsers

**Files:**
- Modify: `test/mt_regression.py`
- Create: `test/fixtures/chiptool_onoff_read_true.txt`,
  `test/fixtures/chiptool_onoff_read_false.txt` (synthetic until Task 11)
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `DEFAULT_CHIPTOOL` (str path), `class ChipTool` with
  `__init__(binary, storage_dir, runner=None)`, `run(args, timeout=60) ->
  (returncode, stdout_str)`, `wipe_storage()`, attribute `storage_dir`;
  module functions `parse_setup_payload(text) -> (passcode, discriminator)
  or None`, `parse_onoff_reports(text) -> list[int]`,
  `parse_onoff_read(text) -> 0|1|None`.
- Consumes: nothing new.

- [ ] **Step 1: create the synthetic read fixtures**

The line shape follows chip-tool's `CHIP:TOO` logger. Task 11 replaces these
with real captures; the leading `#` line marks them.

`test/fixtures/chiptool_onoff_read_true.txt`:

```text
# SYNTHESIZED in T2 Task 3; replaced by a real capture in Task 11.
[1753900000.123456][11111:22222] CHIP:DMG: ReportDataMessage =
[1753900000.123456][11111:22222] CHIP:DMG: {
[1753900000.123999][11111:22222] CHIP:TOO: Endpoint: 1 Cluster: 0x0000_0006 Attribute 0x0000_0000 DataVersion: 1234
[1753900000.124000][11111:22222] CHIP:TOO:   OnOff: TRUE
```

`test/fixtures/chiptool_onoff_read_false.txt`: same four lines with
`OnOff: FALSE` on the last line.

- [ ] **Step 2: write the failing tests**

```python
from mt_regression import (ChipTool, parse_setup_payload,
                           parse_onoff_reports, parse_onoff_read)

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "fixtures")


def fixture(name):
    with open(os.path.join(FIXTURES, name), "r", errors="replace") as f:
        return f.read()


class FakeChipRunner:
    """Scripted stand-in for ChipTool's subprocess runner."""

    def __init__(self, script):
        self.script = list(script)
        self.calls = []

    def __call__(self, argv, timeout):
        self.calls.append((argv, timeout))
        return self.script.pop(0)


class TestChipTool(unittest.TestCase):
    def test_run_appends_storage_and_returns_script(self):
        runner = FakeChipRunner([(0, "hello")])
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/chip-tool", d, runner=runner)
            rc, out = chip.run(["onoff", "read", "on-off", "0x4845", "1"])
        self.assertEqual((rc, out), (0, "hello"))
        argv, timeout = runner.calls[0]
        self.assertEqual(argv[0], "/bin/chip-tool")
        self.assertEqual(argv[-2:], ["--storage-directory", d])
        self.assertEqual(timeout, 60)

    def test_wipe_storage_removes_files_keeps_dir(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "chip_tool_config.ini"), "w").close()
            chip = ChipTool("/bin/chip-tool", d)
            chip.wipe_storage()
            self.assertTrue(os.path.isdir(d))
            self.assertEqual(os.listdir(d), [])


class TestChipToolParsers(unittest.TestCase):
    def test_parse_setup_payload_fixture(self):
        got = parse_setup_payload(fixture("chiptool_parse_setup_payload.txt"))
        self.assertEqual(got, (20202021, 3840))

    def test_parse_setup_payload_garbage_is_none(self):
        self.assertIsNone(parse_setup_payload("no such content"))

    def test_parse_onoff_read_true_false(self):
        self.assertEqual(
            parse_onoff_read(fixture("chiptool_onoff_read_true.txt")), 1)
        self.assertEqual(
            parse_onoff_read(fixture("chiptool_onoff_read_false.txt")), 0)
        self.assertIsNone(parse_onoff_read(""))

    def test_parse_onoff_reports_counts_in_order(self):
        text = (fixture("chiptool_onoff_read_true.txt")
                + fixture("chiptool_onoff_read_false.txt"))
        self.assertEqual(parse_onoff_reports(text), [1, 0])
```

- [ ] **Step 3: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestChipTool TestChipToolParsers -v 2>&1 | tail -5`
Expected: ImportError (`ChipTool` not defined).

- [ ] **Step 4: implement**

In `test/mt_regression.py` (after the `Suite`/runner block):

```python
DEFAULT_CHIPTOOL = os.path.expanduser(
    "~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool")


class ChipTool:
    """One-shot chip-tool invocations (design spec section 2). Every call
    carries --storage-directory so a stale fabric cannot leak between
    runs, and the runner is injectable so self-tests never spawn."""

    def __init__(self, binary, storage_dir, runner=None):
        self.binary = binary
        self.storage_dir = storage_dir
        self._runner = runner or self._default_runner

    @staticmethod
    def _default_runner(argv, timeout):
        proc = subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout)
        return proc.returncode, proc.stdout + proc.stderr

    def run(self, args, timeout=60):
        os.makedirs(self.storage_dir, exist_ok=True)
        argv = ([self.binary] + list(args)
                + ["--storage-directory", self.storage_dir])
        return self._runner(argv, timeout)

    def wipe_storage(self):
        if os.path.isdir(self.storage_dir):
            for name in os.listdir(self.storage_dir):
                path = os.path.join(self.storage_dir, name)
                if os.path.isfile(path):
                    os.remove(path)


def parse_setup_payload(text):
    """(passcode, discriminator) from `payload parse-setup-payload`
    output, or None. Regexes validated against the Task 1 fixture."""
    pc = re.search(r"Passcode:\s*(\d+)", text)
    disc = re.search(r"Long discriminator:\s*(\d+)", text)
    if disc is None:
        disc = re.search(r"Discriminator(?: value)?:\s*(\d+)", text)
    if pc and disc:
        return int(pc.group(1)), int(disc.group(1))
    return None


def parse_onoff_reports(text):
    """Every OnOff attribute value in chip-tool output, in order. Serves
    both one-shot reads and the subscriber's accumulated log; heartbeat
    (empty) reports print no attribute line and are invisible here,
    which is exactly what 2.4's no-new-report window needs."""
    vals = []
    for m in re.finditer(r"OnOff:\s*(TRUE|FALSE|0|1)\b", text,
                         re.IGNORECASE):
        vals.append(1 if m.group(1).upper() in ("TRUE", "1") else 0)
    return vals


def parse_onoff_read(text):
    vals = parse_onoff_reports(text)
    return vals[-1] if vals else None
```

- [ ] **Step 5: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK`.

- [ ] **Step 6: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py test/fixtures/
git commit -m "test: ChipTool wrapper and chip-tool output parsers"
```

---

### Task 4: Subscriber

**Files:**
- Modify: `test/mt_regression.py`
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `class Subscriber` with `__init__(chip, node_id, endpoint=1,
  min_s=0, max_s=5, popen=None)`, `start(settle=10.0) -> bool`,
  `reports() -> list[int]`, `wait_new_report(count_before, timeout) -> bool`,
  `no_new_report(count_before, window) -> bool`, `stop()`, attributes `argv`
  and `out_path`.
- Consumes: `ChipTool` (binary, storage_dir), `parse_onoff_reports`.

- [ ] **Step 1: write the failing tests**

```python
from mt_regression import Subscriber


class TestSubscriber(unittest.TestCase):
    def _chip(self, d):
        return ChipTool("/bin/chip-tool", d)

    def test_argv_shape(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845)
        self.assertEqual(sub.argv[:8],
                         ["/bin/chip-tool", "onoff", "subscribe", "on-off",
                          "0", "5", "0x4845", "1"])
        self.assertIn("--storage-directory", sub.argv)

    def test_reports_and_windows_from_file(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845)
            with open(sub.out_path, "w") as f:
                f.write("CHIP:TOO:   OnOff: TRUE\n")
            self.assertEqual(sub.reports(), [1])
            base = len(sub.reports())
            self.assertFalse(sub.wait_new_report(base, 0.3))
            self.assertTrue(sub.no_new_report(base, 0.3))
            with open(sub.out_path, "a") as f:
                f.write("CHIP:TOO:   OnOff: FALSE\n")
            self.assertTrue(sub.wait_new_report(base, 0.5))

    def test_start_detects_early_death(self):
        """A subscriber whose process dies before the priming report is a
        failed start, not a silent no-report generator."""
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845)
            sub.argv = [sys.executable, "-c", "raise SystemExit(1)"]
            self.assertFalse(sub.start(settle=1.0))
            sub.stop()

    def test_start_sees_priming_report(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845)
            sub.argv = [sys.executable, "-u", "-c",
                        "import time; print('CHIP:TOO:   OnOff: TRUE');"
                        "time.sleep(30)"]
            self.assertTrue(sub.start(settle=5.0))
            sub.stop()
```

- [ ] **Step 2: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestSubscriber -v 2>&1 | tail -5`
Expected: ImportError (`Subscriber` not defined).

- [ ] **Step 3: implement**

```python
class Subscriber:
    """Background `chip-tool onoff subscribe` (design spec section 2).

    At most one instance alive, and no ChipTool.run() while it lives:
    chip-tool's ini storage is not safe for two processes. Its stdout
    goes to a file; parsing reads the file, so report observation works
    the same whether the process is live (hardware) or the file is
    written by a test."""

    def __init__(self, chip, node_id, endpoint=1, min_s=0, max_s=5,
                 popen=None):
        self.chip = chip
        self.argv = [chip.binary, "onoff", "subscribe", "on-off",
                     str(min_s), str(max_s), "0x%X" % node_id,
                     str(endpoint), "--timeout", "120",
                     "--storage-directory", chip.storage_dir]
        self.out_path = os.path.join(chip.storage_dir, "subscribe.log")
        self._popen = popen or subprocess.Popen
        self._proc = None
        self._out = None

    def start(self, settle=10.0):
        """True once the priming report arrives, which is the proof the
        subscription is live. False if the process dies first or the
        settle window passes silently."""
        os.makedirs(self.chip.storage_dir, exist_ok=True)
        self._out = open(self.out_path, "wb")
        self._proc = self._popen(self.argv, stdout=self._out,
                                 stderr=subprocess.STDOUT)
        deadline = time.monotonic() + settle
        while time.monotonic() < deadline:
            if self.reports():
                return True
            if self._proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def reports(self):
        try:
            with open(self.out_path, "r", errors="replace") as f:
                return parse_onoff_reports(f.read())
        except OSError:
            return []

    def wait_new_report(self, count_before, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if len(self.reports()) > count_before:
                return True
            time.sleep(0.2)
        return False

    def no_new_report(self, count_before, window):
        """The full window must pass with nothing new: this is 2.4's
        mode-0 proof, so it deliberately waits the whole window."""
        return not self.wait_new_report(count_before, window)

    def stop(self):
        if self._proc is not None and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(5)
        if self._out is not None:
            self._out.close()
            self._out = None
```

- [ ] **Step 4: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK`.

- [ ] **Step 5: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: background subscriber for controller-side report observation"
```

---

### Task 5: ATLink.await_urc_ts and cmd_retry

**Files:**
- Modify: `test/mt_regression.py` (`ATLink.await_urc`, ~line 124)
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `ATLink.await_urc_ts(pattern, timeout=5.0) -> (ts, line) or
  None` (timestamps are queue-insert times, so comparing them compares wire
  order); `cmd_retry(link, cmd, timeout=None) -> (res, lines)` module
  function retrying exactly once on `-2`.
- Consumes: existing `ATLink` internals; `await_urc` keeps its exact
  current behavior (delegates to `await_urc_ts`).

- [ ] **Step 1: write the failing tests**

```python
from mt_regression import cmd_retry


class TestAwaitUrcTs(unittest.TestCase):
    def test_order_is_preserved_via_timestamps(self):
        link, _ = link_with_reply(b"")
        link.urcs = [(10.0, "+MTEVT:4"), (20.0, "+MTEVT:3")]
        got3 = link.await_urc_ts(r"\+MTEVT:3$", timeout=0.1)
        got4 = link.await_urc_ts(r"\+MTEVT:4$", timeout=0.1)
        self.assertEqual(got3, (20.0, "+MTEVT:3"))
        self.assertEqual(got4, (10.0, "+MTEVT:4"))
        # the out-of-order arrival is visible to the caller:
        self.assertLess(got4[0], got3[0])

    def test_await_urc_still_returns_line_only(self):
        link, _ = link_with_reply(b"")
        link.urcs = [(1.0, "+MTEVT:0")]
        self.assertEqual(link.await_urc(r"\+MTEVT:0$", timeout=0.1),
                         "+MTEVT:0")


class TestCmdRetry(unittest.TestCase):
    def test_retries_once_on_timeout(self):
        class OneShotDeaf:
            """First command gets nothing, second gets OK: the documented
            post-reboot settling quirk (graph N22)."""

            def __init__(self):
                self.calls = 0

            def command(self, cmd, expect=None, timeout=None):
                self.calls += 1
                return (0, ["+MTFABRICS:0"]) if self.calls > 1 else (-2, [])

        link = OneShotDeaf()
        self.assertEqual(cmd_retry(link, "AT+MTFABRICS?"),
                         (0, ["+MTFABRICS:0"]))
        self.assertEqual(link.calls, 2)

    def test_no_retry_on_success_or_error(self):
        class Counting:
            def __init__(self, res):
                self.res, self.calls = res, 0

            def command(self, cmd, expect=None, timeout=None):
                self.calls += 1
                return self.res, []

        for res in (0, 1, -1):
            link = Counting(res)
            cmd_retry(link, "AT")
            self.assertEqual(link.calls, 1)
```

- [ ] **Step 2: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestAwaitUrcTs TestCmdRetry -v 2>&1 | tail -5`
Expected: ImportError (`cmd_retry`), AttributeError (`await_urc_ts`).

- [ ] **Step 3: implement**

Replace `ATLink.await_urc` with:

```python
    def await_urc_ts(self, pattern, timeout=5.0):
        """await_urc, but returning (timestamp, line). Timestamps are the
        queue-insert (read) times and reads preserve wire order, so
        comparing two results compares arrival order: 2.3 uses this to
        prove +MTEVT:4 followed +MTEVT:3 rather than merely existing."""
        rx = re.compile(pattern)
        for i, (ts, u) in enumerate(self.urcs):
            if rx.search(u):
                self.urcs.pop(i)
                return ts, u
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = self._pump_one(deadline)
            if got is not None and rx.search(got):
                return self.urcs.pop()
        return None

    def await_urc(self, pattern, timeout=5.0):
        """Return the first URC matching the regex, consuming it. Checks
        the queue before the wire, so a URC that raced an earlier command
        still satisfies the wait. None on timeout."""
        got = self.await_urc_ts(pattern, timeout)
        return got[1] if got is not None else None
```

Add near `repo_head`:

```python
def cmd_retry(link, cmd, timeout=None):
    """One retry on no-response, for the documented settling quirk: the
    first command after a reboot can time out once (graph N22). Retrying
    on -2 only keeps real ERROR results exact."""
    res, lines = link.command(cmd, timeout=timeout)
    if res == -2:
        res, lines = link.command(cmd, timeout=timeout)
    return res, lines
```

- [ ] **Step 4: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK` (including all pre-existing URC-queue tests).

- [ ] **Step 5: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: timestamped URC awaits and the settling-quirk retry helper"
```

---

### Task 6: CLI, phase-2 gate, main wiring, and truncated-run exit

**Files:**
- Modify: `test/mt_regression.py` (module docstring; `main`, ~line 508)
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `phase2_gate(chip, args) -> str or None`,
  `GATE_REFERENCE_QR = "MT:Y.K9042C00KA0648G00"`; `main` accepts
  `--phase 2`, `--chip-tool`, `--storage`, `--ssid`, `--psk`, `--node-id`;
  exit code is 1 when anything failed, was skipped, or the run was
  truncated (link lost or interrupted).
- Consumes: `ChipTool`, `Phase2Context`, `run_phase2`, `parse_setup_payload`.

- [ ] **Step 1: write the failing tests**

```python
from mt_regression import phase2_gate, GATE_REFERENCE_QR


class TestPhase2Gate(unittest.TestCase):
    def _args(self, **kw):
        base = {"ssid": "net", "psk": "secret"}
        base.update(kw)
        return types.SimpleNamespace(**base)

    def test_missing_creds_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            problem = phase2_gate(chip, self._args(ssid=None))
        self.assertIn("MT_SSID", problem)

    def test_missing_binary_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool(os.path.join(d, "nope"), d)
            problem = phase2_gate(chip, self._args())
        self.assertIn("chip-tool", problem)

    def test_unparseable_reference_payload_abort(self):
        runner = FakeChipRunner([(0, "garbage")])
        with tempfile.TemporaryDirectory() as d:
            binary = os.path.join(d, "chip-tool")
            open(binary, "w").close()
            os.chmod(binary, 0o755)
            chip = ChipTool(binary, d, runner=runner)
            problem = phase2_gate(chip, self._args())
        self.assertIn("parse", problem)

    def test_healthy_gate_passes(self):
        runner = FakeChipRunner(
            [(0, fixture("chiptool_parse_setup_payload.txt"))])
        with tempfile.TemporaryDirectory() as d:
            binary = os.path.join(d, "chip-tool")
            open(binary, "w").close()
            os.chmod(binary, 0o755)
            chip = ChipTool(binary, d, runner=runner)
            self.assertIsNone(phase2_gate(chip, self._args()))
        self.assertEqual(runner.calls[0][0][1:3],
                         ["payload", "parse-setup-payload"])
```

Also update `TestMainSurvivesLostLink.test_lost_link_still_prints_summary`:
the final assertion `self.assertEqual(rc, 0)` becomes
`self.assertEqual(rc, 1)` with the comment:

```python
        # T2 contract change (design spec section 3): a truncated run must
        # not exit 0, even when nothing that ran failed.
        self.assertEqual(rc, 1)
```

- [ ] **Step 2: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestPhase2Gate TestMainSurvivesLostLink -v 2>&1 | tail -5`
Expected: ImportError (`phase2_gate`), and after implementing imports the
lost-link test fails on `rc == 1`.

- [ ] **Step 3: implement**

Module docstring becomes:

```python
"""iLabs AT Hearth regression harness, stages T1+T2: Phases 0, 1 and 2.

Test inventory: docs/TESTING.md sections 5, 6 and 7 (2.1-2.5).
Design decisions: docs/superpowers/specs/2026-07-30-c5-regression-harness-t1-design.md
and .../2026-07-30-c5-regression-harness-t2-design.md.

Phase 1 run: python3 test/mt_regression.py --port /dev/ttyACM0
Phase 2 run: MT_SSID=... MT_PSK=... python3 test/mt_regression.py \
    --port /dev/ttyACM0 --phase 2

Standing rule (T1 design section 8, N23): never have an AT+MTATTR command
in flight while a controller-driven +MTATTR URC is expected; ATLink would
absorb the URC into the command's response lines. Phase 2 steps sequence
around this by construction.
"""
```

Add near `phase0`:

```python
GATE_REFERENCE_QR = "MT:Y.K9042C00KA0648G00"


def phase2_gate(chip, args):
    """Phase-2-only preflight, before anything destructive: chip-tool
    exists, runs as this user (the 2026-07-29 root-owned counters file
    made every non-root run fail), and credentials are present. Returns
    an abort message or None."""
    if not getattr(args, "ssid", None) or not getattr(args, "psk", None):
        return ("phase 2 needs WiFi credentials: export MT_SSID and "
                "MT_PSK (or pass --ssid/--psk)")
    if not (os.path.isfile(chip.binary)
            and os.access(chip.binary, os.X_OK)):
        return ("chip-tool not executable: %s (set MT_CHIPTOOL or "
                "--chip-tool)" % chip.binary)
    try:
        rc, out = chip.run(
            ["payload", "parse-setup-payload", GATE_REFERENCE_QR],
            timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        return "chip-tool failed to run: %s" % exc
    if rc != 0 or parse_setup_payload(out) is None:
        return ("chip-tool cannot parse the reference setup payload; "
                "check it runs as this user (root-owned /tmp/chip_*.ini "
                "is the known cause)")
    return None
```

In `main`, replace the argparse block additions and the run loop:

```python
    ap.add_argument("--phase", type=int, choices=[0, 1, 2], default=None,
                    help="run only this phase (0 runs just the preflight "
                         "gate; 2 is stateful and never runs by default)")
    ap.add_argument("--chip-tool",
                    default=os.environ.get("MT_CHIPTOOL", DEFAULT_CHIPTOOL))
    ap.add_argument("--storage",
                    default=os.environ.get("MT_CHIPTOOL_STORAGE",
                                           "/tmp/mt-regression"))
    ap.add_argument("--ssid", default=os.environ.get("MT_SSID"))
    ap.add_argument("--psk", default=os.environ.get("MT_PSK"))
    ap.add_argument("--node-id", type=lambda x: int(x, 0), default=0x4845,
                    help="node id chip-tool assigns at pairing")
```

After parsing, before opening the port:

```python
    chip = None
    if args.phase == 2:
        chip = ChipTool(args.chip_tool, args.storage)
        problem = phase2_gate(chip, args)
        if problem:
            print("ABORT: " + problem)
            return 2
```

Inside the `try` after `phase0` (replacing the `if args.phase != 0:` body
with a phase-2 branch first; the existing Phase 1 loop is unchanged, only
indented under the `else`):

```python
        if args.phase == 2:
            header["node_id"] = "0x%X" % args.node_id
            chip.wipe_storage()
            ctx = Phase2Context(link, chip, suite, args)
            run_phase2(ctx)
            capture_header(link, header)
        elif args.phase != 0:
            for phase, name, tag, fn, slow in TESTS:
                if args.phase is not None and phase != args.phase:
                    continue
                if args.keyword and args.keyword not in name:
                    continue
                if slow and not args.include_slow:
                    continue
                link.drain()
                try:
                    ok = fn(link)
                except Exception as exc:
                    print("  (exception in %s: %s)" % (name, exc))
                    ok = False
                suite.check(name, ok, tag)
            capture_header(link, header)
```

(The `elif` body is the existing Phase 1 loop verbatim, one level deeper;
only the `if args.phase == 2:` branch above it is new.)

Truncation and the narrowed handler (replacing the current
`except Exception` clause; `serial` is in scope from the function-local
import):

```python
    truncated = False
    try:
        ...
    except KeyboardInterrupt:
        print("\n(interrupted)")
        truncated = True
    except (serial.SerialException, OSError) as exc:
        # Narrow deliberately (T2 design section 5): a harness bug must
        # raise a traceback, not masquerade as a lost link.
        print("(link lost: %s)" % exc)
        truncated = True
    finally:
        port.close()
    suite.summary()
    if args.baseline:
        write_baseline(args.baseline, header, suite)
    return 1 if (suite.failed or suite.skipped or truncated) else 0
```

- [ ] **Step 4: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK`.

- [ ] **Step 5: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: phase 2 CLI, gate, and the truncated-run exit contract"
```

---

### Task 7: Steps 2.1, 2.2 and cleanup, plus the FakeLink test double

**Files:**
- Modify: `test/mt_regression.py`
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `step_2_1_factory_fresh(ctx)`, `step_2_2_codes_stable(ctx)`,
  `step_cleanup_factory_fresh(ctx)`; `PHASE2_STEPS` populated as
  `[("2.1 factory-fresh baseline", ...), ("2.2 onboarding codes stable",
  ...), ("cleanup factory-fresh", ...)]`; test-side `class FakeLink`.
- Consumes: `StepAbort`, `Phase2Context`, `cmd_retry`, `Suite`.

- [ ] **Step 1: write the failing tests**

```python
from mt_regression import (step_2_1_factory_fresh, step_2_2_codes_stable,
                           step_cleanup_factory_fresh)


class FakeLink:
    """ATLink double for Phase 2 step tests: scripted command replies
    (dict cmd -> (res, lines), or a list of those for sequential calls)
    and a scripted URC stream whose timestamps encode wire order."""

    def __init__(self, commands=None, urcs=None):
        self.commands = dict(commands or {})
        self.urc_queue = [(float(i), u) for i, u in enumerate(urcs or [])]
        self.sent = []

    def command(self, cmd, expect=None, timeout=None):
        self.sent.append(cmd)
        v = self.commands.get(cmd, (0, []))
        if isinstance(v, list):
            v = v.pop(0) if v else (-2, [])
        return v

    def await_urc_ts(self, pattern, timeout=5.0):
        rx = re.compile(pattern)
        for i, (ts, u) in enumerate(self.urc_queue):
            if rx.search(u):
                return self.urc_queue.pop(i)
        return None

    def await_urc(self, pattern, timeout=5.0):
        got = self.await_urc_ts(pattern, timeout)
        return got[1] if got is not None else None

    def assert_no_urc(self, pattern, window):
        return self.await_urc(pattern, window) is None

    def drain(self, quiet=0.2):
        dropped = [u for _, u in self.urc_queue]
        self.urc_queue.clear()
        return dropped


CODES = ("+MTCODES:MT:Y.K9042C00KA0648G00,34970112332", )


def fresh_ctx(link, chip=None, storage=None):
    opts = types.SimpleNamespace(node_id=0x4845, ssid="net", psk="secret",
                                 storage=storage)
    return Phase2Context(link=link, chip=chip, suite=Suite(), opts=opts)


class TestStep21(unittest.TestCase):
    HAPPY = {
        "AT+MTCODES?": (0, list(CODES)),
        "AT+MTRESET": (0, []),
        "AT+MTFABRICS?": (0, ["+MTFABRICS:0"]),
        "AT+MTSTATE?": (0, ["+MTSTATE:1,0"]),
    }

    def test_happy_path_scores_all(self):
        link = FakeLink(self.HAPPY, urcs=["+MTREADY", "+MTEVT:0"])
        ctx = fresh_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_1_factory_fresh(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        self.assertEqual(ctx.qr, "MT:Y.K9042C00KA0648G00")
        self.assertEqual(ctx.manual, "34970112332")

    def test_no_ready_aborts(self):
        link = FakeLink(self.HAPPY, urcs=[])
        ctx = fresh_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_2_1_factory_fresh(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep22(unittest.TestCase):
    def test_codes_must_match_capture(self):
        link = FakeLink({"AT+MTCODES?": (0, list(CODES))})
        ctx = fresh_ctx(link)
        ctx.qr, ctx.manual = "MT:Y.K9042C00KA0648G00", "34970112332"
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_2_codes_stable(ctx)
        self.assertEqual(ctx.suite.failed, 0)

    def test_changed_codes_fail(self):
        link = FakeLink({"AT+MTCODES?": (0, ["+MTCODES:MT:XX,00000000000"])})
        ctx = fresh_ctx(link)
        ctx.qr, ctx.manual = "MT:Y.K9042C00KA0648G00", "34970112332"
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_2_codes_stable(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestCleanupStep(unittest.TestCase):
    def test_cleanup_resets_and_wipes(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "chip_tool_config.ini"), "w").close()
            chip = ChipTool("/bin/chip-tool", d)
            link = FakeLink({"AT+MTRESET": (0, []),
                             "AT+MTFABRICS?": (0, ["+MTFABRICS:0"])},
                            urcs=["+MTREADY"])
            ctx = fresh_ctx(link, chip=chip)
            with contextlib.redirect_stdout(io.StringIO()):
                step_cleanup_factory_fresh(ctx)
            self.assertEqual(ctx.suite.failed, 0)
            self.assertEqual(os.listdir(d), [])
```

- [ ] **Step 2: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestStep21 TestStep22 TestCleanupStep -v 2>&1 | tail -5`
Expected: ImportError (`step_2_1_factory_fresh` not defined).

- [ ] **Step 3: implement**

Add before the `PHASE2_STEPS` assignment (and change that assignment):

```python
def step_2_1_factory_fresh(ctx):
    """TESTING.md 2.1. Captures the codes first because 2.2 compares
    against the pre-reset value, then drains so a stale boot URC cannot
    satisfy the post-reset awaits."""
    link, s = ctx.link, ctx.suite
    res, lines = link.command("AT+MTCODES?")
    if res == 0 and lines:
        m = re.fullmatch(r"\+MTCODES:(.+),(\d{11})", lines[0])
        if m:
            ctx.qr, ctx.manual = m.group(1), m.group(2)
    if not s.check("2.1 codes captured before reset", ctx.qr is not None,
                   tag="P2"):
        raise StepAbort("cannot capture onboarding codes")
    link.drain(0.3)
    res, _ = cmd_retry(link, "AT+MTRESET", timeout=5.0)
    ok = s.check("2.1 MTRESET -> OK", res == 0, tag="P2")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("2.1 +MTREADY within 15 s", ready is not None, tag="P2")
    if not ok or ready is None:
        raise StepAbort("device did not come back from AT+MTRESET")
    s.check("2.1 +MTEVT:0 after reboot",
            link.await_urc(r"\+MTEVT:0$", timeout=15.0) is not None,
            tag="P2")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("2.1 fabrics 0", res == 0 and lines == ["+MTFABRICS:0"],
            tag="P2")
    res, lines = cmd_retry(link, "AT+MTSTATE?")
    s.check("2.1 state 1 (window open)",
            res == 0 and lines == ["+MTSTATE:1,0"], tag="P2")


def step_2_2_codes_stable(ctx):
    """TESTING.md 2.2: the codes derive from provisioned commissionable
    data, so a factory reset must not change them."""
    link, s = ctx.link, ctx.suite
    res, lines = cmd_retry(link, "AT+MTCODES?")
    want = ["+MTCODES:%s,%s" % (ctx.qr, ctx.manual)]
    s.check("2.2 codes unchanged by reset", res == 0 and lines == want,
            tag="P2")


def step_cleanup_factory_fresh(ctx):
    """T2 addition (design spec section 4): leave the bench in the
    documented factory-fresh state and scored, because a cleanup that
    fails leaves a bench that lies to the next run."""
    link, s = ctx.link, ctx.suite
    link.drain(0.3)
    res, _ = cmd_retry(link, "AT+MTRESET", timeout=5.0)
    s.check("cleanup MTRESET -> OK", res == 0, tag="P2")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("cleanup +MTREADY within 15 s", ready is not None, tag="P2")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("cleanup fabrics 0", res == 0 and lines == ["+MTFABRICS:0"],
            tag="P2")
    if ctx.chip is not None:
        ctx.chip.wipe_storage()
        s.check("cleanup storage wiped",
                not os.listdir(ctx.chip.storage_dir), tag="P2")


PHASE2_STEPS[:] = [
    ("2.1 factory-fresh baseline", step_2_1_factory_fresh),
    ("2.2 onboarding codes stable", step_2_2_codes_stable),
    ("cleanup factory-fresh", step_cleanup_factory_fresh),
]
```

Note `PHASE2_STEPS[:] =` keeps the list object Task 2's `run_phase2`
closed over. Tasks 8 and 9 extend this literal, keeping cleanup last.

- [ ] **Step 4: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK`.

- [ ] **Step 5: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: phase 2 steps 2.1, 2.2 and the factory-fresh cleanup"
```

---

### Task 8: Step 2.3, commissioning over BLE + WiFi

**Files:**
- Modify: `test/mt_regression.py`
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `step_2_3_commission(ctx)`; `PHASE2_STEPS` becomes
  `[2.1, 2.2, 2.3, cleanup]`.
- Consumes: `ChipTool.run`, `parse_setup_payload`, `FakeLink`,
  `FakeChipRunner`, `fixture`, `ATLink.await_urc_ts` timestamps for the
  order assertion.

- [ ] **Step 1: write the failing tests**

```python
from mt_regression import step_2_3_commission


class TestStep23(unittest.TestCase):
    AT_OK = {
        "AT+MTFABRICS?": (0, ["+MTFABRICS:1"]),
        "AT+MTSTATE?": (0, ["+MTSTATE:2,1"]),
    }

    def _ctx(self, runner, urcs):
        link = FakeLink(self.AT_OK, urcs=urcs)
        d = tempfile.mkdtemp()  # outlives the step; ChipTool.run mkdirs it
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_ctx(link, chip=chip)
        ctx.qr, ctx.manual = "MT:Y.K9042C00KA0648G00", "34970112332"
        return ctx, runner

    def test_happy_path(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (0, "CHIP:TOO: Device commissioning completed with success"),
        ])
        ctx, runner = self._ctx(runner,
                                ["+MTEVT:1", "+MTEVT:3", "+MTEVT:4"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_3_commission(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        pairing_argv = runner.calls[1][0]
        self.assertEqual(pairing_argv[1:4],
                         ["pairing", "ble-wifi", "0x4845"])
        self.assertIn("20202021", pairing_argv)
        self.assertIn("3840", pairing_argv)

    def test_double_close_fails_the_de24_check(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (0, "success"),
        ])
        ctx, _ = self._ctx(runner,
                           ["+MTEVT:1", "+MTEVT:3", "+MTEVT:4", "+MTEVT:4"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_3_commission(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_close_before_complete_fails_order(self):
        """The pre-fix D16 wire order (a 4 leaking at session
        establishment) must fail, not pass by queue-scan accident."""
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (0, "success"),
        ])
        ctx, _ = self._ctx(runner, ["+MTEVT:1", "+MTEVT:4", "+MTEVT:3"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_3_commission(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_pairing_failure_aborts(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (1, "CHIP:TOO: Run command failure"),
        ])
        ctx, _ = self._ctx(runner, [])
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_2_3_commission(ctx)
        self.assertGreater(ctx.suite.failed, 0)
```

- [ ] **Step 2: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestStep23 -v 2>&1 | tail -5`
Expected: ImportError (`step_2_3_commission` not defined).

- [ ] **Step 3: implement**

```python
def step_2_3_commission(ctx):
    """TESTING.md 2.3 plus the DE24 window-event contract: the AT-side
    and controller-side views must agree, and exactly one +MTEVT:4 must
    follow +MTEVT:3 (order proven by timestamps, because a queue scan
    alone would let the old pre-eb15d0f leak pass)."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    rc, out = chip.run(["payload", "parse-setup-payload", ctx.qr],
                       timeout=15)
    parsed = parse_setup_payload(out) if rc == 0 else None
    if not s.check("2.3 QR payload parses", parsed is not None, tag="P2"):
        raise StepAbort("onboarding QR not machine-usable")
    passcode, discriminator = parsed
    rc, out = chip.run(["pairing", "ble-wifi", "0x%X" % ctx.node_id,
                        ctx.opts.ssid, ctx.opts.psk,
                        str(passcode), str(discriminator)], timeout=120)
    paired = s.check("2.3 chip-tool pairing exits 0", rc == 0, tag="P2")
    s.check("2.3 +MTEVT:1 session started",
            link.await_urc(r"\+MTEVT:1$", timeout=90.0) is not None,
            tag="P2")
    got3 = link.await_urc_ts(r"\+MTEVT:3$", timeout=90.0)
    s.check("2.3 +MTEVT:3 commissioning complete", got3 is not None,
            tag="P2")
    got4 = link.await_urc_ts(r"\+MTEVT:4$", timeout=15.0)
    s.check("2.3 exactly one +MTEVT:4, after complete",
            got3 is not None and got4 is not None and got4[0] > got3[0]
            and link.assert_no_urc(r"\+MTEVT:4$", 10.0), tag="P2")
    if not (paired and got3 is not None):
        raise StepAbort("commissioning failed")
    res, lines = link.command("AT+MTFABRICS?")
    s.check("2.3 fabrics 1", res == 0 and lines == ["+MTFABRICS:1"],
            tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.3 state 2 (operational)",
            res == 0 and lines == ["+MTSTATE:2,1"], tag="P2")
```

Update the list literal:

```python
PHASE2_STEPS[:] = [
    ("2.1 factory-fresh baseline", step_2_1_factory_fresh),
    ("2.2 onboarding codes stable", step_2_2_codes_stable),
    ("2.3 commission ble-wifi", step_2_3_commission),
    ("cleanup factory-fresh", step_cleanup_factory_fresh),
]
```

- [ ] **Step 4: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK`.

- [ ] **Step 5: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: phase 2.3 commissioning with the DE24 window-event contract"
```

---

### Task 9: Steps 2.4 and 2.5, attribute round-trips

**Files:**
- Modify: `test/mt_regression.py`
- Test: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `step_2_4_host_to_controller(ctx)`,
  `step_2_5_controller_to_host(ctx)`; final `PHASE2_STEPS` order
  `[2.1, 2.2, 2.3, 2.4, 2.5, cleanup]`.
- Consumes: `Subscriber` (patched per-test via the `subscriber_factory`
  hook below), `ChipTool.run`, `parse_onoff_read`.

- [ ] **Step 1: write the failing tests**

The step takes an injectable subscriber factory so the self-test never
spawns chip-tool; the default is the real `Subscriber`.

```python
from mt_regression import (step_2_4_host_to_controller,
                           step_2_5_controller_to_host)


class FakeSubscriber:
    """Scripted Subscriber: report counts stepped by the test."""

    def __init__(self, counts, start_ok=True):
        self.counts = list(counts)   # successive reports() lengths
        self.start_ok = start_ok
        self.stopped = False

    def start(self, settle=10.0):
        return self.start_ok

    def reports(self):
        n = self.counts[0] if len(self.counts) == 1 else self.counts.pop(0)
        return [1] * n

    def wait_new_report(self, count_before, timeout):
        return len(self.reports()) > count_before

    def no_new_report(self, count_before, window):
        return not (len(self.reports()) > count_before)

    def stop(self):
        self.stopped = True


class TestStep24(unittest.TestCase):
    def _ctx(self, runner, sub):
        link = FakeLink({
            "AT+MTATTR=1,6,0,1": (0, []),
            "AT+MTATTR=1,6,0,0": (0, []),
            "AT+MTATTR=1,6,0,1,1": (0, []),
            "AT+MTATTR=1,6,0,0,0": (0, []),
            "AT+MTATTR=1,6,0": (0, ["+MTATTR:1,6,0,0"]),
        })
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_ctx(link, chip=chip)
        ctx.subscriber_factory = lambda: sub
        return ctx

    def test_happy_path(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_onoff_read_true.txt")),
            (0, fixture("chiptool_onoff_read_false.txt")),
            (0, fixture("chiptool_onoff_read_false.txt")),
        ])
        # reports() call sequence in the step: base before mode-1 (1),
        # wait after mode-1 (2, the new report), base before mode-0 (2),
        # settle window (2, nothing new; last value sticks)
        sub = FakeSubscriber(counts=[1, 2, 2])
        ctx = self._ctx(runner, sub)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_4_host_to_controller(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        self.assertTrue(sub.stopped)

    def test_mode0_report_leak_fails(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_onoff_read_true.txt")),
            (0, fixture("chiptool_onoff_read_false.txt")),
            (0, fixture("chiptool_onoff_read_false.txt")),
        ])
        # a report appears after the mode-0 write: the regression 2.4
        # exists for (base 1, mode-1 report 2, base 2, then 3 in the window)
        sub = FakeSubscriber(counts=[1, 2, 2, 3])
        ctx = self._ctx(runner, sub)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_4_host_to_controller(ctx)
        self.assertGreater(ctx.suite.failed, 0)
        self.assertTrue(sub.stopped)

    def test_subscriber_start_failure_aborts_and_stops(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_onoff_read_true.txt")),
            (0, fixture("chiptool_onoff_read_false.txt")),
        ])
        sub = FakeSubscriber(counts=[0], start_ok=False)
        ctx = self._ctx(runner, sub)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_2_4_host_to_controller(ctx)


class TestStep25(unittest.TestCase):
    def _ctx(self, runner, urcs, reads):
        link = FakeLink({
            "AT+MTATTR=1,6,0,1": (0, []),
            "AT+MTATTR=1,6,0": [(0, [r]) for r in reads],
        }, urcs=urcs)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_ctx(link, chip=chip)

    def test_happy_path(self):
        runner = FakeChipRunner([(0, ""), (0, ""), (0, "")])
        ctx = self._ctx(runner,
                        ["+MTATTR:1,6,0,0", "+MTATTR:1,6,0,1",
                         "+MTATTR:1,6,0,0"],
                        ["+MTATTR:1,6,0,0", "+MTATTR:1,6,0,1",
                         "+MTATTR:1,6,0,0"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_5_controller_to_host(ctx)
        self.assertEqual(ctx.suite.failed, 0)

    def test_missing_urc_fails(self):
        runner = FakeChipRunner([(0, ""), (0, ""), (0, "")])
        ctx = self._ctx(runner, [],
                        ["+MTATTR:1,6,0,0", "+MTATTR:1,6,0,1",
                         "+MTATTR:1,6,0,0"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_5_controller_to_host(ctx)
        self.assertGreater(ctx.suite.failed, 0)
```

- [ ] **Step 2: run tests to verify they fail**

Run: `python3 test/test_mt_regression.py TestStep24 TestStep25 -v 2>&1 | tail -5`
Expected: ImportError.

- [ ] **Step 3: implement**

`Phase2Context.__init__` gains one line:

```python
        self.subscriber_factory = None  # test seam; None means real Subscriber
```

```python
def step_2_4_host_to_controller(ctx):
    """TESTING.md 2.4. Sequencing per the concurrency rule: subscriber
    up, AT-side writes observed, subscriber down, only then controller
    reads. Mode 0 keeping the report suppressed is the one property
    nothing else in the suite can catch."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    for want in (1, 0):
        res, _ = link.command("AT+MTATTR=1,6,0,%d" % want)
        okw = s.check("2.4 AT write %d -> OK" % want, res == 0, tag="P2")
        rc, out = chip.run(["onoff", "read", "on-off",
                            "0x%X" % ctx.node_id, "1"], timeout=30)
        s.check("2.4 controller reads %d" % want,
                rc == 0 and parse_onoff_read(out) == want, tag="P2")
        if not okw:
            raise StepAbort("AT-side attribute write failed")
    factory = ctx.subscriber_factory or (
        lambda: Subscriber(chip, ctx.node_id))
    sub = factory()
    if not s.check("2.4 subscriber starts", sub.start(), tag="P2"):
        sub.stop()
        raise StepAbort("subscription did not come up")
    try:
        base = len(sub.reports())
        res, _ = link.command("AT+MTATTR=1,6,0,1,1")
        s.check("2.4 mode 1 write -> OK", res == 0, tag="P2")
        s.check("2.4 mode 1 produces a report",
                sub.wait_new_report(base, 5.0), tag="P2")
        base = len(sub.reports())
        res, _ = link.command("AT+MTATTR=1,6,0,0,0")
        s.check("2.4 mode 0 write -> OK", res == 0, tag="P2")
        res, lines = link.command("AT+MTATTR=1,6,0")
        s.check("2.4 local read shows 0",
                res == 0 and lines == ["+MTATTR:1,6,0,0"], tag="P2")
        s.check("2.4 mode 0 produces no report",
                sub.no_new_report(base, 5.0), tag="P2")
    finally:
        sub.stop()
    rc, out = chip.run(["onoff", "read", "on-off", "0x%X" % ctx.node_id,
                        "1"], timeout=30)
    s.check("2.4 controller read shows 0 after mode-0 write",
            rc == 0 and parse_onoff_read(out) == 0, tag="P2")


def step_2_5_controller_to_host(ctx):
    """TESTING.md 2.5, the end-to-end proof in the controller-to-host
    direction. The value is set to 1 first so `off` is a real
    transition: a no-change write may fire no callback, and an assertion
    on it would prove nothing."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    res, _ = link.command("AT+MTATTR=1,6,0,1")
    s.check("2.5 precondition write 1 -> OK", res == 0, tag="P2")
    link.drain(0.5)
    for verb, want in (("off", 0), ("on", 1), ("toggle", 0)):
        rc, _ = chip.run(["onoff", verb, "0x%X" % ctx.node_id, "1"],
                         timeout=30)
        okc = s.check("2.5 chip-tool %s exits 0" % verb, rc == 0, tag="P2")
        got = link.await_urc(r"\+MTATTR:1,6,0,%d$" % want, timeout=2.0)
        s.check("2.5 unprompted +MTATTR after %s" % verb, got is not None,
                tag="P2")
        res, lines = link.command("AT+MTATTR=1,6,0")
        s.check("2.5 AT read agrees (%d)" % want,
                res == 0 and lines == ["+MTATTR:1,6,0,%d" % want],
                tag="P2")
        if not okc:
            raise StepAbort("controller-side write failed")
```

Final list literal:

```python
PHASE2_STEPS[:] = [
    ("2.1 factory-fresh baseline", step_2_1_factory_fresh),
    ("2.2 onboarding codes stable", step_2_2_codes_stable),
    ("2.3 commission ble-wifi", step_2_3_commission),
    ("2.4 host to controller", step_2_4_host_to_controller),
    ("2.5 controller to host", step_2_5_controller_to_host),
    ("cleanup factory-fresh", step_cleanup_factory_fresh),
]
```

- [ ] **Step 4: run tests to verify they pass**

Run: `python3 test/test_mt_regression.py 2>&1 | tail -3`
Expected: `OK`.

- [ ] **Step 5: commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: phase 2.4/2.5 attribute round-trips with report observation"
```

---

### Task 10: TESTING.md ride-alongs

**Files:**
- Modify: `docs/TESTING.md`

**Interfaces:** none (documentation).

- [ ] **Step 1: fix the two stale AT+MTCOMMISSION values**

In `docs/TESTING.md` change (2.7, ~line 419):

`AT+MTCOMMISSION=60` becomes `AT+MTCOMMISSION=180`

and (2.10, ~line 458): replace

```
`AT+MTCOMMISSION=30` with no controller attaching: assert `+MTEVT:5` (fail-safe expired)
arrives after the fail-safe expires, and that `AT+MTSTATE?` returns to `2`. Gated
behind `--include-slow` because of the ~90 s wall clock.
```

with

```
`AT+MTCOMMISSION=180` with no controller attaching: assert the window's end is
reported and that `AT+MTSTATE?` returns to `2`. Gated behind `--include-slow`
because of the ~200 s wall clock. (The 180 s floor is CHIP's Matter minimum;
values below it are rejected with `+MTERR:1` since commit eb15d0f. Which URC
ends a never-attached window, `+MTEVT:5` or only `+MTEVT:4`, is pinned when T3
implements this test.)
```

- [ ] **Step 2: add the DE24 note to 2.3**

After the 2.3 assertion list (the line "The AT-side and controller-side views
agreeing is the point of the test: either one alone can pass while the device
is unusable."), append a new paragraph:

```
Since 2026-07-30 (commit eb15d0f, decision DE24) events `0` and `4` are a
pair: exactly one `+MTEVT:4` is raised per reported `+MTEVT:0`, after
`+MTEVT:3` on a successful commissioning. The harness asserts the order and
the absence of a duplicate; both leaks it guards against existed before the
fix (spec `AT_MT_SPEC.md` section 3.11).
```

- [ ] **Step 3: commit**

```bash
git add docs/TESTING.md
git commit -m "docs: TESTING.md catches up with the 180 s window floor and DE24"
```

---

### Task 11: Hardware verification and the lifecycle baseline

Bench-facing. Follow design spec section 8. Requires `MT_SSID`/`MT_PSK` from
the operator: if they are not exported in the environment, STOP and ask
before proceeding.

**Files:**
- Modify: `test/fixtures/chiptool_onoff_read_true.txt`,
  `test/fixtures/chiptool_onoff_read_false.txt` (replace synthetic content
  with real captures)
- Create: `test/baselines/wifi-lifecycle.json`

**Interfaces:**
- Consumes: everything above, the bench (Challenger on its `/dev/serial/by-id`
  port, chip-tool, BLE + WiFi coverage).

- [ ] **Step 1: flash the WiFi image and confirm the start state**

```bash
source ~/esp/esp-idf-v5.4.1/export.sh
PORT=$(readlink -f /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)
python3 fw/flash.py --build-dir build_b4 --port $PORT --bridge espnow
python3 test/mt_regression.py --port $PORT --phase 0
```

Expected: flash OK, preflight `ESP32-C6 Hearth`. The first AT command after
reboot may time out once (settling); rerun the phase 0 command if so.

- [ ] **Step 2: first `--phase 2` run and fixture recapture**

```bash
MT_SSID=... MT_PSK=... python3 test/mt_regression.py --port $PORT --phase 2 2>&1 | tee /tmp/t2-run1.log
```

Expected: all checks pass, exit 0. If `2.3 QR payload parses`, any
`controller reads` or the subscriber checks fail on **parsing** (chip-tool
output shape differing from the synthetic fixtures): capture the real
outputs, replace the two synthetic fixtures with them (keep the filenames),
adjust the `parse_onoff_reports` / `parse_setup_payload` regexes minimally,
rerun `python3 test/test_mt_regression.py` until `OK`, and rerun this step.
Real captures:

```bash
STOR=/tmp/mt-regression-fixture
BIN=~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool
# after a successful pairing (rerun step 2 up to 2.3 if needed):
$BIN onoff read on-off 0x4845 1 --storage-directory $STOR > test/fixtures/chiptool_onoff_read_true.txt 2>&1
```

(with the light set to 1 via `AT+MTATTR=1,6,0,1` first, then again with 0 for
the false fixture).

- [ ] **Step 3: second run proves idempotency**

```bash
MT_SSID=... MT_PSK=... python3 test/mt_regression.py --port $PORT --phase 2 2>&1 | tee /tmp/t2-run2.log
diff <(grep -E "\[(PASS|FAIL|SKIP)\]" /tmp/t2-run1.log) <(grep -E "\[(PASS|FAIL|SKIP)\]" /tmp/t2-run2.log)
```

Expected: identical check lines, all PASS, both runs exit 0.

- [ ] **Step 4: third run records the baseline**

```bash
MT_SSID=... MT_PSK=... python3 test/mt_regression.py --port $PORT --phase 2 --baseline test/baselines/wifi-lifecycle.json
python3 -c "import json; d=json.load(open('test/baselines/wifi-lifecycle.json')); print(d['header']['node_id'], d['header']['transport'], sorted(v for v in d['results'].values()))"
```

Expected: exit 0, header carries `node_id` `0x4845` and transport `WIFI`,
every result `PASS`.

- [ ] **Step 5: hardware-free self-tests still green, then commit**

```bash
python3 test/test_mt_regression.py 2>&1 | tail -3
git add test/baselines/wifi-lifecycle.json test/fixtures/
git commit -m "test: WiFi lifecycle baseline from three clean phase 2 runs"
```

- [ ] **Step 6: leave the bench documented**

Confirm and note in the task report: device factory-fresh (`AT+MTFABRICS?`
0, window open), chip-tool storage wiped, which port the Challenger is on.
