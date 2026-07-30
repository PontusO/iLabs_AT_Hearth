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
    if any(p == phase and n == name for p, n, _, _, _ in TESTS):
        raise ValueError("duplicate test name in phase %r: %r" % (phase, name))
    TESTS.append((phase, name, tag, fn, slow))


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
    here, not inside ATLink, so the harness never guesses device state.
    Every failure path after ATE1 succeeds sends a best-effort ATE0 first,
    so one failed check does not leave the device echoing for the rest of
    the run."""
    if link.command("ATE1")[0] != 0:
        return False
    link.echo = True
    before = link.echo_seen
    if link.command("AT")[0] != 0 or link.echo_seen <= before:
        link.echo = False
        link.command("ATE0")
        return False
    if link.command("ATE0")[0] != 0:
        link.echo = False
        link.command("ATE0")
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
    p("MTSTATE agrees with MTFABRICS", t_state_fabrics_consistent)
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
    except Exception as exc:
        print("(link lost: %s)" % exc)
    finally:
        port.close()
    suite.summary()
    if args.baseline:
        write_baseline(args.baseline, header, suite)
    return 1 if suite.failed else 0


if __name__ == "__main__":
    sys.exit(main())
