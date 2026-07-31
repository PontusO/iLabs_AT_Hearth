#!/usr/bin/env python3
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

import argparse
import json
import os
import re
import shutil
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
    queued on self.urcs with a monotonic timestamp, never dropped, and
    mirrored into urc_history, which drain() never clears (2.6 sweeps it).
    """

    def __init__(self, transport, default_timeout=2.0):
        self.t = transport
        self.default_timeout = default_timeout
        self.urcs = []
        self.urc_history = []
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

    def _queue_urc(self, line):
        entry = (time.monotonic(), line)
        self.urcs.append(entry)
        self.urc_history.append(entry)

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
                self._queue_urc(line)
                continue
            lines.append(line)

    def _pump_one(self, deadline):
        """Read one line outside any command; route it. Returns the line
        if it was a URC, else None."""
        line = self._read_line(deadline)
        if line is None:
            return None
        if line.startswith("+"):
            self._queue_urc(line)
            return line
        self.noise.append(line)
        return None

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
        self.skipped = []
        self.gated = []

    def check(self, name, ok, tag="AT+"):
        ok = bool(ok)
        self.results.append((name, ok, tag))
        print("  [%s] [%s] %s" % ("PASS" if ok else "FAIL", tag, name))
        return ok

    def skip(self, name, reason):
        """A step not run because an earlier step broke its preconditions.
        Counted separately from failures, and never silently: a truncated
        run must not read as a clean one (design spec section 3)."""
        self.skipped.append((name, reason))
        print("  [SKIP] %s: %s" % (name, reason))

    def gate_skip(self, name, flag):
        """A step deliberately gated out (--include-slow, --include-manual,
        -k). Unlike skip(), never fails the run: gating is operator
        intent, not truncation."""
        self.gated.append((name, flag))
        print("  [SKIP] %s (gated: --%s)" % (name, flag.replace("_", "-")))

    @property
    def failed(self):
        return sum(1 for _, ok, _ in self.results if not ok)

    def summary(self):
        passed = sum(1 for _, ok, _ in self.results if ok)
        parts = ["%d passed" % passed, "%d failed" % self.failed]
        if self.skipped:
            parts.append("%d skipped" % len(self.skipped))
        if self.gated:
            parts.append("%d gated" % len(self.gated))
        print("===== RESULT: %s =====" % ", ".join(parts))


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
        self.passcode = None
        self.discriminator = None
        self.chip2 = None         # second controller identity (T3 2.7)
        self.node_id = getattr(opts, "node_id", 0x4845)
        self.subscriber_factory = None  # test seam; None means real Subscriber
        self.relink = None        # installed by main(); steps 2.8/2.9 need it
        self.swd_runner = None    # test seam for swd_reset
        self.power_cycler = None  # test seam for operator_power_cycle
        self.composition = None   # +MTEP: lines captured by run_phase2


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
    ctx.passcode, ctx.discriminator = passcode, discriminator
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


def step_2_7_second_fabric(ctx):
    """TESTING.md 2.7: fabric accounting through an additional window.
    The second controller pairs over the network (the device is already
    on WiFi; verb pinned by T3 Task 1 finding (b): `onnetwork-long`), and
    the DE24 pair (one +MTEVT:4 per +MTEVT:0, after complete) must hold
    for host-opened windows exactly as it does for the boot window."""
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
    # Measured 2026-07-31 (T3 Task 11, n=2 per wait at 0/6/15 s between
    # write and reset): OnOff always boots to 0, so the firmware does
    # not persist it (bug B63, parked; OnOff carries the nonvolatile
    # quality in the Matter data model). The harness pins the real
    # behavior; if B63 is ever fixed this check flips to expect 1.
    s.check("2.8 attribute resets to 0 (no persistence, B63)",
            res == 0 and lines == ["+MTATTR:1,6,0,0"], tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.8 state 2 (operational)",
            res == 0 and lines == ["+MTSTATE:2,1"], tag="P2")
    s.check("2.8 no boot window on commissioned device",
            link.assert_no_urc(r"\+MTEVT:0$", 5.0), tag="P2")


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


WINDOW_EXPIRY_RAISES_EVT5 = False  # T3 Task 1 findings (task-1-findings.md,
# finding (c)): a 200 s capture of a never-attached AT+MTCOMMISSION=180
# window showed exactly one +MTEVT:4 at ~180.2 s and no +MTEVT:5 at any
# point. Keep False; the True branch below documents the pinned
# alternative but is not exercised here.


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


PHASE2_STEPS = []  # populated bottom-of-module once the steps exist


def recover_after_abort(ctx, reason):
    """Best-effort bench recovery after a chain abort: do not strand a
    commissioned device or dirty storage for the next run. Unscored.
    The link may be dead or deliberately closed (a failed relink leaves
    the port closed by contract), so link errors are swallowed here and
    the storage wipes always run; OSError covers SerialException and
    PortNotOpenError, both subclasses."""
    print("  (recovery after abort: %s)" % reason)
    try:
        res, _ = cmd_retry(ctx.link, "AT+MTRESET", timeout=5.0)
        if res == 0:
            ctx.link.await_urc(r"\+MTREADY$", timeout=15.0)
    except OSError as exc:
        print("  (recovery: link unavailable, device reset skipped: %s)"
              % exc)
    for c in (ctx.chip, getattr(ctx, "chip2", None)):
        if c is not None:
            c.wipe_storage()


def run_phase2(ctx):
    """Ordered execution with abort/skip semantics (T2) plus gates and
    -k deselection (T3): a gated-out step is operator intent and exits
    zero; a step whose prerequisite did not run is an abort-skip, so a
    truncated chain still fails loudly. The capture is wrapped in
    try/except OSError the same way recover_after_abort's own
    AT+MTRESET is: a link already dead before the first step runs must
    not stop the chain from at least reaching its skips, it just leaves
    ctx.composition None."""
    try:
        res, lines = ctx.link.command("AT+MTEP?")
        ctx.composition = lines if res == 0 else None
    except OSError:
        ctx.composition = None
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


DEFAULT_CHIPTOOL = os.path.expanduser(
    "~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool")


class ChipTool:
    """One-shot chip-tool invocations (design spec section 2). Every call
    except the option-less payload family carries --storage-directory so
    a stale fabric cannot leak between runs, and the runner is injectable
    so self-tests never spawn."""

    def __init__(self, binary, storage_dir, runner=None):
        self.binary = binary
        self.storage_dir = storage_dir
        self._runner = runner or self._default_runner

    @staticmethod
    def _default_runner(argv, timeout):
        try:
            proc = subprocess.run(argv, capture_output=True, text=True,
                                  timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            # A hung chip-tool must become a scored failure with skip
            # semantics, not a raw traceback: TimeoutExpired is a
            # SubprocessError, which neither run_phase2 (StepAbort only)
            # nor main (serial/OSError only) catches. subprocess.run has
            # already killed the child. Partial output arrives as bytes
            # despite text=True (CPython quirk).
            def _txt(s):
                if isinstance(s, bytes):
                    return s.decode(errors="replace")
                return s or ""
            return 124, (_txt(exc.stdout) + _txt(exc.stderr)
                         + "\n(chip-tool timed out after %s s)" % timeout)
        return proc.returncode, proc.stdout + proc.stderr

    def run(self, args, timeout=60):
        os.makedirs(self.storage_dir, exist_ok=True)
        argv = [self.binary] + list(args)
        # The payload subcommands are pure parsers taking exactly one
        # positional argument and no options; --storage-directory makes
        # them fail with "Wrong arguments number" (found on hardware).
        if args and args[0] != "payload":
            argv += ["--storage-directory", self.storage_dir]
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


def parse_fabric_index(text, node_id):
    """FabricIndex of the fabrics-list entry whose node id matches, from
    `operationalcredentials read fabrics` output. Shape validated against
    the Task 1 fixture (test/fixtures/chiptool_read_fabrics.txt): each
    entry prints `NodeID:` *before* `FabricIndex:`, the opposite order the
    T3 plan guessed, so this tracks the most recently seen node id and
    resolves it when the following FabricIndex line arrives. The fixture's
    NodeID prints as plain decimal with no `0x` prefix; other chip-tool
    revisions are documented to print it as hex, so a `0x` prefix (when
    present) selects base 16. None when the node is not present."""
    last_node = None
    for m in re.finditer(
            r"Node\s*ID:\s*(0x)?([0-9A-Fa-f]+)|FabricIndex:\s*(\d+)",
            text, re.IGNORECASE):
        if m.group(3) is not None:
            if last_node == node_id:
                return int(m.group(3))
            last_node = None
        else:
            last_node = int(m.group(2), 16 if m.group(1) else 10)
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


class Subscriber:
    """Background subscription via `chip-tool interactive start` with the
    subscribe command written to its stdin. Interactive mode is not a
    choice (design spec section 8.1): the real binary's one-shot
    subscribe command exits about 3 s after the priming report, before
    any change-triggered report can arrive; only an interactive session
    keeps the subscription alive. Found on hardware in Task 11.

    At most one instance alive, and no ChipTool.run() while it lives:
    chip-tool's ini storage is not safe for two processes. Its stdout
    goes to a file; parsing reads the file, so report observation works
    the same whether the process is live (hardware) or the file is
    written by a test."""

    def __init__(self, chip, node_id, endpoint=1, min_s=0, max_s=5,
                 popen=None):
        self.chip = chip
        self.argv = [chip.binary, "interactive", "start",
                     "--storage-directory", chip.storage_dir]
        self.subscribe_cmd = ("onoff subscribe on-off %d %d 0x%X %d"
                              % (min_s, max_s, node_id, endpoint))
        self.out_path = os.path.join(chip.storage_dir, "subscribe.log")
        self._popen = popen or subprocess.Popen
        self._proc = None
        self._out = None

    def _send(self, line):
        """A failed write is not an error here: the process death it
        implies is what start()'s poll and stop()'s escalation handle."""
        try:
            self._proc.stdin.write((line + "\n").encode())
            self._proc.stdin.flush()
            return True
        except (OSError, ValueError):
            return False

    def start(self, settle=10.0):
        """True once the priming report arrives, which is the proof the
        subscription is live. False if the process dies first or the
        settle window passes silently."""
        os.makedirs(self.chip.storage_dir, exist_ok=True)
        self._out = open(self.out_path, "wb")
        self._proc = self._popen(self.argv, stdin=subprocess.PIPE,
                                 stdout=self._out,
                                 stderr=subprocess.STDOUT)
        self._send(self.subscribe_cmd)
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
        """quit() first so chip-tool can flush and release its ini
        storage cleanly; SIGTERM and SIGKILL only as escalation."""
        if self._proc is not None and self._proc.poll() is None:
            if self._send("quit()"):
                try:
                    self._proc.wait(5)
                except subprocess.TimeoutExpired:
                    pass
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


def cmd_retry(link, cmd, timeout=None):
    """One retry on no-response, for the documented settling quirk: the
    first command after a reboot can time out once (graph N22). Retrying
    on -2 only keeps real ERROR results exact."""
    res, lines = link.command(cmd, timeout=timeout)
    if res == -2:
        res, lines = link.command(cmd, timeout=timeout)
    return res, lines


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


def make_relink(link, port_path, settle=1.0, pump=2.5, deadline_s=30.0,
                path_exists=None, sleep=None, serial_mod=None):
    """Close the port, run action() while it is closed, wait for the
    device path to come back (SWD reset and power cycles re-enumerate
    USB), reopen and swap the transport in place. Use the by-id path as
    --port or the reopen may chase a renumbered ttyACM.

    Two hardenings from the first T3 hardware run: USB CDC enumeration
    can bounce after a reset (the node appears, the first open works,
    then the node drops and returns while the device reconfigures), so
    the node must survive a settle window before the open is trusted,
    and the fresh port is pumped briefly afterwards so a bounce that
    kills the first read is caught HERE (close the corpse, retry)
    instead of surfacing as a bogus link loss inside a step. Pumping
    routes lines through the normal URC path, so a +MTREADY arriving
    early is queued for the step's await, not lost."""
    if serial_mod is None:
        import serial as serial_mod
    path_exists = path_exists or os.path.exists
    sleep = sleep or time.sleep

    def relink(action):
        try:
            link.t.close()
        except Exception:
            pass
        ok, detail = action()
        if not ok:
            return False, detail
        deadline = time.monotonic() + deadline_s
        while time.monotonic() < deadline:
            if not path_exists(port_path):
                sleep(0.2)
                continue
            sleep(settle)
            if not path_exists(port_path):
                continue
            try:
                cand = serial_mod.Serial(port_path, 115200, timeout=0.05)
            except (serial_mod.SerialException, OSError):
                sleep(0.2)
                continue
            link.t = cand
            link._buf = b""
            try:
                pump_deadline = time.monotonic() + pump
                while time.monotonic() < pump_deadline:
                    link._pump_one(pump_deadline)
                return True, ""
            except (serial_mod.SerialException, OSError):
                try:
                    cand.close()
                except Exception:
                    pass
        return False, "port did not come back: %s" % port_path
    return relink


def flush_print(*args):
    """print with an unconditional flush: operator prompts must reach a
    tee'd log the moment they happen. Python block-buffers stdout when
    piped, and an unflushed prompt cost a whole hardware run (the
    operator was never told to unplug)."""
    print(*args, flush=True)


def operator_power_cycle(port_path, printer=flush_print,
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


GATE_REFERENCE_QR = "MT:Y.K9042C00KA0648G00"


def phase2_gate(chip, args):
    """Phase-2-only preflight, before anything destructive: chip-tool
    exists, runs as this user (the 2026-07-29 root-owned counters file
    made every non-root run fail), and credentials are present. Returns
    an abort message or None."""
    if shutil.which("openocd") is None:
        return ("openocd not on PATH: the 2.8 warm reboot resets the "
                "RP2350 over SWD (see graph N22)")
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
    production units do not fail spuriously (TESTING.md 6.1). Tolerates
    a dead or deliberately closed link (an abort whose relink failed
    leaves the port closed): the header just stays incomplete, instead
    of a misleading link-lost tail after the summary was already
    decided."""
    try:
        return _capture_header_live(link, header)
    except OSError:
        return None


def _capture_header_live(link, header):
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
        "results": dict(
            [(name, "PASS" if ok else "FAIL")
             for name, ok, _ in suite.results]
            + [(name, "SKIP") for name, _ in suite.skipped]),
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
    n("MTCOMMISSION=179 -> +MTERR:1", commission_rejected_stateless("179"))
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=os.environ.get("MT_PORT", "/dev/ttyACM0"))
    ap.add_argument("--phase", type=int, choices=[0, 1, 2], default=None,
                    help="run only this phase (0 runs just the preflight "
                         "gate; 2 is stateful and never runs by default)")
    ap.add_argument("-k", dest="keyword", default=None,
                    help="run only tests whose name contains this substring")
    ap.add_argument("--baseline", default=None,
                    help="write a JSON baseline of this run")
    ap.add_argument("--include-slow", action="store_true",
                    help="include the ~200 s window-expiry test (2.10)")
    ap.add_argument("--include-manual", action="store_true",
                    help="include tests needing an operator at the bench "
                         "(2.9 cold boot)")
    ap.add_argument("--chip-tool",
                    default=os.environ.get("MT_CHIPTOOL", DEFAULT_CHIPTOOL))
    ap.add_argument("--storage",
                    default=os.environ.get("MT_CHIPTOOL_STORAGE",
                                           "/tmp/mt-regression"))
    ap.add_argument("--ssid", default=os.environ.get("MT_SSID"))
    ap.add_argument("--psk", default=os.environ.get("MT_PSK"))
    ap.add_argument("--node-id", type=lambda x: int(x, 0), default=0x4845,
                    help="node id chip-tool assigns at pairing")
    args = ap.parse_args(argv)
    if args.baseline and args.phase == 2 and not (
            args.include_slow and args.include_manual):
        ap.error("--baseline with --phase 2 requires --include-slow and "
                 "--include-manual: a committed baseline must not contain "
                 "gated-out entries")

    chip = None
    if args.phase == 2:
        chip = ChipTool(args.chip_tool, args.storage)
        problem = phase2_gate(chip, args)
        if problem:
            print("ABORT: " + problem)
            return 2

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
        "ssid": args.ssid,
    }
    suite = Suite()
    truncated = False
    try:
        problem = phase0(link, header)
        if problem:
            print("ABORT: " + problem)
            return 2
        if args.phase == 2:
            header["node_id"] = "0x%X" % args.node_id
            chip.wipe_storage()
            ctx = Phase2Context(link, chip, suite, args)
            ctx.relink = make_relink(link, args.port)
            ctx.chip2 = ChipTool(args.chip_tool, args.storage + "-f2")
            ctx.chip2.wipe_storage()
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
    except KeyboardInterrupt:
        print("\n(interrupted)")
        truncated = True
    except (serial.SerialException, OSError) as exc:
        # Narrow deliberately (T2 design section 5): a harness bug must
        # raise a traceback, not masquerade as a lost link.
        print("(link lost: %s)" % exc)
        truncated = True
    finally:
        try:
            link.t.close()
        except Exception:
            pass
    suite.summary()
    if args.baseline:
        write_baseline(args.baseline, header, suite)
    return 1 if (suite.failed or suite.skipped or truncated) else 0


if __name__ == "__main__":
    sys.exit(main())
