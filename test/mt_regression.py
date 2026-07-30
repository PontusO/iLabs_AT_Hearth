#!/usr/bin/env python3
"""iLabs AT Hearth regression harness, stage T1: Phase 0 and Phase 1.

Test inventory: docs/TESTING.md sections 5 and 6.
Design decisions: docs/superpowers/specs/2026-07-30-c5-regression-harness-t1-design.md.

Run: python3 test/mt_regression.py --port /dev/ttyACM0
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone


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

    def raw(self, data, timeout=0.5, echo_of=None, expect=None):
        """Send bytes exactly as given (terminator included by the caller)
        and collect a response. This is how the bare-CR, overlong-line and
        terminator-variant grammar cases reach the parser unmangled."""
        self.t.write(data)
        return self._collect(echo_of, expect, time.monotonic() + timeout)

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


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def repo_head(path):
    """HEAD of a git repo, or None when unresolvable. Both this repo and
    the at_core repo go into the baseline header: at_core is compiled in
    cross-repo, so bisecting a regression needs both hashes."""
    try:
        out = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=5)
        return out.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def phase0(link, header):
    """Filled in by a later task. Returns an abort message or None."""
    return None


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
