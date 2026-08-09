#!/usr/bin/env python3
"""iLabs AT Hearth regression harness, stages T1-T5: Phases 0, 1, 2 and 3.

Test inventory: docs/TESTING.md sections 5, 6 and 7 (2.1-2.5).
Design decisions: docs/superpowers/specs/2026-07-30-c5-regression-harness-t1-design.md,
.../2026-07-30-c5-regression-harness-t2-design.md and
.../2026-08-08-c5-regression-harness-t5-design.md (Phase 3).

Phase 1 run: python3 test/mt_regression.py --port /dev/ttyACM0
Phase 2 run: MT_SSID=... MT_PSK=... python3 test/mt_regression.py \
    --port /dev/ttyACM0 --phase 2
Phase 3 host-only segment: python3 test/mt_regression.py --port /dev/ttyACM0 \
    --phase 3
(only the host-only steps are registered so far; -k "3." will keep
scoping to them once Task 5 appends the controller segment, since -k
matches a literal substring, not a regex, and every step name starts
with its own "3.N ")

Standing rule (T1 design section 8, N23): never have an AT+MTATTR command
in flight while a controller-driven +MTATTR URC is expected; ATLink would
absorb the URC into the command's response lines. Phase 2 and Phase 3
steps sequence around this by construction.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
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


class CmdResponder:
    """Answers a forwarded +MTCMD URC the way a host MCU would.

    Wire form (spec 3.17): +MTCMD:<seq>,<ep>,<cluster>,<command>[,<payload>],
    all decimal; the fifth field is the reserved payload slot (chime sends
    its chimeID there).

    seq 0 is notify-only and is NEVER answered: the firmware rejects
    AT+MTCMDRESP=0,... with +MTERR:1 by design, so answering it would be a
    harness bug, not a legitimate response. Both public methods route
    through _match(), and only expect()'s non-zero branch ever calls
    self.link.command(); there is no code path in this class that can
    send AT+MTCMDRESP for seq 0.

    _match() encodes cluster and command into the await_urc_ts() pattern
    itself, not just "+MTCMD:", so a forward for some OTHER cluster or
    command is never popped off ATLink.urcs by a call that is not asking
    for it: it stays queued for whoever calls expect()/expect_notify()
    for it later. Every other await_urc_ts call site in this module
    follows the same rule (the regex carries the full match criteria) for
    the same reason: ATLink.urcs is a live queue with no way back once an
    entry is popped and discarded (urc_history is a record, not a
    replayable queue), so a broad-match-then-filter-in-Python caller would
    silently and irrecoverably drop any forward it was not the intended
    recipient of."""

    _RX = re.compile(r"\+MTCMD:(\d+),(\d+),(\d+),(\d+)(?:,(\d+))?$")

    def __init__(self, link):
        self.link = link

    def _match(self, cluster, command, payload, timeout):
        """Await the +MTCMD forward for exactly this cluster and command,
        and parse it, or None on timeout or a payload mismatch.

        cluster and command are baked into the await pattern (anchored at
        the start of the line, immediately after the seq and ep fields),
        so a queued or arriving forward for a different cluster/command
        is left untouched on ATLink.urcs rather than being popped and
        discarded here: see the class docstring. payload stays a
        post-parse filter, deliberately: a payload mismatch on the RIGHT
        cluster/command is a genuine test failure to report as None, not
        someone else's forward to leave alone.

        await_urc_ts returns (timestamp, line); only the line matters
        here."""
        pattern = r"^\+MTCMD:\d+,\d+,%d,%d(,|$)" % (cluster, command)
        got = self.link.await_urc_ts(pattern, timeout=timeout)
        if got is None:
            return None
        m = self._RX.match(got[1])
        if not m:
            return None
        fwd = {"seq": int(m.group(1)), "ep": int(m.group(2)),
               "cluster": int(m.group(3)), "command": int(m.group(4)),
               "payload": int(m.group(5)) if m.group(5) else None}
        if payload is not None and fwd["payload"] != payload:
            return None
        return fwd

    def expect(self, cluster, command, verdict, payload=None, timeout=5.0):
        """Await the matching forward and answer it with verdict, unless
        seq is 0: that branch returns without ever touching self.link,
        so a notify can never be answered through this method either."""
        fwd = self._match(cluster, command, payload, timeout)
        if fwd is None:
            return None
        if fwd["seq"] == 0:
            return fwd
        res, _ = self.link.command(
            "AT+MTCMDRESP=%d,%d" % (fwd["seq"], verdict))
        if res != 0:
            return None
        return fwd

    def expect_notify(self, cluster, command, payload=None, timeout=5.0):
        """Await the matching forward and assert it is notify-only (seq
        0). Never sends AT+MTCMDRESP, adjudicated or not."""
        fwd = self._match(cluster, command, payload, timeout)
        if fwd is None or fwd["seq"] != 0:
            return None
        return fwd


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
        intent, not truncation. run_phase2 passes "k=<kw>" for the -k
        case (there is no --k flag to spell), so that one is rendered as
        the real "-k <kw>" invocation instead of falling through the
        --flag.replace() path meant for the include-* flags."""
        self.gated.append((name, flag))
        if flag.startswith("k="):
            label = "-k %s" % flag[2:]
        else:
            label = "--%s" % flag.replace("_", "-")
        print("  [SKIP] %s (gated: %s)" % (name, label))

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
        self.transport = "WIFI"   # set from phase2_gate's detection
        self.dataset = None       # Thread active dataset hex, if any


# Slot order is load-bearing: step_3_2_grammar and friends reference
# endpoints by slot. Ids verified against AT_MT_SPEC.md's device table
# (T5 design spec section 4.2's spec rule); every id below is quoted
# from that table in the task-4 report.
#
# Slot 11 is NOT in the design spec's 10-row table. It is added here,
# deliberately, to make the MTTEMPLEVELS "+MTERR:4" row (deferred by
# Task 1: "ep 4 built as the wrong variant") testable at all: slot 10
# must be the TemperatureLevel variant (AT+MTEP=0x0071,1) so the
# MTTEMPLEVELS OK/comma storage rows have a real Levels cabinet to write
# to, and the two variants are mutually exclusive on one endpoint (spec
# 3.9), so +MTERR:4 needs a second, TemperatureNumber-variant (plain
# 0x0071, variant 0) cabinet that no other slot can double as. This is
# flagged in the task-4 report for review rather than silently assumed.
PHASE3_COMPOSITION = [
    (1, "0x0100"),      # on/off light, regression anchor
    (2, "0x0042"),      # water valve
    (3, "0x0027"),      # mode select
    (4, "0x0146"),      # chime
    (5, "0x0076"),      # smoke/CO alarm
    (6, "0x0011"),      # power source
    (7, "0x0073"),      # laundry washer (opstate trio representative)
    (8, "0x000A"),      # door lock
    (9, "0x000F"),      # generic switch
    (10, "0x0071,1"),   # temp-levels cabinet, TemperatureLevel variant
    (11, "0x0071"),     # temp-number cabinet, variant 0 (MTTEMPLEVELS
                         # +MTERR:4 target only; not in the design spec's
                         # table, see the comment above)
]


class Phase3Context:
    """Shared state the ordered Phase 3 steps hand each other, the same
    role Phase2Context plays for Phase 2. .composition starts as the
    ordered (slot, devtype_hex) list the run intends to declare
    (PHASE3_COMPOSITION); step_3_1_compose does not change its shape,
    since every later step indexes it by slot, not by re-parsing a
    +MTEP? capture the way Phase 2's cleanup step does."""

    def __init__(self, link, chip, suite, opts):
        self.link = link
        self.chip = chip
        self.suite = suite
        self.opts = opts
        self.chip2 = None          # unused by the host-only segment;
                                    # recover_after_abort tolerates None
        self.node_id = getattr(opts, "node_id", 0x4845)
        self.composition = list(PHASE3_COMPOSITION)
        self.transport = "WIFI"    # set from phase3_gate's detection
        self.dataset = None        # Thread active dataset hex, if any
        self.qr = None             # captured by step_3_5_commission
        self.manual = None
        self.passcode = None
        self.discriminator = None
        self.chip_call = None      # test seam; None means the real
                                    # threaded chip.run() (see
                                    # invoke_chip/_threaded_chip_call)


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



def _pairing_tail(out, psk=None, lines=5):
    """A bounded tail of chip-tool output for a failed pairing, so the
    log carries the controller's own error text instead of only an exit
    code (a real flake cost a diagnosis round for want of this). The
    PSK is scrubbed defensively; chip-tool does not normally echo argv,
    but a committed log must not depend on that."""
    tail = " | ".join((out or "").splitlines()[-lines:])
    if psk:
        tail = tail.replace(psk, "***")
    return tail


def pairing_argv(ctx):
    """chip-tool pairing argv for the detected transport. ble-thread's
    shape was pinned by the T4 Task 1 preflight; adjust here if the
    findings differ, not at the call sites."""
    node = "0x%X" % ctx.node_id
    if ctx.transport == "THREAD":
        return ["pairing", "ble-thread", node, "hex:" + ctx.dataset,
                str(ctx.passcode), str(ctx.discriminator)]
    return ["pairing", "ble-wifi", node, ctx.opts.ssid, ctx.opts.psk,
            str(ctx.passcode), str(ctx.discriminator)]


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
    rc, out = chip.run(pairing_argv(ctx), timeout=120)
    paired = s.check("2.3 chip-tool pairing exits 0", rc == 0, tag="P2")
    if not paired:
        print("    (chip-tool tail: %s)"
              % _pairing_tail(out, getattr(ctx.opts, "psk", None)))
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
        s.check("2.5 AT read agrees after %s (%d)" % (verb, want),
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
    if not paired:
        print("    (chip-tool tail: %s)"
              % _pairing_tail(out, getattr(ctx.opts, "psk", None)))
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
    # B63 regression guard: the value survives the reboot since commit
    # 8100af4 (StartUpOnOff null instead of esp-matter's boot-Off
    # default). Before that fix every boot forcibly persisted a 0 over
    # the healthy restore, and this check pinned that behavior.
    s.check("2.8 attribute value survived (B63 guard)",
            res == 0 and lines == ["+MTATTR:1,6,0,1"], tag="P2")
    res, lines = link.command("AT+MTSTATE?")
    s.check("2.8 state 2 (operational)",
            res == 0 and lines == ["+MTSTATE:2,1"], tag="P2")
    s.check("2.8 no boot window on commissioned device",
            link.assert_no_urc(r"\+MTEVT:0$", 5.0), tag="P2")


def step_2_9_cold_boot(ctx):
    """TESTING.md 2.9, the B4.3 boot-loop regression territory: a URC
    fired during esp_matter::start(), before the AT UART exists, once
    boot-looped the device forever. Since the B63 fix (8100af4,
    StartUpOnOff null) no state changes at init, so that path only arms
    if a controller configures StartUpOnOff to change state at boot;
    what this step proves is the commissioned device coming cleanly
    through a true power cycle to +MTREADY with its fabric AND its
    OnOff value intact (the cold-boot half of the B63 guard, alongside
    2.8's warm half)."""
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
    s.check("2.9 value survived cold boot (B63 guard)",
            res == 0 and lines == ["+MTATTR:1,6,0,1"], tag="P2")


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
    rc, out = chip.run(pairing_argv(ctx), timeout=120)
    paired = s.check("2.11 re-commission exits 0", rc == 0, tag="P2")
    if not paired:
        print("    (chip-tool tail: %s)"
              % _pairing_tail(out, getattr(ctx.opts, "psk", None)))
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


def stage_composition(link, devtypes):
    """Stage a composition via the spec 3.9 grammar: AT+MTEPCLEAR opens
    an empty staging session, then one AT+MTEP=<devtype> per entry, in
    order. Returns True iff every command in the sequence answered OK.
    Does not send AT+MTEPAPPLY: the two existing callers (2.12's rig
    restore and, since T5, Phase 3's compose/restore steps) want
    different framing around the apply-and-reboot that follows, so that
    stays the caller's job.

    Extracted from 2.12's inline MTEPCLEAR/MTEP loop (T5 task 4) so Phase
    3 reuses it instead of reimplementing the same grammar; 2.12 itself
    now calls this too, so the two staging call sites cannot drift."""
    ok = link.command("AT+MTEPCLEAR")[0] == 0
    for dt in devtypes:
        ok = link.command("AT+MTEP=%s" % dt)[0] == 0 and ok
    return ok


def step_2_12_rig_restore(ctx):
    """TESTING.md 2.12 plus the T3 cleanup: after the 2.11 MTFRESET the
    old node must be gone and the device commissionable again, and the
    run must hand the next one the documented bench convention:
    factory-fresh WITH the composition declared. The replay uses the
    spec 3.9 staging grammar (stage_composition); MTEPAPPLY persists and
    reboots on its own."""
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
    ok = stage_composition(link, devtypes)
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


def step_3_1_compose(ctx):
    """T5 design spec section 4.2 step 2: declare PHASE3_COMPOSITION and
    pin the boot-rebuild trio CLAUDE.md's "Things that will bite you"
    warns about (endpoint composition must be built before
    esp_matter::start() for ids to be reproducible; a failed
    endpoint::create() aborts the whole composition rather than skipping
    an entry; CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT must cover
    every declared slot). AT+MTFRESET clears any prior state, the spec
    3.9 staging grammar (stage_composition) declares the composition and
    AT+MTEPAPPLY persists + reboots it, and a SECOND, plain AT+MTRESET
    (no composition change) proves the ids survive a reboot that is not
    itself the apply. AT+MTEP? must then read back every slot, in order,
    with exactly the declared device type (and variant suffix, where
    nonzero)."""
    link, s = ctx.link, ctx.suite
    res, _ = cmd_retry(link, "AT+MTFRESET", timeout=5.0)
    if not s.check("3.1 MTFRESET -> OK", res == 0, tag="P3"):
        raise StepAbort("AT+MTFRESET failed")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("3.1 +MTREADY after MTFRESET", ready is not None,
                   tag="P3"):
        raise StepAbort("device did not come back from AT+MTFRESET")
    devtypes = [dt for _slot, dt in PHASE3_COMPOSITION]
    staged = stage_composition(link, devtypes)
    if not s.check("3.1 composition staged", staged, tag="P3"):
        raise StepAbort("staging the Phase 3 composition failed")
    link.drain(0.3)
    res, _ = link.command("AT+MTEPAPPLY", timeout=5.0)
    if not s.check("3.1 MTEPAPPLY -> OK", res == 0, tag="P3"):
        raise StepAbort("AT+MTEPAPPLY failed")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("3.1 +MTREADY after MTEPAPPLY", ready is not None,
                   tag="P3"):
        raise StepAbort("device did not come back from AT+MTEPAPPLY")
    res, _ = cmd_retry(link, "AT+MTRESET", timeout=5.0)
    if not s.check("3.1 second MTRESET -> OK", res == 0, tag="P3"):
        raise StepAbort("AT+MTRESET failed")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("3.1 +MTREADY after second MTRESET", ready is not None,
                   tag="P3"):
        raise StepAbort("device did not come back from the second "
                        "AT+MTRESET")
    res, lines = cmd_retry(link, "AT+MTEP?")
    expected = ["+MTEP:%d,%d,%s" % (i, slot, dt)
               for i, (slot, dt) in enumerate(PHASE3_COMPOSITION)]
    if not s.check("3.1 composition readback exact",
                   res == 0 and lines == expected, tag="P3"):
        raise StepAbort("Phase 3 composition did not read back as declared")


def step_3_2_grammar(ctx):
    """T5 design spec section 3/4.2 step 3: the endpoint-dependent
    grammar rows Task 1 deferred (register_phase1_t5_negative's comments
    name each one), now run against PHASE3_COMPOSITION's real endpoints.
    One check per TESTING.md/AT_MT_SPEC.md row: 40 of Task 1's 41 named
    rows land here. The 41st, AT+MTCMDRESP against an already-answered or
    expired seq, needs a real forwarded command, which needs a
    controller (CmdResponder), so it stays out of this host-only segment
    and is Task 5's job.

    MTALARM's SmokeState-Warning row is followed by an unscored clear so
    ep5 hands step_3_3 a clean ExpressedState, the same
    each-step-establishes-the-next-step's-preconditions discipline Phase
    2 follows."""
    link, s = ctx.link, ctx.suite

    def c(name, fn):
        s.check("3.2 %s" % name, fn(link), tag="P3")

    def ok(cmd, **kw):
        return expect_ok(cmd, **kw)

    def err(cmd, code, **kw):
        return expect_err(cmd, code, **kw)

    # MTSWITCH (TESTING.md 6.2): ep9 is the composition's generic
    # switch, ep0 the root endpoint used for the "wrong cluster" row.
    c("MTSWITCH=99 -> +MTERR:2 (unknown ep)", err("AT+MTSWITCH=99", 2))
    c("MTSWITCH=0 -> +MTERR:3 (root ep has no Switch cluster)",
      err("AT+MTSWITCH=0", 3))
    c("MTSWITCH=<switch ep> -> OK", ok("AT+MTSWITCH=9"))

    # MTLOCK (TESTING.md 6.2): ep8 is the door lock; ep1 (light) stands
    # in for "any endpoint without a DoorLock cluster", a role it plays
    # for several families below.
    c("MTLOCK=99,1 -> +MTERR:2 (unknown ep)", err("AT+MTLOCK=99,1", 2))
    c("MTLOCK on non-door-lock ep -> +MTERR:3", err("AT+MTLOCK=1,1", 3))
    c("MTLOCK=<lock ep>,1 -> OK", ok("AT+MTLOCK=8,1"))

    # MTVALVE (TESTING.md 6.2): ep2 is the water valve.
    c("MTVALVE=99,1 -> +MTERR:2 (unknown ep)", err("AT+MTVALVE=99,1", 2))
    c("MTVALVE on non-valve ep -> +MTERR:3", err("AT+MTVALVE=1,1", 3))
    c("MTVALVE=<valve ep>,1 -> OK", ok("AT+MTVALVE=2,1"))
    c("MTVALVE=<valve ep>,1,50 -> OK (state and level)",
      ok("AT+MTVALVE=2,1,50"))

    # MTMODES (TESTING.md 6.2): ep3 is mode select.
    c('MTMODES=99,0,"Quiet" -> +MTERR:2 (unknown ep)',
      err('AT+MTMODES=99,0,"Quiet"', 2))
    c("MTMODES on non-mode-select ep -> +MTERR:3",
      err('AT+MTMODES=1,0,"Quiet"', 3))
    c('MTMODES=<mode-select ep>,0,"Quiet" -> OK',
      ok('AT+MTMODES=3,0,"Quiet"'))
    c("MTMODES comma inside a label -> OK",
      ok('AT+MTMODES=3,0,"Eco, low"'))
    c("MTATTR CurrentMode round trip on mode-select ep",
      ok("AT+MTATTR=3,80,3", line_re=r"\+MTATTR:3,80,3,\d+"))

    # MTOPSTATE (TESTING.md 6.2): ep7 is the laundry washer (opstate
    # trio representative).
    c("MTOPSTATE=99,1 -> +MTERR:2 (unknown ep)",
      err("AT+MTOPSTATE=99,1", 2))
    c("MTOPSTATE on non-opstate ep -> +MTERR:3",
      err("AT+MTOPSTATE=1,1", 3))
    c("MTOPSTATE=<washer ep>,1 -> OK", ok("AT+MTOPSTATE=7,1"))

    # MTALARM (TESTING.md 6.2): ep5 is the smoke/CO alarm. The four
    # per-field enum-range rows carry +MTERR:1 but need the real
    # SmokeCoAlarm instance (cmd_mtalarm's own comment: validated inside
    # mt_matter_alarm_set(), not the handler), so they are
    # composition-dependent despite the code.
    c("MTALARM=<alarm ep>,1,3 -> +MTERR:1 (SmokeState range 0..2)",
      err("AT+MTALARM=5,1,3", 1))
    c("MTALARM=<alarm ep>,4,2 -> +MTERR:1 (DeviceMuted range 0..1)",
      err("AT+MTALARM=5,4,2", 1))
    c("MTALARM=<alarm ep>,5,2 -> +MTERR:1 (TestInProgress is boolean)",
      err("AT+MTALARM=5,5,2", 1))
    c("MTALARM=<alarm ep>,10,4 -> +MTERR:1 (ContaminationState range 0..3)",
      err("AT+MTALARM=5,10,4", 1))
    c("MTALARM=99,1,1 -> +MTERR:2 (unknown ep)",
      err("AT+MTALARM=99,1,1", 2))
    c("MTALARM on non-alarm ep -> +MTERR:3", err("AT+MTALARM=1,1,1", 3))
    c("MTALARM=<alarm ep>,1,1 -> OK (SmokeState Warning)",
      ok("AT+MTALARM=5,1,1"))
    link.command("AT+MTALARM=5,1,0")  # unscored: clear before step_3_3
    c("MTALARM=<alarm ep>,5,0 -> OK (TestInProgress false)",
      ok("AT+MTALARM=5,5,0"))

    # MTCHIMESOUNDS (TESTING.md 6.2): ep4 is the chime; ep1 (light)
    # again stands in for "any endpoint without a Chime cluster".
    c('MTCHIMESOUNDS=99,1,"Doorbell" -> +MTERR:2 (unknown ep)',
      err('AT+MTCHIMESOUNDS=99,1,"Doorbell"', 2))
    c("MTCHIMESOUNDS on non-chime ep -> +MTERR:3",
      err('AT+MTCHIMESOUNDS=1,1,"Doorbell"', 3))
    c('MTCHIMESOUNDS=<chime ep>,1,"Doorbell" -> OK',
      ok('AT+MTCHIMESOUNDS=4,1,"Doorbell"'))
    c("MTCHIMESOUNDS comma inside a name -> OK",
      ok('AT+MTCHIMESOUNDS=4,1,"Doorbell",2,"Alert, urgent"'))

    # MTCHIME (TESTING.md 6.2): same ep4/ep1 as MTCHIMESOUNDS. Id 9 is
    # not among the sounds MTCHIMESOUNDS just installed (1, 2).
    c("MTCHIME=<chime ep>,0,9 -> +MTERR:1 (chime id 9 not installed)",
      err("AT+MTCHIME=4,0,9", 1))
    c("MTCHIME=99,0,1 -> +MTERR:2 (unknown ep)",
      err("AT+MTCHIME=99,0,1", 2))
    c("MTCHIME on non-chime ep -> +MTERR:3", err("AT+MTCHIME=1,0,1", 3))
    c("MTCHIME=<chime ep>,0,1 -> OK (SelectedChime = 1)",
      ok("AT+MTCHIME=4,0,1"))
    c("MTCHIME=<chime ep>,1,1 -> OK (Enabled = true)",
      ok("AT+MTCHIME=4,1,1"))

    # MTTEMPLEVELS (AT_MT_SPEC.md 3.16; TESTING.md 6.2 has its own table
    # now, around line 367): ep10 is the TemperatureLevel
    # variant, ep11 the TemperatureNumber variant (the +MTERR:4 target,
    # see the PHASE3_COMPOSITION comment), ep1 the "no TemperatureControl
    # cluster" stand-in, ep99 always unknown.
    c('MTTEMPLEVELS=99,"Low" -> +MTERR:2 (unknown ep)',
      err('AT+MTTEMPLEVELS=99,"Low"', 2))
    c("MTTEMPLEVELS on non-templevels ep -> +MTERR:3",
      err('AT+MTTEMPLEVELS=1,"Low"', 3))
    c("MTTEMPLEVELS on TemperatureNumber-variant ep -> +MTERR:4",
      err('AT+MTTEMPLEVELS=11,"Low"', 4))
    c('MTTEMPLEVELS=<levels ep>,"Low","Medium","High" -> OK',
      ok('AT+MTTEMPLEVELS=10,"Low","Medium","High"'))
    c("MTTEMPLEVELS comma inside a label -> OK",
      ok('AT+MTTEMPLEVELS=10,"Wine, red","Wine, white"'))


def step_3_3_selftest_wedge(ctx):
    """T5 design spec section 4.2 step 4: bug B165's host-only
    reproduction, lifted verbatim from the C10 bench report's C12-4b
    (.superpowers/sdd/2026-08-07-seven-type-batch/task-C10-report.md,
    section "C12-4b: the exact old wedge shape, host-only, then a
    reboot"), endpoint substituted 1 -> 5 for PHASE3_COMPOSITION's
    smoke/CO alarm slot. No fabric, no controller: TestInProgress true
    then false completes a self test purely over AT+MTATTR/AT+MTALARM,
    and ExpressedState must read Normal (0) both immediately and after a
    reboot, the direct regression case for the SetExpressedStateByPriority
    recompute (fix 9c8af07, AT_MT_SPEC.md 3.22's DEFECT note). Each
    AT+MTALARM call passes expect="+MTATTR:" so the attribute-changed
    lines it raises land in that command's own response, matching how
    the C10 evidence transcript captured them, rather than being queued
    as unsolicited URCs.

    A second scenario, not from the transcript but the same field, pins
    SmokeState (field 1) driving ExpressedState the same way
    TestInProgress does (AT_MT_SPEC.md 3.22: ExpressedState 1 is
    kSmokeAlarm)."""
    link, s = ctx.link, ctx.suite
    link.drain(0.3)

    def c(name, passed):
        return s.check("3.3 %s" % name, passed, tag="P3")

    res, lines = link.command("AT+MTATTR=5,92,0")
    c("clean start: ExpressedState 0",
      res == 0 and lines == ["+MTATTR:5,92,0,0"])
    res, lines = link.command("AT+MTALARM=5,5,1", expect="+MTATTR:")
    c("TestInProgress true -> OK, ExpressedState Testing",
      res == 0 and lines == ["+MTATTR:5,92,5,1", "+MTATTR:5,92,0,4"])
    res, lines = link.command("AT+MTATTR=5,92,0")
    c("mid-test: ExpressedState 4 (Testing)",
      res == 0 and lines == ["+MTATTR:5,92,0,4"])
    res, lines = link.command("AT+MTALARM=5,5,0", expect="+MTATTR:")
    c("TestInProgress false -> OK, ExpressedState Normal (the B165 fix)",
      res == 0 and lines == ["+MTATTR:5,92,5,0", "+MTATTR:5,92,0,0"])
    res, lines = link.command("AT+MTATTR=5,92,0")
    c("post-test: ExpressedState back to 0",
      res == 0 and lines == ["+MTATTR:5,92,0,0"])
    res, _ = cmd_retry(link, "AT+MTRESET", timeout=5.0)
    if not c("reboot -> OK", res == 0):
        raise StepAbort("AT+MTRESET failed mid wedge-reproduction")
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not c("+MTREADY after reboot", ready is not None):
        raise StepAbort("device did not come back from AT+MTRESET")
    res, lines = link.command("AT+MTATTR=5,92,0")
    c("after reboot: ExpressedState did not resurrect to Testing",
      res == 0 and lines == ["+MTATTR:5,92,0,0"])
    res, lines = link.command("AT+MTATTR=5,92,5")
    c("after reboot: TestInProgress false",
      res == 0 and lines == ["+MTATTR:5,92,5,0"])

    # SmokeState scenario (brief for this task, not from the transcript).
    res, _ = link.command("AT+MTALARM=5,1,1")
    c("SmokeState Warning -> OK", res == 0)
    res, lines = link.command("AT+MTATTR=5,92,0")
    c("SmokeState Warning -> ExpressedState 1 (SmokeAlarm)",
      res == 0 and lines == ["+MTATTR:5,92,0,1"])
    res, _ = link.command("AT+MTALARM=5,1,0")
    c("SmokeState cleared -> OK", res == 0)
    res, lines = link.command("AT+MTATTR=5,92,0")
    c("SmokeState cleared -> ExpressedState back to 0",
      res == 0 and lines == ["+MTATTR:5,92,0,0"])


def step_3_4_stores(ctx):
    """T5 design spec section 4.2 step 5: MTMODES and MTCHIMESOUNDS store
    edges re-pinned against their real endpoints (ep3, ep4), closing the
    loop on Phase 1's order-independence claim for these +MTERR:1 rows
    (register_phase1_t5_negative's docstring: verified against
    mt_at.c's handlers, not just asserted) now that real mode-select and
    chime endpoints exist to run them against."""
    link, s = ctx.link, ctx.suite

    def c(name, fn):
        s.check("3.4 %s" % name, fn(link), tag="P3")

    def ok(cmd, **kw):
        return expect_ok(cmd, **kw)

    def err(cmd, code, **kw):
        return expect_err(cmd, code, **kw)

    c("MTMODES comma inside a label -> OK (ep3)",
      ok('AT+MTMODES=3,0,"Boost, high"'))
    c("MTMODES duplicate id -> +MTERR:1 (ep3)",
      err('AT+MTMODES=3,0,"A",0,"B"', 1))
    c("MTMODES=3 -> +MTERR:1 (0 pairs, ep3)", err("AT+MTMODES=3", 1))
    c("MTMODES 9 pairs -> +MTERR:1 (over the 1..8 limit, ep3)",
      err('AT+MTMODES=3,0,"A",1,"B",2,"C",3,"D",4,"E",5,"F",6,"G",7,"H",8,"I"',
          1))
    c("MTCHIMESOUNDS comma inside a name -> OK (ep4)",
      ok('AT+MTCHIMESOUNDS=4,1,"Alarm, loud"'))
    c("MTCHIMESOUNDS duplicate id -> +MTERR:1 (ep4)",
      err('AT+MTCHIMESOUNDS=4,1,"Doorbell",1,"Chime"', 1))
    c("MTCHIMESOUNDS=4 -> +MTERR:1 (0 pairs, ep4)",
      err("AT+MTCHIMESOUNDS=4", 1))


class _ChipCallHandle:
    """Returned by invoke_chip(): join() blocks for the background
    chip-tool call to finish and returns (rc, out), the same shape
    ChipTool.run() itself returns."""

    def __init__(self, thread, result):
        self._thread = thread
        self._result = result

    def join(self, timeout=None):
        self._thread.join(timeout)
        return self._result.get("rc"), self._result.get("out")


def _threaded_chip_call(chip, argv, timeout=60):
    """Production chip_call: run chip.run(argv, timeout) on a background
    thread and return immediately with a handle to join later.

    Why this exists (spec 3.17, the concurrency requirement the T5
    design spec's section 4.3 prose does not spell out; see the task-5
    report): an adjudicated +MTCMD forward (valve open/close, chime
    PlayChimeSound, the OperationalState quartet, the door lock) blocks
    the CHIP event-loop task, and therefore chip-tool's own wait for the
    InvokeCommandResponse, for up to 1000 ms while the firmware waits on
    AT+MTCMDRESP. ChipTool.run() is synchronous (subprocess.run());
    calling it directly would leave nothing pumping the AT link for that
    whole window, so the harness could never answer in time. Running it
    on a background thread lets the same Python process do both at once:
    this thread blocks in the subprocess call while the thread that
    started it is free to call CmdResponder.expect()/expect_notify() on
    ctx.link. Only one thread ever touches ctx.link (the caller's); this
    thread touches only `chip` and its own local result dict, so there
    is no data race on ATLink's internals.

    Not needed for every forward: the smoke self-test (spec 3.22) is
    notify-only and the SDK answers the controller before the ember
    callback that raises +MTCMD ever runs, so step_3_10_smoke calls
    chip.run() directly instead of going through this. Same for the
    OperationalState in-state guard (runs inside the SDK before the
    delegate, and therefore mt_cmd_forward, is ever reached): nothing
    blocks waiting on a verdict that is never asked for."""
    result = {}

    def _run():
        result["rc"], result["out"] = chip.run(argv, timeout)

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return _ChipCallHandle(t, result)


def invoke_chip(ctx, argv, timeout=60):
    """Start a chip-tool invocation the way every controller-adjudicated
    Phase 3 step needs it (see _threaded_chip_call's docstring): through
    ctx.chip_call when the test seam is set (a synchronous double, so
    self-tests can assert exact ordering without racing a real thread
    against FakeLink's non-blocking, single-pass await_urc_ts), else the
    real threaded call. Returns a handle; call .join(timeout) after
    adjudicating the forward on ctx.link to get (rc, out)."""
    call = ctx.chip_call or _threaded_chip_call
    return call(ctx.chip, argv, timeout)


def step_3_5_commission(ctx):
    """T5 design spec section 4.3: commission the Phase 3 composition.
    Same shape as step_2_3_commission (Phase 2's commissioning proof),
    scoped to what this segment actually needs before the matrix runs:
    chip-tool exits 0, +MTEVT:3 lands, and exactly one fabric exists
    afterward. The DE24 window-close ordering pair (+MTEVT:4 following
    +MTEVT:3, timestamps compared) is Phase 2's own regression pin
    already (step_2_3_commission); Phase 3 does not re-prove it, since
    nothing about DE24 is composition-specific."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    res, lines = link.command("AT+MTCODES?")
    if res == 0 and lines:
        m = re.fullmatch(r"\+MTCODES:(.+),(\d{11})", lines[0])
        if m:
            ctx.qr, ctx.manual = m.group(1), m.group(2)
    if not s.check("3.5 codes captured", ctx.qr is not None, tag="P3"):
        raise StepAbort("cannot capture onboarding codes")
    rc, out = chip.run(["payload", "parse-setup-payload", ctx.qr], timeout=15)
    parsed = parse_setup_payload(out) if rc == 0 else None
    if not s.check("3.5 QR payload parses", parsed is not None, tag="P3"):
        raise StepAbort("onboarding QR not machine-usable")
    ctx.passcode, ctx.discriminator = parsed
    rc, out = chip.run(pairing_argv(ctx), timeout=120)
    paired = s.check("3.5 chip-tool pairing exits 0", rc == 0, tag="P3")
    if not paired:
        print("    (chip-tool tail: %s)"
              % _pairing_tail(out, getattr(ctx.opts, "psk", None)))
    s.check("3.5 +MTEVT:1 session started",
            link.await_urc(r"\+MTEVT:1$", timeout=90.0) is not None,
            tag="P3")
    got3 = link.await_urc_ts(r"\+MTEVT:3$", timeout=90.0)
    s.check("3.5 +MTEVT:3 commissioning complete", got3 is not None,
            tag="P3")
    if not (paired and got3 is not None):
        raise StepAbort("commissioning failed")
    res, lines = link.command("AT+MTFABRICS?")
    if not s.check("3.5 fabrics 1", res == 0 and lines == ["+MTFABRICS:1"],
                   tag="P3"):
        raise StepAbort("fabric count did not settle at 1")


def step_3_6_valve(ctx):
    """T5 design spec 4.3 bullet 1 (valve): open from the controller,
    the +MTCMD actuation forward answered allow, the host's own
    AT+MTVALVE actuation report, and the ValveStateChanged event read
    back. Sequence lifted verbatim from the C10 evidence's "3d" bullet
    (task-C10-report.md), endpoint substituted 1 -> 2 for
    PHASE3_COMPOSITION's valve slot: cluster 129/0x0081 (verified
    against connectedhomeip's generated ClusterId.h), Open command 0,
    CurrentState attribute 4, TargetState attribute 5.

    Deny semantics (spec 3.19): the verdict gates ACTUATION only, never
    the wire response. ValveConfigurationAndControl's server calls the
    delegate synchronously and discards what it returns
    (TEMPORARY_RETURN_IGNORED at both call sites), so the controller
    sees Status::Success whichever verdict the harness gives; a deny run
    would prove nothing different on the wire, so this step only
    exercises allow (contrast step_3_9_opstate and step_3_12's lock
    forward, where a deny genuinely changes what the controller sees).

    This is also where the MTCMDRESP deferred row lands (Task 1's
    register_phase1_t5_negative, "seq already answered/expired ->
    +MTERR:1"; needs a real forward, out of the host-only segment's
    scope per Task 4's report): the C10 evidence's own "3d" bullet ties
    "the answered non-zero-seq +MTCMD regression pin" to this exact
    forward, so the re-answer-a-stale-seq pin is proven against the same
    seq right after CmdResponder answers it once."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    res, lines = link.command("AT+MTVALVE=2,0", expect="+MTATTR:")
    s.check("3.6 known start state: valve closed",
            res == 0 and lines == ["+MTATTR:2,129,4,0"], tag="P3")

    handle = invoke_chip(ctx, ["valveconfigurationandcontrol", "open", node,
                              "2"], timeout=30)
    fwd = responder.expect(cluster=129, command=0, verdict=1, timeout=5.0)
    if not s.check("3.6 +MTCMD forward answered allow", fwd is not None,
                   tag="P3"):
        handle.join(30)
        raise StepAbort("valve open forward was never answered")
    rc, _ = handle.join(30)
    s.check("3.6 chip-tool open exits 0", rc == 0, tag="P3")

    res, _ = link.command("AT+MTCMDRESP=%d,1" % fwd["seq"])
    s.check("3.6 stale-seq re-answer -> +MTERR:1 (MTCMDRESP deferred row)",
            res == 1, tag="P3")

    res, _ = link.command("AT+MTVALVE=2,1")
    s.check("3.6 host reports actuation -> OK", res == 0, tag="P3")

    rc, out = chip.run(["valveconfigurationandcontrol", "read",
                        "current-state", node, "2"], timeout=30)
    s.check("3.6 controller reads CurrentState Open",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")

    rc, out = chip.run(["valveconfigurationandcontrol", "read-event",
                        "valve-state-changed", node, "2"], timeout=30)
    # >= 1, not == 1: step_3_2_grammar already wrote AT+MTVALVE=2,1 twice
    # host-only (spec 3.19: UpdateCurrentState() fires the event on every
    # call, not just on a real change), before any controller existed to
    # read the event log, so entries from that earlier segment are still
    # in the buffer.
    s.check("3.6 ValveStateChanged event observed",
            rc == 0 and parse_event_count(out, "ValveStateChanged") >= 1,
            tag="P3")


def step_3_7_modes(ctx):
    """T5 design spec 4.3 bullet 2 (mode select): SupportedModes reads
    back verbatim including a comma-containing label (spec 3.20), then a
    ChangeToMode round trip with the unsolicited CurrentMode +MTATTR URC
    (cluster 80/0x0050, attribute 3) spec 3.20 documents -- "the host
    sees that the ordinary way, a +MTATTR URC ... not a URC specific to
    this command." No CmdResponder needed: ChangeToMode is not a
    forwarded command, the SDK sets CurrentMode itself after validating
    against SupportedModes.

    Modes are re-established here rather than relying on step_3_4's
    trailing state, so this step is self-contained and its "comma label
    pinned" assertion is not coupled to an earlier step's exact last
    call."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    res, _ = link.command('AT+MTMODES=3,0,"Quiet",1,"Eco, low"')
    s.check("3.7 modes established (comma label) -> OK", res == 0, tag="P3")

    rc, out = chip.run(["modeselect", "read", "supported-modes", node, "3"],
                       timeout=30)
    s.check("3.7 SupportedModes verbatim (comma label survives)",
            rc == 0 and parse_string_list(out) == ["Quiet", "Eco, low"],
            tag="P3")

    res, _ = link.command("AT+MTATTR=3,80,3,0")
    s.check("3.7 CurrentMode reset to 0 -> OK", res == 0, tag="P3")
    link.drain(0.3)

    rc, _ = chip.run(["modeselect", "change-to-mode", "1", node, "3"],
                     timeout=30)
    s.check("3.7 chip-tool change-to-mode exits 0", rc == 0, tag="P3")
    got = link.await_urc(r"\+MTATTR:3,80,3,1$", timeout=5.0)
    s.check("3.7 CurrentMode URC on the AT link", got is not None, tag="P3")

    res, lines = link.command("AT+MTATTR=3,80,3")
    s.check("3.7 AT read agrees (CurrentMode 1)",
            res == 0 and lines == ["+MTATTR:3,80,3,1"], tag="P3")


def step_3_8_chime(ctx):
    """T5 design spec 4.3 bullet 3 (chime): InstalledChimeSounds
    verbatim (comma label), PlayChimeSound allow AND deny with the
    chimeID visible in the fifth +MTCMD payload field, and the
    Enabled=false short-circuit. Cluster 1366/0x0556, command 0
    (PlayChimeSound), chimeID 7 -- lifted verbatim from the C10
    evidence's "3e" bullet, which used the identical chimeID.

    Deny semantics differ from the valve (spec 3.24): "the host's
    verdict reaches the controller exactly as given, Status::Success on
    allow or Status::Failure on deny, with no SDK-side remapping" -- the
    chime deny IS the wire status (0x1), unlike the valve, where the
    verdict never reaches the wire at all.

    Enabled=false short-circuit (spec 3.24): the cluster answers
    Status::Success itself, with no delegate call, so no +MTCMD ever
    arrives; this is the SDK's own short-circuit, not a forwarding
    failure, hence assert_no_urc rather than a responder timeout."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    res, _ = link.command(
        'AT+MTCHIMESOUNDS=4,1,"Doorbell",2,"Alert, urgent",7,"Westminster"')
    s.check("3.8 InstalledChimeSounds established (comma label) -> OK",
            res == 0, tag="P3")
    rc, out = chip.run(["chime", "read", "installed-chime-sounds", node,
                        "4"], timeout=30)
    s.check("3.8 InstalledChimeSounds verbatim",
            rc == 0 and parse_string_list(out)
            == ["Doorbell", "Alert, urgent", "Westminster"], tag="P3")

    res, _ = link.command("AT+MTCHIME=4,1,1")
    s.check("3.8 Enabled = true -> OK", res == 0, tag="P3")

    handle = invoke_chip(ctx, ["chime", "play-chime-sound", node, "4",
                              "--ChimeID", "7"], timeout=30)
    fwd = responder.expect(cluster=1366, command=0, verdict=1, payload=7,
                           timeout=5.0)
    s.check("3.8 allow: +MTCMD forward answered, chimeID 7 in payload",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.8 allow: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.8 allow: wire status 0x0 (Success)",
            parse_status(out) == 0x0, tag="P3")

    handle = invoke_chip(ctx, ["chime", "play-chime-sound", node, "4",
                              "--ChimeID", "7"], timeout=30)
    fwd = responder.expect(cluster=1366, command=0, verdict=0, payload=7,
                           timeout=5.0)
    s.check("3.8 deny: +MTCMD forward answered, chimeID 7 in payload",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    # Task-7 fix F2: a denied PlayChimeSound is a bare StatusIB Failure
    # (0x1), and chip-tool maps any non-success StatusIB to a non-zero
    # exit (task-7-report.md section 5, H2). rc == 0 on a deny can never
    # happen; the companion wire-status check below is what proves the
    # deny landed, since a non-zero exit alone does not say which error.
    s.check("3.8 deny: chip-tool reports Failure (rc != 0)", rc != 0,
            tag="P3")
    s.check("3.8 deny: wire status 0x1 (Failure)",
            parse_status(out) == 0x1, tag="P3")

    res, _ = link.command("AT+MTCHIME=4,1,0")
    s.check("3.8 Enabled = false -> OK", res == 0, tag="P3")
    link.drain(0.3)
    rc, out = chip.run(["chime", "play-chime-sound", node, "4", "--ChimeID",
                        "7"], timeout=30)
    s.check("3.8 disabled: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.8 disabled: no +MTCMD raised (SDK short-circuit)",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")
    s.check("3.8 disabled: controller still sees Success",
            parse_status(out) == 0x0, tag="P3")


def step_3_9_opstate(ctx):
    """T5 design spec 4.3 bullet 4 (OperationalState/washer): Pause id
    0, Stop id 1, Start id 2, Resume id 3, cluster 96/0x0060 -- lifted
    verbatim from the C10 "Re-run 3f" evidence that proved defect
    F-C10-1's fix, endpoint substituted 6 -> 7 for PHASE3_COMPOSITION's
    washer slot.

    Deny semantics (spec 3.21; verified against main.cpp's
    HearthOpStateDelegate::forward(), which calls err.Set() with the
    verdict directly): unlike the valve, the verdict IS the wire
    response. Instance::HandlePauseState() and its Stop/Start/Resume
    siblings copy err straight into the OperationalCommandResponse, so
    ErrorStateID 0 (kNoError) means allow and 2
    (kUnableToCompleteOperation) means deny, both read via parse_status's
    ErrorStateID fallback.

    The in-state guard (verified in main.cpp: HearthOpStateDelegate::
    forward() has no state check of its own -- the guard lives in the
    SDK's own Instance, before the delegate, and therefore
    mt_cmd_forward, is ever reached) is proven last: a pause sent from
    Stopped answers ErrorStateID 3 (kCommandInvalidInState) with NO
    +MTCMD raised, via a synchronous chip.run() (no responder thread
    needed, since nothing blocks waiting on a verdict that is never
    asked for)."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    def allow(name, verb, command, next_state):
        handle = invoke_chip(ctx, ["operationalstate", verb, node, "7"],
                             timeout=30)
        fwd = responder.expect(cluster=96, command=command, verdict=1,
                               timeout=5.0)
        s.check("3.9 %s +MTCMD forward answered allow" % name,
                fwd is not None, tag="P3")
        rc, out = handle.join(30)
        s.check("3.9 %s chip-tool exits 0 (allow)" % name, rc == 0,
                tag="P3")
        s.check("3.9 %s ErrorStateID 0 (kNoError) on allow" % name,
                parse_status(out) == 0, tag="P3")
        res, _ = link.command("AT+MTOPSTATE=7,%d" % next_state)
        s.check("3.9 host reports %s transition -> OK" % name, res == 0,
                tag="P3")

    res, _ = link.command("AT+MTOPSTATE=7,0")
    s.check("3.9 known start state: Stopped -> OK", res == 0, tag="P3")

    allow("start", "start", 2, 1)    # Stopped -> Running
    allow("pause", "pause", 0, 2)    # Running -> Paused
    allow("resume", "resume", 3, 1)  # Paused -> Running
    allow("stop", "stop", 1, 0)      # Running -> Stopped

    res, _ = link.command("AT+MTOPSTATE=7,1")
    s.check("3.9 deny precondition: Running -> OK", res == 0, tag="P3")
    handle = invoke_chip(ctx, ["operationalstate", "pause", node, "7"],
                         timeout=30)
    fwd = responder.expect(cluster=96, command=0, verdict=0, timeout=5.0)
    s.check("3.9 pause +MTCMD forward answered deny", fwd is not None,
            tag="P3")
    rc, out = handle.join(30)
    s.check("3.9 pause chip-tool exits 0 (deny)", rc == 0, tag="P3")
    s.check("3.9 ErrorStateID 2 (kUnableToCompleteOperation) on deny",
            parse_status(out) == 2, tag="P3")
    rc, out = chip.run(["operationalstate", "read", "operational-state",
                        node, "7"], timeout=30)
    s.check("3.9 state unchanged by deny (still Running)",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")

    res, _ = link.command("AT+MTOPSTATE=7,0")
    s.check("3.9 in-state guard precondition: Stopped -> OK", res == 0,
            tag="P3")
    link.drain(0.3)
    rc, out = chip.run(["operationalstate", "pause", node, "7"], timeout=30)
    s.check("3.9 in-state guard: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.9 in-state guard: ErrorStateID 3 (kCommandInvalidInState)",
            parse_status(out) == 3, tag="P3")
    s.check("3.9 in-state guard: no +MTCMD raised",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")


def step_3_10_smoke(ctx):
    """T5 design spec 4.3 bullet 5 (smoke/CO alarm): the self-test
    lifecycle end to end, from a real controller, TWICE in a row (the
    B165/F-C10-2 regression pin, spec 3.22's DEFECT note), plus
    AT+MTALARM SmokeState Warning with the SmokeAlarm event and
    ExpressedState tracking both directions (host-only proved this in
    step_3_3; this repeats the SmokeState half through a controller
    read, per the design spec 4.3 bullet).

    SelfTestRequest is notify-only (spec 3.22: "the SDK has already
    answered the controller before the ember callback ... ever runs"),
    so chip.run() is called directly, no invoke_chip/responder thread
    needed: nothing blocks the controller's own reply on an
    AT+MTCMDRESP the host never has to send for seq 0."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    res, lines = link.command("AT+MTATTR=5,92,0")
    s.check("3.10 clean start: ExpressedState 0",
            res == 0 and lines == ["+MTATTR:5,92,0,0"], tag="P3")

    rc, out = chip.run(["smokecoalarm", "read-event", "self-test-complete",
                        node, "5"], timeout=30)
    before = parse_event_count(out, "SelfTestComplete") if rc == 0 else None
    s.check("3.10 SelfTestComplete baseline read", before is not None,
            tag="P3")

    for cycle, want_count in ((1, (before or 0) + 1), (2, (before or 0) + 2)):
        rc, out = chip.run(["smokecoalarm", "self-test-request", node, "5"],
                           timeout=30)
        s.check("3.10 cycle %d: chip-tool self-test-request exits 0"
                % cycle, rc == 0, tag="P3")
        # The B165/F-C10-1 wedge symptom on a re-broken build: the second
        # request answered Status::Busy (0x9c) with no +MTCMD at all.
        s.check("3.10 cycle %d: not answered Busy (0x9c)" % cycle,
                "0x9c" not in out.lower() and "busy" not in out.lower(),
                tag="P3")
        fwd = responder.expect_notify(cluster=92, command=0, timeout=5.0)
        s.check("3.10 cycle %d: +MTCMD:0,... notify received" % cycle,
                fwd is not None, tag="P3")
        res, lines = link.command("AT+MTALARM=5,5,0", expect="+MTATTR:")
        s.check("3.10 cycle %d: completion -> OK" % cycle,
                res == 0 and lines == ["+MTATTR:5,92,5,0", "+MTATTR:5,92,0,0"],
                tag="P3")
        rc, out = chip.run(["smokecoalarm", "read-event",
                            "self-test-complete", node, "5"], timeout=30)
        s.check("3.10 cycle %d: SelfTestComplete count is %d" % (cycle,
                want_count),
                rc == 0 and parse_event_count(out, "SelfTestComplete")
                == want_count, tag="P3")

    res, lines = link.command("AT+MTALARM=5,1,1", expect="+MTATTR:")
    s.check("3.10 SmokeState Warning -> OK",
            res == 0 and lines == ["+MTATTR:5,92,1,1", "+MTATTR:5,92,0,1"],
            tag="P3")
    rc, out = chip.run(["smokecoalarm", "read-event", "smoke-alarm", node,
                        "5"], timeout=30)
    s.check("3.10 SmokeAlarm event observed controller-side",
            rc == 0 and parse_event_count(out, "SmokeAlarm") >= 1, tag="P3")
    rc, out = chip.run(["smokecoalarm", "read", "expressed-state", node,
                        "5"], timeout=30)
    s.check("3.10 ExpressedState 1 (SmokeAlarm) controller-side",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")

    res, lines = link.command("AT+MTALARM=5,1,0", expect="+MTATTR:")
    s.check("3.10 SmokeState cleared -> OK",
            res == 0 and lines == ["+MTATTR:5,92,1,0", "+MTATTR:5,92,0,0"],
            tag="P3")
    rc, out = chip.run(["smokecoalarm", "read", "expressed-state", node,
                        "5"], timeout=30)
    s.check("3.10 ExpressedState back to 0 controller-side",
            rc == 0 and parse_int_attr(out) == 0, tag="P3")


def step_3_11_power(ctx):
    """T5 design spec 4.3 bullet 6 (power source): BatPercentRemaining
    written over AT+MTATTR in wire half-percent units (0..200, spec
    3.9's "hand-added by the thunk" note) and read back from the
    controller. Cluster 47/0x002F, attribute 12/0x0C, both verified
    against connectedhomeip's generated PowerSource ClusterId.h/
    AttributeIds.h, not transcribed from memory.

    Task 3's report flagged the read argv as unverified ("no chip-tool
    cluster-verb capture exists in the C10 evidence... likely
    ["powersource", "read", "feature-map", ...] by analogy"). Verified
    this round, NOT by inference: `chip-tool powersource read --help`
    against the pinned binary
    (~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool)
    lists "bat-percent-remaining" as a real attribute verb, so the read
    below uses that directly rather than feature-map (which would read
    the wrong attribute)."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    res, lines = link.command("AT+MTATTR=6,47,12,164")
    s.check("3.11 AT+MTATTR write BatPercentRemaining=164 -> OK",
            res == 0 and lines == ["+MTATTR:6,47,12,164"], tag="P3")

    rc, out = chip.run(["powersource", "read", "bat-percent-remaining",
                        node, "6"], timeout=30)
    s.check("3.11 controller reads BatPercentRemaining 164 (wire units)",
            rc == 0 and parse_int_attr(out) == 164, tag="P3")


# Task-7 fix F3: LockDoor/UnlockDoor are timed-invoke commands per the
# Matter spec. Without --timedInteractionTimeoutMs, chip-tool's
# TimedRequest carries no timeout and the SDK answers 0xc6
# NEEDS_TIMED_INTERACTION before the delegate is ever called, so no
# +MTCMD is raised (task-7-report.md section 5, H3). The value here
# bounds only the window between chip-tool's TimedRequest action and
# the InvokeRequest that follows it on the SAME already-open CASE
# session: chip-tool sends the two back to back with no user-observable
# gap (tens of milliseconds on a LAN at most), and the timed-interaction
# timer's job is done the instant the InvokeRequest arrives, BEFORE
# mt_door_lock_adjudicate() and the harness's own CmdResponder round
# trip (its own 5 s window, plus the firmware's 1000 ms AT+MTCMDRESP
# deadline) ever run. 5000 ms is two orders of magnitude more than the
# pre-invoke gap needs and cannot race the adjudication delay, because
# the two happen in strictly separate phases of the same call.
DOORLOCK_TIMED_INVOKE_MS = "5000"


def step_3_12_lock_switch_levels(ctx):
    """T5 design spec 4.3 bullets 7-9 (door lock, switch, temp levels).

    Door lock deny: LockDoor has no cluster-specific response the way
    OperationalState's OperationalCommandResponse does (spec 3.17: the
    door lock is the example given for "a deny surfaces as
    Status::Failure to the controller"), so a deny is a plain StatusIB
    failure, not a Success wrapper with an embedded verdict field.

    Task-7 fix F4 derived "3.12 deny: wire status 0x1 (Failure)" below
    from door-lock-server.cpp's HandleRemoteLockOperation(), which ends
    with `commandObj->AddStatus(commandPath, success ? Status::Success
    : Status::Failure)`, and main.cpp's mt_door_lock_adjudicate() (the
    emberAfPluginDoorLockOn* callback), which returns exactly the
    mt_cmd_forward() boolean with no cluster-specific response wrapper,
    the same bare-StatusIB shape step_3_8's chime deny uses. Both the
    allow and deny paths were then observed live on both transports
    (wifi-devicetypes.json at 7e5f9a7, thread-devicetypes.json at
    9f90f48, both 165/165): the WiFi run and the Thread run each
    answered wire status 0x1 on deny, 0x0 on allow, confirming the
    derivation.

    Switch events are fire-and-forget (spec 3.15): AT+MTSWITCH never
    echoes on the AT link, so the only way to observe it is a
    controller-side event read.

    Temp levels: no C10 evidence transcript touches TemperatureControl
    either (the temp-levels cabinet is Task 4's slot 10/11 addition, new
    ground). SupportedTemperatureLevels is list[string], not
    list[struct{Label}] like SupportedModes/InstalledChimeSounds, so
    parse_string_list's Label:/Name: pattern does not apply; see
    parse_indexed_list's docstring for the print-path verification."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    res, _ = link.command("AT+MTLOCK=8,1")
    s.check("3.12 AT+MTLOCK=8,1 -> OK (Locked)", res == 0, tag="P3")
    rc, out = chip.run(["doorlock", "read", "lock-state", node, "8"],
                       timeout=30)
    s.check("3.12 controller reads LockState (Locked)",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")

    handle = invoke_chip(ctx, ["doorlock", "lock-door", node, "8",
                              "--timedInteractionTimeoutMs",
                              DOORLOCK_TIMED_INVOKE_MS], timeout=30)
    fwd = responder.expect(cluster=257, command=0, verdict=1, timeout=5.0)
    s.check("3.12 LockDoor +MTCMD forward answered allow", fwd is not None,
            tag="P3")
    rc, out = handle.join(30)
    s.check("3.12 chip-tool lock-door exits 0 (allow)", rc == 0, tag="P3")
    # Vacuousness check (task-7-report.md section 10, concern 2): rc == 0
    # is unambiguous for LockDoor (chip-tool exits 0 iff the StatusIB is
    # Success), but the explicit wire-status assertion is kept alongside
    # it anyway, matching step_3_8's allow/deny symmetry, so a future
    # reader never has to re-derive that rc == 0 already implies 0x0.
    s.check("3.12 allow: wire status 0x0 (Success)",
            parse_status(out) == 0x0, tag="P3")

    handle = invoke_chip(ctx, ["doorlock", "lock-door", node, "8",
                              "--timedInteractionTimeoutMs",
                              DOORLOCK_TIMED_INVOKE_MS], timeout=30)
    fwd = responder.expect(cluster=257, command=0, verdict=0, timeout=5.0)
    s.check("3.12 LockDoor +MTCMD forward answered deny", fwd is not None,
            tag="P3")
    rc, out = handle.join(30)
    s.check("3.12 chip-tool lock-door reports Failure on deny (rc != 0)",
            rc != 0, tag="P3")
    # De-vacuous (task-7-report.md section 6, INFERENCE-WRONG): this
    # exact check went green for three runs on 0xc6
    # NEEDS_TIMED_INTERACTION, an error it was never testing for (H3).
    # "some non-zero exit" is not enough on its own; pin WHICH failure
    # the way step_3_8 does with parse_status. Observed live on both
    # transports (see the docstring above): wire status 0x1 on deny.
    s.check("3.12 deny: wire status 0x1 (Failure)",
            parse_status(out) == 0x1, tag="P3")

    link.drain(0.3)
    res, _ = link.command("AT+MTSWITCH=9")
    s.check("3.12 AT+MTSWITCH=9 -> OK", res == 0, tag="P3")
    rc, out = chip.run(["switch", "read-event", "initial-press", node, "9"],
                       timeout=30)
    s.check("3.12 InitialPress event observed controller-side",
            rc == 0 and parse_event_count(out, "InitialPress") >= 1,
            tag="P3")

    res, _ = link.command('AT+MTTEMPLEVELS=10,"Low","Medium, high"')
    s.check("3.12 AT+MTTEMPLEVELS=10 (comma label) -> OK", res == 0,
            tag="P3")
    rc, out = chip.run(["temperaturecontrol", "read",
                        "supported-temperature-levels", node, "10"],
                       timeout=30)
    s.check("3.12 SupportedTemperatureLevels verbatim (comma label survives)",
            rc == 0 and parse_indexed_list(out) == ["Low", "Medium, high"],
            tag="P3")


def step_3_14_root_urc_sweep(ctx):
    """Design spec 4.3's standing-disciplines bullet ("no
    `+MTATTR:0,...` ever"), pinned by a dedicated sweep for the same
    reason step_2_6_root_urc_sweep exists for Phase 2: this project
    already judged "no step touches endpoint 0, so it can't happen" an
    insufficient guarantee once before (that is exactly what
    step_2_6_root_urc_sweep was built to stop relying on), and the
    T5 task 5 review raised the identical gap for Phase 3.

    Scored last and unconditional (no "requires"), spanning the WHOLE
    Phase 3 run's URC history from step_3_1 onward, not only the
    controller segment (3.5-3.12): the same scope step_2_6 gives Phase
    2, since a stray root-endpoint attribute report could in principle
    surface from any of the several reboots in the chain (step_3_1's
    two resets, the commissioning cycle in step_3_5), not only from a
    controller-driven write."""
    bad = [u for _, u in ctx.link.urc_history
           if u.startswith("+MTATTR:0,")]
    ctx.suite.check(
        "3.14 no root-endpoint +MTATTR URC in the whole run",
        not bad, tag="P3")
    if bad:
        print("    offending (first 5): %s" % bad[:5])


def step_3_13_restore(ctx):
    """T5 design spec 4.4's restore, as its own named step: AT+MTFRESET,
    the standard single-0x0100 composition, AT+MTEPAPPLY (which per spec
    3.9 persists and reboots -- restore_standard_state's actual
    +MTREADY wait target), +MTEP:0,1,0x0100 and zero fabrics.

    Deliberately NOT added to PHASE3_STEPS. run_phase3's finally already
    calls restore_standard_state(ctx.link, ctx.suite) unconditionally on
    every exit path, clean finish or any abort (design spec 4.4), which
    is this function's entire body; registering a second, identical step
    in the normal chain would double every reset/apply cycle at the end
    of a clean run for no additional coverage, and print two sets of
    identically-named "restore ..." checks in the same report. This
    function exists so "the restore step" has the name the brief and
    Task 4's report anticipated, and can be called or tested on its own,
    while there remains exactly one restore code path -- Task 4's
    report: 'the two restore paths cannot drift', because there is only
    one."""
    return restore_standard_state(ctx.link, ctx.suite)


def restore_standard_state(link, suite=None):
    """Factory-reset and redeclare the bench's standard single-0x0100
    composition (design spec section 4.4). Shared by run_phase3's
    finally hook now, and by Task 5's full step_3_13_restore later, so
    the two restore paths cannot drift. Unscored (suite=None) for a bare
    best-effort recovery in recover_after_abort's style; when suite is
    given, each stage is checked, so a restoration failure is visible in
    the report rather than silently swallowed (spec 4.4: "the report
    states whether restoration succeeded"). Returns True iff every stage
    succeeded."""
    def check(name, cond):
        if suite is not None:
            suite.check(name, cond, tag="P3")
        return cond

    ok = True
    res, _ = cmd_retry(link, "AT+MTFRESET", timeout=5.0)
    ok = check("restore MTFRESET -> OK", res == 0) and ok
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    ok = check("restore +MTREADY after MTFRESET", ready is not None) and ok
    staged = stage_composition(link, ["0x0100"])
    ok = check("restore composition staged", staged) and ok
    link.drain(0.3)
    res, _ = link.command("AT+MTEPAPPLY", timeout=5.0)
    ok = check("restore MTEPAPPLY -> OK", res == 0) and ok
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    ok = check("restore +MTREADY after MTEPAPPLY", ready is not None) and ok
    res, lines = cmd_retry(link, "AT+MTEP?")
    ok = check("restore composition reads +MTEP:0,1,0x0100",
               res == 0 and lines == ["+MTEP:0,1,0x0100"]) and ok
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    ok = check("restore fabrics 0",
               res == 0 and lines == ["+MTFABRICS:0"]) and ok
    return ok


PHASE3_STEPS = []  # host-only entries below; Task 5 appends the
                   # controller segment. step_3_13_restore is deliberately
                   # NOT appended here: see its docstring and run_phase3's.


def run_phase3(ctx):
    """Ordered execution for Phase 3 (design spec section 4), the same
    abort/skip/gate/-k semantics run_phase2 uses. PHASE3_STEPS registers
    the full chain: the host-only segment (3.1-3.4, no fabric, no
    controller), the commissioned matrix (3.5-3.12, one family per
    step), and the root-endpoint URC sweep (3.14, scored last, mirroring
    step_2_6_root_urc_sweep) that closes design spec 4.3's
    standing-disciplines bullet. step_3_13_restore exists as a named
    function but is deliberately NOT in this list: the finally block
    below already runs its body (restore_standard_state) unconditionally
    on every exit path, so registering it too would run the same
    reset/apply cycle twice on a clean pass.

    The finally block runs restore_standard_state unconditionally, on
    both a clean finish and an abort: Phase 3 starts destructively
    (AT+MTFRESET in step_3_1) and must never leave the bench in a
    non-standard state, matching design spec section 4.4. The report
    prints the restoration outcome explicitly either way."""
    abort_reason = None
    ran = set()
    kw = getattr(ctx.opts, "keyword", None)
    try:
        for step in PHASE3_STEPS:
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
    finally:
        restored = restore_standard_state(ctx.link, ctx.suite)
        print("  (bench restoration: %s)" % ("OK" if restored else "FAILED"))


PHASE3_STEPS[:] = [
    {"name": "3.1 compose + boot-rebuild pin", "fn": step_3_1_compose},
    {"name": "3.2 endpoint-dependent grammar", "fn": step_3_2_grammar,
     "requires": ["3.1 compose + boot-rebuild pin"]},
    {"name": "3.3 self-test wedge reproduction",
     "fn": step_3_3_selftest_wedge,
     "requires": ["3.1 compose + boot-rebuild pin"]},
    {"name": "3.4 store grammar edges", "fn": step_3_4_stores,
     "requires": ["3.1 compose + boot-rebuild pin"]},
    {"name": "3.5 commission", "fn": step_3_5_commission,
     "requires": ["3.4 store grammar edges"]},
    {"name": "3.6 valve", "fn": step_3_6_valve,
     "requires": ["3.5 commission"]},
    {"name": "3.7 mode select", "fn": step_3_7_modes,
     "requires": ["3.5 commission"]},
    {"name": "3.8 chime", "fn": step_3_8_chime,
     "requires": ["3.5 commission"]},
    {"name": "3.9 operational state", "fn": step_3_9_opstate,
     "requires": ["3.5 commission"]},
    {"name": "3.10 smoke/CO alarm", "fn": step_3_10_smoke,
     "requires": ["3.5 commission"]},
    {"name": "3.11 power source", "fn": step_3_11_power,
     "requires": ["3.5 commission"]},
    {"name": "3.12 lock, switch, temp levels",
     "fn": step_3_12_lock_switch_levels, "requires": ["3.5 commission"]},
    {"name": "3.14 root-endpoint URC sweep", "fn": step_3_14_root_urc_sweep},
]


DEFAULT_CHIPTOOL = os.path.expanduser(
    "~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool")

DEFAULT_OTCTL = "/mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/src/git/ot-br-posix/build/otbr/third_party/openthread/repo/src/posix/ot-ctl"


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
        # Delegate to the shared subprocess runner
        return _subprocess_runner(argv, timeout, label="chip-tool")

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


def parse_int_attr(text):
    """The last plain `<Label>: <int>` value chip-tool prints, e.g.
    `CurrentMode: 2`. Anchored to `]` + 2-or-more spaces so it skips the
    single-space `Endpoint: ... DataVersion: ...` summary line that
    precedes every attribute value line: that line embeds several
    `Word: number` fragments (DataVersion among them) that would
    otherwise win as the last match. Validated against the Task 3
    fixture (test/fixtures/t5/modes-current-mode.txt, `CurrentMode: 2`).
    None on no match, never raises."""
    vals = re.findall(r"\]\s{2,}[A-Za-z_][\w]*:\s*(-?\d+)\s*$",
                      text, re.MULTILINE)
    return int(vals[-1]) if vals else None


def parse_status(text):
    """The status of a command response, as an int. Two wire shapes
    exist and both are tried, last match wins:
      - a generic StatusIB failure, printed `status = 0x81
        (UNSUPPORTED_COMMAND),`;
      - OperationalState's business-logic verdict, printed
        `ErrorStateID: 2`, which spec 3.21 calls "the wire response"
        for that cluster (a Success at the StatusIB level can still
        carry a non-zero ErrorStateID).
    None on no match, never raises. Validated against the Task 3
    fixture (test/fixtures/t5/opstate-pause-deny.txt, ErrorStateID 2)."""
    status = re.findall(r"status\s*=\s*0x([0-9A-Fa-f]+)", text)
    if status:
        return int(status[-1], 16)
    error_state = re.findall(r"ErrorStateID:\s*(\d+)", text)
    if error_state:
        return int(error_state[-1])
    return None


def parse_accepted_command_list(text):
    """Every command id in an `AcceptedCommandList` (or similarly
    numbered `[n]: <id> (Name)` list) chip-tool read, in order. Shape
    validated against the Task 3 fixture
    (test/fixtures/t5/opstate-accepted-command-list.txt: Pause, Stop,
    Start, Resume -> [0, 1, 2, 3]). Empty list on no match, never
    raises."""
    return [int(m) for m in
            re.findall(r"\[\d+\]:\s*(\d+)\s*\(", text)]


def parse_event_count(text, event_name):
    """How many times `event_name` was reported in a chip-tool
    `read-event` capture, counting each `<event_name>: {` block.
    Shape validated against the Task 3 fixture
    (test/fixtures/t5/smoke-selftest-event-count.txt: two
    SelfTestComplete events -> 2). 0 on no match, never raises."""
    return len(re.findall(r"\b" + re.escape(event_name) + r":\s*\{", text))


def parse_string_list(text):
    """Every label in a chip-tool struct-list read that names its
    labels `Label:` (SupportedModes) or `Name:` (InstalledChimeSounds),
    in order, verbatim including any comma inside the label text --
    the field runs to end of line, so `Label: Normal, standard` keeps
    its comma. Shape validated against the Task 3 fixture
    (test/fixtures/t5/modes-supported-modes.txt: ["Quiet", "Normal,
    standard", "Boost"]). Empty list on no match, never raises."""
    return [m.rstrip() for m in
            re.findall(r"(?:Label|Name):\s*(.+)$", text, re.MULTILINE)]


def parse_indexed_list(text):
    """Every value in a chip-tool `[n]: <value>` list read where the
    list holds a plain scalar or string, not a struct with a `Label:`/
    `Name:` field the way parse_string_list's targets do: the shape
    `SupportedTemperatureLevels` (`list[string]`, not `list[struct]`)
    uses. Added in T5 task 5, one parser beyond Task 3's original five,
    because none of those five cover a bare list[string] read: no C10
    evidence transcript touches TemperatureControl at all (the
    temp-levels cabinet is Task 4's slot 10/11 addition, new ground the
    seven-type batch never exercised), so this is verified against the
    pinned SDK's own print path instead of a bench capture --
    connectedhomeip/examples/chip-tool/commands/clusters/DataModelLogger.h:
    the `DecodableList<T>` template labels each entry `"[i]"` (around
    line 116-136) and, for a `CharSpan` element (around line 47-50),
    prints it through `LogString(label, indent, value)` with no
    wrapping struct -- exactly the `[n]: <value>` shape this regex
    targets, one level of indentation deeper than the "N entries"
    summary line `parse_string_list`'s target also carries. Empty list
    on no match, never raises."""
    return [m.rstrip() for m in
            re.findall(r"\[\d+\]:\s*(.+)$", text, re.MULTILINE)]


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
        # This file-read path is outside _subprocess_runner, so it never
        # goes through the central ANSI strip (_strip_ansi) that F1 (task
        # 7 fix round 1) added there. Harmless today because
        # parse_onoff_reports is not end-anchored, so a trailing escape
        # cannot break its match; a future end-anchored parser reading
        # Subscriber output must strip first.
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


_ANSI_SGR_RE = re.compile(r"\x1b\[[0-9;]*m")


def _strip_ansi(text):
    """Remove ANSI SGR (colour) escape sequences from chip-tool output.

    Task 7's first live bench run (task-7-report.md section 5, defect H1)
    found the pinned chip-tool writes SGR sequences around every log line
    even when stdout is a pipe, not a TTY, so a raw line ends
    '...value\\x1b[0m' rather than '...value'. Three T5 parsers
    (parse_int_attr, parse_string_list, parse_indexed_list) are anchored
    to end of line and either fail to match or capture the trailing
    escape into the value. Stripped once here, centrally, in the
    subprocess runner, so every parser present and future sees clean
    text rather than patching each regex individually.

    \\x1b\\[[0-9;]*m covers every escape sequence found in the raw bench
    capture (chiptool-capture-run5.txt): the pattern was checked against
    that whole file and every escape in it is a plain SGR sequence ending
    'm', no cursor-movement or OSC codes among them."""
    return _ANSI_SGR_RE.sub("", text)


def _subprocess_runner(argv, timeout, label="process"):
    """Shared subprocess runner for ChipTool and otctl_run. Runs argv with
    the given timeout, converting TimeoutExpired to rc=124 (timeout code).
    A hung process must become a scored failure with skip semantics, not a
    raw traceback: TimeoutExpired is a SubprocessError, which neither
    run_phase2 (StepAbort only) nor main (serial/OSError only) catches.
    subprocess.run has already killed the child. Partial output arrives as
    bytes despite text=True (CPython quirk).

    Output is stripped of ANSI SGR sequences (_strip_ansi) before it is
    returned, on both the timeout and the normal exit path, so every
    caller -- ChipTool.run() and otctl_run() alike -- sees clean text."""
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        def _txt(s):
            if isinstance(s, bytes):
                return s.decode(errors="replace")
            return s or ""
        return 124, _strip_ansi(_txt(exc.stdout) + _txt(exc.stderr)
                     + "\n(%s timed out after %s s)" % (label, timeout))
    return proc.returncode, _strip_ansi(proc.stdout + proc.stderr)


def otctl_run(args, binary, runner=None, timeout=10):
    """One-shot ot-ctl invocation with the ChipTool runner contract:
    a hung ot-ctl becomes a nonzero rc, not a raw TimeoutExpired."""
    argv = (list(binary) if isinstance(binary, (list, tuple))
            else [binary]) + list(args)
    if runner is None:
        runner = lambda argv, timeout: _subprocess_runner(argv, timeout, label="ot-ctl")
    return runner(argv, timeout)


def parse_dataset(text):
    """The active dataset hex from `ot-ctl dataset active -x` output:
    the first line that is entirely hex and plausibly long. Validated
    against the Task 1 fixture."""
    for line in (text or "").splitlines():
        line = line.strip().lstrip("> ")
        if re.fullmatch(r"[0-9a-fA-F]{16,}", line):
            return line
    return None


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
                         unplug_timeout=180.0, clock=time.monotonic):
    """2.9's power cycle, observed rather than trusted: the device path
    must actually disappear before the step counts the cycle as having
    happened. A prompt alone would pass with no power cycle at all.

    The window is generous because the prompt may reach the operator
    through a relay (a log watcher plus a phone notification), which
    ate most of the original 60 s on a real run; waiting costs nothing
    when the operator is quick, since the loop exits on the unplug."""
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


def _transport_gate(chip, args, link, otctl):
    """Shared preflight body for Phase 2 and Phase 3: detects the
    transport from the device and branches, WiFi needs credentials,
    Thread needs a live border router and its active dataset (design
    spec T4 section 2), then confirms chip-tool exists and runs as this
    user (the 2026-07-29 root-owned counters file made every non-root
    run fail). Returns (problem, transport, dataset); problem is None on
    success.

    Factored out of phase2_gate (T5 task 4) so Phase 3's gate reuses this
    exact logic instead of copy-pasting it: phase2_gate's own self-tests
    are the guard that the extraction changed nothing observable."""
    if shutil.which("openocd") is None:
        return ("openocd not on PATH: the 2.8 warm reboot resets the "
                "RP2350 over SWD (see graph N22)", None, None)
    transport = getattr(args, "transport", None)
    if transport is None:
        res, lines = cmd_retry(link, "AT+MTNET?")
        m = re.match(r"\+MTNET:(WIFI|THREAD),", lines[0]) \
            if res == 0 and lines else None
        if not m:
            return ("cannot detect the transport: AT+MTNET? gave %r"
                    % (lines,), None, None)
        transport = m.group(1)
    dataset = None
    if transport == "THREAD":
        rc, out = otctl(["state"], args.ot_ctl)
        if rc != 0:
            return ("otbr-agent is not answering (ot-ctl state failed): "
                    "start it per the T4 runbook (graph F36: run "
                    "otbr-agent directly, D-Bus policy required)",
                    transport, None)
        lines = (out or "").strip().splitlines()
        role = (lines[0] if lines else "<empty>").strip().lstrip("> ")
        if role not in ("leader", "router", "child"):
            return ("the Thread network is down (ot-ctl state: %s); "
                    "bring it up before a Thread phase 2 run" % role,
                    transport, None)
        dataset = getattr(args, "dataset", None)
        if not dataset:
            rc, out = otctl(["dataset", "active", "-x"], args.ot_ctl)
            dataset = parse_dataset(out) if rc == 0 else None
        if not dataset:
            return ("no usable Thread dataset (ot-ctl dataset active -x "
                    "gave nothing; set MT_DATASET to override)",
                    transport, None)
    else:
        if not getattr(args, "ssid", None) or not getattr(args, "psk", None):
            return ("phase 2 needs WiFi credentials: export MT_SSID and "
                    "MT_PSK (or pass --ssid/--psk)", transport, None)
    if not (os.path.isfile(chip.binary)
            and os.access(chip.binary, os.X_OK)):
        return ("chip-tool not executable: %s (set MT_CHIPTOOL or "
                "--chip-tool)" % chip.binary, transport, dataset)
    try:
        rc, out = chip.run(
            ["payload", "parse-setup-payload", GATE_REFERENCE_QR],
            timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        return ("chip-tool failed to run: %s" % exc, transport, dataset)
    if rc != 0 or parse_setup_payload(out) is None:
        return ("chip-tool cannot parse the reference setup payload; "
                "check it runs as this user (root-owned /tmp/chip_*.ini "
                "is the known cause)", transport, dataset)
    return None, transport, dataset


def phase2_gate(chip, args, link, otctl=otctl_run):
    """Phase-2-only preflight, before anything destructive: the shared
    transport/chip-tool checks (_transport_gate) plus nothing else, since
    Phase 2's own destructive precondition (a factory-fresh-or-restorable
    device) is established by its first step, not the gate.

    otctl is a test seam, in the same style as ChipTool's injectable
    runner: production call sites never pass it, so the real otctl_run
    runs against the real binary; self-tests substitute a callable with
    the same (args, binary) -> (rc, out) signature."""
    return _transport_gate(chip, args, link, otctl)


def phase3_gate(chip, args, link, otctl=otctl_run):
    """Phase-3-only preflight: the same shared transport/chip-tool checks
    as Phase 2 (design spec T5 section 4.1: "same code path as Phase 2's
    gate"), so the two transports (and Thread's border-router liveness
    check) behave identically for both phases. Phase 3's own destructive
    precondition, that the run starts with AT+MTFRESET so its composition
    can be declared deterministically, is enforced by step_3_1_compose
    itself, not the gate: --phase 3 never running by default is main()'s
    job (the same policy phase2_gate's caller already follows), and the
    gate's job stays "is this bench usable at all", not "is state already
    clean".

    otctl is the same test seam phase2_gate takes."""
    return _transport_gate(chip, args, link, otctl)


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


def register_phase1_t5_negative():
    """T5: state-safe grammar rows for the post-August-1 command families
    (TESTING.md 6.2 / AT_MT_SPEC.md 3.16), per the design spec's state-safety
    split (docs/superpowers/specs/2026-08-08-c5-regression-harness-t5-design.md
    section 3): bare-ERROR command-form rows and +MTERR:1 parse/range rows
    only, since every one of them is rejected inside the handler itself,
    before mt_at.c ever calls into a mt_matter_* bridge function (confirmed
    against mt_at.c's cmd_mt* handlers). Rows needing +MTERR:2/3, +MTERR:4,
    or OK all need a real, known endpoint from a declared composition and
    belong to Phase 3, which owns one; each family's skipped rows are named
    in a comment where they land instead.
    """
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    # MTSWITCH (TESTING.md 6.2, switch-colorlight round)
    n("MTSWITCH? -> ERROR (query form)", expect_err("AT+MTSWITCH?", -1))
    n("MTSWITCH no args -> ERROR", expect_err("AT+MTSWITCH", -1))
    n("MTSWITCH= -> +MTERR:1 (empty argument)", expect_err("AT+MTSWITCH=", 1))
    n("MTSWITCH=zz -> +MTERR:1 (ep not numeric)", expect_err("AT+MTSWITCH=zz", 1))
    # MTSWITCH=99 (+MTERR:2, unknown endpoint), MTSWITCH=0 (+MTERR:3, root ep
    # has no Switch cluster) and MTSWITCH=1 (OK) all need a known composition:
    # Phase 3.

    # MTCMDRESP / the verdict mailbox (TESTING.md 6.2, command forwarding C1)
    n("MTCMDRESP? -> ERROR (query form)", expect_err("AT+MTCMDRESP?", -1))
    n("MTCMDRESP no args -> ERROR", expect_err("AT+MTCMDRESP", -1))
    n("MTCMDRESP=1 -> +MTERR:1 (fewer than 2 params)",
      expect_err("AT+MTCMDRESP=1", 1))
    n("MTCMDRESP=1,1,1 -> +MTERR:1 (more than 2 params)",
      expect_err("AT+MTCMDRESP=1,1,1", 1))
    n("MTCMDRESP=zz,1 -> +MTERR:1 (seq not numeric)",
      expect_err("AT+MTCMDRESP=zz,1", 1))
    n("MTCMDRESP=1,zz -> +MTERR:1 (verdict not numeric)",
      expect_err("AT+MTCMDRESP=1,zz", 1))
    n("MTCMDRESP=1,2 -> +MTERR:1 (verdict outside {0,1})",
      expect_err("AT+MTCMDRESP=1,2", 1))
    n("MTCMDRESP=99,1 -> +MTERR:1 (no forward pending)",
      expect_err("AT+MTCMDRESP=99,1", 1))
    n("MTCMDRESP=0,1 -> +MTERR:1 (seq 0 reserved)",
      expect_err("AT+MTCMDRESP=0,1", 1))
    # MTCMDRESP against a seq that was already answered or expired carries
    # the same +MTERR:1, but needs a real forward to have happened first
    # (mt_cmdbox_answer() has real prior state to check against): Phase 3.

    # MTLOCK (TESTING.md 6.2, doorlock round)
    n("MTLOCK? -> ERROR (query form)", expect_err("AT+MTLOCK?", -1))
    n("MTLOCK no args -> ERROR", expect_err("AT+MTLOCK", -1))
    n("MTLOCK=1 -> +MTERR:1 (fewer than 2 params)", expect_err("AT+MTLOCK=1", 1))
    n("MTLOCK=1,1,1,1 -> +MTERR:1 (more than 3 params)",
      expect_err("AT+MTLOCK=1,1,1,1", 1))
    n("MTLOCK=zz,1 -> +MTERR:1 (ep not numeric)", expect_err("AT+MTLOCK=zz,1", 1))
    n("MTLOCK=1,zz -> +MTERR:1 (state not numeric)", expect_err("AT+MTLOCK=1,zz", 1))
    n("MTLOCK=1,3 -> +MTERR:1 (state outside 0..2)", expect_err("AT+MTLOCK=1,3", 1))
    n("MTLOCK=1,1,zz -> +MTERR:1 (source not numeric)",
      expect_err("AT+MTLOCK=1,1,zz", 1))
    n("MTLOCK=1,1,11 -> +MTERR:1 (source above max)",
      expect_err("AT+MTLOCK=1,1,11", 1))
    # MTLOCK=99,1 (+MTERR:2), a non-door-lock ep (+MTERR:3) and a real
    # door-lock ep (OK) all need a known composition: Phase 3.

    # MTVALVE (TESTING.md 6.2, 0.5.0 C2)
    n("MTVALVE? -> ERROR (query form)", expect_err("AT+MTVALVE?", -1))
    n("MTVALVE no args -> ERROR", expect_err("AT+MTVALVE", -1))
    n("MTVALVE=1 -> +MTERR:1 (too few params)", expect_err("AT+MTVALVE=1", 1))
    n("MTVALVE=1,1,1,1 -> +MTERR:1 (too many)", expect_err("AT+MTVALVE=1,1,1,1", 1))
    n("MTVALVE=zz,1 -> +MTERR:1 (ep not numeric)", expect_err("AT+MTVALVE=zz,1", 1))
    n("MTVALVE=1,zz -> +MTERR:1 (state not numeric)", expect_err("AT+MTVALVE=1,zz", 1))
    n("MTVALVE=1,3 -> +MTERR:1 (state outside 0..2)", expect_err("AT+MTVALVE=1,3", 1))
    n("MTVALVE=1,1,zz -> +MTERR:1 (level not numeric)",
      expect_err("AT+MTVALVE=1,1,zz", 1))
    n("MTVALVE=1,1,101 -> +MTERR:1 (level above 100)",
      expect_err("AT+MTVALVE=1,1,101", 1))
    # MTVALVE=99,1 (+MTERR:2), a non-valve ep (+MTERR:3), and a real valve ep
    # with or without a level (both OK) all need a known composition: Phase 3.

    # MTMODES (TESTING.md 6.2, 0.5.0 C3)
    n("MTMODES? -> ERROR (query form)", expect_err("AT+MTMODES?", -1))
    n("MTMODES no args -> ERROR", expect_err("AT+MTMODES", -1))
    n("MTMODES=1 -> +MTERR:1 (no pairs after the endpoint)",
      expect_err("AT+MTMODES=1", 1))
    n('MTMODES=zz,0,"Quiet" -> +MTERR:1 (ep not numeric)',
      expect_err('AT+MTMODES=zz,0,"Quiet"', 1))
    n('MTMODES=1,zz,"Quiet" -> +MTERR:1 (mode not numeric)',
      expect_err('AT+MTMODES=1,zz,"Quiet"', 1))
    n("MTMODES=1,0,Quiet -> +MTERR:1 (missing quotes)",
      expect_err("AT+MTMODES=1,0,Quiet", 1))
    n("MTMODES quote character inside a label -> +MTERR:1",
      expect_err(r'AT+MTMODES=1,0,"Lo\"w"', 1))
    n('MTMODES=1,0,"" -> +MTERR:1 (empty label)',
      expect_err('AT+MTMODES=1,0,""', 1))
    n("MTMODES label over 32 bytes -> +MTERR:1",
      expect_err('AT+MTMODES=1,0,"12345678901234567890123456789012X"', 1))
    n('MTMODES=1,0,"Quiet",0,"Silent" -> +MTERR:1 (mode 0 repeated)',
      expect_err('AT+MTMODES=1,0,"Quiet",0,"Silent"', 1))
    n("MTMODES 9 pairs -> +MTERR:1 (over the 1..8 limit)",
      expect_err(
          'AT+MTMODES=1,0,"A",1,"B",2,"C",3,"D",4,"E",5,"F",6,"G",7,"H",8,"I"',
          1))
    # MTMODES=99,... (+MTERR:2), a non-mode-select ep (+MTERR:3), both OK
    # storage rows (plain and comma-in-label), and the CurrentMode
    # AT+MTATTR round trip all need a known composition: Phase 3.

    # MTOPSTATE (TESTING.md 6.2, 0.5.0 C4)
    n("MTOPSTATE? -> ERROR (query form)", expect_err("AT+MTOPSTATE?", -1))
    n("MTOPSTATE no args -> ERROR", expect_err("AT+MTOPSTATE", -1))
    n("MTOPSTATE=1 -> +MTERR:1 (fewer than 2 params)",
      expect_err("AT+MTOPSTATE=1", 1))
    n("MTOPSTATE=1,1,1 -> +MTERR:1 (more than 2 params)",
      expect_err("AT+MTOPSTATE=1,1,1", 1))
    n("MTOPSTATE=zz,1 -> +MTERR:1 (ep not numeric)",
      expect_err("AT+MTOPSTATE=zz,1", 1))
    n("MTOPSTATE=1,zz -> +MTERR:1 (state not numeric)",
      expect_err("AT+MTOPSTATE=1,zz", 1))
    n("MTOPSTATE=1,3 -> +MTERR:1 (state 3/Error reserved)",
      expect_err("AT+MTOPSTATE=1,3", 1))
    n("MTOPSTATE=1,4 -> +MTERR:1 (state outside range)",
      expect_err("AT+MTOPSTATE=1,4", 1))
    # MTOPSTATE=99,1 (+MTERR:2), a non-opstate ep (+MTERR:3), and a real
    # washer/dishwasher/dryer ep (OK) all need a known composition: Phase 3.

    # MTALARM (TESTING.md 6.2, 0.5.0 C5)
    n("MTALARM? -> ERROR (query form)", expect_err("AT+MTALARM?", -1))
    n("MTALARM no args -> ERROR", expect_err("AT+MTALARM", -1))
    n("MTALARM=1,1 -> +MTERR:1 (fewer than 3 params)",
      expect_err("AT+MTALARM=1,1", 1))
    n("MTALARM=1,1,1,1 -> +MTERR:1 (more than 3 params)",
      expect_err("AT+MTALARM=1,1,1,1", 1))
    n("MTALARM=zz,1,1 -> +MTERR:1 (ep not numeric)",
      expect_err("AT+MTALARM=zz,1,1", 1))
    n("MTALARM=1,zz,1 -> +MTERR:1 (field not numeric)",
      expect_err("AT+MTALARM=1,zz,1", 1))
    n("MTALARM=1,0,0 -> +MTERR:1 (field 0/ExpressedState is derived)",
      expect_err("AT+MTALARM=1,0,0", 1))
    n("MTALARM=1,12,0 -> +MTERR:1 (field outside 1..11)",
      expect_err("AT+MTALARM=1,12,0", 1))
    n("MTALARM=1,1,zz -> +MTERR:1 (value not numeric)",
      expect_err("AT+MTALARM=1,1,zz", 1))
    # The four per-field enum-range rows (AT+MTALARM=<alarm ep>,1,3 and
    # friends: field 1/4/5/10's value bounds) are validated inside
    # mt_matter_alarm_set(), not the handler: cmd_mtalarm's own comment says
    # it needs the field's real SDK enum type, which only a genuine
    # SmokeCoAlarm cluster instance has. Not order-independent, so not here,
    # despite carrying +MTERR:1. MTALARM=99,1,1 (+MTERR:2), a non-alarm ep
    # (+MTERR:3), the two OK rows, the self-test bench case, and the B165
    # regression pin all need a known composition too: all of the above go
    # to Phase 3.

    # MTCHIMESOUNDS (TESTING.md 6.2, 0.5.0 C6; bench used chime ep 3,
    # non-chime ep 7)
    n("MTCHIMESOUNDS? -> ERROR (query form)", expect_err("AT+MTCHIMESOUNDS?", -1))
    n("MTCHIMESOUNDS no args -> ERROR", expect_err("AT+MTCHIMESOUNDS", -1))
    n("MTCHIMESOUNDS=3 -> +MTERR:1 (no pairs)", expect_err("AT+MTCHIMESOUNDS=3", 1))
    n("MTCHIMESOUNDS=3,1,Doorbell -> +MTERR:1 (missing quotes)",
      expect_err("AT+MTCHIMESOUNDS=3,1,Doorbell", 1))
    n("MTCHIMESOUNDS duplicate id -> +MTERR:1 (id 1 repeated)",
      expect_err('AT+MTCHIMESOUNDS=3,1,"Doorbell",1,"Chime"', 1))
    n('MTCHIMESOUNDS=3,1,"" -> +MTERR:1 (empty name)',
      expect_err('AT+MTCHIMESOUNDS=3,1,""', 1))
    # MTCHIMESOUNDS=99,... (+MTERR:2), ep 7/non-chime (+MTERR:3), and both OK
    # storage rows (plain and comma-in-name) all need a known composition:
    # Phase 3.

    # MTCHIME (TESTING.md 6.2, 0.5.0 C6; same bench ep 3/7 as MTCHIMESOUNDS)
    n("MTCHIME? -> ERROR (query form)", expect_err("AT+MTCHIME?", -1))
    n("MTCHIME no args -> ERROR", expect_err("AT+MTCHIME", -1))
    n("MTCHIME=3,0 -> +MTERR:1 (fewer than 3 params)",
      expect_err("AT+MTCHIME=3,0", 1))
    n("MTCHIME=3,0,1,1 -> +MTERR:1 (more than 3 params)",
      expect_err("AT+MTCHIME=3,0,1,1", 1))
    n("MTCHIME=3,2,1 -> +MTERR:1 (2 not a valid <what>)",
      expect_err("AT+MTCHIME=3,2,1", 1))
    n("MTCHIME=3,1,2 -> +MTERR:1 (2 not a valid bool for Enabled)",
      expect_err("AT+MTCHIME=3,1,2", 1))
    # MTCHIME=3,0,9 (chime id 9 not installed) is validated inside
    # mt_matter_chime_set(), not the handler (cmd_mtchime's own comment: the
    # SDK's SetSelectedChime() answers Status::NotFound), so it needs real
    # installed sounds from a live AT+MTCHIMESOUNDS call first. MTCHIME=99,0,1
    # (+MTERR:2), ep 7/non-chime (+MTERR:3), and the two OK rows all need a
    # known composition too: all of the above go to Phase 3.

    # MTTEMPLEVELS (doorlock round). TESTING.md 6.2 now has its own
    # dedicated table (around line 367), in addition to the pattern
    # cross-references from the MTLOCK and MTMODES tables ("the
    # AT+MTTEMPLEVELS pattern" / "rule"); its worked example still lives
    # in AT_MT_SPEC.md 3.16, used as the row source here.
    n("MTTEMPLEVELS? -> ERROR (set-only, query form)",
      expect_err("AT+MTTEMPLEVELS?", -1))
    n("MTTEMPLEVELS no args -> ERROR", expect_err("AT+MTTEMPLEVELS", -1))
    n("MTTEMPLEVELS=4 -> +MTERR:1 (no labels)", expect_err("AT+MTTEMPLEVELS=4", 1))
    n("MTTEMPLEVELS=4,Low -> +MTERR:1 (missing quotes)",
      expect_err("AT+MTTEMPLEVELS=4,Low", 1))
    n("MTTEMPLEVELS quote character inside a label -> +MTERR:1",
      expect_err(r'AT+MTTEMPLEVELS=4,"Low","Lo\"w"', 1))
    # MTTEMPLEVELS=9,"Low" (+MTERR:2, no endpoint 9), MTTEMPLEVELS=1,"Low"
    # (+MTERR:3, ep 1 has no TemperatureControl cluster),
    # MTTEMPLEVELS=4,"Low" (+MTERR:4, ep 4 built as the wrong variant), and
    # the two OK storage rows (MTTEMPLEVELS=4,"Low","Medium","High" and
    # MTTEMPLEVELS=4,"Wine, red","Wine, white", comma inside a label) all
    # need a known composition: Phase 3.


register_phase1_t5_negative()


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


def exit_code(suite, truncated):
    """main()'s pass/fail verdict: nonzero on any scored failure, any
    abort-skip, or a truncated run. Gated entries (--include-slow,
    --include-manual, -k: operator intent, not truncation) must never
    tip this on their own (T3 final review finding 1b). Kept as its own
    function, not inlined in main(), so a self-test can call the real
    return path instead of re-deriving the same boolean expression."""
    return 1 if (suite.failed or suite.skipped or truncated) else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=os.environ.get("MT_PORT", "/dev/ttyACM0"))
    ap.add_argument("--phase", type=int, choices=[0, 1, 2, 3], default=None,
                    help="run only this phase (0 runs just the preflight "
                         "gate; 2 and 3 are stateful and never run by "
                         "default)")
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
    ap.add_argument("--transport", choices=["WIFI", "THREAD"], default=None,
                    help="override transport detection (default: ask the "
                         "device via AT+MTNET?)")
    ap.add_argument("--dataset", default=os.environ.get("MT_DATASET"),
                    help="Thread operational dataset hex (default: fetch "
                         "from ot-ctl at the gate)")
    ap.add_argument("--ot-ctl", dest="ot_ctl",
                    default=os.environ.get("MT_OTCTL", DEFAULT_OTCTL))
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
    if args.phase in (2, 3):
        chip = ChipTool(args.chip_tool, args.storage)

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
        # Always null, never args.ssid: baseline files are committed to a
        # public repo, and the project constraint is that neither PSK nor
        # SSID may appear in a committed file. The SSID identifies the
        # user's home network (bug B181 made the cost of loose
        # credential-adjacent hygiene concrete). The key stays present so
        # all six baseline files keep the same header shape.
        "ssid": None,
    }
    suite = Suite()
    truncated = False
    try:
        problem = phase0(link, header)
        if problem:
            print("ABORT: " + problem)
            return 2
        if args.phase == 2:
            problem, transport, dataset = phase2_gate(chip, args, link)
            if problem:
                print("ABORT: " + problem)
                return 2
            header["node_id"] = "0x%X" % args.node_id
            chip.wipe_storage()
            ctx = Phase2Context(link, chip, suite, args)
            ctx.transport = transport
            ctx.dataset = dataset
            ctx.relink = make_relink(link, args.port)
            ctx.chip2 = ChipTool(args.chip_tool, args.storage + "-f2")
            ctx.chip2.wipe_storage()
            run_phase2(ctx)
            capture_header(link, header)
        elif args.phase == 3:
            problem, transport, dataset = phase3_gate(chip, args, link)
            if problem:
                print("ABORT: " + problem)
                return 2
            header["node_id"] = "0x%X" % args.node_id
            chip.wipe_storage()
            ctx = Phase3Context(link, chip, suite, args)
            ctx.transport = transport
            ctx.dataset = dataset
            run_phase3(ctx)
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
    return exit_code(suite, truncated)


if __name__ == "__main__":
    sys.exit(main())
