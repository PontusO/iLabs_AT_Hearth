# C5 Regression Harness T1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `test/mt_regression.py`, the Phase 0 + Phase 1 AT protocol conformance harness from `docs/TESTING.md` §5 and §6, with a hardware-free self-test, and hardware-verify it on both firmware images.

**Architecture:** One Python program with an `ATLink` class that talks to an injected stream-like transport (pyserial on hardware, a scripted fake in the self-test), a timestamped URC queue, and a table-driven `check()` runner whose report matches the ESP-NOW RegressionSuite. Spec: `docs/superpowers/specs/2026-07-30-c5-regression-harness-t1-design.md`. Test content authority: `docs/TESTING.md`.

**Tech Stack:** Python 3, standard library plus `pyserial` (already used by `fw/flash.py`). Self-test uses stdlib `unittest`.

## Global Constraints

- No em dashes anywhere: code, comments, commit messages, docs. Colon, comma, parentheses or a full stop instead.
- Dependencies: Python 3 stdlib + `pyserial` only. No pytest, no CHIP bindings, no chip-tool in T1.
- Python style: 4-space indent, snake_case, module docstring naming the spec.
- Report lines exactly: `  [PASS] [AT+] <name>` / `  [FAIL] [AT-] <name>` and summary `===== RESULT: N passed, M failed =====` (matches the ESP-NOW RegressionSuite).
- Exit codes: 0 all pass, 1 any failure, 2 Phase 0 preflight abort.
- Phase 1 must not change device state. The event mask is the sole exception and is restored to `0x0800003F`.
- Facts pinned from firmware source (do not re-derive): `AT+CGMI` -> `iLabs Electronics`, `AT+CGMM` -> `ESP32-C6 Hearth`, `AT+CGMR` -> `0.1.0` (from `mt_at_config.h`); `AT+MTNET?` -> `+MTNET:<WIFI|THREAD>,<enabled>,<connected>,<mismatch>` (FOUR fields, the mismatch field was appended by P2; TESTING.md's three-field regex is stale); `AT+MTATTR` read echoes parameters in normalized decimal (`+MTATTR:%lu,%lu,%lu,%ld`), so a hex request `AT+MTATTR=0,0x0028,0x0002` answers `+MTATTR:0,40,2,<v>`; `AT+MTEVT?` answers prefix `+MTEVTMASK:`; line limit `MT_AT_LINE_MAX` is 512.
- The harness reads the AT stream only (`/dev/ttyACM0` via the RP2350 bridge). Never merge in the console stream.
- Commit after every task. Messages explain why, per repo convention.

## File Structure

```
test/mt_regression.py        the harness: ATLink, Suite, tests, CLI (created Task 1, grown through Task 8)
test/test_mt_regression.py   hardware-free self-test: FakeTransport + unittest (created Task 1, grown through Task 4)
test/baselines/thread.json   committed baseline, Thread image (Task 9)
test/baselines/wifi.json     committed baseline, WiFi image (Task 10)
```

---

### Task 1: ATLink line assembly and result mapping

**Files:**
- Create: `test/mt_regression.py`
- Create: `test/test_mt_regression.py`

**Interfaces:**
- Produces: `class ATLink(transport, default_timeout=2.0)` with `command(cmd: str, expect: str | None = None, timeout: float | None = None) -> tuple[int, list[str]]`. Result mapping: `0` = OK, `n > 0` = `+MTERR:n` then `ERROR`, `-1` = bare `ERROR`, `-2` = no terminal before timeout. `lines` = the command's own intermediate response lines. Attribute `urcs: list[tuple[float, str]]` collects stray `+` lines. Transport contract: `read(n) -> bytes` (may return `b""` immediately), `write(bytes) -> int`.
- Produces (test side): `class FakeTransport` with `rx: bytes` (pre-loaded device output), `tx: bytes` (harness writes), `on_write: callable | None` (returns bytes appended to `rx` when the harness writes).

- [ ] **Step 1: Write the failing test**

Create `test/test_mt_regression.py`:

```python
#!/usr/bin/env python3
"""Hardware-free self-test for the T1 regression harness.

Exercises ATLink against a scripted fake transport, so the result
mapping and URC handling are pinned without a board on the desk.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mt_regression import ATLink


class FakeTransport:
    """Stream double: read() drains a scripted rx buffer, write() records
    and can trigger a scripted reply via on_write."""

    def __init__(self, on_write=None):
        self.rx = b""
        self.tx = b""
        self.on_write = on_write

    def read(self, n=1):
        chunk, self.rx = self.rx[:n], self.rx[n:]
        return chunk

    def write(self, data):
        self.tx += data
        if self.on_write:
            self.rx += self.on_write(data)
        return len(data)


def link_with_reply(reply):
    ft = FakeTransport(on_write=lambda d: reply)
    return ATLink(ft, default_timeout=0.2), ft


class TestResultMapping(unittest.TestCase):
    def test_ok_with_intermediate(self):
        link, ft = link_with_reply(b"+MTVER:0.1.0\r\nOK\r\n")
        res, lines = link.command("AT+MTVER?")
        self.assertEqual(res, 0)
        self.assertEqual(lines, ["+MTVER:0.1.0"])
        self.assertEqual(ft.tx, b"AT+MTVER?\r\n")

    def test_mterr_code(self):
        link, _ = link_with_reply(b"+MTERR:3\r\nERROR\r\n")
        res, lines = link.command("AT+MTATTR=1,0xFFFF,0")
        self.assertEqual(res, 3)
        self.assertEqual(lines, [])

    def test_bare_error(self):
        link, _ = link_with_reply(b"ERROR\r\n")
        res, _ = link.command("AT+MTVER=1")
        self.assertEqual(res, -1)

    def test_timeout(self):
        link, _ = link_with_reply(b"")
        res, _ = link.command("AT+MTSTATE?")
        self.assertEqual(res, -2)

    def test_plain_text_intermediate(self):
        link, _ = link_with_reply(b"iLabs Electronics\r\nOK\r\n")
        res, lines = link.command("AT+CGMI")
        self.assertEqual(res, 0)
        self.assertEqual(lines, ["iLabs Electronics"])

    def test_urc_during_command_is_queued(self):
        link, _ = link_with_reply(b"+MTEVT:10,1\r\n+MTVER:0.1.0\r\nOK\r\n")
        res, lines = link.command("AT+MTVER?")
        self.assertEqual(res, 0)
        self.assertEqual(lines, ["+MTVER:0.1.0"])
        self.assertEqual([u for _, u in link.urcs], ["+MTEVT:10,1"])

    def test_explicit_expect_prefix(self):
        link, _ = link_with_reply(b"+MTEVTMASK:0x0800003F\r\nOK\r\n")
        res, lines = link.command("AT+MTEVT?", expect="+MTEVTMASK:")
        self.assertEqual(res, 0)
        self.assertEqual(lines, ["+MTEVTMASK:0x0800003F"])
        self.assertEqual(link.urcs, [])

    def test_lone_cr_and_lf_and_crlf_terminators(self):
        link, _ = link_with_reply(b"+MTVER:0.1.0\nOK\r")
        res, lines = link.command("AT+MTVER?")
        self.assertEqual(res, 0)
        self.assertEqual(lines, ["+MTVER:0.1.0"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 test/test_mt_regression.py`
Expected: `ModuleNotFoundError: No module named 'mt_regression'` (or ImportError for `ATLink`).

- [ ] **Step 3: Write minimal implementation**

Create `test/mt_regression.py`:

```python
#!/usr/bin/env python3
"""iLabs AT Hearth regression harness, stage T1: Phase 0 and Phase 1.

Test inventory: docs/TESTING.md sections 5 and 6.
Design decisions: docs/superpowers/specs/2026-07-30-c5-regression-harness-t1-design.md.

Run: python3 test/mt_regression.py --port /dev/ttyACM0
"""

import time


class ATLink:
    """Line-level client for the AT+MT link.

    Talks to a stream-like transport (read(n) -> bytes, write(bytes)) so the
    self-test can inject a fake. command() returns (result, lines) where
    result is 0 for OK, n > 0 for +MTERR:n then ERROR, -1 for a bare ERROR,
    and -2 when no terminal response arrived before the deadline. lines are
    the command's own intermediate response lines. Stray +... lines are
    queued on self.urcs with a monotonic timestamp, never dropped.
    """

    def __init__(self, transport, default_timeout=2.0):
        self.t = transport
        self.default_timeout = default_timeout
        self.urcs = []
        self.echo = False
        self.echo_seen = 0
        self.noise = []
        self._buf = b""

    def _read_line(self, deadline):
        """Return the next complete line (str, terminators stripped), or
        None when the deadline passes first. Accepts CR, LF and CRLF."""
        while True:
            for i, b in enumerate(self._buf):
                if b in (0x0D, 0x0A):
                    line = self._buf[:i]
                    rest = self._buf[i + 1:]
                    if b == 0x0D and rest[:1] == b"\n":
                        rest = rest[1:]
                    self._buf = rest
                    if line:
                        return line.decode("utf-8", "replace")
                    break  # empty line: rescan the shortened buffer
            else:
                chunk = self.t.read(64)
                if chunk:
                    self._buf += chunk
                elif time.monotonic() > deadline:
                    return None
                else:
                    time.sleep(0.005)

    @staticmethod
    def _derive_expect(cmd):
        if not cmd.upper().startswith("AT+"):
            return None
        name = cmd[3:].split("=")[0].rstrip("?")
        return "+" + name.upper() + ":" if name else None

    def command(self, cmd, expect=None, timeout=None):
        if expect is None:
            expect = self._derive_expect(cmd)
        self.t.write(cmd.encode("ascii") + b"\r\n")
        deadline = time.monotonic() + (timeout or self.default_timeout)
        return self._collect(cmd, expect, deadline)

    def _collect(self, echo_of, expect, deadline):
        lines = []
        err = None
        while True:
            line = self._read_line(deadline)
            if line is None:
                return -2, lines
            if self.echo and echo_of is not None and line == echo_of:
                self.echo_seen += 1
                continue
            if line == "OK":
                return 0, lines
            if line == "ERROR":
                return (err if err is not None else -1), lines
            if line.startswith("+MTERR:"):
                try:
                    err = int(line[len("+MTERR:"):])
                except ValueError:
                    err = -1
                continue
            if expect and line.startswith(expect):
                lines.append(line)
                continue
            if line.startswith("+"):
                self.urcs.append((time.monotonic(), line))
                continue
            lines.append(line)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 test/test_mt_regression.py`
Expected: all 8 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: ATLink core with the ESP-NOW-style result mapping

Transport is injected rather than opened internally, which is what
lets the result mapping and URC routing be pinned by a PC-only
self-test before the harness ever touches a board."
```

---

### Task 2: URC queue operations

**Files:**
- Modify: `test/mt_regression.py` (add methods to `ATLink`)
- Modify: `test/test_mt_regression.py` (add `TestUrcQueue`)

**Interfaces:**
- Consumes: `ATLink`, `FakeTransport` from Task 1.
- Produces: `ATLink.await_urc(pattern: str, timeout: float = 5.0) -> str | None` (regex search, consumes the matched URC, checks the queue first, then reads the wire), `ATLink.assert_no_urc(pattern: str, window: float) -> bool` (True when nothing matching arrives in the window), `ATLink.drain(quiet: float = 0.2) -> list[str]` (reads for a quiet period, empties the queue, returns what was discarded).

- [ ] **Step 1: Write the failing test**

Append to `test/test_mt_regression.py` (above the `__main__` block):

```python
class TestUrcQueue(unittest.TestCase):
    def test_await_urc_from_queue(self):
        link, _ = link_with_reply(b"")
        link.urcs.append((0.0, "+MTEVT:3"))
        self.assertEqual(link.await_urc(r"\+MTEVT:3", timeout=0.05), "+MTEVT:3")
        self.assertEqual(link.urcs, [])

    def test_await_urc_from_wire(self):
        link, ft = link_with_reply(b"")
        ft.rx = b"+MTATTR:1,6,0,1\r\n"
        self.assertEqual(link.await_urc(r"\+MTATTR:1,6,0,\d", timeout=0.2),
                         "+MTATTR:1,6,0,1")

    def test_await_urc_timeout_returns_none(self):
        link, _ = link_with_reply(b"")
        self.assertIsNone(link.await_urc(r"\+MTREADY", timeout=0.05))

    def test_await_urc_queues_non_matching(self):
        link, ft = link_with_reply(b"")
        ft.rx = b"+MTEVT:10,1\r\n+MTREADY\r\n"
        self.assertEqual(link.await_urc(r"\+MTREADY", timeout=0.2), "+MTREADY")
        self.assertEqual([u for _, u in link.urcs], ["+MTEVT:10,1"])

    def test_assert_no_urc(self):
        link, ft = link_with_reply(b"")
        self.assertTrue(link.assert_no_urc(r"\+MTREADY", 0.05))
        ft.rx = b"+MTREADY\r\n"
        self.assertFalse(link.assert_no_urc(r"\+MTREADY", 0.2))

    def test_drain_empties_queue_and_wire(self):
        link, ft = link_with_reply(b"")
        link.urcs.append((0.0, "+MTEVT:0"))
        ft.rx = b"+MTEVT:18,1\r\n"
        dropped = link.drain(quiet=0.05)
        self.assertEqual(dropped, ["+MTEVT:0", "+MTEVT:18,1"])
        self.assertEqual(link.urcs, [])
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 test/test_mt_regression.py`
Expected: the six new tests fail with `AttributeError: 'ATLink' object has no attribute 'await_urc'` (and friends); Task 1 tests still pass.

- [ ] **Step 3: Write minimal implementation**

Add to `ATLink` in `test/mt_regression.py` (import `re` at the top of the file):

```python
    def _pump_one(self, deadline):
        """Read one line outside any command; route it. Returns the line
        if it was a URC, else None."""
        line = self._read_line(deadline)
        if line is None:
            return None
        if line.startswith("+"):
            self.urcs.append((time.monotonic(), line))
            return line
        self.noise.append(line)
        return None

    def await_urc(self, pattern, timeout=5.0):
        """Return the first URC matching the regex, consuming it. Checks
        the queue before the wire, so a URC that raced an earlier command
        still satisfies the wait. None on timeout."""
        rx = re.compile(pattern)
        for i, (_, u) in enumerate(self.urcs):
            if rx.search(u):
                return self.urcs.pop(i)[1]
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = self._pump_one(deadline)
            if got is not None and rx.search(got):
                self.urcs.pop()
                return got
        return None

    def assert_no_urc(self, pattern, window):
        """True when nothing matching arrives within the window. This is
        the no-reboot proof for the reset-form negatives."""
        return self.await_urc(pattern, timeout=window) is None

    def drain(self, quiet=0.2):
        """Read until the link is quiet, then discard and return every
        queued URC, so one test's stray URC cannot satisfy the next
        test's expectation."""
        deadline = time.monotonic() + quiet
        while time.monotonic() < deadline:
            self._pump_one(deadline)
        dropped = [u for _, u in self.urcs]
        self.urcs.clear()
        return dropped
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 test/test_mt_regression.py`
Expected: all 14 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: URC queue with await, absence proof and drain

assert_no_urc exists for the reset-form negatives: their point is
proving the device did NOT reboot, which needs a bounded wait for
+MTREADY to not arrive, not just a result code."
```

---

### Task 3: Echo handling and raw byte sends

**Files:**
- Modify: `test/mt_regression.py` (add `ATLink.raw`)
- Modify: `test/test_mt_regression.py` (add `TestEchoAndRaw`)

**Interfaces:**
- Consumes: `ATLink._collect` from Task 1.
- Produces: `ATLink.raw(data: bytes, timeout: float = 0.5, echo_of: str | None = None, expect: str | None = None) -> tuple[int, list[str]]` (write bytes verbatim, collect with the same result mapping). Echo contract: when `link.echo` is True, a line equal to the sent command is skipped and `link.echo_seen` incremented; tests manage `link.echo` explicitly after `ATE1`/`ATE0` (ATLink does not flip it itself).

- [ ] **Step 1: Write the failing test**

Append to `test/test_mt_regression.py`:

```python
class TestEchoAndRaw(unittest.TestCase):
    def test_echo_line_skipped_and_counted(self):
        link, _ = link_with_reply(b"AT+MTVER?\r\n+MTVER:0.1.0\r\nOK\r\n")
        link.echo = True
        res, lines = link.command("AT+MTVER?")
        self.assertEqual(res, 0)
        self.assertEqual(lines, ["+MTVER:0.1.0"])
        self.assertEqual(link.echo_seen, 1)

    def test_echo_off_leaves_counter(self):
        link, _ = link_with_reply(b"+MTVER:0.1.0\r\nOK\r\n")
        res, _ = link.command("AT+MTVER?")
        self.assertEqual(res, 0)
        self.assertEqual(link.echo_seen, 0)

    def test_raw_bare_cr_times_out(self):
        link, ft = link_with_reply(b"")
        res, _ = link.raw(b"\r", timeout=0.05)
        self.assertEqual(res, -2)
        self.assertEqual(ft.tx, b"\r")

    def test_raw_with_terminator_variants(self):
        for term in (b"\r", b"\n", b"\r\n"):
            link, ft = link_with_reply(b"OK\r\n")
            res, _ = link.raw(b"AT" + term)
            self.assertEqual(res, 0, term)
            self.assertEqual(ft.tx, b"AT" + term)

    def test_raw_overlong_line_bare_error(self):
        link, _ = link_with_reply(b"ERROR\r\n")
        res, _ = link.raw(b"AT+MT" + b"X" * 600 + b"\r\n", timeout=0.2)
        self.assertEqual(res, -1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 test/test_mt_regression.py`
Expected: `AttributeError: 'ATLink' object has no attribute 'raw'` for the raw tests; the two echo tests already pass (echo logic landed in Task 1's `_collect`).

- [ ] **Step 3: Write minimal implementation**

Add to `ATLink`:

```python
    def raw(self, data, timeout=0.5, echo_of=None, expect=None):
        """Send bytes exactly as given (terminator included by the caller)
        and collect a response. This is how the bare-CR, overlong-line and
        terminator-variant grammar cases reach the parser unmangled."""
        self.t.write(data)
        return self._collect(echo_of, expect, time.monotonic() + timeout)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 test/test_mt_regression.py`
Expected: all 19 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "test: raw sends for the grammar edge cases

command() always appends CRLF, but the bare-CR, overlong-line and
per-terminator cases are ABOUT the bytes on the wire, so they need
a path that sends exactly what the test says."
```

---

### Task 4: Suite runner, registry, CLI, report, exit codes

**Files:**
- Modify: `test/mt_regression.py` (add `Suite`, `TESTS`, `add_test`, `main`)
- Modify: `test/test_mt_regression.py` (add `TestSuite`)

**Interfaces:**
- Consumes: `ATLink` complete from Tasks 1 to 3.
- Produces: `class Suite` with `check(name: str, ok: bool, tag: str = "AT+") -> bool`, `results: list[tuple[str, bool, str]]`, property `failed: int`, `summary()`. Global `TESTS: list[tuple[int, str, str, callable, bool]]` (phase, name, tag, fn, slow) filled via `add_test(phase, name, fn, tag="AT+", slow=False)`; each `fn(link) -> bool`. `main(argv=None) -> int` returns the process exit code. Tasks 6 to 8 plug into `phase0()` (defined Task 6) and `TESTS`.

- [ ] **Step 1: Write the failing test**

Append to `test/test_mt_regression.py`:

```python
from mt_regression import Suite


class TestSuite(unittest.TestCase):
    def test_check_scores_and_prints(self):
        s = Suite()
        self.assertTrue(s.check("MTVER? emits +MTVER:", True))
        self.assertFalse(s.check("MTCOMMISSION=901 -> +MTERR:1", False, tag="AT-"))
        self.assertEqual(s.failed, 1)
        self.assertEqual(s.results[0], ("MTVER? emits +MTVER:", True, "AT+"))
        self.assertEqual(s.results[1],
                         ("MTCOMMISSION=901 -> +MTERR:1", False, "AT-"))
```

Note: `Suite.check` prints as a side effect; the test asserts scoring, not stdout.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 test/test_mt_regression.py`
Expected: `ImportError: cannot import name 'Suite'`.

- [ ] **Step 3: Write the implementation**

Add to `test/mt_regression.py` (imports at top: `argparse`, `json`, `os`, `subprocess`, `sys`, `from datetime import datetime, timezone`):

```python
class Suite:
    """Scores and prints checks in the ESP-NOW RegressionSuite's format,
    so the two rigs' reports read the same."""

    def __init__(self):
        self.results = []

    def check(self, name, ok, tag="AT+"):
        ok = bool(ok)
        self.results.append((name, ok, tag))
        print("  [%s] [%s] %s" % ("PASS" if ok else "FAIL", tag, name))
        return ok

    @property
    def failed(self):
        return sum(1 for _, ok, _ in self.results if not ok)

    def summary(self):
        passed = sum(1 for _, ok, _ in self.results if ok)
        print("===== RESULT: %d passed, %d failed =====" % (passed, self.failed))


TESTS = []


def add_test(phase, name, fn, tag="AT+", slow=False):
    TESTS.append((phase, name, tag, fn, slow))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=os.environ.get("MT_PORT", "/dev/ttyACM0"))
    ap.add_argument("--phase", type=int, choices=[0, 1], default=None,
                    help="run only this phase (0 runs just the preflight gate)")
    ap.add_argument("-k", dest="keyword", default=None,
                    help="run only tests whose name contains this substring")
    ap.add_argument("--baseline", default=None,
                    help="write a JSON baseline of this run")
    ap.add_argument("--include-slow", action="store_true",
                    help="reserved for Phase 2; no effect in T1")
    args = ap.parse_args(argv)

    import serial
    try:
        port = serial.Serial(args.port, 115200, timeout=0.05)
    except serial.SerialException as exc:
        print("ABORT: cannot open %s: %s" % (args.port, exc))
        return 2

    link = ATLink(port)
    header = {
        "port": args.port,
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "fw_repo_head": repo_head(REPO_ROOT),
        "at_core_repo_head": repo_head(
            os.path.join(os.path.dirname(REPO_ROOT), "iLabs_AT_ESP-now")),
        "ssid": os.environ.get("MT_SSID"),
    }
    suite = Suite()
    try:
        problem = phase0(link, header)
        if problem:
            print("ABORT: " + problem)
            return 2
        if args.phase != 0:
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
    except KeyboardInterrupt:
        print("\n(interrupted)")
    finally:
        port.close()
    suite.summary()
    if args.baseline:
        write_baseline(args.baseline, header, suite)
    return 1 if suite.failed else 0


if __name__ == "__main__":
    sys.exit(main())
```

Also add the three helpers `main` references, with `REPO_ROOT` beside them (Task 5 fills in `write_baseline` and `capture_header`; Task 6 fills in `phase0`; stubs keep this task runnable):

```python
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def repo_head(path):
    """HEAD of a git repo, or None when unresolvable. Both this repo and
    the at_core repo go into the baseline header: at_core is compiled in
    cross-repo, so bisecting a regression needs both hashes."""
    try:
        out = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=5)
        return out.stdout.strip() or None
    except OSError:
        return None


def phase0(link, header):
    """Filled in by a later task. Returns an abort message or None."""
    return None


def capture_header(link, header):
    """Filled in by a later task."""


def write_baseline(path, header, suite):
    """Filled in by a later task."""
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 test/test_mt_regression.py`
Expected: all 20 tests PASS.

Also run: `python3 test/mt_regression.py --help`
Expected: usage text with `--port`, `--phase`, `-k`, `--baseline`, `--include-slow`; exit 0.

- [ ] **Step 5: Commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "feat: check runner, registry and CLI for the harness

Report format is the ESP-NOW RegressionSuite's on purpose: the two
rigs' reports are meant to be read side by side. Exit codes 0/1/2
make the run CI-wirable, which the on-board sketch never was."
```

---

### Task 5: Baseline JSON and header capture

**Files:**
- Modify: `test/mt_regression.py` (replace the `capture_header` and `write_baseline` stubs)
- Modify: `test/test_mt_regression.py` (add `TestBaseline`)

**Interfaces:**
- Consumes: `Suite`, `ATLink`, `repo_head` from Task 4.
- Produces: `capture_header(link, header)` fills `header["net"]`, `header["transport"]`, `header["qr_payload"]`, `header["manual_code"]` from unscored `AT+MTNET?` / `AT+MTCODES?` queries. `write_baseline(path, header, suite)` writes `{"header": {...}, "results": {name: "PASS"|"FAIL"}}` as sorted, indented JSON.

- [ ] **Step 1: Write the failing test**

Append to `test/test_mt_regression.py` (imports at its top: `import json`, `import tempfile`; and extend the `from mt_regression import ...` line with `capture_header, write_baseline`):

```python
class TestBaseline(unittest.TestCase):
    def test_capture_header(self):
        link, _ = link_with_reply(b"")
        replies = {
            b"AT+MTNET?\r\n": b"+MTNET:THREAD,1,1,0\r\nOK\r\n",
            b"AT+MTCODES?\r\n": b"+MTCODES:MT:Y.K90AFN004-JZ59D00,34970112332\r\nOK\r\n",
        }
        link.t.on_write = lambda d: replies.get(d, b"ERROR\r\n")
        header = {}
        capture_header(link, header)
        self.assertEqual(header["transport"], "THREAD")
        self.assertEqual(header["net"], "+MTNET:THREAD,1,1,0")
        self.assertEqual(header["qr_payload"], "MT:Y.K90AFN004-JZ59D00")
        self.assertEqual(header["manual_code"], "34970112332")

    def test_write_baseline(self):
        s = Suite()
        s.check("MTVER? emits +MTVER:", True)
        s.check("MTCOMMISSION=901 -> +MTERR:1", False, tag="AT-")
        with tempfile.NamedTemporaryFile("r", suffix=".json") as f:
            write_baseline(f.name, {"port": "/dev/null"}, s)
            data = json.load(open(f.name))
        self.assertEqual(data["header"]["port"], "/dev/null")
        self.assertEqual(data["results"]["MTVER? emits +MTVER:"], "PASS")
        self.assertEqual(data["results"]["MTCOMMISSION=901 -> +MTERR:1"], "FAIL")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 test/test_mt_regression.py`
Expected: the two new tests fail (`capture_header` stub sets nothing, `write_baseline` stub writes nothing).

- [ ] **Step 3: Write the implementation**

Replace the two stubs in `test/mt_regression.py` (add `import re` if not present from Task 2):

```python
def capture_header(link, header):
    """Record transport and onboarding codes into the report header.
    Unscored: the scored format checks live in Phase 1. The baseline, not
    the harness, carries the expected code values, so provisioned
    production units do not fail spuriously (TESTING.md 6.1)."""
    res, lines = link.command("AT+MTNET?")
    if res == 0 and lines:
        header["net"] = lines[0]
        m = re.match(r"\+MTNET:(WIFI|THREAD),", lines[0])
        if m:
            header["transport"] = m.group(1)
    res, lines = link.command("AT+MTCODES?")
    if res == 0 and lines:
        m = re.fullmatch(r"\+MTCODES:(.+),(\d{11})", lines[0])
        if m:
            header["qr_payload"] = m.group(1)
            header["manual_code"] = m.group(2)


def write_baseline(path, header, suite):
    data = {
        "header": header,
        "results": {name: ("PASS" if ok else "FAIL")
                    for name, ok, _ in suite.results},
    }
    with open(path, "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")
    print("baseline written: %s" % path)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 test/test_mt_regression.py`
Expected: all 22 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "feat: JSON baseline with the two repo heads in the header

A regression is meant to be a diff between baselines, and bisecting
one needs the at_core hash as well as this repo's: the engine is
compiled in cross-repo, so either repo can move independently."
```

---

### Task 6: Phase 0 preflight

**Files:**
- Modify: `test/mt_regression.py` (replace the `phase0` stub)
- Modify: `test/test_mt_regression.py` (add `TestPhase0`)

**Interfaces:**
- Consumes: `ATLink` complete; `main` already calls `phase0(link, header)` and aborts with exit 2 on a non-None return.
- Produces: `phase0(link, header) -> str | None` per TESTING.md §5: drains boot URCs, gates on `AT` within 1 s, `AT+CGMM` equal to `ESP32-C6 Hearth` (with a wrong-personality message for `ESP-NOW`), captures `AT+CGMR` into `header["cgmr"]`.

- [ ] **Step 1: Write the failing test**

Append to `test/test_mt_regression.py` (extend the import line with `phase0`):

```python
def scripted_link(replies):
    link, _ = link_with_reply(b"")
    link.default_timeout = 0.2
    link.t.on_write = lambda d: replies.get(d, b"ERROR\r\n")
    return link


class TestPhase0(unittest.TestCase):
    HEALTHY = {
        b"AT\r\n": b"OK\r\n",
        b"AT+CGMM\r\n": b"ESP32-C6 Hearth\r\nOK\r\n",
        b"AT+CGMR\r\n": b"0.1.0\r\nOK\r\n",
    }

    def test_healthy_device_passes(self):
        header = {}
        self.assertIsNone(phase0(scripted_link(self.HEALTHY), header))
        self.assertEqual(header["cgmr"], "0.1.0")

    def test_wrong_personality_named(self):
        replies = dict(self.HEALTHY)
        replies[b"AT+CGMM\r\n"] = b"ESP32-C6 ESP-NOW\r\nOK\r\n"
        problem = phase0(scripted_link(replies), {})
        self.assertIn("personality", problem)
        self.assertIn("flash.py", problem)

    def test_dead_link_aborts(self):
        problem = phase0(scripted_link({}), {})
        self.assertIn("AT", problem)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 test/test_mt_regression.py`
Expected: the three new tests fail (the stub returns None and fills nothing).

- [ ] **Step 3: Write the implementation**

Replace the `phase0` stub:

```python
def phase0(link, header):
    """Link preflight per TESTING.md 5. A hard gate, not a scored test:
    if this fails, the rest of the report is noise. Returns an abort
    message, or None when the link is usable."""
    link.drain(0.3)
    if link.command("AT", timeout=1.0)[0] != 0:
        return ("AT does not answer OK within 1 s: dead parser task, wrong "
                "port, or the console stream instead of the AT stream")
    res, lines = link.command("AT+CGMM")
    model = lines[0] if lines else ""
    if res != 0 or not model:
        return "AT+CGMM gave no model string"
    if "ESP-NOW" in model:
        return ("wrong personality: this board runs the ESP-NOW firmware; "
                "reflash Hearth with python3 fw/flash.py --build-dir "
                "build_b4 --port <port> --bridge espnow")
    if model != "ESP32-C6 Hearth":
        return "unexpected model: %r" % model
    res, lines = link.command("AT+CGMR")
    if res != 0 or not lines:
        return "AT+CGMR failed"
    header["cgmr"] = lines[0]
    print("  [GATE] preflight ok: %s, firmware %s" % (model, lines[0]))
    return None
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 test/test_mt_regression.py`
Expected: all 25 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add test/mt_regression.py test/test_mt_regression.py
git commit -m "feat: Phase 0 preflight gate with wrong-personality detection

The single most likely operator error on a bench carrying both
firmwares is the ESP-NOW image on the board, so that case aborts
with the reflash command instead of scoring 40 noise failures."
```

---

### Task 7: Phase 1 positive tests and cross-checks

**Files:**
- Modify: `test/mt_regression.py` (add helpers and the positive registrations)

**Interfaces:**
- Consumes: `add_test`, `ATLink`, `Suite` as defined above.
- Produces: helper factories `expect_ok(cmd, line_re=None, expect=None)` and `expect_err(cmd, want, expect=None)`, each returning an `fn(link) -> bool`; a `register_phase1()` function called at module scope so `main` sees the tests. Task 8 reuses both helpers.

- [ ] **Step 1: Add the helpers and positive tests**

Add to `test/mt_regression.py` after `add_test`:

```python
def expect_ok(cmd, line_re=None, expect=None):
    """Command terminates OK; when line_re is given, the first response
    line must fullmatch it."""
    def fn(link):
        res, lines = link.command(cmd, expect=expect)
        if res != 0:
            return False
        if line_re is None:
            return True
        return bool(lines) and re.fullmatch(line_re, lines[0]) is not None
    return fn


def expect_err(cmd, want, expect=None):
    """Command fails in exactly the expected way: want > 0 is a +MTERR
    code, -1 a bare ERROR, -2 no response. Exactness is the point
    (TESTING.md 6.3): 'an error happened' would let code collapses
    through."""
    def fn(link):
        res, _ = link.command(cmd, expect=expect,
                              timeout=1.0 if want == -2 else None)
        return res == want
    return fn


def t_echo_on_off(link):
    """ATE1 then ATE0, echo observed on and off. link.echo is managed
    here, not inside ATLink, so the harness never guesses device state."""
    if link.command("ATE1")[0] != 0:
        return False
    link.echo = True
    before = link.echo_seen
    if link.command("AT")[0] != 0 or link.echo_seen <= before:
        link.echo = False
        return False
    if link.command("ATE0")[0] != 0:
        link.echo = False
        return False
    link.echo = False
    before = link.echo_seen
    res, lines = link.command("AT")
    return res == 0 and link.echo_seen == before and not lines


def t_cgmr_matches_mtver(link):
    r1, a = link.command("AT+CGMR")
    r2, b = link.command("AT+MTVER?")
    return r1 == 0 and r2 == 0 and bool(a) and bool(b) \
        and b[0] == "+MTVER:" + a[0]


def t_attr_hex_equals_decimal(link):
    r1, dec = link.command("AT+MTATTR=1,6,0")
    r2, hexa = link.command("AT+MTATTR=1,0x0006,0x0000")
    return r1 == 0 and r2 == 0 and dec == hexa


def t_state_fabrics_consistent(link):
    """State 2 implies fabrics > 0, state 0 implies fabrics == 0, and the
    count in +MTSTATE agrees with +MTFABRICS. State 1 may hold either:
    an open window outranks the fabric count in mt_matter_state()."""
    r1, s = link.command("AT+MTSTATE?")
    r2, f = link.command("AT+MTFABRICS?")
    if r1 != 0 or r2 != 0 or not s or not f:
        return False
    ms = re.fullmatch(r"\+MTSTATE:([012]),(\d+)", s[0])
    mf = re.fullmatch(r"\+MTFABRICS:(\d+)", f[0])
    if not ms or not mf or ms.group(2) != mf.group(1):
        return False
    state, count = int(ms.group(1)), int(mf.group(1))
    if state == 2 and count == 0:
        return False
    if state == 0 and count != 0:
        return False
    return True


def t_codes_stable(link):
    r1, a = link.command("AT+MTCODES?")
    r2, b = link.command("AT+MTCODES?")
    return r1 == 0 and r2 == 0 and bool(a) and a == b


def t_evt_mask_set_read_restore(link):
    """The one piece of state Phase 1 may touch, restored immediately:
    a wide-open mask makes later URC assertions race BLE and
    connectivity chatter (TESTING.md 6.1)."""
    ok = link.command("AT+MTEVT=0xFFFFFFFF")[0] == 0
    res, lines = link.command("AT+MTEVT?", expect="+MTEVTMASK:")
    ok = ok and res == 0 and lines == ["+MTEVTMASK:0xFFFFFFFF"]
    ok = link.command("AT+MTEVT=0x0800003F")[0] == 0 and ok
    res, lines = link.command("AT+MTEVT?", expect="+MTEVTMASK:")
    return ok and res == 0 and lines == ["+MTEVTMASK:0x0800003F"]
```

Then the registrations (QR payload alphabet is CHIP base38: digits, upper-case letters, minus and full stop):

```python
def register_phase1_positive():
    p = lambda name, fn: add_test(1, name, fn, tag="AT+")
    p("AT -> OK", expect_ok("AT"))
    p("ATE1/ATE0 echo on and off", t_echo_on_off)
    p("CGMI -> iLabs Electronics",
      expect_ok("AT+CGMI", line_re=r"iLabs Electronics"))
    p("CGMM -> ESP32-C6 Hearth",
      expect_ok("AT+CGMM", line_re=r"ESP32-C6 Hearth"))
    p("CGMR equals MTVER? field", t_cgmr_matches_mtver)
    p("MTVER? emits +MTVER:", expect_ok("AT+MTVER?", line_re=r"\+MTVER:.+"))
    p("MTSTATE? format", expect_ok("AT+MTSTATE?",
                                   line_re=r"\+MTSTATE:[012],\d+"))
    p("MTFABRICS? format", expect_ok("AT+MTFABRICS?",
                                     line_re=r"\+MTFABRICS:\d+"))
    p("MTCODES? format", expect_ok(
        "AT+MTCODES?", line_re=r"\+MTCODES:MT:[0-9A-Z.\-]+,\d{11}"))
    p("MTCODES? stable across reads", t_codes_stable)
    p("lower-case dispatch", expect_ok("at+mtver?", line_re=r"\+MTVER:.+",
                                       expect="+MTVER:"))
    p("MTATTR read 1,6,0", expect_ok("AT+MTATTR=1,6,0",
                                     line_re=r"\+MTATTR:1,6,0,[01]"))
    p("MTATTR hex equals decimal", t_attr_hex_equals_decimal)
    p("MTATTR root VendorID read", expect_ok(
        "AT+MTATTR=0,0x0028,0x0002", line_re=r"\+MTATTR:0,40,2,\d+"))
    p("MTEVT? boot default mask", expect_ok(
        "AT+MTEVT?", line_re=r"\+MTEVTMASK:0x0800003F",
        expect="+MTEVTMASK:"))
    p("MTEVT mask set/readback/restore", t_evt_mask_set_read_restore)
    p("MTNET? format", expect_ok(
        "AT+MTNET?", line_re=r"\+MTNET:(WIFI|THREAD),[01],[01],[01]"))
    p("MTBAUD? -> 115200", expect_ok("AT+MTBAUD?",
                                     line_re=r"\+MTBAUD:115200"))
    p("MTFLOW? -> 0", expect_ok("AT+MTFLOW?", line_re=r"\+MTFLOW:0"))
    p("MTFLOW=0 accepted", expect_ok("AT+MTFLOW=0"))


register_phase1_positive()
```

Notes locked in here, so the implementer does not rediscover them:
- `+MTNET` asserts FOUR fields. The mismatch field is always emitted (P2, spec §3.12); TESTING.md's three-field form is stale.
- `MTATTR read 1,6,0` presumes the rig composition (endpoint 1, OnOff cluster). That is the standard bench state from C4; a bare device fails it with `+MTERR:2`, which Phase 0 does not guard. Acceptable for T1: the baseline names the bench.
- `MTEVT? boot default mask` asserts current mask equals the boot default. The set/read/restore test restores it, so reruns stay green provided the session started at the default.
- `lower-case dispatch` passes `expect="+MTVER:"` explicitly: the auto-derived prefix upper-cases the command name anyway, but being explicit keeps the test independent of that detail.

- [ ] **Step 2: Run the self-test (no regressions)**

Run: `python3 test/test_mt_regression.py`
Expected: all 25 tests still PASS (this task adds device-facing tests, which only hardware can run; the self-test guards the helpers indirectly through Tasks 1 to 6).

Also run: `python3 -c "import sys; sys.path.insert(0, 'test'); import mt_regression as m; print(len(m.TESTS))"`
Expected: `20` (the positive registrations).

- [ ] **Step 3: Commit**

```bash
git add test/mt_regression.py
git commit -m "feat: Phase 1 positive table and cross-checks

MTNET asserts four fields, not TESTING.md's three: the P2 mismatch
field is appended unconditionally, and a harness that pinned the
stale shape would fail on every current build."
```

---

### Task 8: Phase 1 negative tests

**Files:**
- Modify: `test/mt_regression.py` (add negative helpers and registrations)

**Interfaces:**
- Consumes: `expect_err`, `add_test`, `ATLink.raw`, `ATLink.assert_no_urc` as defined above.
- Produces: `register_phase1_negative()` called at module scope, directly after `register_phase1_positive()`.

- [ ] **Step 1: Add the negative helpers and registrations**

Add to `test/mt_regression.py`:

```python
def reject_without_reboot(cmd):
    """The four reset-form negatives: a dispatch regression here wipes
    the fabric, so each carries its own no-reboot proof (no +MTREADY
    within 3 s), not just a result code (TESTING.md 6.2)."""
    def fn(link):
        res, _ = link.command(cmd)
        return res == -1 and link.assert_no_urc(r"\+MTREADY", 3.0)
    return fn


def commission_rejected_stateless(argtext):
    """Rejected before any side effect: AT+MTSTATE? before and after
    must agree, which is the assertion that proves it."""
    def fn(link):
        r0, before = link.command("AT+MTSTATE?")
        res, _ = link.command("AT+MTCOMMISSION=" + argtext)
        r1, after = link.command("AT+MTSTATE?")
        return r0 == 0 and r1 == 0 and res == 1 and before == after
    return fn


def t_bare_cr_no_response(link):
    """Deliberate parser behaviour, easy to break: an empty line gets no
    response at all, not ERROR."""
    res, _ = link.raw(b"\r", timeout=0.5)
    return res == -2


def t_overlong_line(link):
    res, _ = link.raw(b"AT+MT" + b"X" * 600 + b"\r\n", timeout=1.0)
    return res == -1


def t_terminator_variants(link):
    for term in (b"\r", b"\n", b"\r\n"):
        res, _ = link.raw(b"AT" + term, timeout=1.0)
        if res != 0:
            return False
    return True


def register_phase1_negative():
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    # grammar and dispatch (shared at_core)
    n("MTBOGUS -> +MTERR:8", expect_err("AT+MTBOGUS", 8))
    n("non-AT line -> ERROR", expect_err("HELLO", -1))
    n("trailing char after ? -> ERROR", expect_err("AT+MTVER?X", -1))
    n("overlong line -> ERROR", t_overlong_line)
    n("bare CR -> no response", t_bare_cr_no_response)
    n("CR, LF and CRLF all terminate", t_terminator_variants)

    # wrong command form: bare ERROR, never a code (TESTING.md 6.3)
    n("MTVER=1 -> ERROR", expect_err("AT+MTVER=1", -1))
    n("CGMI? -> ERROR", expect_err("AT+CGMI?", -1))
    n("MTSTATE (exec) -> ERROR", expect_err("AT+MTSTATE", -1))
    n("MTFABRICS=1 -> ERROR", expect_err("AT+MTFABRICS=1", -1))
    n("MTCODES=1 -> ERROR", expect_err("AT+MTCODES=1", -1))
    n("MTRESET? -> ERROR, no reboot", reject_without_reboot("AT+MTRESET?"))
    n("MTRESET=1 -> ERROR, no reboot", reject_without_reboot("AT+MTRESET=1"))
    n("MTFRESET? -> ERROR, no reboot", reject_without_reboot("AT+MTFRESET?"))
    n("MTFRESET=1 -> ERROR, no reboot", reject_without_reboot("AT+MTFRESET=1"))
    n("MTCOMMISSION? -> ERROR", expect_err("AT+MTCOMMISSION?", -1))
    n("MTATTR? -> ERROR", expect_err("AT+MTATTR?", -1))
    n("MTATTR (exec) -> ERROR", expect_err("AT+MTATTR", -1))

    # MTCOMMISSION range and parse guards, all state-safe
    n("MTCOMMISSION=29 -> +MTERR:1", commission_rejected_stateless("29"))
    n("MTCOMMISSION=901 -> +MTERR:1", commission_rejected_stateless("901"))
    n("MTCOMMISSION=abc -> +MTERR:1", commission_rejected_stateless("abc"))
    n("MTCOMMISSION= -> +MTERR:1", commission_rejected_stateless(""))
    n("MTCOMMISSION=300x -> +MTERR:1", commission_rejected_stateless("300x"))

    # MTATTR argument validation: exact codes, walking ep/cluster/attr/type
    n("MTATTR=1,6 -> +MTERR:1", expect_err("AT+MTATTR=1,6", 1))
    n("MTATTR=1,6,0,1,2 -> +MTERR:1", expect_err("AT+MTATTR=1,6,0,1,2", 1))
    n("MTATTR six params -> +MTERR:1", expect_err("AT+MTATTR=1,6,0,1,0,9", 1))
    n("MTATTR=x,6,0 -> +MTERR:1", expect_err("AT+MTATTR=x,6,0", 1))
    n("MTATTR=1,zz,0 -> +MTERR:1", expect_err("AT+MTATTR=1,zz,0", 1))
    n("MTATTR=1,6,0,z -> +MTERR:1", expect_err("AT+MTATTR=1,6,0,z", 1))
    n("MTATTR=99,6,0 -> +MTERR:2", expect_err("AT+MTATTR=99,6,0", 2))
    n("MTATTR=1,0xFFFF,0 -> +MTERR:3", expect_err("AT+MTATTR=1,0xFFFF,0", 3))
    n("MTATTR=1,6,0xFFFF -> +MTERR:4", expect_err("AT+MTATTR=1,6,0xFFFF", 4))
    n("MTATTR NodeLabel -> +MTERR:5",
      expect_err("AT+MTATTR=0,0x0028,0x0005", 5))

    # MTEVT and MTNET
    n("MTEVT (exec) -> ERROR", expect_err("AT+MTEVT", -1))
    n("MTEVT=zz -> +MTERR:1", expect_err("AT+MTEVT=zz", 1))
    n("MTEVT= -> +MTERR:1", expect_err("AT+MTEVT=", 1))
    n("MTNET (exec) -> ERROR", expect_err("AT+MTNET", -1))
    n("MTNET=1 -> ERROR", expect_err("AT+MTNET=1", -1))

    # MTBAUD and MTFLOW: rejections only, a rate switch owns a reconnect
    # and does not belong in a one-line assertion (TESTING.md 6.2)
    n("MTBAUD (exec) -> ERROR", expect_err("AT+MTBAUD", -1))
    n("MTBAUD=12345 -> +MTERR:1", expect_err("AT+MTBAUD=12345", 1))
    n("MTBAUD=zz -> +MTERR:1", expect_err("AT+MTBAUD=zz", 1))
    n("MTBAUD=1843200 -> +MTERR:1", expect_err("AT+MTBAUD=1843200", 1))
    n("MTFLOW (exec) -> ERROR", expect_err("AT+MTFLOW", -1))
    n("MTFLOW=4 -> +MTERR:1", expect_err("AT+MTFLOW=4", 1))
    n("MTFLOW=1 -> +MTERR:1 (RTS/CTS unwired)", expect_err("AT+MTFLOW=1", 1))
    n("MTFLOW=3 -> +MTERR:1 (RTS/CTS unwired)", expect_err("AT+MTFLOW=3", 1))


register_phase1_negative()
```

- [ ] **Step 2: Run the self-test and count the registry**

Run: `python3 test/test_mt_regression.py`
Expected: all 25 tests PASS.

Run: `python3 -c "import sys; sys.path.insert(0, 'test'); import mt_regression as m; print(len(m.TESTS))"`
Expected: `66` (20 positive + 46 negative).

- [ ] **Step 3: Commit**

```bash
git add test/mt_regression.py
git commit -m "feat: Phase 1 negatives with exact codes and no-reboot proofs

Every negative asserts the exact +MTERR code or the exact bare-ERROR
shape, both directions: a code collapsing into another, or into a
bare ERROR, is what this table exists to catch, and it is invisible
to a test that only checks for failure."
```

---

### Task 9: Hardware verification on the Thread image, commit the baseline

**Files:**
- Create: `test/baselines/thread.json`

**Interfaces:**
- Consumes: the complete harness from Tasks 1 to 8; the bench device (Thread image, commissioned, port `/dev/ttyACM0` via the RP2350 bridge with the espnow bridge sketch).

Bench facts, so nothing is rediscovered: `/dev/ttyACM0` is the DUT AT link, `/dev/ttyACM1` the Debug Probe, `/dev/ttyACM2` the Thread border router RCP (never open it). The device is commissioned on a Thread fabric; Phase 1 must leave that intact, and proving so is part of this task.

- [ ] **Step 1: Confirm the port is free and the device answers**

Run: `fuser /dev/ttyACM0 || echo free`
Expected: `free` (if a process holds it, stop that process first; do not proceed).

- [ ] **Step 2: First harness run, scored**

Run: `python3 test/mt_regression.py --port /dev/ttyACM0`
Expected: `[GATE] preflight ok: ESP32-C6 Hearth, firmware 0.1.0`, then 66 checks, `===== RESULT: 66 passed, 0 failed =====`, exit code 0 (`echo $?`).

If any check fails: STOP and diagnose against TESTING.md §8 before touching the harness. A failure here is either a real firmware regression or a harness bug; systematic-debugging applies, and the GPIO2 console (via the espnow bridge) carries the firmware's side of the story.

- [ ] **Step 3: Second run proves state safety**

Run: `python3 test/mt_regression.py --port /dev/ttyACM0`
Expected: identical result. In particular `MTSTATE? format`, `MTFABRICS? format`, `t_state_fabrics_consistent` and `MTEVT? boot default mask` all still pass, which proves the first run left the commissioned fabric and the event mask exactly as it found them.

- [ ] **Step 4: Record and commit the baseline**

```bash
mkdir -p test/baselines
python3 test/mt_regression.py --port /dev/ttyACM0 --baseline test/baselines/thread.json
git add test/baselines/thread.json
git commit -m "test: Thread-image baseline for the T1 harness

Recorded against the commissioned bench device: fabric count 1 and
transport THREAD in the header are the bench state, not assertions."
```

---

### Task 10: Reflash to WiFi build_b4, verify, commit the wifi baseline

**Files:**
- Create: `test/baselines/wifi.json`

**Interfaces:**
- Consumes: the harness; `fw/flash.py` (auto-BOOTSEL flash, no button press needed).

Context the implementer must know: flashing the WiFi image onto a device holding a Thread-era fabric creates exactly the P2 transport-mismatch state (`+MTNET:WIFI,_,_,1`, boot window auto-opened, `+MTEVT:27`). The harness's format-only assertions pass regardless, but the committed wifi baseline should describe a clean device, not a bench accident. So this task erases the test fabric with `AT+MTRESET` after flashing. That fabric is a disposable test artifact from the 2026-07-29 Thread verification; the composition survives `AT+MTRESET` by design.

- [ ] **Step 1: Flash the WiFi image**

Run: `python3 fw/flash.py --build-dir build_b4 --port /dev/ttyACM0 --bridge espnow`
Expected: flash completes and the espnow bridge is reinstalled; the script reports success.

- [ ] **Step 2: Erase the Thread-era test fabric**

```bash
python3 - <<'EOF'
import sys, time
sys.path.insert(0, "test")
import serial
from mt_regression import ATLink
port = serial.Serial("/dev/ttyACM0", 115200, timeout=0.05)
link = ATLink(port)
link.drain(0.5)
res, _ = link.command("AT+MTRESET", timeout=5.0)
print("AT+MTRESET ->", res)
ready = link.await_urc(r"\+MTREADY", timeout=20.0)
print("ready:", ready)
port.close()
assert res == 0 and ready is not None
EOF
```

Expected: `AT+MTRESET -> 0` then `ready: +MTREADY`.

- [ ] **Step 3: Two harness runs, scored, then the baseline**

Run twice: `python3 test/mt_regression.py --port /dev/ttyACM0`
Expected both times: `===== RESULT: 66 passed, 0 failed =====`, exit 0. The device is now uncommissioned with a window auto-opened, so expect state 1 with fabric count 0 inside the consistency check, and `+MTNET:WIFI,...,0` as the last field (no mismatch after the reset).

Then: `python3 test/mt_regression.py --port /dev/ttyACM0 --baseline test/baselines/wifi.json`

- [ ] **Step 4: Commit, close out T1**

```bash
git add test/baselines/wifi.json
git commit -m "test: WiFi-image baseline for the T1 harness

Recorded after an AT+MTRESET, so the baseline describes a clean
uncommissioned WiFi device rather than the P2 mismatch state the
reflash transiently created."
```

Then update the task graph: note on node S8 that T1 is built and hardware-verified on both images; T2/T3 remain, still gated on the +MTEVT:4 decision (D16).

---

## Self-Review (completed at plan time)

- Spec coverage: design §2 CLI -> Task 4; §3 ATLink/URC -> Tasks 1 to 3; §4 inventory -> Tasks 7 and 8; §5 report/baseline -> Tasks 4 and 5; §6 error handling -> Tasks 4 and 6; §7 verification -> Tasks 9 and 10. TESTING.md §6.1/§6.2 rows were cross-checked line by line against Tasks 7 and 8; the deliberate deviations (four-field MTNET, no positive MTBAUD set) are annotated where they occur.
- Placeholders: the Task 4 stubs for `phase0`/`capture_header`/`write_baseline` are explicit forward declarations filled by named later tasks, not TBDs.
- Type consistency: `fn(link) -> bool` everywhere; `TESTS` tuple shape `(phase, name, tag, fn, slow)` consistent across Tasks 4, 7, 8; `command()` return shape `(int, list[str])` consistent across all tasks.
