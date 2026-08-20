#!/usr/bin/env python3
"""iLabs AT Hearth regression harness, stages T1-T5: Phases 0, 1, 2 and 3.

Test inventory: TESTING.md sections 5, 6 and 7 (2.1-2.5).
Design decisions: superpowers/specs/2026-07-30-c5-regression-harness-t1-design.md,
.../2026-07-30-c5-regression-harness-t2-design.md and
.../2026-08-08-c5-regression-harness-t5-design.md (Phase 3).

PORT HAZARD (C2 WiFi bench): resolve the link through /dev/serial/by-id,
NEVER as /dev/ttyACM<n>. On this bench /dev/ttyACM0 is the Nabu Casa ZBT-2
Thread RCP, and writing AT bytes into its live Spinel link kills otbr-agent
(TESTING.md section 2). There is deliberately NO default port: a run
that guesses is worse than a run that refuses, so --port (or MT_PORT) is
required.

    P=$(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)

Phase 1 run: python3 test/mt_regression.py --port "$P"
Phase 2 run: MT_SSID=... MT_PSK=... python3 test/mt_regression.py \
    --port "$P" --phase 2
Phase 3 run (host-only segment plus the full controller matrix, including
the RVC + Microwave batch's slots 12-13, the composed appliance
round's slots 14-20, energy round A's slots 21-22, energy round B's
slots 23-24, energy round C1's slots 25-27 and energy round C2's slot 28):
python3 test/mt_regression.py \
    --port "$P" --phase 3
(-k "3." scopes to Phase 3's own steps, since -k matches a literal
substring, not a regex, and every step name starts with its own "3.N ")

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


# The AT+MT commands whose RESPONSE lines do not carry the "+<name>:"
# prefix a host would derive from the command name. ATLink._derive_expect()
# consults this before falling back to the name rule; a response line the
# expect prefix does not match is routed into the URC queue instead, so a
# wrong prefix does not merely fail a check, it makes every "lines == []"
# assertion on that command TRUE regardless of what the device answered.
#
# B267 (C2 WiFi bench, BLOCKING) was the second member of this family:
# AT+MTROWGET answers "+MTROW:" lines (spec 3.28, emit_row_line() in
# mt_at.c). Four Phase 3 checks failed on a correct device, and worse,
# t_row_evse_meter_staged's two count-0 cases (one of them the "staged
# rows must be abandoned, not committed" regression task 2's review
# demanded) had been reporting PASS while measuring nothing.
#
# The first member was already known: AT+MTEVT? answers "+MTEVTMASK:"
# (spec 3.11, deliberately, so a +MTEVT URC landing between a command and
# its terminal response cannot be mistaken for the reply). That one was
# handled at each call site instead, which is exactly what let the second
# member ship: a call site is a place to forget. The table is the single
# place both live now, and test_mt_regression.py's
# TestResponsePrefixAudit checks it against an independently written
# truth table covering every command in mt_at.c's dispatch table.
#
# One collision is inherent and is NOT fixable here: AT+MTATTR's read
# response and the controller-driven +MTATTR URC are the same line shape,
# so ATLink cannot tell them apart. That is the standing rule in this
# file's module docstring (T1 design section 8, N23): never have an
# AT+MTATTR command in flight while a +MTATTR URC is expected. Phase 2
# and Phase 3 sequence around it by construction.
RESPONSE_PREFIX_EXCEPTIONS = {
    "MTEVT": "+MTEVTMASK:",
    "MTROWGET": "+MTROW:",
}


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
        """The prefix a command's own response lines carry. The name rule
        ("AT+MTVER?" -> "+MTVER:") holds for all but the two commands in
        RESPONSE_PREFIX_EXCEPTIONS; see that table for why the exception
        lives here and not at the call sites."""
        if not cmd.upper().startswith("AT+"):
            return None
        name = cmd[3:].split("=")[0].rstrip("?").upper()
        if not name:
            return None
        return RESPONSE_PREFIX_EXCEPTIONS.get(name, "+" + name + ":")

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

    Wire form (spec 3.17): +MTCMD:<seq>,<ep>,<cluster>,<command>[,<p1>[,
    <p2>[,<p3>[,<p4>[,<p5>]]]]], all decimal, up to five trailing payload
    fields (widened from the single optional field to four by the RVC +
    Microwave batch's mt_cmd_forward_fields(), then to five by energy
    round B: chime's PlayChimeSound chimeID and ModeBase's ChangeToMode
    newMode still send one, MicrowaveOvenControl's SetCookingParameters
    sends four, and WaterHeaterManagement's Boost is the first five-field
    consumer: duration, the packed presence/bool-value mask, then the
    numeric optionals whose presence bits are set, spec 3.17's worked
    example being "3600,265,80"). A field a given invoke did not carry
    renders as an empty position, not an omitted one, so the tail can hold
    interior gaps (e.g. ",,80,1" for p1/p2 absent, p3=80, p4=1); _RX and
    _match() below parse every position, not just the first.

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

    # Group 5 captures the WHOLE tail after <command> (0 to 5 repetitions
    # of ",<digits-or-empty>"; 4 until energy round B widened the
    # documented arity for Boost, spec 3.17), not just one field: _match()
    # below strips the tail's own leading comma and splits the remainder
    # on "," to recover each position, empty string -> None. A bare
    # four-field forward with no payload at all still matches (0
    # repetitions, group 5 ""), the identical backward-compatible shape
    # the old single-field regex accepted. Boost's own tail never carries
    # the empty-position form (its mask says which fields follow, so
    # absent optionals append nothing, spec 3.17), but the "\d*"
    # alternative stays: this regex serves every consumer, and
    # SetCookingParameters' interior gaps are real.
    _RX = re.compile(r"\+MTCMD:(\d+),(\d+),(\d+),(\d+)((?:,\d*){0,5})$")

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

        payload accepts either shape a caller needs: a bare int compares
        against fields[0] only (the legacy single-field consumers: chime's
        chimeID, ModeBase's ChangeToMode newMode); a list/tuple compares
        against the full parsed fields list exactly, position by position,
        including any interior None for an empty field -- the shape
        MicrowaveOvenControl's four-field SetCookingParameters needs.

        await_urc_ts returns (timestamp, line); only the line matters
        here."""
        pattern = r"^\+MTCMD:\d+,\d+,%d,%d(,|$)" % (cluster, command)
        got = self.link.await_urc_ts(pattern, timeout=timeout)
        if got is None:
            return None
        m = self._RX.match(got[1])
        if not m:
            return None
        tail = m.group(5)
        fields = ([int(f) if f != "" else None for f in tail[1:].split(",")]
                  if tail else [])
        fwd = {"seq": int(m.group(1)), "ep": int(m.group(2)),
               "cluster": int(m.group(3)), "command": int(m.group(4)),
               "fields": fields,
               "payload": fields[0] if fields else None}
        if payload is not None:
            if isinstance(payload, (list, tuple)):
                if fields != list(payload):
                    return None
            elif fwd["payload"] != payload:
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
        self.na = []
        self.gated = []

    def check(self, name, ok, tag="AT+"):
        ok = bool(ok)
        self.results.append((name, ok, tag))
        print("  [%s] [%s] %s" % ("PASS" if ok else "FAIL", tag, name))
        return ok

    def skip(self, name, reason):
        """A step not run because an earlier step broke its preconditions
        (run_phase2's generic "requires step that did not run" use).
        Counted separately from failures, but a truncated run must not
        read as a clean one (design spec section 3), so this DOES tip
        exit_code() nonzero. For a step that is not a precondition
        failure but architecturally does not apply to this run at all
        (a capability the current transport lacks), use not_applicable()
        instead: bench defect C found that conflating the two made every
        clean WiFi Phase 2 run exit 1 with zero failures, because
        step_2_13's WiFi self-skip landed here and this bucket fails the
        run on purpose."""
        self.skipped.append((name, reason))
        print("  [SKIP] %s: %s" % (name, reason))

    def not_applicable(self, name, reason):
        """A step whose architecture does not apply to this run at all,
        decided by the step itself rather than inherited from a broken
        precondition: step_2_13_thread_reboot_reattach on a WiFi
        transport is the motivating case, since a WiFi image has no
        Thread mesh to reattach to and the row is unreachable, not
        broken. Recorded and printed so it is never silently absent from
        a report (the same never-silent rule skip() follows), but
        exit_code() does NOT tip on it: a fact about the run's
        environment must not read as a failure the way a truncated chain
        does. Bench defect C's controller ruling: keep both buckets
        visible in the printed summary and the baseline, distinguished by
        name and by status string ("SKIP" vs "N/A"), and change only what
        the EXIT CODE means, never what gets recorded."""
        self.na.append((name, reason))
        print("  [SKIP] %s (not applicable: %s)" % (name, reason))

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
        if self.na:
            parts.append("%d n/a" % len(self.na))
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
        self.image = None         # "combined"/"wifi"/"thread"; set by
                                   # main() from AT+MTTRANSPORT? itself,
                                   # since a device that answers it IS the
                                   # combined image (task 4 report)
        self.dataset = None       # Thread active dataset hex, if any
        self.sleeper = None       # test seam; None means time.sleep. Only
                                   # 2.7's window settle uses it, so the
                                   # self-test can assert the wait without
                                   # spending it (Phase3Context carries the
                                   # same seam for step_3_26)


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
    (12, "0x0074"),     # robotic vacuum cleaner (RvcRunMode, RvcCleanMode,
                         # RvcOperationalState), RVC + Microwave batch,
                         # endpoint 12 to match AT_MT_SPEC.md 3.20.1/3.21's
                         # own worked examples verbatim
    (13, "0x0079"),     # microwave oven (MicrowaveOvenMode,
                         # MicrowaveOvenControl, base OperationalState),
                         # RVC + Microwave batch, endpoint 13 to match
                         # AT_MT_SPEC.md 3.20.1's own worked example
    # Composed appliance round (0.7.0), task 6: the composed trio. The
    # third field in an entry string is the PARENT STAGING INDEX per spec
    # 3.9's AT+MTEP=<devtype>[,<variant>[,<parent_idx>]] grammar, and
    # endpoint ids are assigned sequentially from 1, so index = endpoint
    # id - 1 throughout this table (index 13 = ep 14, and so on).
    #
    # ModeBase delegate pool budget (MT_MB_MAX_LISTS = 16, mt_matter.h:
    # 8 -> 12 in energy round C1, 12 -> 16 in C2): this composition
    # consumes 12 of the 16 slots: RVC 2 (RvcRunMode + RvcCleanMode, slot
    # 12), microwave 1 (slot 13), fridge parent 1 (slot 14, its own
    # RefrigeratorAndTCCMode), the two fridge cabinets 2 (slots 15-16,
    # Cooler conditional cluster each), oven cavity 1 (slot 18, OvenMode),
    # water heater 1 (slot 23, WaterHeaterMode, energy round B), battery
    # storage 1 and standalone DEM 1 (slots 26-27, DeviceEnergyManagement
    # Mode each, energy round C1), and the EVSE 2 (slot 28,
    # EnergyEvseMode plus the DEM triple's DeviceEnergyManagementMode,
    # energy round C2). The remaining 4 are DELIBERATE headroom (see the
    # round C1 comment at slot 25); a table edit that took the pool past
    # 16 without raising MT_MB_MAX_LISTS would abort the boot rebuild
    # when the pool ran dry (mk_* thunks return null on exhaustion and a
    # failed create aborts the whole composition, CLAUDE.md).
    (14, "0x0070"),        # refrigerator parent (composed trio round)
    (15, "0x0071,0,13"),   # number cabinet under the fridge (index 13 = ep 14)
    (16, "0x0071,1,13"),   # levels cabinet under the fridge
    (17, "0x007B"),        # oven parent
    (18, "0x0071,0,16"),   # oven cavity (index 16 = ep 17); Heater cluster set
    (19, "0x0078"),        # cooktop (parent for the surface; not previously
                            # composed)
    (20, "0x0077,0,18"),   # cook surface under the cooktop (index 18 = ep 19)
    # Energy round A (0.8.0), task 5: the two measurement-push types
    # behind AT+MTMEAS (spec 3.25, variant scheme spec 3.9). The variant
    # assignment is deliberately FLIPPED relative to the round's
    # plan-time table (sensor full, meter power-only): the 1.5.1 device
    # library marks ElectricalEnergyMeasurement MANDATORY on the meter
    # type while the sensor's XML lists its measurement clusters as a
    # pick-at-least-one choice, so a variant-1 meter would be a
    # non-conformant composition; flipping keeps both variants exercised
    # on conformant endpoints (spec 3.9's own conformance note: "the
    # strictly conformant current-clamp declaration is the variant-1
    # sensor"). Every check keeps its meaning with the targets swapped:
    # the EEM-absence and energy-push +MTERR:3 checks target slot 21,
    # the full power/energy/event/subscription checks slot 22.
    (21, "0x0510,1"),      # electrical sensor, variant 1 power-only (the
                            # conformant current-clamp case; EEM-absence
                            # checks target this slot)
    (22, "0x0514"),        # electrical meter, variant 0 power + energy
                            # (full measurement surface; energy/event
                            # checks target this slot)
    # Energy round B (0.9.0), task 4: the water heater and heat pump
    # behind the 0x94 push table, the Boost/CancelBoost forwards and the
    # derived boost events (spec 3.17/3.25). The water heater is variant
    # 0 (FULL: EnergyManagement + TankPercent features, the three gated
    # attribute shells, and the composed Electrical Sensor graft the
    # device type XML mandates); the minimal variant 1 is NOT in this
    # table: its feature-gate refusals are exercised by Phase 1's staged
    # scratch composition (t_meas_staged_wh_min), which restores the
    # single-light standard state after itself. Measurement pool budget
    # (MT_MEAS_MAX = 8, mt_matter.h, raised from 4 by energy round C1):
    # slots 21-24 were the four EPM/PT consumers that hit the old pool
    # exactly, and round C1's slots 25-26 make six of eight; see the
    # round C1 comment below for why the remaining two are deliberate
    # headroom rather than another exhaustion boundary.
    (23, "0x050F"),        # water heater, variant 0 FULL (WHM + Thermostat
                            # + WaterHeaterMode + the 0x0510 sensor graft)
    (24, "0x0309"),        # heat pump (0x0309 + 0x0011 power source +
                            # 0x0510 sensor identity on one endpoint; no
                            # WHM, no Thermostat: the disclosed SDK-parity
                            # gap, mt_devtypes.cpp)
    # Energy round C1 (0.10.0), task 4: solar power, battery storage and
    # the standalone Device Energy Management ESA, behind the 0x0098 push
    # table, the new AT+MTDEMCAP family and the PowerAdjust forwards
    # (spec 3.17/3.25/3.26). All three slots are VARIANT 0, the full
    # surface; every variant-1 refusal row lives in Phase 1's staged
    # scratch composition (t_staged_variant1_energy_c1), the
    # t_meas_staged_wh_min division.
    #
    # Budget HEADROOM IS DELIBERATE this round, unlike round B's three
    # exact fits: the pools are proven and the exact-fit experiment ended
    # with B247 (design spec section 5).
    #
    # The standing figures, updated by round C2 and by its final review
    # (which found the three pool self-tests had gone BLIND to slot 28:
    # their if-chains had no 0x050C arm, so they kept asserting the
    # pre-C2 constants and passed while measuring nothing about the slot
    # the round added). Endpoints 28 of MT_COMP_MAX_ENDPOINTS 28, i.e.
    # ZERO headroom, the table's last free slot spent (CONFIG_ESP_MATTER_
    # MAX_DYNAMIC_ENDPOINT_COUNT stays MT_COMP_MAX_ENDPOINTS + 1 = 29,
    # the B247 invariant: root endpoint 0 counts against the SDK's
    # limit); measurement pool 7 of MT_MEAS_MAX 8; ModeBase pool 12 of
    # MT_MB_MAX_LISTS 16; DEM delegate pool 3 of MT_DEM_MAX 4; EVSE
    # delegate pool 1 of MT_EVSE_MAX 2; MeterIdentification pool 0 of
    # MT_METER_MAX 2 (the meter is a Phase 1 staged type only). All six
    # budgets are now pinned by their own self-tests in
    # test_mt_regression.py, so a table edit that overruns one fails the
    # host suite instead of the bench's boot rebuild.
    (25, "0x0017"),        # solar power, variant 0 FULL (the composed
                            # 0x0510 sensor graft WITH EEM; variant 1 is
                            # the disclosed sub-conformant current-clamp
                            # shape, Phase 1's staged row)
    (26, "0x0018"),        # battery storage, variant 0 FULL (battery|
                            # rechargeable PowerSource with the SDK's RECHG
                            # conformance defect fixed, the sensor graft,
                            # and the DEM triple: 0x050D + DEM + DEMMode)
    (27, "0x050D"),        # device energy management, standalone, variant 0
                            # (ControllableESA: the PowerAdjustment feature,
                            # so PowerAdjustmentCapability, both PA commands
                            # and both PA events exist)
    # Energy round C2 (0.12.0), task 13: the AT+MTROW nested-payload family
    # and Energy EVSE 0x050C. ONE new permanent slot, not the design
    # spec's aspirational three (8.3): the design spec's OWN pool table
    # (section 4) earmarked exactly one new endpoint for this round
    # against MT_COMP_MAX_ENDPOINTS 28 with 27 already declared here
    # ("one for C2's EVSE", sdkconfig.defaults's own comment above
    # CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT), and this task's file
    # list (test/mt_regression.py, test/test_mt_regression.py, test/
    # baselines/*) carries no firmware/Kconfig files to raise the cap
    # with. Variant 0 (the SOC feature) gets the permanent slot, matching
    # the water heater precedent (round B: the FULL/richer variant is
    # permanent, the minimal one is staged): every SOC-mandatory row, the
    # 70-row full-schedule proof and merge-by-day all need the SOC-feature
    # arm anyway, since the SOC-optional arm's negative rows are cheaper
    # to prove on a scratch endpoint. EVSE variant 1 (no SOC) and the
    # Electrical Utility Meter (0x0511, no cluster COMMANDS at all, so no
    # controller interaction this round's checks need) are both covered
    # by Phase 1's staged scratch composition instead (t_row_evse_meter_
    # staged, t_row_meter_pool_exhaustion, register_phase1_t12_negative),
    # the t_meas_staged_wh_min / t_staged_variant1_energy_c1 division.
    # B266: written BARE, not "0x050C,0". This string is used twice, as
    # the AT+MTEP= argument (where ",0" is legal) and as the expected
    # AT+MTEP? readback line (where it is not: spec 3.9 renders <variant>
    # only when nonzero). The redundant token cost the C2 WiFi bench the
    # whole of Phase 3, 1 failed and 25 skipped. Enforced now by
    # test_every_entry_is_written_the_way_the_device_renders_it.
    (28, "0x050C"),        # energy EVSE, variant 0 (SOC feature: every
                            # charging target must carry targetSoC)
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
        self.max_endpoints = None  # None means the whole table; an int
                                    # truncates .composition to that many
                                    # entries and makes run_phase3 report
                                    # every step targeting a higher
                                    # endpoint as not-applicable
        self.subscriber_factory = None  # test seam; None means real
                                        # Subscriber (step_3_21's
                                        # ActivePower subscription)
        self.transport = "WIFI"    # set from phase3_gate's detection
        self.dataset = None        # Thread active dataset hex, if any
        self.qr = None             # captured by step_3_5_commission
        self.manual = None
        self.passcode = None
        self.discriminator = None
        self.chip_call = None      # test seam; None means the real
                                    # threaded chip.run() (see
                                    # invoke_chip/_threaded_chip_call)
        self.clock = None          # test seam; None means time.monotonic.
                                    # step_3_26 measures the PowerAdjust
                                    # duration clock against it (see
                                    # PA_CLOCK_GAP_S below)
        self.sleeper = None        # test seam; None means time.sleep


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
    alone would let the old pre-6046a50 leak pass).

    0.11.0 task 3, bench defect C: on a Thread run this is ALSO where the
    role-change event gets observed, moved here from a dedicated
    post-reboot row (step_2_13_thread_reboot_reattach used to try it)
    because the reboot path turns out to have no observable window at
    all: the event mask is RAM-only (AT_MT_SPEC.md 3.11) and resets to
    the default on every boot, and even setting it right after +MTREADY
    is too late, since the Thread stack reattaches during
    esp_matter::start(), before mt_at_start() brings the AT link up.
    Measured on the rig: mask set 0.05 s after +MTREADY, first
    AT+MTTHREAD? at 0.09 s already read REED, attached=1. There is no
    reboot between here and the device's last boot, so a mask set right
    here, before chip-tool's pairing call begins, HOLDS for the whole
    join. Bench evidence the window really exists here: +MTEVT:28
    observed carrying UNASSIGNED then REED as the device joined.

    Bit 28 is now double-gated (a real CHIP RoleChanged transition AND a
    token different from the last one emitted, design spec 2.2 as
    amended by the bench-A/B fix), so this asserts at least one +MTEVT:28
    arrives, never an exact count: the bench's own three-events-in-10ms
    case (UNASSIGNED, REED, REED) collapses to two distinct-token events
    under the current firmware, and a host-visible count that drops
    further under different join timing must not read as a regression.
    +MTEVT:25 is NOT gated on a token change and is asserted to still
    fire, unchanged.

    The mask enable/observe/restore all live inside `if thread_mask:`,
    so a WiFi run's code path is untouched (no new check even appears in
    that report; TESTING.md documents this as the established
    per-transport-content pattern, not a new mechanism); the restore
    runs in a `finally` block so it happens even if pairing itself
    fails and this step raises StepAbort."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    thread_mask = ctx.transport == "THREAD"
    mask = "0x%08X" % (0x0800003F | (1 << 25) | (1 << 28))  # 0x1A00003F
    try:
        if thread_mask:
            res, _ = link.command("AT+MTEVT=%s" % mask)
            s.check("2.3 MTEVT mask enable bits 25+28 -> OK (Thread role "
                    "observation window)", res == 0, tag="P2")
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
        if thread_mask:
            got28 = link.await_urc(r"\+MTEVT:28,", timeout=5.0)
            role_ok = False
            if got28 is not None:
                m = re.fullmatch(r"\+MTEVT:28,(.+)", got28)
                role_ok = (m is not None and
                          MT_THREAD_ROLE_TOKEN_RE.fullmatch(m.group(1))
                          is not None)
            s.check("2.3 MTEVT:28 arrives with a legal role token during "
                    "the join", role_ok, tag="P2")
            evt25 = link.await_urc(r"\+MTEVT:25$", timeout=5.0)
            s.check("2.3 MTEVT:25 fires during the join",
                    evt25 is not None, tag="P2")
        res, lines = link.command("AT+MTFABRICS?")
        s.check("2.3 fabrics 1", res == 0 and lines == ["+MTFABRICS:1"],
                tag="P2")
        res, lines = link.command("AT+MTSTATE?")
        s.check("2.3 state 2 (operational)",
                res == 0 and lines == ["+MTSTATE:2,1"], tag="P2")
    finally:
        if thread_mask:
            # Guarded like run_phase2's own AT+MTEP? capture and
            # recover_after_abort's AT+MTRESET (review F2): a link that
            # died during a failed pairing must not turn this restore
            # into an unhandled OSError, which would propagate out of
            # the finally block and skip the abort's recovery, summary
            # and baseline write entirely.
            try:
                link.command("AT+MTEVT=0x0800003F")
            except OSError as exc:
                print("  (2.3 mask restore: link unavailable, skipped: %s)"
                      % exc)


def step_2_4_host_to_controller(ctx):
    """TESTING.md 2.4. Sequencing per the concurrency rule: subscriber
    up, AT-side writes observed, subscriber down, only then controller
    reads. Mode 0 keeping the report suppressed is the one property
    nothing else in the suite can catch. Also pins the same-value mode-0
    fix (found sweeping B264): a mode-0 re-push of the value the attribute
    already holds must answer OK, not a bare ERROR, and emit no +MTATTR
    line either."""
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
        # "No report" (below) is the fabric side only. POST_UPDATE, the
        # +MTATTR line's source, runs on any ACTUAL value change regardless
        # of mode (both notify=true and notify=false call set_val_internal()
        # with call_callbacks=true), so this 1->0 write really does emit
        # +MTATTR:1,6,0,0 on the AT link. It arrives BEFORE the write's own
        # OK, and ATLink._derive_expect() maps "AT+MTATTR=..." to expect
        # "+MTATTR:", so it lands in THIS command's response lines rather
        # than in the URC queue: the same echo convention step 3.11 and the
        # rest of the suite already assert on (spec 3.8, TESTING.md 6.3).
        # Asserting it here also proves the same-value check below found
        # nothing because nothing was emitted, not because a leftover had
        # been consumed earlier (B265: this was asserted through
        # link.await_urc() first, which no real device can satisfy).
        res, lines = link.command("AT+MTATTR=1,6,0,0,0")
        s.check("2.4 mode 0 write -> OK", res == 0, tag="P2")
        s.check("2.4 mode 0 write still echoes +MTATTR on an actual value "
                "change", lines == ["+MTATTR:1,6,0,0"], tag="P2")
        res, lines = link.command("AT+MTATTR=1,6,0")
        s.check("2.4 local read shows 0",
                res == 0 and lines == ["+MTATTR:1,6,0,0"], tag="P2")
        s.check("2.4 mode 0 produces no report",
                sub.no_new_report(base, 5.0), tag="P2")

        # A same-value re-push (the value already equals what mode 0 just
        # set) used to fall through esp_matter's ESP_ERR_NOT_FINISHED to a
        # bare ERROR here: the attribute already held exactly the value
        # asked for, so the write succeeded by any definition that matters
        # to a host, and now answers OK. No POST_UPDATE callback runs for
        # an unchanged value in either mode (set_val_internal() returns
        # before reaching it), so no +MTATTR line either, in contrast with
        # the real value-change write above. Both carriers are asserted:
        # the echo (where a change would appear) and the URC queue (where
        # an asynchronous report arriving late would appear).
        res, lines = link.command("AT+MTATTR=1,6,0,0,0")
        s.check("2.4 same-value mode-0 re-push -> OK, not a bare ERROR",
                res == 0, tag="P2")
        s.check("2.4 same-value mode-0 re-push echoes no +MTATTR",
                lines == [], tag="P2")
        s.check("2.4 same-value mode-0 re-push raises no +MTATTR URC",
                link.assert_no_urc(r"\+MTATTR:1,6,0", 1.5), tag="P2")
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


SECOND_WINDOW_SETTLE_S = 15.0
# 2.7 opens a second commissioning window and immediately asks a second
# controller to find it by long discriminator. The discriminator is fixed
# for the device, so every window in a session republishes the SAME
# commissionable mDNS record under a new address, and `onnetwork-long`
# resolving before the republication lands picks up the previous window's
# address and dies in PASE (`PASESession.cpp:311 CHIP Error 0x00000032:
# Timeout`) without ever reaching the device.
#
# Found on the 0.11.0 bench round, on Thread, where the record travels
# through the border router's advertising proxy and the window is widest.
# Isolated outside the harness rather than guessed at: pairing with no gap
# failed twice, pairing after an intervening `discover commissionables`
# succeeded, and pairing after a bare 15 s settle with no browse at all
# succeeded too, which is what rules out the browse and names elapsed time
# as the variable. A settle, not a retry: a retry pays chip-tool's 120 s
# pairing timeout again before it helps, and the second attempt would race
# the same republication.
def step_2_7_second_fabric(ctx):
    """TESTING.md 2.7: fabric accounting through an additional window.
    The second controller pairs over the network (the device is already
    on WiFi; verb pinned by T3 Task 1 finding (b): `onnetwork-long`), and
    the DE24 pair (one +MTEVT:4 per +MTEVT:0, after complete) must hold
    for host-opened windows exactly as it does for the boot window.

    The SECOND_WINDOW_SETTLE_S wait above the pairing is load-bearing and
    must stay BEFORE the invocation; see that constant's comment."""
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
    (ctx.sleeper or time.sleep)(SECOND_WINDOW_SETTLE_S)
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
    # 1dd02a2 (StartUpOnOff null instead of esp-matter's boot-Off
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
    boot-looped the device forever. Since the B63 fix (1dd02a2,
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


def step_2_13_thread_reboot_reattach(ctx):
    """0.11.0 task 3, reworked TWICE: F1 moved the reboot off
    `AT+MTRESET` (which factory-resets and erases the Thread dataset)
    onto the SWD path step 2.8 uses; the bench then found this row's own
    premise about observing an EVENT here was unreachable at all (bench
    defect C). The event mask is RAM-only and resets to the default on
    every boot (`AT_MT_SPEC.md` 3.11), so a mask set before an SWD reset
    is gone when the device reappears, and setting it right after
    `+MTREADY` is STILL too late: the Thread stack reattaches during
    `esp_matter::start()`, before `mt_at_start()` brings the AT link up
    at all, so every reattach transition on a reboot is over before a
    host could ever subscribe. Measured on the rig: mask set 0.05 s after
    `+MTREADY`, and the very first `AT+MTTHREAD?` at 0.09 s already read
    `REED`, `attached`=`1`. (The one post-reboot role event that DOES
    exist, a delayed REED-to-ROUTER promotion, was measured at +86.6 s:
    real, but not a contract-timed transition any row can wait on. The
    actual event OBSERVATION for this round now lives in
    `step_2_3_commission` instead, during the live commissioning window,
    where the bench found the transitions really do happen:
    `+MTEVT:28` carrying `UNASSIGNED` then `REED`.)

    So this row keeps only what the bench proved DOES hold across a
    non-factory reboot, with no event-mask dependency at all: the SWD
    reset (`ctx.relink(lambda: swd_reset(ctx.swd_runner))`, the exact
    mechanism step 2.8 uses, never touching Matter's factory-reset path,
    HARDWARE-VERIFIED), the fabric-survived guard (`AT+MTFABRICS?` reads
    `1` straight after the reboot: F1's self-validating guard,
    HARDWARE-VERIFIED), and the ends-attached assertion (`AT+MTTHREAD?`
    reads back a role in `MT_THREAD_ATTACHED_ROLES` with `<attached>` =
    `1`, HARDWARE-VERIFIED). The ends-attached read is a short bounded
    poll (up to 5 attempts, 1 s apart) rather than one immediate read:
    the sub-100-ms reattach speed measured on the rig means one read
    would very likely already be enough, but polling costs nothing and
    buys headroom against bench timing variance this round has already
    been burned by twice.

    Thread-only: on a WiFi image there is no Thread mesh to reattach to,
    so this row is architecturally not applicable, and it reports through
    `Suite.not_applicable()` rather than `Suite.skip()`: bench defect C's
    controller ruling was that conflating "not applicable" with
    "precondition broken" made every clean WiFi Phase 2 run exit 1 with
    zero failures, which made the exit code useless as a WiFi pass
    signal. See `Suite.not_applicable`'s own docstring for the full
    reasoning; only `main()`'s exit code changes meaning, nothing about
    what gets recorded or printed."""
    link, s = ctx.link, ctx.suite
    name = "2.13 Thread reattach survives a non-factory reboot"
    if ctx.transport != "THREAD":
        s.not_applicable(name, "WIFI transport: no Thread mesh to "
                         "reattach to")
        return
    ok, detail = ctx.relink(lambda: swd_reset(ctx.swd_runner))
    if not s.check("2.13 SWD reset, port back", ok, tag="P2"):
        raise StepAbort("bridge did not come back after SWD reset: %s"
                        % detail)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("2.13 +MTREADY within 15 s", ready is not None,
                   tag="P2"):
        raise StepAbort("device did not come back after the reset")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    if not s.check("2.13 fabric survived the reboot (self-validating "
                   "guard, F1)", res == 0 and lines == ["+MTFABRICS:1"],
                   tag="P2"):
        raise StepAbort("fabric did not survive the reboot: this row's "
                        "reboot mechanism must not touch Matter's "
                        "factory-reset path")
    attached_ok = False
    for attempt in range(5):
        res, lines = cmd_retry(link, "AT+MTTHREAD?")
        m = (re.match(r"\+MTTHREAD:([A-Z_]+|\d+),([01]),", lines[0])
             if res == 0 and lines else None)
        attached_ok = (m is not None and
                      m.group(1) in MT_THREAD_ATTACHED_ROLES and
                      m.group(2) == "1")
        if attached_ok or attempt == 4:
            break
        time.sleep(1.0)
    s.check("2.13 AT+MTTHREAD? reports an attached role after reattach",
            attached_ok, tag="P2")


def step_2_14_transport_switch(ctx):
    """P2 (AT_MT_SPEC.md 3.12.1, 3.12.2), task 4: the combined image can
    reach the reflash-induced mismatch state 3.12.1 verified against a
    real reflash between the WiFi and Thread images, without a reflash
    at all. AT+MTTRANSPORT= followed by a reboot changes the active
    stack exactly as a reflash would, and the mismatch detection,
    +MTEVT:27 and AT+MTNET?'s <mismatch> flag behave the same either
    way: both routes end at the identical condition, a fabric CHIP has
    not provisioned on the transport that is now active.

    The reboot that applies the switch is deliberately NOT AT+MTRESET,
    the same review finding F1 already settled for step 2.13, and for
    the identical reason: AT+MTRESET calls esp_matter::factory_reset(),
    which erases the fabric as part of the command itself, before the
    device even reboots (step 2.11's own "fabrics 0 after MTRESET"
    check, made straight after +MTREADY, is the proof this happens
    synchronously). Erasing the fabric here would erase the one thing
    the mismatch condition needs (fabric_count > 0 && the active
    transport unprovisioned), so +MTEVT:27 would never fire and this
    row would test nothing while still reporting PASS on every other
    line -- the exact "check whose input can never arrive" shape B267
    already cost this harness once. The reboot instead uses
    ctx.relink(lambda: swd_reset(ctx.swd_runner)), the identical
    non-factory mechanism steps 2.8 and 2.13 use.

    Placement: runs before 2.11, not after, for the same reason 2.13
    does (its own docstring, and TESTING.md's "Runs before 2.11, not
    after"): 2.11 factory-resets the device by its end (fabrics -> 0),
    and this row needs a live commissioned fabric to mismatch in the
    first place. It also restores the active transport back to
    ctx.transport before returning: pairing_argv() and every controller
    call after this point key off ctx.transport, which is fixed for the
    whole run and is the transport the bench (WiFi AP or Thread border
    router) is actually wired for, so leaving the switch applied would
    strand 2.11's re-commissioning, not just this row.

    Combined image only: build_wifi and build_thread never register
    AT+MTTRANSPORT (spec 3.12.2), so this reports itself not_applicable
    rather than skip (the same three-bucket rule step 2.13 follows), or
    a clean single-transport Phase 2 run would exit nonzero with zero
    failures."""
    link, s = ctx.link, ctx.suite
    name = "2.14 transport switch"
    if ctx.image != "combined":
        s.not_applicable(name, "single-transport image: AT+MTTRANSPORT "
                         "does not exist")
        return

    res, lines = cmd_retry(link, "AT+MTTRANSPORT?")
    if not s.check("2.14 precondition: AT+MTTRANSPORT? answers OK",
                   res == 0 and bool(lines), tag="P2"):
        raise StepAbort("AT+MTTRANSPORT? failed on the combined image")
    active, stored = lines[0].split(":", 1)[1].split(",")
    if not s.check("2.14 precondition: active and stored agree with "
                   "ctx.transport", active == stored == ctx.transport,
                   tag="P2"):
        raise StepAbort("device not in the expected pre-switch state")
    other = "THREAD" if active == "WIFI" else "WIFI"

    s.check("2.14 AT+MTTRANSPORT=%s -> OK" % other,
            link.command("AT+MTTRANSPORT=%s" % other)[0] == 0, tag="P2")
    res, lines = link.command("AT+MTTRANSPORT?")
    s.check("2.14 stored transport changed to %s" % other,
            res == 0 and lines
            and lines[0].split(":", 1)[1].split(",")[1] == other, tag="P2")
    link.drain(0.3)
    ok, detail = ctx.relink(lambda: swd_reset(ctx.swd_runner))
    if not s.check("2.14 SWD reset, port back", ok, tag="P2"):
        raise StepAbort("bridge did not come back after the switch: %s"
                        % detail)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("2.14 +MTREADY on the mismatched boot",
                   ready is not None, tag="P2"):
        raise StepAbort("device did not come back after the switch")

    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    if not s.check("2.14 fabric survived the switch (self-validating "
                   "guard, F1)", res == 0 and lines == ["+MTFABRICS:1"],
                   tag="P2"):
        raise StepAbort("no fabric to mismatch: the reboot erased it")
    res, lines = cmd_retry(link, "AT+MTTRANSPORT?")
    s.check("2.14 active transport is now %s" % other,
            res == 0 and lines
            and lines[0].split(":", 1)[1].split(",")[0] == other, tag="P2")
    s.check("2.14 +MTEVT:27 raised on the mismatched boot",
            link.await_urc(r"\+MTEVT:27$", timeout=20.0) is not None,
            tag="P2")
    res, lines = cmd_retry(link, "AT+MTNET?")
    s.check("2.14 AT+MTNET? reports the mismatch",
            res == 0 and lines and lines[0].split(",")[-1] == "1",
            tag="P2")

    # Restore before returning: 2.11's re-commissioning (pairing_argv
    # keys off ctx.transport) and every step after this one need the
    # device back on the transport the bench is actually wired for.
    s.check("2.14 restore: AT+MTTRANSPORT=%s -> OK" % ctx.transport,
            link.command("AT+MTTRANSPORT=%s" % ctx.transport)[0] == 0,
            tag="P2")
    link.drain(0.3)
    ok, detail = ctx.relink(lambda: swd_reset(ctx.swd_runner))
    if not s.check("2.14 restore: SWD reset, port back", ok, tag="P2"):
        raise StepAbort("bridge did not come back after the restore: %s"
                        % detail)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    if not s.check("2.14 restore: +MTREADY", ready is not None, tag="P2"):
        raise StepAbort("device did not come back after the restore")
    res, lines = cmd_retry(link, "AT+MTFABRICS?")
    s.check("2.14 restore: fabric still present",
            res == 0 and lines == ["+MTFABRICS:1"], tag="P2")
    res, lines = cmd_retry(link, "AT+MTNET?")
    s.check("2.14 restore: mismatch cleared",
            res == 0 and lines and lines[0].split(",")[-1] == "0",
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
    endpoint::create() stops the rebuild there, nothing after it gets
    built, and the already-created prefix stays live for that boot
    (B247), so no entry is skipped and endpoint ids never slide;
    CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT must cover
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
    devtypes = [dt for _slot, dt in ctx.composition]
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
               for i, (slot, dt) in enumerate(ctx.composition)]
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
    and is Task 5's job. The composed appliance round's AT+MTALARM
    migration adds a 41st row of its own here (=<alarm ep>,0,0, TESTING.md
    6.2): it needs the real smoke/CO alarm slot, so it cannot ride Phase
    1's half of the migration (register_phase1_t7_negative).

    MTALARM's SmokeState-Warning row is followed by an unscored clear so
    ep5 hands step_3_3 a clean ExpressedState, the same
    each-step-establishes-the-next-step's-preconditions discipline Phase
    2 follows.

    The AT+MTROW family's three round-trip rows used to live here and are
    now step_3_2b_row_round_trip, because they are the ONLY rows in this
    step that need endpoint 28. Keeping them here declared max_ep 28 for
    the whole step, so any capped run (the combined image, whose measured
    WiFi-active cap is 20) lost all 44 checks to reach 3 of them. The
    split costs no bench time and leaves this step's highest endpoint at
    11, the TemperatureNumber-variant slot."""
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
    # The composed appliance round's migration row (TESTING.md 6.2): field
    # 0 now passes cmd_mtalarm()'s union gate (it is a legal
    # RefrigeratorAlarm bit), and on an endpoint that DOES carry
    # SmokeCoAlarm the ExpressedState-is-derived rejection lives in the
    # bridge's smoke branch (mt_matter_alarm_set(), main.cpp), still
    # +MTERR:1. Phase 1 carries the other half of the migration (=1,0,0
    # -> +MTERR:3 on the single-light rig, register_phase1_t7_negative).
    c("MTALARM=<alarm ep>,0,0 -> +MTERR:1 (ExpressedState derived, "
      "rejected in the bridge's smoke branch)",
      err("AT+MTALARM=5,0,0", 1))
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


def step_3_2b_row_round_trip(ctx):
    """AT+MTROW / AT+MTROWAPPLY / AT+MTROWGET (energy round C2, task 13):
    ep 28 is the EVSE, variant 0 (SOC feature). Every lookup-error and
    value-boundary row for this family lives in Phase 1
    (register_phase1_t12_negative), using ep 1/ep 99 as the "any
    endpoint"/"unknown endpoint" stand-ins, since AT+MTROW's own staging
    never checks ep against the composition (mt_rows.c's file comment: it
    is pure RAM). This is the one round trip Phase 1 cannot stage: a real
    commit reaching a real EnergyEvse store, and a real readback of it.
    The full-schedule, merge-by-day and SOC-variant-rule proofs on this
    same endpoint are step_3_27_evse's job; the unscored clear below
    leaves the schedule EMPTY again before handing off, the MTALARM
    precedent in step_3_2_grammar.

    Split out of step_3_2_grammar for the 1.0.0 release gate: these three
    rows are the only ones in that step that address endpoint 28, and
    carrying them there forced max_ep 28 on all 44 of its checks, so a
    capped run lost 41 checks that its own composition can serve. This
    step is the part that genuinely cannot run below 28 endpoints."""
    link, s = ctx.link, ctx.suite

    def c(name, fn):
        s.check("3.2b %s" % name, fn(link), tag="P3")

    def ok(cmd, **kw):
        return expect_ok(cmd, **kw)

    c("MTROW=28,1,0,2,480,80,25000000 -> OK (staging a real target; SOC "
      "feature present on variant 0, so SoC is supplied)",
      ok("AT+MTROW=28,1,0,2,480,80,25000000"))
    c("MTROWAPPLY=28,1,1 -> OK (commits to the real EnergyEvse store)",
      ok("AT+MTROWAPPLY=28,1,1"))
    c("MTROWGET=28,1 -> the committed row read back verbatim",
      ok("AT+MTROWGET=28,1", line_re=r"\+MTROW:0,1,2,480,80,25000000"))
    link.command("AT+MTROWAPPLY=28,1,0")  # unscored: clear before 3.27


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
    recompute (fix 0c1f3c0, AT_MT_SPEC.md 3.22's DEFECT note). Each
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
    (wifi-devicetypes.json at ca72791, thread-devicetypes.json at
    4836e33, both 165/165): the WiFi run and the Thread run each
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


def step_3_15_rvc(ctx):
    """RVC + Microwave batch, harness task: the Robotic Vacuum Cleaner
    slot (PHASE3_COMPOSITION 12, endpoint 12, matching AT_MT_SPEC.md
    3.20.1/3.21's own worked examples verbatim so every id in this step is
    cross-checked against a line in the spec, not just derived here).

    RvcRunMode (cluster 84/0x54) and RvcCleanMode (85/0x55) are staged over
    the cluster-aware AT+MTMODES form (spec 3.20.1): RvcRunMode's command
    mixes an explicit kIdle tag (0x4000) with a tag-0 default (which must
    resolve to kCleaning, 0x4001, since it is not the first mode); the
    controller read-back on cluster 0x54 asserts both the labels
    (parse_string_list, already proven against ModeSelect's identical
    `Label:` field) and the tags (parse_mode_tag_values, INFERENCE, see its
    own docstring). RvcCleanMode is staged too (both modes tag-0, which
    must both resolve to kVacuum, 0x4001, unlike RvcRunMode's per-position
    rule) but not read back a second time: 3.20.1's tag-0 table is the same
    mechanism on a second cluster, and 3.16's "asserting labels and tag
    values on cluster 0x54" is explicit about which cluster carries the
    read-back check. RvcCleanMode's ChangeToMode is never invoked at all
    in this step (no allow/deny round trip on cluster 85), so it cannot
    carry the same-mode short-circuit disease the RvcRunMode deny below
    was found to have: there is no repeated-value sequence to have it in.

    ChangeToMode is adjudicated for RvcRunMode (spec 3.20.1, unlike
    MicrowaveOvenMode, which has none): allow and deny both forward
    cluster 84 command 0 with the requested mode as the single payload
    field (the same one-field shape chime's PlayChimeSound uses), and the
    verdict decides the wire ChangeToModeResponse's own `status` field (0
    kSuccess / 2 kGenericFailure, spec 3.20.1), parsed by
    parse_change_to_mode_status (INFERENCE, see its own docstring) since
    that response is a StatusIB Success either way -- parse_status's own
    branches would find nothing here. The deny MUST target a mode
    different from whatever the allow just installed as CurrentMode: see
    the comment at the deny invoke below (Task 9 bench finding,
    task-9-report.md section 6).

    RvcOperationalState (97/0x61) registers Pause (0), Resume (3) and
    GoHome (0x80) (spec 3.17/3.21): all three are exercised both ways
    (allow and deny), the same verdict-is-wire-response shape step_3_9's
    washer uses (ErrorStateID 0 kNoError / 2 kUnableToCompleteOperation),
    from base states (Running/Paused) the base OperationalState::Delegate
    already treats as compatible for Pause/Resume, and Running (not
    short-circuited) for GoHome. AT+MTOPSTATE's RVC-only states (0x40
    SeekingCharger, 0x41 Charging, 0x42 Docked) are then each set and read
    back controller-side in turn.

    Two washer-ep negative membership rows from the cluster-aware
    AT+MTOPSTATE table (AT_MT_SPEC.md 3.21's example, TESTING.md 6.2) ride
    along here since they need PHASE3_COMPOSITION's washer slot (7) and a
    known RVC-only value, and this is the step that first has both: `0x40`
    and its decimal alias `64` are both legal union members
    (cmd_mtopstate()'s own gate passes them) but illegal on the washer's
    plain OperationalState cluster, so mt_matter_opstate_set() narrows both
    to +MTERR:1, distinct from the +MTERR:3 "no such cluster at all" row
    step_3_2_grammar already covers.

    GoHome's two no-forward server guards (spec 3.21, the design spec
    section 9 trace) are pinned exactly like step_3_9's in-state guard: a
    GoHome from Docked reaches `operational-state-server.cpp`'s own guard
    before HearthRvcOpStateDelegate ever sees it, answering ErrorStateID 3
    (kCommandInvalidInState) with no +MTCMD raised at all; a GoHome from
    SeekingCharger is the cluster's own already-there no-op, answering
    ErrorStateID 0 with, again, no forward. Both are proven with a
    synchronous chip.run() (no CmdResponder thread needed), the same
    reasoning step_3_9's in-state guard and step_3_10's notify-only
    self-test use: nothing blocks a verdict that is never asked for.

    State is restored to 0 (Stopped, the state RvcOperationalState shares
    with the base cluster) at the end, so a later step or the finally
    restore does not inherit RVC-only state."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    # Cluster-aware AT+MTOPSTATE negative membership, washer ep (7):
    # 0x40/64 are legal union members but illegal on the plain
    # OperationalState cluster (spec 3.21's own worked example).
    s.check("3.15 AT+MTOPSTATE=<washer ep>,0x40 -> +MTERR:1 (RVC-only "
            "state, not legal on the base cluster)",
            expect_err("AT+MTOPSTATE=7,0x40", 1)(link), tag="P3")
    s.check("3.15 AT+MTOPSTATE=<washer ep>,64 -> +MTERR:1 (decimal alias "
            "of 0x40, same rejection)",
            expect_err("AT+MTOPSTATE=7,64", 1)(link), tag="P3")

    # --- modes (spec 3.20.1) ---
    res, _ = link.command(
        'AT+MTMODES=12,84,0,16384,"Idle",1,0,"Cleaning"')
    s.check("3.15 RvcRunMode staged (explicit kIdle tag + tag-0 default) "
            "-> OK", res == 0, tag="P3")
    res, _ = link.command('AT+MTMODES=12,85,0,0,"Vacuum",1,0,"Mop"')
    s.check("3.15 RvcCleanMode staged (tag-0 default on both modes) -> OK",
            res == 0, tag="P3")

    rc, out = chip.run(["rvcrunmode", "read", "supported-modes", node,
                        "12"], timeout=30)
    s.check("3.15 RvcRunMode SupportedModes labels verbatim",
            rc == 0 and parse_string_list(out) == ["Idle", "Cleaning"],
            tag="P3")
    s.check("3.15 RvcRunMode ModeTags: explicit kIdle (0x4000), defaulted "
            "kCleaning (0x4001)",
            rc == 0 and parse_mode_tag_values(out) == [0x4000, 0x4001],
            tag="P3")

    # --- ChangeToMode: allow and deny (spec 3.20.1) ---
    handle = invoke_chip(ctx, ["rvcrunmode", "change-to-mode", "1", node,
                              "12"], timeout=30)
    fwd = responder.expect(cluster=84, command=0, verdict=1, payload=1,
                           timeout=5.0)
    s.check("3.15 ChangeToMode allow: forward answered, payload == mode 1",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.15 ChangeToMode allow: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.15 ChangeToMode allow: status 0 (kSuccess)",
            parse_change_to_mode_status(out) == 0, tag="P3")

    # The deny MUST ask for a mode different from CurrentMode. Asking
    # again for mode 1 (what the allow above just installed) hits
    # mode-base-server.cpp:401-410's own same-mode short-circuit ("If the
    # NewMode field is the same as the value of the CurrentMode attribute
    # the ChangeToModeResponse command SHALL have the Status field set to
    # Success"): the SDK answers kSuccess itself, before
    # HandleChangeToMode() is ever called, so no +MTCMD is raised at all.
    # Found live on the WiFi bench (task-9-report.md section 6, both
    # failing checks traced to this exact line); mode 0 is the other mode
    # this step already declared over AT+MTMODES above, so no extra
    # staging is needed to pick a legitimately different, still-supported
    # target.
    handle = invoke_chip(ctx, ["rvcrunmode", "change-to-mode", "0", node,
                              "12"], timeout=30)
    fwd = responder.expect(cluster=84, command=0, verdict=0, payload=0,
                           timeout=5.0)
    s.check("3.15 ChangeToMode deny: forward answered, payload == mode 0",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.15 ChangeToMode deny: chip-tool exits 0 (Success at the "
            "StatusIB level; the verdict lives in the response struct)",
            rc == 0, tag="P3")
    s.check("3.15 ChangeToMode deny: status 2 (kGenericFailure)",
            parse_change_to_mode_status(out) == 2, tag="P3")

    rc, out = chip.run(["rvcrunmode", "read", "current-mode", node, "12"],
                       timeout=30)
    s.check("3.15 ChangeToMode deny: CurrentMode still 1 (deny changed "
            "nothing)", rc == 0 and parse_int_attr(out) == 1, tag="P3")

    # --- RvcOperationalState: Pause/Resume/GoHome, both verdicts ---
    def allow(name, verb, command, precondition, next_state):
        res, _ = link.command("AT+MTOPSTATE=12,%d" % precondition)
        s.check("3.15 %s allow precondition -> OK" % name, res == 0,
                tag="P3")
        handle = invoke_chip(ctx, ["rvcoperationalstate", verb, node, "12"],
                             timeout=30)
        fwd = responder.expect(cluster=97, command=command, verdict=1,
                               timeout=5.0)
        s.check("3.15 %s +MTCMD forward answered allow" % name,
                fwd is not None, tag="P3")
        rc, out = handle.join(30)
        s.check("3.15 %s chip-tool exits 0 (allow)" % name, rc == 0,
                tag="P3")
        s.check("3.15 %s ErrorStateID 0 (kNoError) on allow" % name,
                parse_status(out) == 0, tag="P3")
        res, _ = link.command("AT+MTOPSTATE=12,%d" % next_state)
        s.check("3.15 host reports %s transition -> OK" % name, res == 0,
                tag="P3")

    def deny(name, verb, command, precondition):
        res, _ = link.command("AT+MTOPSTATE=12,%d" % precondition)
        s.check("3.15 %s deny precondition -> OK" % name, res == 0,
                tag="P3")
        handle = invoke_chip(ctx, ["rvcoperationalstate", verb, node, "12"],
                             timeout=30)
        fwd = responder.expect(cluster=97, command=command, verdict=0,
                               timeout=5.0)
        s.check("3.15 %s +MTCMD forward answered deny" % name,
                fwd is not None, tag="P3")
        rc, out = handle.join(30)
        s.check("3.15 %s chip-tool exits 0 (deny; Success at the StatusIB "
                "level, the verdict lives in ErrorStateID)" % name,
                rc == 0, tag="P3")
        s.check("3.15 %s ErrorStateID 2 (kUnableToCompleteOperation) on "
                "deny" % name, parse_status(out) == 2, tag="P3")

    allow("pause", "pause", 0, 1, 2)             # Running -> Paused
    allow("resume", "resume", 3, 2, 1)           # Paused -> Running
    allow("go-home", "go-home", 0x80, 1, 0x40)   # Running -> SeekingCharger
    deny("pause", "pause", 0, 1)                 # Running, denied
    deny("resume", "resume", 3, 2)               # Paused, denied
    deny("go-home", "go-home", 0x80, 1)          # Running, denied

    # --- AT+MTOPSTATE 0x40/0x41/0x42, each read back controller-side ---
    for state, name in ((0x40, "SeekingCharger"), (0x41, "Charging"),
                        (0x42, "Docked")):
        res, _ = link.command("AT+MTOPSTATE=12,%d" % state)
        s.check("3.15 AT+MTOPSTATE=12,0x%02X (%s) -> OK" % (state, name),
                res == 0, tag="P3")
        rc, out = chip.run(["rvcoperationalstate", "read",
                            "operational-state", node, "12"], timeout=30)
        s.check("3.15 controller reads OperationalState 0x%02X (%s)"
                % (state, name),
                rc == 0 and parse_int_attr(out) == state, tag="P3")

    # --- GoHome no-forward server guards (spec 3.21) ---
    res, _ = link.command("AT+MTOPSTATE=12,0x42")
    s.check("3.15 guard precondition: Docked -> OK", res == 0, tag="P3")
    link.drain(0.3)
    rc, out = chip.run(["rvcoperationalstate", "go-home", node, "12"],
                       timeout=30)
    s.check("3.15 GoHome from Docked: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.15 GoHome from Docked: ErrorStateID 3 "
            "(kCommandInvalidInState)", parse_status(out) == 3, tag="P3")
    s.check("3.15 GoHome from Docked: no +MTCMD raised (server guard, "
            "delegate never reached)",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")

    res, _ = link.command("AT+MTOPSTATE=12,0x40")
    s.check("3.15 guard precondition: SeekingCharger -> OK", res == 0,
            tag="P3")
    link.drain(0.3)
    rc, out = chip.run(["rvcoperationalstate", "go-home", node, "12"],
                       timeout=30)
    s.check("3.15 GoHome from SeekingCharger: chip-tool exits 0", rc == 0,
            tag="P3")
    s.check("3.15 GoHome from SeekingCharger: ErrorStateID 0 (kNoError, "
            "already-seeking no-op)", parse_status(out) == 0, tag="P3")
    s.check("3.15 GoHome from SeekingCharger: no +MTCMD raised (cluster's "
            "own no-op, delegate never reached)",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")

    # --- restore ---
    res, _ = link.command("AT+MTOPSTATE=12,0")
    s.check("3.15 state restored to 0 (Stopped) at step end", res == 0,
            tag="P3")


def step_3_16_microwave(ctx):
    """RVC + Microwave batch, harness task: the Microwave Oven slot
    (PHASE3_COMPOSITION 13, endpoint 13, matching AT_MT_SPEC.md 3.20.1's
    own worked example). MicrowaveOvenMode (94/0x5E) has no ChangeToMode
    command at all (spec 3.20.1): its mode is selected only through
    SetCookingParameters' cookMode field, so this step's mode coverage is
    staging plus a controller read-back, not a change round trip the way
    step_3_15's RvcRunMode gets.

    SetCookingParameters (cluster 95/0x5F command 0, spec 3.17) is the
    first four-field consumer the widened CmdResponder exists for:
    payload order is cookMode, cookTime, power, startAfter (spec 3.17's
    own field list), all four present on this firmware's actual wire
    traffic per Task 4's trace (`power` always resolves because this
    firmware never enables PowerInWatts, and `cookMode`/`cookTime`/
    `startAfter` are resolved by the SDK before the delegate ever runs).
    Unlike RvcRunMode's ChangeToMode (step_3_15's own same-mode
    short-circuit finding, Task 9), SetCookingParameters carries no
    "unchanged value" short-circuit of any kind: read against the pinned
    SDK (microwave-oven-control-server.cpp's HandleSetCookingParameters(),
    the whole VerifyOrExit chain from opState through the power/watt
    branches), every check is a range/support/state check, never a
    comparison against the delegate's current CookTime/PowerSetting, so
    the delegate is always called once validation passes. The allow and
    deny below already use genuinely different values (90/80 vs 60/50)
    for this reason, and that is confirmed safe by source, not by
    accident.
    Allow reads CookTime/PowerSetting back from the controller afterward
    (spec 3.17: both are Instance/delegate-owned, never ember-backed, so
    only a controller read reaches them). Deny is a bare StatusIB failure
    (spec 3.17: HandleSetCookingParametersCallback returns a Status
    directly, no GenericOperationalError indirection the way
    OperationalState's family uses), so it is pinned the T5-vacuousness
    way (task-7-report.md section 6): rc != 0 AND the wire status is
    checked to be exactly 0x1 (Failure), not just "some nonzero exit",
    reusing parse_status's existing StatusIB branch (no new parser needed
    here, unlike ChangeToModeResponse's embedded status field).

    AddMoreTime (command 1, spec 3.17) carries a single payload field, but
    it is NOT the delta the controller sent (`TimeToAdd`): it is the
    server-computed absolute `finalCookTimeSec`, so the assertion is
    against CookTime-after-add, not the argument chip-tool was given.

    The washer-rule opstate spot check closes the loop on spec 3.21's own
    claim that Microwave Oven wires "the same plain OperationalState
    cluster, not a derived one": a Pause from Stopped answers ErrorStateID
    3 with no forward, the identical in-state guard step_3_9 already
    proved for the washer, now observed on the microwave's own endpoint.

    State is restored to 0 (Stopped) at the end; the opstate spot check
    already leaves it there (Pause from Stopped never changes the state),
    but the explicit command below makes that fact resilient to a later
    change in what the spot check itself does."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    # --- mode list (spec 3.20.1) ---
    res, _ = link.command('AT+MTMODES=13,94,0,0,"Normal"')
    s.check("3.16 MicrowaveOvenMode staged (tag-0 default) -> OK",
            res == 0, tag="P3")
    rc, out = chip.run(["microwaveovenmode", "read", "supported-modes",
                        node, "13"], timeout=30)
    s.check("3.16 MicrowaveOvenMode SupportedModes label verbatim",
            rc == 0 and parse_string_list(out) == ["Normal"], tag="P3")
    s.check("3.16 MicrowaveOvenMode ModeTags: defaulted kNormal (0x4000)",
            rc == 0 and parse_mode_tag_values(out) == [0x4000], tag="P3")

    # --- SetCookingParameters: allow, four-field payload ---
    handle = invoke_chip(ctx, ["microwaveovencontrol",
                              "set-cooking-parameters", node, "13",
                              "--CookMode", "0", "--CookTime", "90",
                              "--PowerSetting", "80",
                              "--StartAfterSetting", "0"], timeout=30)
    fwd = responder.expect(cluster=95, command=0, verdict=1,
                           payload=[0, 90, 80, 0], timeout=5.0)
    s.check("3.16 SetCookingParameters allow: forward answered, all four "
            "fields present (cookMode=0, cookTime=90, power=80, "
            "startAfter=0)", fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.16 SetCookingParameters allow: chip-tool exits 0", rc == 0,
            tag="P3")

    rc, out = chip.run(["microwaveovencontrol", "read", "cook-time", node,
                        "13"], timeout=30)
    s.check("3.16 controller reads CookTime 90",
            rc == 0 and parse_int_attr(out) == 90, tag="P3")
    rc, out = chip.run(["microwaveovencontrol", "read", "power-setting",
                        node, "13"], timeout=30)
    s.check("3.16 controller reads PowerSetting 80",
            rc == 0 and parse_int_attr(out) == 80, tag="P3")

    # --- SetCookingParameters: deny, bare StatusIB Failure ---
    handle = invoke_chip(ctx, ["microwaveovencontrol",
                              "set-cooking-parameters", node, "13",
                              "--CookMode", "0", "--CookTime", "60",
                              "--PowerSetting", "50",
                              "--StartAfterSetting", "0"], timeout=30)
    fwd = responder.expect(cluster=95, command=0, verdict=0,
                           payload=[0, 60, 50, 0], timeout=5.0)
    s.check("3.16 SetCookingParameters deny: forward answered", fwd is not None,
            tag="P3")
    rc, out = handle.join(30)
    s.check("3.16 SetCookingParameters deny: chip-tool reports Failure "
            "(rc != 0)", rc != 0, tag="P3")
    s.check("3.16 SetCookingParameters deny: wire status 0x1 (Failure, "
            "which failure WHICH is pinned, not just any nonzero exit)",
            parse_status(out) == 0x1, tag="P3")
    rc, out = chip.run(["microwaveovencontrol", "read", "cook-time", node,
                        "13"], timeout=30)
    s.check("3.16 CookTime unchanged by the deny (still 90)",
            rc == 0 and parse_int_attr(out) == 90, tag="P3")

    # --- AddMoreTime: allow, single-field ABSOLUTE finalCookTimeSec ---
    handle = invoke_chip(ctx, ["microwaveovencontrol", "add-more-time",
                              "30", node, "13"], timeout=30)
    fwd = responder.expect(cluster=95, command=1, verdict=1, payload=120,
                           timeout=5.0)
    s.check("3.16 AddMoreTime allow: forward answered, payload == 120 "
            "(the server-computed absolute finalCookTimeSec, 90+30, NOT "
            "the 30 s delta chip-tool sent)", fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.16 AddMoreTime allow: chip-tool exits 0", rc == 0, tag="P3")
    rc, out = chip.run(["microwaveovencontrol", "read", "cook-time", node,
                        "13"], timeout=30)
    s.check("3.16 controller reads CookTime 120 after AddMoreTime",
            rc == 0 and parse_int_attr(out) == 120, tag="P3")

    # --- washer-rule opstate spot check (spec 3.21) ---
    res, _ = link.command("AT+MTOPSTATE=13,0")
    s.check("3.16 opstate spot check precondition: Stopped -> OK",
            res == 0, tag="P3")
    link.drain(0.3)
    rc, out = chip.run(["operationalstate", "pause", node, "13"],
                       timeout=30)
    s.check("3.16 opstate spot check: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.16 opstate spot check: ErrorStateID 3 "
            "(kCommandInvalidInState, same washer in-state guard)",
            parse_status(out) == 3, tag="P3")
    s.check("3.16 opstate spot check: no +MTCMD raised",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")

    # --- restore ---
    res, _ = link.command("AT+MTOPSTATE=13,0")
    s.check("3.16 state restored to 0 (Stopped) at step end", res == 0,
            tag="P3")


def step_3_17_composed_fridge(ctx):
    """Composed appliance round, task 6: the composed refrigerator
    (PHASE3_COMPOSITION slots 14-16: parent 0x0070 at ep 14, a
    TemperatureNumber cabinet at ep 15 and a TemperatureLevel cabinet at
    ep 16, both parented to staging index 13).

    AT+MTEP? line shapes (spec 3.9): a parented entry prints five fields
    (variant unconditionally shown once a parent is present, "never
    without the fourth also present"), an unparented one stays
    byte-identical to before parenting existed. step_3_1 already asserts
    the whole 20-line readback; this step re-asserts the three fridge
    lines verbatim and consecutive so the family's own report names them.

    PartsList/ServerList reads: parse_parts_list (INFERENCE, see its
    docstring) for the plain-integer PartsList; ServerList prints
    `[n]: <id> (<Name>)` through DataModelLogger.h's LogClusterId
    (:162-183, std::to_string(id) + " (" + ClusterIdToText(id) + ")"),
    the same shape parse_accepted_command_list already parses
    (INFERENCE for this new application until Tasks 11/12 confirm it on
    the wire). The Cooler conditional cluster
    (RefrigeratorAndTemperatureControlledCabinetMode, 82/0x52) must be
    present on the parented cabinet 15 and ABSENT on the standalone
    cabinet 11: the cluster set is derived from the parent's device type
    at rebuild time, not encoded in the blob (mt_devtypes.cpp's
    parent-conditional machinery), and this pair of reads is the direct
    wire observation of that derivation.

    Cluster-aware AT+MTMODES (spec 3.20.1; TESTING.md 6.2's <fridge ep>
    row, verbatim first, hex cluster notation): tag 0 resolves to kAuto
    (0x00) for EVERY mode on cluster 0x52, unlike RvcRunMode's
    per-position rule. A second, two-mode staging follows so the
    ChangeToMode round trip has a mode to move to and a different one to
    deny: ModeBase answers a request for the mode already in CurrentMode
    with Success itself, before the delegate is consulted
    (mode-base-server.cpp:401-410, bug B196, found live in the RVC round,
    TESTING.md 8.7), so the deny below MUST target a different mode from
    the one the allow installed; the CurrentMode read-back after it pins
    that the deny changed nothing.

    AT+MTALARM fridge rows (TESTING.md 6.2's cluster-aware table):
    =14,0,1 sets RefrigeratorAlarm (87/0x57) bit 0/DoorOpen and fires
    Notify (SetStateValue writes the attribute AND emits the event,
    refrigerator-alarm-server.cpp:115-149); the controller reads State
    bit 0 set and the Notify event with becameActive bit 0
    (parse_notify_active, INFERENCE). =14,1,1 answers +MTERR:1 (bit 1
    passes the union gate but is not in Supported: the fridge thunk's
    config is mask=1 state=0 supported=1, bit 0 only) and =14,0,2
    answers +MTERR:1 (2 outside 0..1 for a bit value). DoorOpen is
    cleared again at step end so no later step or the finally restore
    inherits an active alarm."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    # --- AT+MTEP? five-field line shape (spec 3.9) ---
    parent_line = "+MTEP:13,14,0x0070"
    cab_lines = ["+MTEP:14,15,0x0071,0,13", "+MTEP:15,16,0x0071,1,13"]
    res, lines = cmd_retry(link, "AT+MTEP?")
    ok_parent = res == 0 and parent_line in lines
    s.check("3.17 AT+MTEP? fridge parent line verbatim (three-field, "
            "unparented shape)", ok_parent, tag="P3")
    idx = lines.index(parent_line) if ok_parent else -1
    s.check("3.17 AT+MTEP? cabinet lines verbatim and consecutive "
            "(five-field: variant then parent index)",
            ok_parent and lines[idx + 1:idx + 3] == cab_lines, tag="P3")

    # --- PartsList: exactly the declared children ---
    rc, out = chip.run(["descriptor", "read", "parts-list", node, "14"],
                       timeout=30)
    s.check("3.17 PartsList of ep 14 is exactly [15, 16]",
            rc == 0 and parse_parts_list(out) == [15, 16], tag="P3")

    # --- parent-conditional Cooler cluster: on 15, NOT on standalone 11 ---
    rc, out = chip.run(["descriptor", "read", "server-list", node, "15"],
                       timeout=30)
    s.check("3.17 cabinet 15 server list carries "
            "RefrigeratorAndTCCMode (82)",
            rc == 0 and 82 in parse_accepted_command_list(out), tag="P3")
    rc, out = chip.run(["descriptor", "read", "server-list", node, "11"],
                       timeout=30)
    servers11 = parse_accepted_command_list(out)
    # bool(servers11) de-vacuouses the absence check: an empty parse
    # (wrong shape, dead read) must fail, not pass by "82 not in []".
    s.check("3.17 standalone cabinet 11 does NOT carry cluster 82",
            rc == 0 and bool(servers11) and 82 not in servers11, tag="P3")

    # --- cluster-aware AT+MTMODES on 0x52 (spec 3.20.1) ---
    s.check('3.17 AT+MTMODES=15,0x52,0,0,"Auto" -> OK (TESTING.md row '
            "verbatim, hex cluster notation)",
            expect_ok('AT+MTMODES=15,0x52,0,0,"Auto"')(link), tag="P3")
    res, _ = link.command('AT+MTMODES=15,0x52,0,0,"Auto",1,0,"Rapid"')
    s.check("3.17 two-mode list staged for the ChangeToMode round trip "
            "-> OK", res == 0, tag="P3")
    rc, out = chip.run(["refrigeratorandtemperaturecontrolledcabinetmode",
                        "read", "supported-modes", node, "15"], timeout=30)
    s.check("3.17 SupportedModes labels verbatim",
            rc == 0 and parse_string_list(out) == ["Auto", "Rapid"],
            tag="P3")
    s.check("3.17 ModeTags: tag-0 default kAuto (0) on every mode",
            rc == 0 and parse_mode_tag_values(out) == [0, 0], tag="P3")

    # --- ChangeToMode: allow and deny (B196: different modes) ---
    handle = invoke_chip(ctx, ["refrigeratorandtemperaturecontrolledcabinetmode",
                              "change-to-mode", "1", node, "15"], timeout=30)
    fwd = responder.expect(cluster=82, command=0, verdict=1, payload=1,
                           timeout=5.0)
    s.check("3.17 ChangeToMode allow: forward answered, payload == mode 1",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.17 ChangeToMode allow: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.17 ChangeToMode allow: status 0 (kSuccess)",
            parse_change_to_mode_status(out) == 0, tag="P3")

    # Deny mode 0, NOT mode 1: the allow just installed 1 as CurrentMode,
    # and a same-mode request never reaches the delegate (B196, see the
    # docstring above).
    handle = invoke_chip(ctx, ["refrigeratorandtemperaturecontrolledcabinetmode",
                              "change-to-mode", "0", node, "15"], timeout=30)
    fwd = responder.expect(cluster=82, command=0, verdict=0, payload=0,
                           timeout=5.0)
    s.check("3.17 ChangeToMode deny: forward answered, payload == mode 0",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.17 ChangeToMode deny: chip-tool exits 0 (Success at the "
            "StatusIB level; the verdict lives in the response struct)",
            rc == 0, tag="P3")
    s.check("3.17 ChangeToMode deny: status 2 (kGenericFailure)",
            parse_change_to_mode_status(out) == 2, tag="P3")
    rc, out = chip.run(["refrigeratorandtemperaturecontrolledcabinetmode",
                        "read", "current-mode", node, "15"], timeout=30)
    s.check("3.17 ChangeToMode deny: CurrentMode still 1 (deny changed "
            "nothing)", rc == 0 and parse_int_attr(out) == 1, tag="P3")

    # --- AT+MTALARM fridge rows (TESTING.md 6.2, cluster-aware table) ---
    res, _ = link.command("AT+MTALARM=14,0,1")
    s.check("3.17 AT+MTALARM=14,0,1 -> OK (DoorOpen set, Notify fired)",
            res == 0, tag="P3")
    rc, out = chip.run(["refrigeratoralarm", "read", "state", node, "14"],
                       timeout=30)
    s.check("3.17 controller reads State bit 0 set",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")
    rc, out = chip.run(["refrigeratoralarm", "read-event", "notify", node,
                        "14"], timeout=30)
    active = parse_notify_active(out)
    s.check("3.17 Notify event observed, becameActive bit 0 set",
            rc == 0 and parse_event_count(out, "Notify") >= 1
            and bool(active) and active[-1] & 1 == 1, tag="P3")
    s.check("3.17 AT+MTALARM=14,1,1 -> +MTERR:1 (bit 1 not in Supported)",
            expect_err("AT+MTALARM=14,1,1", 1)(link), tag="P3")
    s.check("3.17 AT+MTALARM=14,0,2 -> +MTERR:1 (2 outside 0..1 for a "
            "bit value)", expect_err("AT+MTALARM=14,0,2", 1)(link), tag="P3")

    # --- restore ---
    res, _ = link.command("AT+MTALARM=14,0,0")
    s.check("3.17 restore: DoorOpen cleared at step end", res == 0,
            tag="P3")


def step_3_18_oven_cavity(ctx):
    """Composed appliance round, task 6: the oven with one cavity
    (PHASE3_COMPOSITION slots 17-18: parent 0x007B at ep 17, a
    TemperatureNumber cabinet at ep 18 parented to staging index 16, so
    the cabinet gets the HEATER conditional cluster set: OvenMode
    (73/0x49) plus OvenCavityOperationalState (72/0x48),
    mt_cabinet_add_heater(), mt_devtypes.cpp).

    Both cavity clusters are HAND-ROLLED shells (esp-matter ships no
    cluster::oven_mode or cluster::oven_cavity_operational_state
    namespace at all, ARCHITECTURE.md 8.9, the 8.6 disease at its most
    severe grade), so their command entries are observable ONLY on the
    wire; builds and host suites cannot see a missing or extra command
    entry. This step is the regression net for exactly that: Stop (0x01)
    and Start (0x02) must reach the AT link as +MTCMD:<seq>,18,72,1 / ,2
    and adjudicate both ways with the verdict-is-the-response shape
    (ErrorStateID 0 kNoError / 2 kUnableToCompleteOperation, same as
    step_3_9's washer), while Pause (0x00), which
    OperationalState_Oven.xml revision 2 marks <disallowConform/> and the
    shell therefore deliberately does NOT register, must answer StatusIB
    0x81 UNSUPPORTED_COMMAND with NO +MTCMD raised. chip-tool's own
    ovencavityoperationalstate verbs are stop/start only (the pinned
    binary agrees with the XML), so the pause probe goes through
    command-by-id 0 with an empty JSON payload; the `status = 0x81`
    print shape parse_status reads on that path is CONFIRMED (composed
    appliance round, HELD on the bench on both transports, TESTING.md
    8.8).

    OvenMode rows (TESTING.md 6.2): the <cavity ep> staging row verbatim
    (tag 0 resolves to kBake, 0x4000, for every mode on cluster 0x49;
    16386/0x4002 is kGrill given explicitly), read back with labels and
    resolved tags; =18,0x52 answers +MTERR:3 (82 is a legal <cluster> id
    but the cabinet's ModeBase cluster is derived from its PARENT, so
    the same endpoint answers 0x49 and refuses 0x52). ChangeToMode is
    adjudicated allow (mode 1) then deny (mode 0, the B196
    different-mode rule, see step_3_17), with the CurrentMode read-back.

    AT+MTOPSTATE membership rows (TESTING.md 6.2): =18,1 OK with a
    controller read-back (the cavity's plain 0..2 state space is served
    through the shared opstate delegate pool keyed by (ep, cluster));
    =18,0x40 +MTERR:1 (OvenCavityOperationalState derives from
    OperationalState but adds NO derived-number-space states, so 0x40 is
    narrowed to MT_ATTR_ERR_VALUE in the bridge exactly as on the washer,
    spec 3.21). State is restored to 0 (Stopped) at step end."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    # --- PartsList and the cavity's conditional server list ---
    rc, out = chip.run(["descriptor", "read", "parts-list", node, "17"],
                       timeout=30)
    s.check("3.18 PartsList of ep 17 is exactly [18]",
            rc == 0 and parse_parts_list(out) == [18], tag="P3")
    rc, out = chip.run(["descriptor", "read", "server-list", node, "18"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.18 cavity server list carries OvenMode (73) and "
            "OvenCavityOperationalState (72)",
            rc == 0 and 73 in servers and 72 in servers, tag="P3")

    # --- cluster-aware AT+MTMODES on 0x49 (spec 3.20.1) ---
    s.check('3.18 AT+MTMODES=18,0x49 (tag-0 kBake + explicit kGrill) '
            "-> OK",
            expect_ok('AT+MTMODES=18,0x49,0,0,"Bake",1,16386,"Grill"')(link),
            tag="P3")
    s.check("3.18 AT+MTMODES=18,0x52 -> +MTERR:3 (cabinet's ModeBase "
            "cluster is derived from its parent: oven cavity refuses the "
            "fridge cluster)",
            expect_err('AT+MTMODES=18,0x52,0,0,"Auto"', 3)(link), tag="P3")
    rc, out = chip.run(["ovenmode", "read", "supported-modes", node, "18"],
                       timeout=30)
    s.check("3.18 OvenMode SupportedModes labels verbatim",
            rc == 0 and parse_string_list(out) == ["Bake", "Grill"],
            tag="P3")
    s.check("3.18 ModeTags: defaulted kBake (0x4000), explicit kGrill "
            "(0x4002)",
            rc == 0 and parse_mode_tag_values(out) == [0x4000, 0x4002],
            tag="P3")

    # --- ChangeToMode: allow and deny (B196: different modes) ---
    handle = invoke_chip(ctx, ["ovenmode", "change-to-mode", "1", node,
                              "18"], timeout=30)
    fwd = responder.expect(cluster=73, command=0, verdict=1, payload=1,
                           timeout=5.0)
    s.check("3.18 ChangeToMode allow: forward answered, payload == mode 1",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.18 ChangeToMode allow: chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.18 ChangeToMode allow: status 0 (kSuccess)",
            parse_change_to_mode_status(out) == 0, tag="P3")
    handle = invoke_chip(ctx, ["ovenmode", "change-to-mode", "0", node,
                              "18"], timeout=30)
    fwd = responder.expect(cluster=73, command=0, verdict=0, payload=0,
                           timeout=5.0)
    s.check("3.18 ChangeToMode deny: forward answered, payload == mode 0",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.18 ChangeToMode deny: chip-tool exits 0 (verdict lives in "
            "the response struct)", rc == 0, tag="P3")
    s.check("3.18 ChangeToMode deny: status 2 (kGenericFailure)",
            parse_change_to_mode_status(out) == 2, tag="P3")
    rc, out = chip.run(["ovenmode", "read", "current-mode", node, "18"],
                       timeout=30)
    s.check("3.18 ChangeToMode deny: CurrentMode still 1 (deny changed "
            "nothing)", rc == 0 and parse_int_attr(out) == 1, tag="P3")

    # --- OvenCavityOperationalState: Stop/Start, both verdicts ---
    res, _ = link.command("AT+MTOPSTATE=18,0")
    s.check("3.18 known start state: Stopped -> OK", res == 0, tag="P3")

    handle = invoke_chip(ctx, ["ovencavityoperationalstate", "start", node,
                              "18"], timeout=30)
    fwd = responder.expect(cluster=72, command=2, verdict=1, timeout=5.0)
    s.check("3.18 start +MTCMD:<seq>,18,72,2 forward answered allow",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.18 start chip-tool exits 0 (allow)", rc == 0, tag="P3")
    s.check("3.18 start ErrorStateID 0 (kNoError) on allow",
            parse_status(out) == 0, tag="P3")
    res, _ = link.command("AT+MTOPSTATE=18,1")
    s.check("3.18 host reports start transition -> OK", res == 0, tag="P3")

    handle = invoke_chip(ctx, ["ovencavityoperationalstate", "stop", node,
                              "18"], timeout=30)
    fwd = responder.expect(cluster=72, command=1, verdict=0, timeout=5.0)
    s.check("3.18 stop +MTCMD:<seq>,18,72,1 forward answered deny",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.18 stop chip-tool exits 0 (deny)", rc == 0, tag="P3")
    s.check("3.18 stop ErrorStateID 2 (kUnableToCompleteOperation) on "
            "deny", parse_status(out) == 2, tag="P3")
    rc, out = chip.run(["ovencavityoperationalstate", "read",
                        "operational-state", node, "18"], timeout=30)
    s.check("3.18 state unchanged by the denied stop (still Running)",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")

    handle = invoke_chip(ctx, ["ovencavityoperationalstate", "stop", node,
                              "18"], timeout=30)
    fwd = responder.expect(cluster=72, command=1, verdict=1, timeout=5.0)
    s.check("3.18 stop forward answered allow", fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.18 stop chip-tool exits 0 (allow)", rc == 0, tag="P3")
    s.check("3.18 stop ErrorStateID 0 (kNoError) on allow",
            parse_status(out) == 0, tag="P3")
    res, _ = link.command("AT+MTOPSTATE=18,0")
    s.check("3.18 host reports stop transition -> OK", res == 0, tag="P3")

    handle = invoke_chip(ctx, ["ovencavityoperationalstate", "start", node,
                              "18"], timeout=30)
    fwd = responder.expect(cluster=72, command=2, verdict=0, timeout=5.0)
    s.check("3.18 start forward answered deny", fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.18 start chip-tool exits 0 (deny)", rc == 0, tag="P3")
    s.check("3.18 start ErrorStateID 2 (kUnableToCompleteOperation) on "
            "deny", parse_status(out) == 2, tag="P3")

    # --- Pause: disallowConform, the wire-only observability net ---
    link.drain(0.3)
    rc, out = chip.run(["ovencavityoperationalstate", "command-by-id", "0",
                        "{}", node, "18"], timeout=30)
    s.check("3.18 pause: chip-tool reports failure (rc != 0)", rc != 0,
            tag="P3")
    s.check("3.18 pause answers 0x81 UNSUPPORTED_COMMAND "
            "(disallowConform: the hand-rolled shell registers no Pause "
            "entry)", parse_status(out) == 0x81, tag="P3")
    s.check("3.18 pause: no +MTCMD raised (command never dispatched)",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")

    # --- AT+MTOPSTATE membership rows (TESTING.md 6.2) ---
    s.check("3.18 AT+MTOPSTATE=18,1 -> OK (Running, plain 0..2 state "
            "space served)", expect_ok("AT+MTOPSTATE=18,1")(link), tag="P3")
    rc, out = chip.run(["ovencavityoperationalstate", "read",
                        "operational-state", node, "18"], timeout=30)
    s.check("3.18 controller reads OperationalState 1 (Running)",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")
    s.check("3.18 AT+MTOPSTATE=18,0x40 -> +MTERR:1 (no derived-number-"
            "space states on the cavity)",
            expect_err("AT+MTOPSTATE=18,0x40", 1)(link), tag="P3")

    # --- restore ---
    res, _ = link.command("AT+MTOPSTATE=18,0")
    s.check("3.18 state restored to 0 (Stopped) at step end", res == 0,
            tag="P3")


def step_3_19_cook_surface(ctx):
    """Composed appliance round, task 6: the cook surface under the
    cooktop (PHASE3_COMPOSITION slots 19-20: cooktop 0x0078 at ep 19,
    cook surface 0x0077 variant 0 at ep 20, parented to staging index
    18; the cook surface REQUIRES a parent, the one composition rule
    mt_devtype_parent_ok() enforces unconditionally).

    OnOff rides the surface with the OffOnly feature (CookSurface.xml
    1.5.1: a controller may switch a surface off, never on):
    cluster::on_off::create() adds ONLY command::create_off(), so the
    AcceptedCommandList is exactly {Off} and an On invoke answers
    StatusIB 0x81 UNSUPPORTED_COMMAND from the interaction model itself
    (mk_cook_surface(), mt_devtypes.cpp; another entry in the same
    only-observable-on-the-wire family as step_3_18's pause). Off is
    asserted through both observation points: chip-tool exits 0 and
    AT+MTATTR reads OnOff false (6/0x0006 attribute 0, ember-backed,
    unlike the ModeBase/opstate attributes the RVC round proved
    URC-less). The rejected On is followed by the same AT read to pin
    that nothing changed.

    The temperature setpoint (TemperatureControl 86/0x56 attribute 0,
    TemperatureNumber variant, i16 in 0.01 C units) round-trips the
    other direction: written host-side over AT+MTATTR, read back from
    the controller. 2500 = 25.00 C."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    rc, out = chip.run(["descriptor", "read", "parts-list", node, "19"],
                       timeout=30)
    s.check("3.19 PartsList of ep 19 is exactly [20]",
            rc == 0 and parse_parts_list(out) == [20], tag="P3")

    rc, out = chip.run(["onoff", "off", node, "20"], timeout=30)
    s.check("3.19 chip-tool onoff off exits 0 (Off is the one accepted "
            "command)", rc == 0, tag="P3")
    res, lines = link.command("AT+MTATTR=20,6,0")
    s.check("3.19 AT reads OnOff false after off",
            res == 0 and lines == ["+MTATTR:20,6,0,0"], tag="P3")

    link.drain(0.3)
    rc, out = chip.run(["onoff", "on", node, "20"], timeout=30)
    s.check("3.19 onoff on: chip-tool reports failure (rc != 0)", rc != 0,
            tag="P3")
    s.check("3.19 onoff on answers 0x81 UNSUPPORTED_COMMAND (OffOnly: "
            "no On entry in the AcceptedCommandList)",
            parse_status(out) == 0x81, tag="P3")
    res, lines = link.command("AT+MTATTR=20,6,0")
    s.check("3.19 OnOff still false after the rejected on",
            res == 0 and lines == ["+MTATTR:20,6,0,0"], tag="P3")

    res, lines = link.command("AT+MTATTR=20,86,0,2500")
    s.check("3.19 AT+MTATTR TemperatureSetpoint write (25.00 C) -> OK",
            res == 0 and lines == ["+MTATTR:20,86,0,2500"], tag="P3")
    rc, out = chip.run(["temperaturecontrol", "read",
                        "temperature-setpoint", node, "20"], timeout=30)
    s.check("3.19 controller reads TemperatureSetpoint 2500",
            rc == 0 and parse_int_attr(out) == 2500, tag="P3")


def step_3_20_electrical_sensor(ctx):
    """Energy round A (0.8.0), task 5: the Electrical Sensor slot
    (PHASE3_COMPOSITION 21, endpoint 21, 0x0510 variant 1 power-only:
    the strictly conformant current-clamp declaration, spec 3.9's own
    conformance note, which is why the EEM-absence checks live HERE and
    the full-surface checks on the meter at 22; see the composition
    table's flip comment).

    Server-list membership is the wire-only observability net for the
    variant scheme: ElectricalPowerMeasurement (144) and PowerTopology
    (156, the sensor-only NodeTopology declaration, spec 3.9) present,
    ElectricalEnergyMeasurement (145) ABSENT, and therefore an energy
    push on this endpoint answers +MTERR:3 (TESTING.md 6.2's MTMEAS
    table read with this composition's variant flip: the energy push is
    the cluster-not-carried case on the sensor). The remaining 6.2
    endpoint-dependent MTMEAS rows land here too: unknown endpoint
    (+MTERR:2), the light ep and the non-measurement cluster
    (+MTERR:3), the unknown-field and Frequency-range rows (+MTERR:1,
    the bridge's validate pass answering before anything is applied).
    The three-pair power push is confirmed by controller read-back
    only: every measurement value is Instance-owned, no AT+MTATTR path
    and no +MTATTR URC ever (spec 3.25's rule, pinned here with
    assert_no_urc). Voltage/ActiveCurrent/ActivePower read via
    parse_int_attr's CONFIRMED `]  <Label>: <int>` shape; the pinned
    DataModelLogger decodes each as Nullable<int64_t> and prints
    through the integral overload's std::to_string, the same print
    path parse_parts_list documents.

    Breadcrumb rows (TESTING.md 6.2's 64-bit boundary table, the u64
    Phase 3 rows; brief step 1's verification, resolved YES):
    GeneralCommissioning Breadcrumb (cluster 0x30, attribute 0x00,
    endpoint 0) IS reachable through the AT+MTATTR pipeline in the
    pinned SDK. esp-matter's root node creates it ATTRIBUTE_FLAG_WRITABLE
    | ATTRIBUTE_FLAG_MANAGED_INTERNALLY (esp_matter_cluster.cpp:492,
    esp_matter_attribute.cpp create_breadcrumb, esp_matter_uint64), so
    attribute::get_val()/set_val() route through the DataModel
    provider's ReadAttribute/WriteAttribute (kInternal) into the pinned
    CHIP's code-driven GeneralCommissioningCluster
    (general-commissioning-cluster.cpp: ReadAttribute encodes
    mBreadCrumb, WriteAttribute decodes a u64 and calls SetBreadCrumb),
    which is exactly the path mt_matter_attr_read/_write take. Two
    shape consequences, both asserted: the WRITE answers a plain OK
    with NO +MTATTR echo line (managed-internally attributes bypass
    esp-matter's attribute update callback, main.cpp's AirQuality
    precedent, and endpoint 0 never raises +MTATTR URCs by design,
    spec 4), so the proof is the explicit read-back; and the response
    line prints the PARSED ids in decimal (%lu, mt_at.c), so a read
    commanded with 0x30 answers +MTATTR:0,48,0,<val>, the same
    normalization TESTING.md 6.1's hex-parsing row pins. The 2^32
    write/read pair is the brief's >32-bit width proof; the UINT64_MAX
    pair is the boundary table's own u64 row verbatim (the value a
    signed carrier cannot represent); Breadcrumb is restored to 0 at
    step end (its commissioning-complete reset value). The table's
    sibling <i64 attr> row has NO reachable target in this composition
    (the round's only i64 attributes are the Instance-owned measurement
    values, spec 3.25); see TESTING.md's note under the boundary table,
    and the signed full-width wire proof in step_3_21's ActivePower
    4294967297 push."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    # --- server list: the variant-1 sensor's cluster set ---
    rc, out = chip.run(["descriptor", "read", "server-list", node, "21"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.20 sensor server list carries EPM (144) and "
            "PowerTopology (156)",
            rc == 0 and 144 in servers and 156 in servers, tag="P3")
    s.check("3.20 sensor server list: EEM (145) ABSENT (variant 1 "
            "power-only)", rc == 0 and 145 not in servers, tag="P3")

    # --- TESTING.md 6.2 endpoint-dependent MTMEAS rows ---
    s.check("3.20 AT+MTMEAS=99,144,0,5 -> +MTERR:2 (unknown endpoint)",
            expect_err("AT+MTMEAS=99,144,0,5", 2)(link), tag="P3")
    s.check("3.20 AT+MTMEAS=1,144,0,5 -> +MTERR:3 (light ep carries no "
            "measurement cluster)",
            expect_err("AT+MTMEAS=1,144,0,5", 3)(link), tag="P3")
    s.check("3.20 AT+MTMEAS=21,6,0,5 -> +MTERR:3 (cluster 6/OnOff is "
            "not a measurement cluster, even here)",
            expect_err("AT+MTMEAS=21,6,0,5", 3)(link), tag="P3")
    s.check("3.20 AT+MTMEAS=21,144,99,5 -> +MTERR:1 (field 99 unknown "
            "to 0x0090: the bridge's validate pass)",
            expect_err("AT+MTMEAS=21,144,99,5", 1)(link), tag="P3")
    s.check("3.20 AT+MTMEAS=21,144,3,-50 -> +MTERR:1 (Frequency signed "
            "at parse but its XML range is 0..1000000)",
            expect_err("AT+MTMEAS=21,144,3,-50", 1)(link), tag="P3")
    s.check("3.20 AT+MTMEAS=21,145,0,1500000 -> +MTERR:3 (variant-1 "
            "sensor does not carry EEM: the energy push is refused)",
            expect_err("AT+MTMEAS=21,145,0,1500000", 3)(link), tag="P3")

    # --- three-pair power push, controller read-back only ---
    res, lines = link.command("AT+MTMEAS=21,144,0,230000,1,433,2,99590")
    s.check("3.20 three-pair power push -> OK", res == 0 and lines == [],
            tag="P3")
    s.check("3.20 no +MTATTR URC from the push (Instance-owned values, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:21,", 1.5),
            tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read", "voltage",
                        node, "21"], timeout=30)
    s.check("3.20 controller reads Voltage 230000",
            rc == 0 and parse_int_attr(out) == 230000, tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read",
                        "active-current", node, "21"], timeout=30)
    s.check("3.20 controller reads ActiveCurrent 433",
            rc == 0 and parse_int_attr(out) == 433, tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read",
                        "active-power", node, "21"], timeout=30)
    s.check("3.20 controller reads ActivePower 99590",
            rc == 0 and parse_int_attr(out) == 99590, tag="P3")

    # --- Breadcrumb: the u64 ember round trip (see the docstring) ---
    res, lines = link.command("AT+MTATTR=0,0x30,0,4294967296")
    s.check("3.20 Breadcrumb write 2^32 -> OK, no echo line "
            "(managed-internally: the callback path is bypassed)",
            res == 0 and lines == [], tag="P3")
    res, lines = link.command("AT+MTATTR=0,0x30,0")
    s.check("3.20 Breadcrumb reads back 4294967296 (not truncated to a "
            "32-bit 0)",
            res == 0 and lines == ["+MTATTR:0,48,0,4294967296"], tag="P3")
    res, lines = link.command("AT+MTATTR=0,0x30,0,18446744073709551615")
    s.check("3.20 Breadcrumb write UINT64_MAX -> OK",
            res == 0 and lines == [], tag="P3")
    res, lines = link.command("AT+MTATTR=0,0x30,0")
    s.check("3.20 Breadcrumb reads back 18446744073709551615 (u64 full "
            "width: the value a signed carrier cannot represent)",
            res == 0
            and lines == ["+MTATTR:0,48,0,18446744073709551615"],
            tag="P3")
    res, _ = link.command("AT+MTATTR=0,0x30,0,0")
    s.check("3.20 Breadcrumb restore write 0 -> OK", res == 0, tag="P3")
    res, lines = link.command("AT+MTATTR=0,0x30,0")
    s.check("3.20 Breadcrumb reads back 0 after restore",
            res == 0 and lines == ["+MTATTR:0,48,0,0"], tag="P3")


def step_3_21_electrical_meter(ctx):
    """Energy round A (0.8.0), task 5: the Electrical Meter slot
    (PHASE3_COMPOSITION 22, endpoint 22, 0x0514 variant 0, power PLUS
    energy: both measurement clusters are mandatory on the meter's own
    device type XML, the conformance reason this slot is the variant-0
    one; see the composition table's flip comment).

    Server list: EPM (144) and EEM (145) present, PowerTopology (156)
    ABSENT (the meter has no topology cluster at all, spec 3.9). The
    power push includes an ActivePower of 4294967297 mW (2^32 + 1),
    the signed-64-bit wire proof: parse-side i64, delegate store,
    controller decode as Nullable<int64_t>, printed full width through
    the integral overload (see step_3_20's docstring; also the stand-in
    for the boundary table's un-automatable <i64 attr> row, TESTING.md's
    note under that table). A subscription (the Subscriber machinery,
    parameterized to electricalpowermeasurement/active-power) must see
    a subsequent ActivePower push land as a change report: sequencing
    per the Subscriber contract, no ChipTool.run() while it lives, so
    all one-shot reads happen before start() or after stop(). The
    subscription report shape (parse_active_power_reports) is
    INFERENCE until the bench; derivation in that parser's docstring.

    Energy: two pushes (imported only, then imported + exported in one
    push), confirmed by a CumulativeEnergyImported read whose struct
    carries the LAST pushed energy (parse_energy_values, INFERENCE,
    derivation in its docstring) and by read-event
    cumulative-energy-measured observing BOTH events (the SDK's
    NotifyCumulativeEnergyMeasured writes attributes and emits the
    event in one call, spec 3.25), with both pushed energies present
    across the event structs. Event-name block shape is
    parse_event_count's CONFIRMED `<EventName>: {` form; the event's
    field layout (EnergyImported/EnergyExported wrapping
    EnergyMeasurementStruct) is the INFERENCE part, carried by
    parse_energy_values. No command traffic exists on this endpoint by
    construction: neither measurement cluster nor PowerTopology
    declares a single accepted command (spec 3.25/3.9), so this step
    scripts none, and a self-test pins that."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    # --- server list: full measurement surface, no topology ---
    rc, out = chip.run(["descriptor", "read", "server-list", node, "22"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.21 meter server list carries EPM (144) and EEM (145)",
            rc == 0 and 144 in servers and 145 in servers, tag="P3")
    s.check("3.21 meter server list: PowerTopology (156) ABSENT (the "
            "meter has no topology cluster)",
            rc == 0 and 156 not in servers, tag="P3")

    # --- power push with ActivePower above 2^32 ---
    res, lines = link.command("AT+MTMEAS=22,144,0,230000,1,433,2,4294967297")
    s.check("3.21 power push (ActivePower 2^32 + 1) -> OK",
            res == 0 and lines == [], tag="P3")
    s.check("3.21 no +MTATTR URC from the push (Instance-owned values, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:22,", 1.5),
            tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read", "voltage",
                        node, "22"], timeout=30)
    s.check("3.21 controller reads Voltage 230000",
            rc == 0 and parse_int_attr(out) == 230000, tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read",
                        "active-current", node, "22"], timeout=30)
    s.check("3.21 controller reads ActiveCurrent 433",
            rc == 0 and parse_int_attr(out) == 433, tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read",
                        "active-power", node, "22"], timeout=30)
    s.check("3.21 controller reads ActivePower 4294967297 (64-bit, not "
            "truncated)", rc == 0 and parse_int_attr(out) == 4294967297,
            tag="P3")

    # --- subscription: an ActivePower push lands as a report ---
    factory = ctx.subscriber_factory or (
        lambda: Subscriber(chip, ctx.node_id, endpoint=22,
                           cluster="electricalpowermeasurement",
                           attribute="active-power",
                           parser=parse_active_power_reports))
    sub = factory()
    started = s.check("3.21 ActivePower subscriber starts (priming "
                      "report)", sub.start(), tag="P3")
    try:
        base = len(sub.reports()) if started else 0
        res, _ = link.command("AT+MTMEAS=22,144,2,99590")
        s.check("3.21 ActivePower push under subscription -> OK",
                res == 0, tag="P3")
        s.check("3.21 subscription reports the ActivePower update",
                started and sub.wait_new_report(base, 5.0), tag="P3")
    finally:
        sub.stop()

    # --- energy: two pushes, attribute read-backs, the event ---
    res, lines = link.command("AT+MTMEAS=22,145,0,1500000")
    s.check("3.21 energy push 1 (imported 1500000) -> OK",
            res == 0 and lines == [], tag="P3")
    rc, out = chip.run(["electricalenergymeasurement", "read",
                        "cumulative-energy-imported", node, "22"],
                       timeout=30)
    s.check("3.21 CumulativeEnergyImported struct carries energy "
            "1500000 (the 6.2 OK row's read-back, verbatim)",
            rc == 0 and 1500000 in parse_energy_values(out), tag="P3")
    res, lines = link.command("AT+MTMEAS=22,145,0,1600000,1,20000")
    s.check("3.21 energy push 2 (imported 1600000, exported 20000 in "
            "one push) -> OK", res == 0 and lines == [], tag="P3")
    rc, out = chip.run(["electricalenergymeasurement", "read",
                        "cumulative-energy-imported", node, "22"],
                       timeout=30)
    s.check("3.21 CumulativeEnergyImported struct carries energy "
            "1600000 after push 2 (the newest push serves the read)",
            rc == 0 and 1600000 in parse_energy_values(out), tag="P3")
    rc, out = chip.run(["electricalenergymeasurement", "read-event",
                        "cumulative-energy-measured", node, "22"],
                       timeout=30)
    s.check("3.21 both CumulativeEnergyMeasured events observed (one "
            "per push)",
            rc == 0
            and parse_event_count(out, "CumulativeEnergyMeasured") >= 2,
            tag="P3")
    vals = parse_energy_values(out)
    s.check("3.21 events carry the pushed energies (1500000 and "
            "1600000)",
            rc == 0 and 1500000 in vals and 1600000 in vals, tag="P3")


def step_3_22_water_heater(ctx):
    """Energy round B (0.9.0), task 4: the Water Heater slot
    (PHASE3_COMPOSITION 23, endpoint 23, 0x050F variant 0 FULL: WHM
    feature map EnergyManagement|TankPercent, the three gated attribute
    shells, and the composed Electrical Sensor graft the device type XML
    mandates; mt_devtypes.cpp's mk_water_heater()).

    Server list: WHM (148), Thermostat (513) and WaterHeaterMode (158)
    from the SDK's water_heater::create(), plus the graft's EPM (144),
    EEM (145) and PowerTopology (156). The 6.2 full-variant negative
    rows land here: field 9 unknown to 0x94 (+MTERR:1, the bridge's
    validate pass) and field 4 negative (+MTERR:1: on the FULL variant
    the EnergyManagement gate passes and the XML's min-0 range check
    answers; the minimal variant's +MTERR:3 sibling, gate outranks
    range, is Phase 1's t_meas_staged_wh_min). The 0x94 pushes are
    Instance-owned like every AT+MTMEAS value (no +MTATTR URC ever,
    spec 3.25, pinned with assert_no_urc), so controller reads are the
    only read-back; EstimatedHeatRequired is pushed above 2^32
    (4294967297) as the family's 64-bit width proof, the step_3_21
    ActivePower reasoning on the round B cluster.

    Boost chain (spec 3.17): the in-state guard runs FIRST, while
    BoostState is still Inactive from boot: CancelBoost answers
    Status::Success on the wire (chip-tool exits 0) WITHOUT raising any
    +MTCMD and without a BoostEnded event, the cluster test plan's
    TC_EWATERHTR_2_2 step 26 contract (HandleCancelBoost()'s guard runs
    before mt_cmd_forward_fields(), main.cpp), proven synchronously the
    step_3_9 in-state-guard way since nothing blocks on a verdict that
    is never asked for. Then the adjudicated chain: chip-tool's boost
    with duration 3600, oneShot true, targetPercentage 80 must forward
    the design spec's canonical vector VERBATIM (tail "3600,265,80",
    mask = bit0|bit3|bit8: presence oneShot + targetPercentage, value
    oneShot true), answered allow; BoostState stays 0 after the allow
    (the host decided, it has not DONE it yet, the valve split), starts
    only on the host's own AT+MTMEAS BoostState push, whose
    Inactive-to-Active transition derives exactly one BoostStarted
    event carrying the accepted command's cached parameters; a
    same-state repeat push emits NO second event (the ledger's
    same-state rule; the attribute is still reported dirty, invisible
    here without a subscription); CancelBoost while Active forwards
    payload-less and, after the host pushes Inactive, exactly one
    BoostEnded follows. The BoostStarted boostInfo assertion (Duration/
    TargetPercentage labels) is derived from the pinned generated
    DataModelLogger.cpp (WaterHeaterBoostInfoStruct's LogValue prints
    "Duration"/"OneShot"/"TargetPercentage" through the integral/bool
    overloads): INFERENCE until the bench, parse_energy_values'
    confidence class.

    Thermostat, both directions (spec 3.5's no-new-surface claim made
    observable): these attributes are EMBER-served, the deliberate
    contrast with everything above, so a controller write to
    OccupiedHeatingSetpoint (513/0x12) DOES raise an unprompted +MTATTR
    URC (the design-spec rule the host library's setpoint callbacks
    rest on), and the host's LocalTemperature push over plain AT+MTATTR
    echoes its response line in normalized decimal and reads back from
    the controller.

    WaterHeaterMode (spec 3.20.1/3.4): staged over the cluster-aware
    AT+MTMODES with one tag-0 default, which must resolve to kManual
    (0x4001, main.cpp's pinned default for this cluster, unlike
    RvcRunMode's per-position rule) and one explicit kTimed (0x4002);
    ChangeToMode is adjudicated allow (cluster 158 command 0, one-field
    payload), and the B196 same-mode short-circuit is pinned the RVC
    round's way: a second ChangeToMode to the now-current mode answers
    kSuccess from mode-base-server.cpp itself with NO +MTCMD raised."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    # --- server list: SDK trio plus the variant-0 sensor graft ---
    rc, out = chip.run(["descriptor", "read", "server-list", node, "23"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.22 water heater server list carries WHM (148), Thermostat "
            "(513) and WaterHeaterMode (158)",
            rc == 0 and 148 in servers and 513 in servers
            and 158 in servers, tag="P3")
    s.check("3.22 variant-0 sensor graft present: EPM (144), EEM (145), "
            "PowerTopology (156)",
            rc == 0 and 144 in servers and 145 in servers
            and 156 in servers, tag="P3")

    # --- TESTING.md 6.2 full-variant 0x94 negative rows ---
    s.check("3.22 AT+MTMEAS=23,148,9,1 -> +MTERR:1 (field 9 unknown to "
            "0x94: the bridge's validate pass)",
            expect_err("AT+MTMEAS=23,148,9,1", 1)(link), tag="P3")
    s.check("3.22 AT+MTMEAS=23,148,4,-5 -> +MTERR:1 (FULL variant: the "
            "gate passes and the XML min-0 range check answers)",
            expect_err("AT+MTMEAS=23,148,4,-5", 1)(link), tag="P3")

    # --- 0x94 pushes, controller read-back only ---
    res, lines = link.command("AT+MTMEAS=23,148,0,4,1,4")
    s.check("3.22 HeaterTypes/HeatDemand push (both HeatPump, 4) -> OK",
            res == 0 and lines == [], tag="P3")
    res, lines = link.command("AT+MTMEAS=23,148,3,300,4,4294967297,5,60")
    s.check("3.22 tank trio push (EstimatedHeatRequired 2^32 + 1) -> OK",
            res == 0 and lines == [], tag="P3")
    s.check("3.22 no +MTATTR URC from the pushes (Instance-owned values, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:23,", 1.5),
            tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read", "heater-types",
                        node, "23"], timeout=30)
    s.check("3.22 controller reads HeaterTypes 4",
            rc == 0 and parse_int_attr(out) == 4, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read", "heat-demand",
                        node, "23"], timeout=30)
    s.check("3.22 controller reads HeatDemand 4",
            rc == 0 and parse_int_attr(out) == 4, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read", "tank-volume",
                        node, "23"], timeout=30)
    s.check("3.22 controller reads TankVolume 300",
            rc == 0 and parse_int_attr(out) == 300, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read",
                        "estimated-heat-required", node, "23"], timeout=30)
    s.check("3.22 controller reads EstimatedHeatRequired 4294967297 "
            "(64-bit, not truncated)",
            rc == 0 and parse_int_attr(out) == 4294967297, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read", "tank-percentage",
                        node, "23"], timeout=30)
    s.check("3.22 controller reads TankPercentage 60",
            rc == 0 and parse_int_attr(out) == 60, tag="P3")

    # --- in-state guard, BEFORE any boost (BoostState Inactive) ---
    link.drain(0.3)
    rc, out = chip.run(["waterheatermanagement", "cancel-boost", node,
                        "23"], timeout=30)
    s.check("3.22 in-state guard: CancelBoost while Inactive exits 0 "
            "(Status::Success, TC_EWATERHTR_2_2 step 26)", rc == 0,
            tag="P3")
    s.check("3.22 in-state guard: no +MTCMD raised (the firmware answers "
            "without waking the host)",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read-event",
                        "boost-ended", node, "23"], timeout=30)
    s.check("3.22 in-state guard: no BoostEnded event",
            rc == 0 and parse_event_count(out, "BoostEnded") == 0,
            tag="P3")

    # --- Boost: the canonical vector, adjudicated allow ---
    handle = invoke_chip(ctx, ["waterheatermanagement", "boost",
                              '{"duration": 3600, "oneShot": true, '
                              '"targetPercentage": 80}', node, "23"],
                         timeout=30)
    fwd = responder.expect(cluster=148, command=0, verdict=1,
                           payload=[3600, 265, 80], timeout=5.0)
    s.check("3.22 Boost forward answered allow with the canonical tail "
            "3600,265,80 (mask bit0|bit3|bit8 = 265)", fwd is not None,
            tag="P3")
    rc, out = handle.join(30)
    s.check("3.22 Boost chip-tool exits 0 (allow -> Status::Success)",
            rc == 0, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read", "boost-state",
                        node, "23"], timeout=30)
    s.check("3.22 BoostState still 0 after the allow (the host decided; "
            "the boost starts on the host's own push)",
            rc == 0 and parse_int_attr(out) == 0, tag="P3")
    res, _ = link.command("AT+MTMEAS=23,148,2,1")
    s.check("3.22 BoostState Active push -> OK", res == 0, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read", "boost-state",
                        node, "23"], timeout=30)
    s.check("3.22 controller reads BoostState 1 (Active)",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read-event",
                        "boost-started", node, "23"], timeout=30)
    s.check("3.22 exactly one BoostStarted event",
            rc == 0 and parse_event_count(out, "BoostStarted") == 1,
            tag="P3")
    s.check("3.22 BoostStarted carries the accepted Boost's parameters "
            "(Duration 3600, TargetPercentage 80)",
            rc == 0 and re.search(r"Duration:\s*3600\b", out) is not None
            and re.search(r"TargetPercentage:\s*80\b", out) is not None,
            tag="P3")
    res, _ = link.command("AT+MTMEAS=23,148,2,1")
    s.check("3.22 same-state BoostState push -> OK", res == 0, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read-event",
                        "boost-started", node, "23"], timeout=30)
    s.check("3.22 still exactly one BoostStarted (same-state push emits "
            "no event)",
            rc == 0 and parse_event_count(out, "BoostStarted") == 1,
            tag="P3")

    # --- CancelBoost while Active: adjudicated, then BoostEnded ---
    handle = invoke_chip(ctx, ["waterheatermanagement", "cancel-boost",
                              node, "23"], timeout=30)
    fwd = responder.expect(cluster=148, command=1, verdict=1, timeout=5.0)
    s.check("3.22 CancelBoost forward answered allow (payload-less)",
            fwd is not None and fwd["fields"] == [], tag="P3")
    rc, out = handle.join(30)
    s.check("3.22 CancelBoost chip-tool exits 0", rc == 0, tag="P3")
    res, _ = link.command("AT+MTMEAS=23,148,2,0")
    s.check("3.22 BoostState Inactive push -> OK", res == 0, tag="P3")
    rc, out = chip.run(["waterheatermanagement", "read-event",
                        "boost-ended", node, "23"], timeout=30)
    s.check("3.22 exactly one BoostEnded event",
            rc == 0 and parse_event_count(out, "BoostEnded") == 1,
            tag="P3")

    # --- Thermostat: ember-served, both directions ---
    link.drain(0.3)
    rc, out = chip.run(["thermostat", "write", "occupied-heating-setpoint",
                        "2200", node, "23"], timeout=30)
    s.check("3.22 controller writes OccupiedHeatingSetpoint 2200: "
            "chip-tool exits 0", rc == 0, tag="P3")
    s.check("3.22 unprompted +MTATTR URC for the setpoint write "
            "(ember-served, NOT Instance-owned: the 0.6.0 rule's "
            "deliberate contrast)",
            link.await_urc(r"\+MTATTR:23,513,18,2200$",
                           timeout=5.0) is not None, tag="P3")
    res, lines = link.command("AT+MTATTR=23,513,18")
    s.check("3.22 AT read agrees (OccupiedHeatingSetpoint 2200)",
            res == 0 and lines == ["+MTATTR:23,513,18,2200"], tag="P3")
    res, lines = link.command("AT+MTATTR=23,0x0201,0,2150")
    s.check("3.22 LocalTemperature push over AT+MTATTR -> OK, echo line "
            "in normalized decimal",
            res == 0 and lines == ["+MTATTR:23,513,0,2150"], tag="P3")
    rc, out = chip.run(["thermostat", "read", "local-temperature", node,
                        "23"], timeout=30)
    s.check("3.22 controller reads LocalTemperature 2150",
            rc == 0 and parse_int_attr(out) == 2150, tag="P3")

    # --- WaterHeaterMode: list, ChangeToMode, B196 short-circuit ---
    res, _ = link.command('AT+MTMODES=23,158,0,0,"Manual",1,16386,"Timed"')
    s.check("3.22 WaterHeaterMode staged (tag-0 default + explicit "
            "kTimed) -> OK", res == 0, tag="P3")
    rc, out = chip.run(["waterheatermode", "read", "supported-modes",
                        node, "23"], timeout=30)
    s.check("3.22 SupportedModes labels verbatim",
            rc == 0 and parse_string_list(out) == ["Manual", "Timed"],
            tag="P3")
    s.check("3.22 ModeTags: defaulted kManual (0x4001), explicit kTimed "
            "(0x4002)",
            rc == 0 and parse_mode_tag_values(out) == [0x4001, 0x4002],
            tag="P3")
    handle = invoke_chip(ctx, ["waterheatermode", "change-to-mode", "1",
                              node, "23"], timeout=30)
    fwd = responder.expect(cluster=158, command=0, verdict=1, payload=1,
                           timeout=5.0)
    s.check("3.22 ChangeToMode allow: forward answered, payload == mode 1",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.22 ChangeToMode allow: chip-tool exits 0", rc == 0,
            tag="P3")
    s.check("3.22 ChangeToMode allow: status 0 (kSuccess)",
            parse_change_to_mode_status(out) == 0, tag="P3")
    rc, out = chip.run(["waterheatermode", "read", "current-mode", node,
                        "23"], timeout=30)
    s.check("3.22 CurrentMode is 1 (the short-circuit's precondition, "
            "observed, not assumed)",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")
    link.drain(0.3)
    rc, out = chip.run(["waterheatermode", "change-to-mode", "1", node,
                        "23"], timeout=30)
    s.check("3.22 same-mode short-circuit: chip-tool exits 0 (B196: the "
            "SDK answers before the delegate)", rc == 0, tag="P3")
    s.check("3.22 same-mode short-circuit: status 0 (kSuccess from "
            "mode-base-server.cpp itself)",
            parse_change_to_mode_status(out) == 0, tag="P3")
    s.check("3.22 same-mode short-circuit: no +MTCMD raised",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")


def step_3_23_heat_pump(ctx):
    """Energy round B (0.9.0), task 4: the Heat Pump slot
    (PHASE3_COMPOSITION 24, endpoint 24, 0x0309; mt_devtypes.cpp's
    mk_heat_pump(), the hand-built heat_pump::add() body minus DEM).

    Identity is the step's distinguishing check: ONE endpoint carrying
    three device types (0x0309 Heat Pump, 0x0011 Power Source wired,
    0x0510 Electrical Sensor), the composedDeviceTypes flattening the
    thunk builds, read from the descriptor's DeviceTypeList
    (parse_device_types). Server list: PowerSource (47) plus the full
    sensor surface EPM (144), EEM (145), PowerTopology (156); no WHM
    (148) and no Thermostat (513), the disclosed SDK-parity gap
    (heat_pump::config_t has no thermostat member at all), pinned as an
    absence so a future thunk change surfaces here. A 0x94 push on this
    endpoint answers +MTERR:3 (no WHM cluster), the same
    endpoint-does-not-serve-that-data code as everywhere else.

    The power push carries a NEGATIVE ActivePower (-1500000 mW): EPM's
    power fields are signed (spec 3.25's table), the heat pump is the
    device type whose sign actually MEANS something (heating vs cooling
    draw in the round's own FullAPI example), and no earlier step pushes
    any negative measurement, so this is the sign path's only wire
    coverage. Energy is pushed once and confirmed via the attribute
    struct and the CumulativeEnergyMeasured event, the step_3_21 shapes
    (parse_energy_values / parse_event_count).

    Pool boundary, by construction: with slots 21-23's pushes already
    OK, this step's pushes prove the FOURTH and last MT_MEAS_MAX pool
    pair serves its endpoint, the design spec section 5's deliberate
    exhaustion-boundary coverage. No command traffic exists on this
    endpoint (none of its clusters declares an accepted command), the
    step_3_20/3_21 rule, so this step scripts none."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    # --- identity: three device types on one endpoint ---
    rc, out = chip.run(["descriptor", "read", "device-type-list", node,
                        "24"], timeout=30)
    types = parse_device_types(out)
    s.check("3.23 device type list carries 0x0309 (777), 0x0011 (17) and "
            "the composed 0x0510 sensor (1296)",
            rc == 0 and 777 in types and 17 in types and 1296 in types,
            tag="P3")

    # --- server list: sensor surface + power source, no WHM/Thermostat ---
    rc, out = chip.run(["descriptor", "read", "server-list", node, "24"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.23 server list carries PowerSource (47), EPM (144), EEM "
            "(145) and PowerTopology (156)",
            rc == 0 and 47 in servers and 144 in servers
            and 145 in servers and 156 in servers, tag="P3")
    s.check("3.23 no WHM (148) and no Thermostat (513) on the heat pump "
            "(the disclosed SDK-parity gap, pinned)",
            rc == 0 and bool(servers) and 148 not in servers
            and 513 not in servers, tag="P3")

    # --- 0x94 on a measurement endpoint without the WHM cluster ---
    s.check("3.23 AT+MTMEAS=24,148,0,1 -> +MTERR:3 (heat pump carries no "
            "WaterHeaterManagement cluster)",
            expect_err("AT+MTMEAS=24,148,0,1", 3)(link), tag="P3")

    # --- power push with a negative ActivePower ---
    res, lines = link.command("AT+MTMEAS=24,144,0,230000,1,433,2,-1500000")
    s.check("3.23 power push with NEGATIVE ActivePower (-1500000 mW, the "
            "signed EPM field) -> OK", res == 0 and lines == [], tag="P3")
    s.check("3.23 no +MTATTR URC from the push (Instance-owned values, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:24,", 1.5),
            tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read", "voltage",
                        node, "24"], timeout=30)
    s.check("3.23 controller reads Voltage 230000",
            rc == 0 and parse_int_attr(out) == 230000, tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read",
                        "active-power", node, "24"], timeout=30)
    s.check("3.23 controller reads ActivePower -1500000 (sign preserved "
            "through the whole pipeline)",
            rc == 0 and parse_int_attr(out) == -1500000, tag="P3")

    # --- energy push, attribute read-back, the event ---
    res, lines = link.command("AT+MTMEAS=24,145,0,2500000")
    s.check("3.23 energy push (imported 2500000) -> OK",
            res == 0 and lines == [], tag="P3")
    rc, out = chip.run(["electricalenergymeasurement", "read",
                        "cumulative-energy-imported", node, "24"],
                       timeout=30)
    s.check("3.23 CumulativeEnergyImported struct carries energy 2500000",
            rc == 0 and 2500000 in parse_energy_values(out), tag="P3")
    rc, out = chip.run(["electricalenergymeasurement", "read-event",
                        "cumulative-energy-measured", node, "24"],
                       timeout=30)
    s.check("3.23 CumulativeEnergyMeasured event observed with the "
            "pushed energy",
            rc == 0
            and parse_event_count(out, "CumulativeEnergyMeasured") >= 1
            and 2500000 in parse_energy_values(out), tag="P3")


def step_3_24_solar_power(ctx):
    """Energy round C1 (0.10.0), task 4: the Solar Power slot
    (PHASE3_COMPOSITION 25, endpoint 25, 0x0017 variant 0 FULL;
    mt_devtypes.cpp's mk_solar_power(), hand-built because
    solar_power::add() assigns feature_flags over caller pre-seeds and
    creates EPM Voltage/ActiveCurrent as non-null zeros, ARCHITECTURE.md
    8.12).

    Identity: ONE endpoint carrying three device types (0x0017 Solar
    Power = 23, 0x0011 Power Source = 17, the composed 0x0510 Electrical
    Sensor = 1296), the step_3_23 shape on this round's first type.

    Server list: PowerSource (47) plus the variant-0 graft's EPM (144),
    EEM (145) and PowerTopology (156). DeviceEnergyManagement (152) is
    pinned ABSENT: SolarPower.xml requests no DEM (unlike battery
    storage's over-delivery), so both a 0x0098 push and an AT+MTDEMCAP
    on this endpoint answer +MTERR:3, the cluster-not-carried code.
    Those two rows are the Phase 3 half of spec 3.26's lookup division;
    their +MTERR:2 and light-endpoint siblings live in step_3_26.

    The energy push is the EXPORTED counter (field 1) at 4294967297 mWh,
    two firsts in one row: no earlier step reads the exported side back
    at all (step_3_21 pushes it only alongside imported), and no earlier
    step proves the exported side survives the 64-bit pipeline. Solar is
    the device type whose exported energy is the whole point. The
    measurement values stay Instance-owned like every AT+MTMEAS value,
    so controller reads are the only read-back and assert_no_urc pins
    that no +MTATTR ever follows (spec 3.25).

    Pool position, by construction: this endpoint is the FIFTH EPM/PT
    pool pair (MT_MEAS_MAX raised 4 -> 8 this round), so its pushes are
    the proof the raise took effect; slot 26 is the sixth, and the
    remaining two are deliberate headroom (the composition table's own
    round C1 comment). No command traffic exists on this endpoint (none
    of its clusters declares an accepted command), the step_3_20/3_21
    rule, so this step scripts none."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    # --- identity: three device types on one endpoint ---
    rc, out = chip.run(["descriptor", "read", "device-type-list", node,
                        "25"], timeout=30)
    types = parse_device_types(out)
    s.check("3.24 device type list carries 0x0017 (23), 0x0011 (17) and "
            "the composed 0x0510 sensor (1296)",
            rc == 0 and 23 in types and 17 in types and 1296 in types,
            tag="P3")

    # --- server list: full sensor surface, no DEM ---
    rc, out = chip.run(["descriptor", "read", "server-list", node, "25"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.24 server list carries PowerSource (47), EPM (144), EEM "
            "(145) and PowerTopology (156)",
            rc == 0 and 47 in servers and 144 in servers
            and 145 in servers and 156 in servers, tag="P3")
    s.check("3.24 no DeviceEnergyManagement (152) on solar power "
            "(SolarPower.xml requests none: pinned as an absence)",
            rc == 0 and bool(servers) and 152 not in servers, tag="P3")

    # --- the DEM-less endpoint answers both DEM surfaces with 3 ---
    s.check("3.24 AT+MTMEAS=25,152,0,1 -> +MTERR:3 (solar carries no "
            "DeviceEnergyManagement cluster)",
            expect_err("AT+MTMEAS=25,152,0,1", 3)(link), tag="P3")
    s.check("3.24 AT+MTDEMCAP=25,1,0 -> +MTERR:3 (same cluster-missing "
            "code on the new command family, spec 3.26)",
            expect_err("AT+MTDEMCAP=25,1,0", 3)(link), tag="P3")

    # --- power push, controller read-back only ---
    res, lines = link.command("AT+MTMEAS=25,144,0,230000,1,433,2,99590")
    s.check("3.24 power push -> OK (the fifth MT_MEAS_MAX pool pair, the "
            "raise made observable)", res == 0 and lines == [], tag="P3")
    s.check("3.24 no +MTATTR URC from the push (Instance-owned values, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:25,", 1.5),
            tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read", "voltage",
                        node, "25"], timeout=30)
    s.check("3.24 controller reads Voltage 230000",
            rc == 0 and parse_int_attr(out) == 230000, tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read",
                        "active-power", node, "25"], timeout=30)
    s.check("3.24 controller reads ActivePower 99590",
            rc == 0 and parse_int_attr(out) == 99590, tag="P3")

    # --- energy: the EXPORTED side, above 2^32 ---
    res, lines = link.command("AT+MTMEAS=25,145,1,4294967297")
    s.check("3.24 exported energy push (2^32 + 1 mWh) -> OK",
            res == 0 and lines == [], tag="P3")
    rc, out = chip.run(["electricalenergymeasurement", "read",
                        "cumulative-energy-exported", node, "25"],
                       timeout=30)
    s.check("3.24 CumulativeEnergyExported struct carries energy "
            "4294967297 (the exported side's only read-back anywhere, "
            "at full 64-bit width)",
            rc == 0 and 4294967297 in parse_energy_values(out), tag="P3")
    rc, out = chip.run(["electricalenergymeasurement", "read-event",
                        "cumulative-energy-measured", node, "25"],
                       timeout=30)
    s.check("3.24 CumulativeEnergyMeasured event observed with the "
            "exported energy",
            rc == 0
            and parse_event_count(out, "CumulativeEnergyMeasured") >= 1
            and 4294967297 in parse_energy_values(out), tag="P3")


def step_3_25_battery_storage(ctx):
    """Energy round C1 (0.10.0), task 4: the Battery Storage slot
    (PHASE3_COMPOSITION 26, endpoint 26, 0x0018 variant 0 FULL;
    mt_devtypes.cpp's mk_battery_storage()).

    Identity: FOUR device types on one endpoint (0x0018 = 24, 0x0011 =
    17, the composed 0x0510 sensor = 1296, and 0x050D
    DeviceEnergyManagement = 1293, which the SDK build bolts on as
    over-delivery and this thunk keeps on variant 0 only).

    The battery attributes are the round's no-new-surface claim (design
    spec 3.5) made observable: they are EMBER-served, so plain AT+MTATTR
    reads and writes reach them and a controller read agrees, the
    deliberate contrast with every Instance-owned value on the same
    endpoint. BatChargeState (attribute 26/0x1A) is the important one:
    the SDK's own battery_storage::add() creates the four RECHG-GATED
    attributes without the RECHG feature bit and omits the
    RECHG-mandatory BatChargeState entirely, so its mere presence here
    is the wire evidence for the conformance fix (battery|rechargeable
    in the thunk's PowerSource config, ARCHITECTURE.md 8.12);
    BatFunctionalWhileCharging (28/0x1C) is its sibling, read-only here
    because feature::rechargeable::add() is what created it. The
    BatPercentRemaining row is commanded in HEX to pin the same decimal
    normalization TESTING.md 6.1's hex-parsing row does, then written at
    its legal boundary (200) and one past it (201): the B264 pin, an
    in-width value ember refuses on its own min/max, which must answer
    +MTERR:1, not a bare ERROR (main.cpp's mt_matter_attr_write()), and
    must not change the stored value. BatCapacity is written twice: once
    at the SDK example's own scale (5000), then again past its old
    0x00..0xFFFF bound at 13500000 (13.5 kWh), the B263 pin against
    mt_devtypes.cpp regressing back to the SDK's example-derived cap
    instead of the cluster XML's real uint32 range.

    DEM triple: this endpoint is the composition's FIRST DEM-bearing
    one, so it takes the FIRST HearthDemDelegate pool slot (1 of
    MT_DEM_MAX 4) and a capability install here plus a controller
    read-back proves the pool serves at all. The proof that it serves
    more than its first endpoint belongs to the standalone ESA at slot
    27 (step_3_26), which takes the second slot and where the full
    PowerAdjust protocol lives rather than being run twice.
    DeviceEnergyManagementMode (159/0x9F) is staged over the
    cluster-aware AT+MTMODES with one tag-0 default, which must resolve
    to kNoOptimization (0x4000, main.cpp's pinned default for this
    cluster) and one explicit kDeviceOptimization (0x4001).

    INFERENCE in this step's body (energy round C1, awaiting the bench;
    the parse_energy_values confidence class, with the parsers' own
    shapes documented in their docstrings): `ESACanGenerate:\\s*TRUE`,
    asserted with a regex rather than through parse_int_attr because the
    field is a bool and the pinned DataModelLogger.h bool overload
    prints the literal `TRUE`/`FALSE`. If the bench sees `1`, that is a
    one-line harness fix, not a firmware defect. The battery attribute
    labels themselves ride parse_int_attr's CONFIRMED
    `]  <Label>: <int>` shape; the chip-tool attribute NAMES were read
    from the pinned Commands.h registration block, so those are not
    inferred."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id

    # --- identity: four device types on one endpoint ---
    rc, out = chip.run(["descriptor", "read", "device-type-list", node,
                        "26"], timeout=30)
    types = parse_device_types(out)
    s.check("3.25 device type list carries 0x0018 (24), 0x0011 (17), the "
            "composed 0x0510 sensor (1296) and 0x050D DEM (1293)",
            rc == 0 and 24 in types and 17 in types and 1296 in types
            and 1293 in types, tag="P3")

    # --- server list: sensor graft, power source, the DEM pair ---
    rc, out = chip.run(["descriptor", "read", "server-list", node, "26"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.25 server list carries PowerSource (47), EPM (144), EEM "
            "(145) and PowerTopology (156)",
            rc == 0 and 47 in servers and 144 in servers
            and 145 in servers and 156 in servers, tag="P3")
    s.check("3.25 server list carries DeviceEnergyManagement (152) and "
            "DeviceEnergyManagementMode (159): the variant-0 DEM triple",
            rc == 0 and 152 in servers and 159 in servers, tag="P3")

    # --- battery attributes: ember-served, both directions ---
    res, lines = link.command("AT+MTATTR=26,47,11,12600")
    s.check("3.25 BatVoltage write (12.6 V) -> OK, echo line",
            res == 0 and lines == ["+MTATTR:26,47,11,12600"], tag="P3")
    rc, out = chip.run(["powersource", "read", "bat-voltage", node, "26"],
                       timeout=30)
    s.check("3.25 controller reads BatVoltage 12600",
            rc == 0 and parse_int_attr(out) == 12600, tag="P3")
    res, lines = link.command("AT+MTATTR=26,0x2F,0x0C,180")
    s.check("3.25 BatPercentRemaining write in hex (90%, half-percent "
            "units) -> OK, echo line in normalized decimal",
            res == 0 and lines == ["+MTATTR:26,47,12,180"], tag="P3")
    rc, out = chip.run(["powersource", "read", "bat-percent-remaining",
                        node, "26"], timeout=30)
    s.check("3.25 controller reads BatPercentRemaining 180",
            rc == 0 and parse_int_attr(out) == 180, tag="P3")

    # --- B264 pin: an in-width, out-of-bounds write answers +MTERR:1 ---
    # BatPercentRemaining's created range really is 0..200 (half-percent,
    # PowerSourceCluster.xml:643-650), so 200 is the legal upper boundary
    # and 201 is in width (fits uint8) but past ember's own min/max. Before
    # the B264 fix the refusal answered a bare ERROR, indistinguishable
    # from a malformed command; the fix maps ember's ESP_ERR_INVALID_ARG to
    # MT_ATTR_ERR_VALUE, so it now carries +MTERR:1 like every other
    # out-of-range AT+MTATTR value (main.cpp's mt_matter_attr_write()).
    res, lines = link.command("AT+MTATTR=26,47,12,200")
    s.check("3.25 BatPercentRemaining write at the legal boundary (200) "
            "-> OK, echo line (B264)",
            res == 0 and lines == ["+MTATTR:26,47,12,200"], tag="P3")
    res, lines = link.command("AT+MTATTR=26,47,12,201")
    s.check("3.25 BatPercentRemaining write one past the boundary (201, "
            "in width, past ember's 0..200) -> +MTERR:1, not a bare ERROR "
            "(B264)", res == 1 and lines == [], tag="P3")
    rc, out = chip.run(["powersource", "read", "bat-percent-remaining",
                        node, "26"], timeout=30)
    s.check("3.25 controller reads BatPercentRemaining still 200: the "
            "refused write did not change the attribute (B264)",
            rc == 0 and parse_int_attr(out) == 200, tag="P3")

    res, lines = link.command("AT+MTATTR=26,47,26,1")
    s.check("3.25 BatChargeState write (1 IsCharging) -> OK: the "
            "attribute the SDK build omits entirely exists here (the "
            "RECHG conformance fix)",
            res == 0 and lines == ["+MTATTR:26,47,26,1"], tag="P3")
    rc, out = chip.run(["powersource", "read", "bat-charge-state", node,
                        "26"], timeout=30)
    s.check("3.25 controller reads BatChargeState 1",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")
    res, lines = link.command("AT+MTATTR=26,47,28")
    s.check("3.25 BatFunctionalWhileCharging reads 0 (the second "
            "RECHG-mandatory attribute, present and defaulted)",
            res == 0 and lines == ["+MTATTR:26,47,28,0"], tag="P3")
    res, lines = link.command("AT+MTATTR=26,47,24,5000")
    s.check("3.25 BatCapacity write (5000 mAh) -> OK, echo line",
            res == 0 and lines == ["+MTATTR:26,47,24,5000"], tag="P3")
    rc, out = chip.run(["powersource", "read", "bat-capacity", node, "26"],
                       timeout=30)
    s.check("3.25 controller reads BatCapacity 5000",
            rc == 0 and parse_int_attr(out) == 5000, tag="P3")

    # --- B263 pin: BatCapacity at a realistic home-battery scale ---
    # mt_devtypes.cpp used to mirror the SDK example's 0x00..0xFFFF bound
    # (esp_matter_endpoint.cpp:1958), which cannot hold a home-scale pack
    # nameplate under any plausible unit: 13.5 kWh is 13,500,000 mWh, or
    # about 281,250 mAh at 48 V, and 65535 reaches neither.
    # PowerSourceCluster.xml types BatCapacity uint32 with no
    # <constraint> (and carries no unit at all), so the fix widens the
    # bound to the full uint32 domain; this row writes past the old cap
    # and reads the same value back, which is exactly the write the B263
    # bug report could not make. 13500000 is the value, not a unit claim.
    res, lines = link.command("AT+MTATTR=26,47,24,13500000")
    s.check("3.25 BatCapacity write past the old 0xFFFF cap (13.5 kWh, "
            "13500000) -> OK, echo line (B263)",
            res == 0 and lines == ["+MTATTR:26,47,24,13500000"], tag="P3")
    rc, out = chip.run(["powersource", "read", "bat-capacity", node, "26"],
                       timeout=30)
    s.check("3.25 controller reads BatCapacity 13500000 (B263)",
            rc == 0 and parse_int_attr(out) == 13500000, tag="P3")

    # --- the sixth measurement pool pair, charging draw ---
    res, lines = link.command("AT+MTMEAS=26,144,0,230000,2,-2200000")
    s.check("3.25 power push with a negative ActivePower (charging draw) "
            "-> OK (the sixth MT_MEAS_MAX pool pair)",
            res == 0 and lines == [], tag="P3")
    s.check("3.25 no +MTATTR URC from the push (Instance-owned values, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:26,144", 1.5),
            tag="P3")
    rc, out = chip.run(["electricalpowermeasurement", "read",
                        "active-power", node, "26"], timeout=30)
    s.check("3.25 controller reads ActivePower -2200000",
            rc == 0 and parse_int_attr(out) == -2200000, tag="P3")

    # --- DEM: the first pool slot serves this endpoint ---
    res, lines = link.command("AT+MTMEAS=26,152,0,5,1,1")
    s.check("3.25 ESA identity push (ESAType BatteryStorage 5, "
            "ESACanGenerate true) -> OK", res == 0 and lines == [],
            tag="P3")
    s.check("3.25 no +MTATTR URC from the 0x0098 push (Instance-owned, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:26,152", 1.5),
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read", "esatype", node,
                        "26"], timeout=30)
    s.check("3.25 controller reads ESAType 5 (BatteryStorage)",
            rc == 0 and parse_int_attr(out) == 5, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "esacan-generate", node, "26"], timeout=30)
    s.check("3.25 controller reads ESACanGenerate TRUE",
            rc == 0 and re.search(r"ESACanGenerate:\s*TRUE", out)
            is not None, tag="P3")
    res, _ = link.command(
        "AT+MTDEMCAP=26,2,1,1000000,5000000,60,1800")
    s.check("3.25 AT+MTDEMCAP one entry, cause 2 "
            "(GridOptimizationAdjustment) -> OK", res == 0, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "power-adjustment-capability", node, "26"],
                       timeout=30)
    s.check("3.25 controller reads the single capability entry back "
            "verbatim",
            rc == 0
            and parse_power_adjust_entries(out) == [(1000000, 5000000,
                                                     60, 1800)], tag="P3")
    s.check("3.25 capability cause is the pushed baseline 2",
            rc == 0 and parse_cause_values(out) == [2], tag="P3")

    # --- DeviceEnergyManagementMode: tag-0 default + explicit tag ---
    res, _ = link.command(
        'AT+MTMODES=26,159,0,0,"NoOptimization",1,16385,"DeviceOpt"')
    s.check("3.25 DEMMode staged (tag-0 default + explicit "
            "kDeviceOptimization) -> OK", res == 0, tag="P3")
    rc, out = chip.run(["deviceenergymanagementmode", "read",
                        "supported-modes", node, "26"], timeout=30)
    s.check("3.25 SupportedModes labels verbatim",
            rc == 0 and parse_string_list(out) == ["NoOptimization",
                                                   "DeviceOpt"], tag="P3")
    s.check("3.25 ModeTags: defaulted kNoOptimization (0x4000), explicit "
            "kDeviceOptimization (0x4001)",
            rc == 0 and parse_mode_tag_values(out) == [0x4000, 0x4001],
            tag="P3")


# Energy round C1: the two numbers that make the PowerAdjust duration-clock
# rule assertable. Design spec 3.3 and TC_DEM_2_2 step 14 say a re-adjust
# accepted while ESAState is already PowerAdjustActive does NOT re-arm the
# clock, so the eventual PowerAdjustEnd's duration measures from the FIRST
# accept. A firmware that re-armed it (one line, main.cpp's
# `if (!in_progress)` around the m_pa_start_ms write) reports a duration
# short by however long the first accept and the re-adjust are apart, so
# the check is: reported >= (end - first accept) - slack.
#
# PA_CLOCK_GAP_S is the separation step_3_26 GUARANTEES between the two
# accepts. On a real rig the three chip-tool round trips in between already
# exceed it and the wait is a no-op; it exists so the check keeps its
# discriminating power on a fast rig rather than silently degrading into
# "some integer was printed", which is exactly the hole this constant was
# added to close.
#
# PA_CLOCK_SLACK_S covers the firmware's integer-second truncation (up to
# 1 s, since duration is (now_ms - start_ms) / 1000) plus link and
# round-trip jitter between the harness's timestamps and the firmware's.
# It must stay well under PA_CLOCK_GAP_S or the check stops separating.
PA_CLOCK_GAP_S = 6.0
PA_CLOCK_SLACK_S = 2.0


def _hold_clock_gap(ctx, since, gap=PA_CLOCK_GAP_S):
    """Block until `gap` seconds have passed since `since` on ctx's clock.

    Both the clock and the sleeper are ctx seams so the self-test can drive
    the duration bound deterministically instead of against wall time; the
    real run uses time.monotonic/time.sleep. Returns the clock reading it
    stopped at."""
    clock = ctx.clock or time.monotonic
    sleeper = ctx.sleeper or time.sleep
    while True:
        now = clock()
        if now >= since + gap:
            return now
        sleeper(min(0.5, since + gap - now))


def step_3_26_dem(ctx):
    """Energy round C1 (0.10.0), task 4: the standalone Device Energy
    Management ESA (PHASE3_COMPOSITION 27, endpoint 27, 0x050D variant 0
    ControllableESA; mt_devtypes.cpp's mk_dem() through
    mt_add_dem_triple()), and with it the whole PowerAdjust protocol:
    AT+MTDEMCAP (spec 3.26), the adjudicated 152/0 and 152/1 forwards
    (spec 3.17) and the two LogEvent-emitted events.

    Identity and absences: 0x050D (1293) alone, DEM (152) and DEMMode
    (159) served, no PowerSource (47) and no EPM (144), so a power push
    here answers +MTERR:3. AcceptedCommandList is exactly {0, 1}, which
    is the IRON RULE's observability net (design spec 2.5): the ember
    command entries and the Instance's FeatureMap-derived list agree
    only because feature::power_adjustment::add() created both, and a
    hand-set FeatureMap bit would diverge them into no-status invokes.
    AttributeList carries exactly {0,1,2,3,4,5,7}: no Forecast (6, the
    disclosed PFR/SFR gap) and, the point of the check, NO NINTH id.
    Field 6 (AdjustmentEnergyUse) is an EVENT CARRIER, not an attribute
    (spec 3.25), so it must never appear in an attribute read; that
    absence is pinned three ways here, by the attribute-list shape, by
    assert_no_urc on the push, and by the value surfacing only in
    PowerAdjustEnd's EnergyUse field.

    DE270: a direct AT+MTATTR write to ESAState (attribute 2, Instance-
    served) answers +MTERR:11, not a bare ERROR, for BOTH an in-range
    value (1/Online) and one AT+MTMEAS's own validate pass would reject
    (9, outside ESAStateEnum) -- the code is about writability, not the
    value. A control row against Forecast (attribute 6, genuinely absent
    from this thunk, the PFR/SFR gap above) still answers +MTERR:4, so
    the two codes are distinguishable: 11 means "exists but not
    writable", 4 means "no such attribute".

    Server-side in-state guard, run FIRST while ESAState is still Online
    from boot: CancelPowerAdjustRequest answers InvalidInState (0xCB)
    FROM THE CHIP SERVER (device-energy-management-server.cpp checks
    ESAState before calling the delegate), so unlike round B's
    CancelBoost the firmware has nothing to guard and NO +MTCMD is ever
    raised. Both halves are asserted.

    Pool position: this endpoint takes the SECOND HearthDemDelegate slot
    (2 of MT_DEM_MAX 4), the battery at slot 26 having taken the first,
    so the AT+MTDEMCAP round trip below is also the proof the pool
    serves more than its first endpoint. Nothing here reads the slot
    index directly; the evidence is that step 3.25's install reads back
    on endpoint 26 and this step's install reads back on endpoint 27, so
    two slots are demonstrably routed to two endpoints.

    The PowerAdjust chain, in order:
      - AT+MTDEMCAP installs two entries, cause 1
        (LocalOptimizationAdjustment) as the baseline; a controller read
        gets the struct list back; n=0 makes the whole capability read
        null; the two entries are re-installed for the chain.
      - The canonical vector is power 5000000000 (above 2^32, the 64-bit
        pipeline through a command payload rather than a push) with
        duration 60, admitted by the first capability entry.
      - DENY once: the forward carries the exact tail 5000000000,60,0,
        the verdict maps to Status::Failure on the wire, and NOTHING
        happens (no PowerAdjustStart, ESAState still Online).
      - ALLOW: same vector; the FIRMWARE owns the transition (contrast
        round B's Boost, where the host pushed BoostState), so ESAState
        reads PowerAdjustActive with no host push at all, exactly one
        PowerAdjustStart is emitted, and the capability's cause is
        stamped 1 (LocalOptimization -> LocalOptimizationAdjustment).
      - RE-ADJUST while active, with the other cause: the forward still
        reaches the host and the accept still stamps the cause 2
        (GridOptimization -> GridOptimizationAdjustment), but NO second
        PowerAdjustStart is emitted and the duration clock is not
        re-armed (TC_DEM_2_2 step 14). The harness pins the
        SUPPRESSION, which is why the assertion is "still exactly one".
      - The host pushes field 6, then ends the adjustment by pushing
        ESAState Online: PowerAdjustEnd(NormalCompletion = 0, measured
        duration, EnergyUse 120000), and the capability cause is
        restored to the AT+MTDEMCAP baseline 1.
      - A second adjustment ended by cancel-power-adjust-request: the
        payload-less forward is answered allow, PowerAdjustEnd carries
        cause 4 (Cancelled) and EnergyUse 0, because the first End
        CONSUMED the field-6 cache and nothing re-pushed it.

    Same-state pushes suppress BOTH the event and the attribute report:
    ESAState is reported on change only (the reference implementation's
    shape, the deliberate divergence from round B's report-per-sample
    BoostState). The event half is invisible without a subscription, so
    the closing segment subscribes to ESAState the step_3_21 way, pushes
    the CURRENT state and requires NO report, then pushes a different
    one and requires one. Subscriber contract: it runs last, and no
    ChipTool.run() happens while it lives.

    INFERENCE in this step's body (energy round C1, awaiting the bench;
    the parse_energy_values confidence class, with the three new
    parsers' own shapes documented in their docstrings):

      - `EnergyUse:` and `Duration:` as the PowerAdjustEnd event's field
        labels, both plain decimal through the integral overload. Only
        the labels are inferred; the VALUES are the round's own
        contracts (120000 then 0 for the consumed cache; the duration
        bound above).
      - `PowerAdjustmentCapability:\\s*null` as the null rendering, from
        DataModelLogger.h's Nullable template, which logs the literal
        string "null" and nothing else. This is what separates n=0 from
        an empty list, so if the bench sees an empty-list rendering
        instead, the DISTINCTION still has to hold; only the text of the
        assertion moves.
      - parse_event_count against a FIELDLESS event block
        (`PowerAdjustStart: {` immediately followed by `}`). The
        `<EventName>: {` form parse_event_count matches is CONFIRMED,
        but no earlier step counts an event with no fields at all, and
        every PowerAdjustStart assertion here is a count."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    responder = CmdResponder(link)

    # --- identity, absences, and the two derived lists ---
    rc, out = chip.run(["descriptor", "read", "device-type-list", node,
                        "27"], timeout=30)
    types = parse_device_types(out)
    s.check("3.26 device type list is 0x050D (1293) alone",
            rc == 0 and types == [1293], tag="P3")
    rc, out = chip.run(["descriptor", "read", "server-list", node, "27"],
                       timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.26 server list carries DEM (152) and DEMMode (159)",
            rc == 0 and 152 in servers and 159 in servers, tag="P3")
    s.check("3.26 no PowerSource (47) and no EPM (144) on the standalone "
            "ESA (the thin add() shape, pinned as an absence)",
            rc == 0 and bool(servers) and 47 not in servers
            and 144 not in servers, tag="P3")
    s.check("3.26 AT+MTMEAS=27,144,0,230000 -> +MTERR:3 (no "
            "ElectricalPowerMeasurement cluster here)",
            expect_err("AT+MTMEAS=27,144,0,230000", 3)(link), tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "accepted-command-list", node, "27"], timeout=30)
    s.check("3.26 AcceptedCommandList is exactly {0, 1} (the iron rule: "
            "feature::power_adjustment::add() created the ember entries "
            "AND the FeatureMap bit, so the two surfaces agree)",
            rc == 0
            and sorted(parse_accepted_command_list(out)) == [0, 1],
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "attribute-list", node, "27"], timeout=30)
    local = sorted(a for a in parse_accepted_command_list(out)
                   if a < 0xF000)
    s.check("3.26 AttributeList is exactly {0,1,2,3,4,5,7}: no Forecast "
            "(6, the disclosed PFR/SFR gap) and NO attribute for field 6 "
            "(AdjustmentEnergyUse is an event carrier, spec 3.25)",
            rc == 0 and local == [0, 1, 2, 3, 4, 5, 7], tag="P3")

    # --- AT+MTDEMCAP lookup and value rows (spec 3.26) ---
    s.check("3.26 AT+MTDEMCAP=99,1,0 -> +MTERR:2 (unknown endpoint)",
            expect_err("AT+MTDEMCAP=99,1,0", 2)(link), tag="P3")
    s.check("3.26 AT+MTDEMCAP=1,1,0 -> +MTERR:3 (the light carries no "
            "DeviceEnergyManagement cluster)",
            expect_err("AT+MTDEMCAP=1,1,0", 3)(link), tag="P3")
    s.check("3.26 AT+MTDEMCAP=27,3,0 -> +MTERR:1 (cause 3 outside "
            "PowerAdjustReasonEnum: the bridge's own range check)",
            expect_err("AT+MTDEMCAP=27,3,0", 1)(link), tag="P3")
    s.check("3.26 AT+MTDEMCAP=27,1,1,5000,1000,60,3600 -> +MTERR:1 "
            "(minPower > maxPower would make the server validate against "
            "an empty interval)",
            expect_err("AT+MTDEMCAP=27,1,1,5000,1000,60,3600", 1)(link),
            tag="P3")
    s.check("3.26 AT+MTDEMCAP=27,1,1,1000,5000,3600,60 -> +MTERR:1 "
            "(minDuration > maxDuration, the same interval rule)",
            expect_err("AT+MTDEMCAP=27,1,1,1000,5000,3600,60", 1)(link),
            tag="P3")

    # --- 0x0098 field validation on an endpoint that DOES serve it ---
    s.check("3.26 AT+MTMEAS=27,152,7,1 -> +MTERR:1 (field 7 unknown to "
            "0x0098: the bridge's validate pass)",
            expect_err("AT+MTMEAS=27,152,7,1", 1)(link), tag="P3")
    s.check("3.26 AT+MTMEAS=27,152,0,14 -> +MTERR:1 (ESAType 0x0E is the "
            "gap between the contiguous 0x00..0x0D and kOther 0xFF)",
            expect_err("AT+MTMEAS=27,152,0,14", 1)(link), tag="P3")
    s.check("3.26 AT+MTMEAS=27,152,2,9 -> +MTERR:1 (9 is outside "
            "ESAStateEnum 0..4)",
            expect_err("AT+MTMEAS=27,152,2,9", 1)(link), tag="P3")
    s.check("3.26 AT+MTMEAS=27,152,5,4 -> +MTERR:1 (4 is outside "
            "OptOutStateEnum 0..3)",
            expect_err("AT+MTMEAS=27,152,5,4", 1)(link), tag="P3")

    # --- DE270: AT+MTATTR write to an Instance-served attribute answers
    # +MTERR:11, not a bare ERROR, and the code is not about the value ---
    s.check("3.26 AT+MTATTR=27,152,2,1 -> +MTERR:11 (ESAState is "
            "Instance-served: exists and 1/Online is a legal "
            "ESAStateEnum value, but this path cannot write it, DE270)",
            expect_err("AT+MTATTR=27,152,2,1", 11)(link), tag="P3")
    s.check("3.26 AT+MTATTR=27,152,2,9 -> +MTERR:11 too (the generic "
            "AT+MTATTR path never reaches enum validation for a "
            "managed-internally attribute, unlike AT+MTMEAS's own "
            "validate pass above, which answers +MTERR:1 for this exact "
            "value: DE270's failure is not about the value at all)",
            expect_err("AT+MTATTR=27,152,2,9", 11)(link), tag="P3")
    s.check("3.26 CONTROL: AT+MTATTR=27,152,6,1 -> +MTERR:4 (Forecast, "
            "id 6, is genuinely absent from this thunk's AttributeList, "
            "the PFR/SFR gap pinned above; distinguishes +MTERR:11's "
            "\"exists but not writable\" from a lookup failure)",
            expect_err("AT+MTATTR=27,152,6,1", 4)(link), tag="P3")

    # --- identity pushes, including kOther and the +-5 GmW envelope ---
    res, lines = link.command(
        "AT+MTMEAS=27,152,0,255,3,-5000000000,4,5000000000")
    s.check("3.26 ESA identity push (ESAType kOther 0xFF, AbsMin/MaxPower "
            "+-5 GmW, both above 2^32) -> OK",
            res == 0 and lines == [], tag="P3")
    s.check("3.26 no +MTATTR URC from the push (Instance-owned values, "
            "spec 3.25)", link.assert_no_urc(r"\+MTATTR:27,", 1.5),
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read", "esatype", node,
                        "27"], timeout=30)
    s.check("3.26 controller reads ESAType 255 (kOther, the enum's "
            "non-contiguous top value)",
            rc == 0 and parse_int_attr(out) == 255, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read", "abs-min-power",
                        node, "27"], timeout=30)
    s.check("3.26 controller reads AbsMinPower -5000000000 (signed, "
            "64-bit, not truncated)",
            rc == 0 and parse_int_attr(out) == -5000000000, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read", "abs-max-power",
                        node, "27"], timeout=30)
    s.check("3.26 controller reads AbsMaxPower 5000000000",
            rc == 0 and parse_int_attr(out) == 5000000000, tag="P3")

    # --- the server's in-state guard, while ESAState is still Online ---
    link.drain(0.3)
    rc, out = chip.run(["deviceenergymanagement",
                        "cancel-power-adjust-request", node, "27"],
                       timeout=30)
    s.check("3.26 in-state guard: cancel while not PowerAdjustActive "
            "fails (rc != 0)", rc != 0, tag="P3")
    s.check("3.26 in-state guard: status 0xCB InvalidInState, FROM THE "
            "CHIP SERVER (the firmware has no guard of its own here)",
            parse_status(out) == 0xCB, tag="P3")
    s.check("3.26 in-state guard: no +MTCMD raised (the server answers "
            "before the delegate runs)",
            link.assert_no_urc(r"\+MTCMD:", 2.0), tag="P3")

    # --- AT+MTDEMCAP end to end: install, read back, null, reinstall ---
    res, _ = link.command("AT+MTDEMCAP=27,1,2,1000000000,10000000000,30,"
                          "3600,500,2000,30,600")
    s.check("3.26 AT+MTDEMCAP two entries, cause 1 -> OK", res == 0,
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "power-adjustment-capability", node, "27"],
                       timeout=30)
    s.check("3.26 controller reads both capability entries back verbatim "
            "(maxPower above 2^32 through the struct list)",
            rc == 0
            and parse_power_adjust_entries(out) == [
                (1000000000, 10000000000, 30, 3600),
                (500, 2000, 30, 600)], tag="P3")
    s.check("3.26 capability cause is the pushed baseline 1",
            rc == 0 and parse_cause_values(out) == [1], tag="P3")
    res, _ = link.command("AT+MTDEMCAP=27,1,0")
    s.check("3.26 AT+MTDEMCAP n=0 -> OK", res == 0, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "power-adjustment-capability", node, "27"],
                       timeout=30)
    s.check("3.26 n=0 makes the whole capability read null (not an empty "
            "list: the distinction the server's ConstraintError rests on)",
            rc == 0 and parse_power_adjust_entries(out) == []
            and re.search(r"PowerAdjustmentCapability:\s*null", out)
            is not None, tag="P3")
    res, _ = link.command("AT+MTDEMCAP=27,1,2,1000000000,10000000000,30,"
                          "3600,500,2000,30,600")
    s.check("3.26 capability reinstalled for the adjust chain -> OK",
            res == 0, tag="P3")

    # --- deny once: the forward reaches the host, nothing else happens ---
    link.drain(0.3)
    handle = invoke_chip(ctx, ["deviceenergymanagement",
                              "power-adjust-request", "5000000000", "60",
                              "0", node, "27"], timeout=30)
    fwd = responder.expect(cluster=152, command=0, verdict=0,
                           payload=[5000000000, 60, 0], timeout=5.0)
    s.check("3.26 PowerAdjustRequest forward answered DENY with the "
            "canonical tail 5000000000,60,0", fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.26 deny: chip-tool reports failure (rc != 0)", rc != 0,
            tag="P3")
    s.check("3.26 deny: status 0x01 Failure (the verdict IS the wire "
            "response)", parse_status(out) == 0x1, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read-event",
                        "power-adjust-start", node, "27"], timeout=30)
    s.check("3.26 deny: no PowerAdjustStart event",
            rc == 0 and parse_event_count(out, "PowerAdjustStart") == 0,
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read", "esastate",
                        node, "27"], timeout=30)
    s.check("3.26 deny: ESAState still 1 (Online)",
            rc == 0 and parse_int_attr(out) == 1, tag="P3")

    # --- allow: the FIRMWARE owns the transition ---
    handle = invoke_chip(ctx, ["deviceenergymanagement",
                              "power-adjust-request", "5000000000", "60",
                              "0", node, "27"], timeout=30)
    fwd = responder.expect(cluster=152, command=0, verdict=1,
                           payload=[5000000000, 60, 0], timeout=5.0)
    # The duration clock starts HERE in the firmware (the accept, not the
    # invoke), so this is the reference the PowerAdjustEnd bound below
    # measures against. Taken right after the verdict went out, which is
    # a few ms EARLIER than the firmware's own stamp: that direction is
    # safe, since it can only make the measured elapsed longer than the
    # reported duration, which the slack absorbs.
    t_allow = (ctx.clock or time.monotonic)()
    s.check("3.26 PowerAdjustRequest forward answered ALLOW",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.26 allow: chip-tool exits 0 (Status::Success)", rc == 0,
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read", "esastate",
                        node, "27"], timeout=30)
    s.check("3.26 allow: ESAState reads 3 (PowerAdjustActive) with NO "
            "host push: the firmware owns this transition",
            rc == 0 and parse_int_attr(out) == 3, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read-event",
                        "power-adjust-start", node, "27"], timeout=30)
    s.check("3.26 allow: exactly one PowerAdjustStart event",
            rc == 0 and parse_event_count(out, "PowerAdjustStart") == 1,
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "power-adjustment-capability", node, "27"],
                       timeout=30)
    s.check("3.26 allow: capability cause stamped 1 (LocalOptimization "
            "-> LocalOptimizationAdjustment)",
            rc == 0 and parse_cause_values(out) == [1], tag="P3")

    # --- re-adjust while active: forward yes, second Start NO ---
    # Guarantee the accepts are far enough apart for the duration bound
    # below to tell a re-armed clock from normal jitter (PA_CLOCK_GAP_S).
    _hold_clock_gap(ctx, t_allow)
    handle = invoke_chip(ctx, ["deviceenergymanagement",
                              "power-adjust-request", "5000000000", "60",
                              "1", node, "27"], timeout=30)
    fwd = responder.expect(cluster=152, command=0, verdict=1,
                           payload=[5000000000, 60, 1], timeout=5.0)
    s.check("3.26 re-adjust: the forward still reaches the host (only "
            "the event and the clock are suppressed)", fwd is not None,
            tag="P3")
    rc, out = handle.join(30)
    s.check("3.26 re-adjust: chip-tool exits 0", rc == 0, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read-event",
                        "power-adjust-start", node, "27"], timeout=30)
    s.check("3.26 re-adjust: STILL exactly one PowerAdjustStart "
            "(TC_DEM_2_2 step 14: SUCCESS and no event sent)",
            rc == 0 and parse_event_count(out, "PowerAdjustStart") == 1,
            tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "power-adjustment-capability", node, "27"],
                       timeout=30)
    s.check("3.26 re-adjust: capability cause restamped 2 "
            "(GridOptimization -> GridOptimizationAdjustment)",
            rc == 0 and parse_cause_values(out) == [2], tag="P3")

    # --- field 6, then the host's normal end ---
    res, lines = link.command("AT+MTMEAS=27,152,6,120000")
    s.check("3.26 AdjustmentEnergyUse push (field 6) -> OK",
            res == 0 and lines == [], tag="P3")
    s.check("3.26 field 6 marks nothing dirty and raises no URC (event "
            "carrier, not an attribute)",
            link.assert_no_urc(r"\+MTATTR:27,", 1.5), tag="P3")
    # Taken BEFORE the push that triggers the emission, so the firmware's
    # own end timestamp is at or after it: again the safe direction.
    t_end = (ctx.clock or time.monotonic)()
    res, _ = link.command("AT+MTMEAS=27,152,2,1")
    s.check("3.26 host ends the adjustment by pushing ESAState Online "
            "-> OK", res == 0, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read-event",
                        "power-adjust-end", node, "27"], timeout=30)
    s.check("3.26 exactly one PowerAdjustEnd event",
            rc == 0 and parse_event_count(out, "PowerAdjustEnd") == 1,
            tag="P3")
    s.check("3.26 PowerAdjustEnd cause 0 (NormalCompletion) with the "
            "cached EnergyUse 120000 and a Duration field",
            rc == 0 and parse_cause_values(out) == [0]
            and re.search(r"EnergyUse:\s*120000\b", out) is not None
            and re.search(r"Duration:\s*\d+", out) is not None, tag="P3")
    # The other half of TC_DEM_2_2 step 14, and the reason the re-adjust
    # ran at all: the clock was NOT re-armed, so this duration measures
    # from the FIRST accept. A re-armed clock reports the far shorter
    # interval since the re-adjust, which is at least PA_CLOCK_GAP_S
    # short of the bound.
    dur = re.search(r"Duration:\s*(\d+)", out)
    floor_s = (t_end - t_allow) - PA_CLOCK_SLACK_S
    # The check NAME must stay byte-stable: it is the baseline's key, so
    # the measured numbers go to stdout beside it, never into it.
    print("    (duration rule: reported %s s, floor %.1f s, allow-to-end "
          "span %.1f s)" % (dur.group(1) if dur else "none", floor_s,
                            t_end - t_allow))
    s.check("3.26 PowerAdjustEnd duration measures from the FIRST accept, "
            "not the re-adjust (the clock is not re-armed)",
            rc == 0 and dur is not None
            and int(dur.group(1)) >= floor_s, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read",
                        "power-adjustment-capability", node, "27"],
                       timeout=30)
    s.check("3.26 PowerAdjustEnd restored the capability cause to the "
            "AT+MTDEMCAP baseline 1",
            rc == 0 and parse_cause_values(out) == [1], tag="P3")

    # --- second adjustment, ended by cancel: the Cancelled path ---
    handle = invoke_chip(ctx, ["deviceenergymanagement",
                              "power-adjust-request", "5000000000", "60",
                              "0", node, "27"], timeout=30)
    fwd = responder.expect(cluster=152, command=0, verdict=1,
                           payload=[5000000000, 60, 0], timeout=5.0)
    s.check("3.26 second adjustment: forward answered allow",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.26 second adjustment: chip-tool exits 0", rc == 0, tag="P3")
    handle = invoke_chip(ctx, ["deviceenergymanagement",
                              "cancel-power-adjust-request", node, "27"],
                         timeout=30)
    fwd = responder.expect(cluster=152, command=1, verdict=1, timeout=5.0)
    s.check("3.26 CancelPowerAdjustRequest forward answered allow "
            "(payload-less)",
            fwd is not None and fwd["fields"] == [], tag="P3")
    rc, out = handle.join(30)
    s.check("3.26 cancel: chip-tool exits 0", rc == 0, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read-event",
                        "power-adjust-end", node, "27"], timeout=30)
    s.check("3.26 a second PowerAdjustEnd event",
            rc == 0 and parse_event_count(out, "PowerAdjustEnd") == 2,
            tag="P3")
    s.check("3.26 the cancel's PowerAdjustEnd carries cause 4 (Cancelled) "
            "and EnergyUse 0: the first End CONSUMED the field-6 cache",
            rc == 0 and 4 in parse_cause_values(out)
            and re.search(r"EnergyUse:\s*0\b", out) is not None, tag="P3")
    rc, out = chip.run(["deviceenergymanagement", "read", "esastate",
                        node, "27"], timeout=30)
    s.check("3.26 cancel: ESAState back to 1 (Online), reset by the "
            "firmware", rc == 0 and parse_int_attr(out) == 1, tag="P3")

    # --- DeviceEnergyManagementMode on the standalone ESA ---
    res, _ = link.command(
        'AT+MTMODES=27,159,0,0,"NoOptimization",1,16387,"GridOpt"')
    s.check("3.26 DEMMode staged (tag-0 default + explicit "
            "kGridOptimization) -> OK", res == 0, tag="P3")
    rc, out = chip.run(["deviceenergymanagementmode", "read",
                        "supported-modes", node, "27"], timeout=30)
    s.check("3.26 SupportedModes labels verbatim",
            rc == 0 and parse_string_list(out) == ["NoOptimization",
                                                   "GridOpt"], tag="P3")
    s.check("3.26 ModeTags: defaulted kNoOptimization (0x4000), explicit "
            "kGridOptimization (0x4003)",
            rc == 0 and parse_mode_tag_values(out) == [0x4000, 0x4003],
            tag="P3")

    # --- LAST: the on-change-only ESAState report contract ---
    factory = ctx.subscriber_factory or (
        lambda: Subscriber(chip, ctx.node_id, endpoint=27,
                           cluster="deviceenergymanagement",
                           attribute="esastate",
                           parser=parse_esa_state_reports))
    sub = factory()
    started = s.check("3.26 ESAState subscriber starts (priming report)",
                      sub.start(), tag="P3")
    try:
        base = len(sub.reports()) if started else 0
        res, _ = link.command("AT+MTMEAS=27,152,2,1")
        s.check("3.26 same-state ESAState push (Online again) -> OK",
                res == 0, tag="P3")
        s.check("3.26 same-state push produces NO report: ESAState is "
                "reported on change only, the deliberate divergence from "
                "round B's report-per-sample BoostState",
                started and sub.no_new_report(base, 3.0), tag="P3")
        res, _ = link.command("AT+MTMEAS=27,152,2,2")
        s.check("3.26 changed ESAState push (Fault) -> OK", res == 0,
                tag="P3")
        s.check("3.26 the CHANGED push does report (so the silence above "
                "is on-change-only, not a dead subscription)",
                started and sub.wait_new_report(base, 5.0), tag="P3")
    finally:
        sub.stop()
    res, _ = link.command("AT+MTMEAS=27,152,2,1")
    s.check("3.26 ESAState restored to 1 (Online) at step end", res == 0,
            tag="P3")


# Energy round C2 final review, C1: EVERY EnergyEvse command is
# mustUseTimedInvoke="true" (energy-evse-cluster.xml lines 241-283, all
# seven of Disable, EnableCharging, EnableDischarging, StartDiagnostics,
# SetTargets, GetTargets, ClearTargets, checked one by one, not sampled).
# Without --timedInteractionTimeoutMs the SDK answers 0xc6
# NEEDS_TIMED_INTERACTION before HearthEvseDelegate ever runs, so NO
# +MTCMD is raised at all: the allow arm's responder.expect() times out
# and fails, and, worse, the DENY arm PASSES VACUOUSLY, because its
# assertion is rc != 0 and rc is nonzero for entirely the wrong reason.
# The four invocations below were all shipped in that state and could
# never have exercised the EVSE command surface on any bench.
#
# Same value and same reasoning as DOORLOCK_TIMED_INVOKE_MS above (see
# its comment for why 5000 ms cannot race the adjudication delay: the
# timed-interaction timer's job is finished the instant the
# InvokeRequest arrives, which is BEFORE the delegate, the CmdResponder
# round trip and the firmware's own 1000 ms AT+MTCMDRESP deadline).
# Kept as a separate constant rather than reusing the door lock's so
# that a future change to either cluster's window cannot silently move
# the other's.
EVSE_TIMED_INVOKE_MS = "5000"


def step_3_27_evse(ctx):
    """Energy round C2 (0.12.0), task 13: the EVSE endpoint's identity
    (PHASE3_COMPOSITION 28, endpoint 28, 0x050C variant 0) and the
    AT+MTROW-family behaviours that need a REAL, committed EnergyEvse
    store rather than a stage: the SOC-variant rule's POSITIVE arm
    (SoC mandatory), merge-by-day and a full 70-row schedule.

    Adjudicates Disable and EnableCharging with CmdResponder (fix round
    1, task 13 report Finding 3): both are ordinary scalar-tail forwards
    on the UNCHANGED 1000 ms path, the same shape as WHM Boost and DEM
    PowerAdjustRequest, both already proven in this harness. SetTargets
    is deliberately still bench-only: it alone goes through the brand
    new row-bearing +MTCMD form (the 3000 ms window, the seq-qualified
    AT+MTROWGET pull, the two-buffer concurrency argument, task 6's
    report), which is a genuinely different, unproven mechanic, not
    merely "a command this round added". Scripting an untested
    CmdResponder sequence for THAT form, with no way to catch a mistake
    before the consolidated bench session, would be worse than not
    writing it: a broken automated step silently reports the wrong thing
    forever, where a careful prose procedure gets a human's judgement on
    the very first run. SetTargets ALLOWED/DENIED, the empty-target-list
    day clear, the verdict-window etiquette and the WiFi mDNS pause are
    all in this task's report as an exact, ordered bench procedure
    instead, collecting task 6's own bench sections (8, 9, 10) rather
    than re-deriving them.

    Disable/EnableCharging's exact chip-tool argument shape was verified
    against the real binary's own --help before writing this, not
    assumed from task 6's report (which used an unverified JSON-blob
    form): "energyevse enable-charging ChargingEnabledUntil
    MinimumChargeCurrent MaximumChargeCurrent destination-id
    endpoint-id...", three plain positional scalars, and chip-tool
    accepts the literal token "null" for the nullable first field (it
    proceeds past argument parsing with no complaint). Task 14 should
    correct the bench procedure text task 6's report seeded.

    Requires "3.5 commission" purely for the identity reads below
    (a real controller must exist to ask); every row after that is
    AT-only and would work identically on an uncommissioned device.
    Deliberately NOT requiring anything from step_3_2b_row_round_trip's
    AT+MTROW round trip beyond it having left the schedule EMPTY (its
    own unscored clear, matching the MTALARM precedent), since this
    step's own first action is a fresh apply-count-0 in any case."""
    link, s, chip = ctx.link, ctx.suite, ctx.chip
    node = "0x%X" % ctx.node_id
    ep = 28

    # ---- identity ----
    # Bug caught during fix round 1 while researching Finding 2's precise
    # expected lists (mt_devtypes.cpp traced, not assumed): mk_energy_evse
    # composes TWO more device types onto this same endpoint, the same
    # shape heat pump/solar/battery already use (step_3_23/24/25's own
    # "three device types on one endpoint" membership checks, followed
    # here rather than an exact-list equality). energy_evse::add() (the
    # SDK) adds 0x050C (1292) itself; mt_graft_electrical_sensor(ep, true,
    # ...) adds 0x0510 (1296, the composed Electrical Sensor,
    # electrical_sensor::add()'s own add_device_type()); mt_add_dem_
    # triple(ep, false, ...) adds 0x050D (1293) via its own explicit
    # add_device_type() call (device-energy-management-server has no
    # add() helper of its own to do it). An assertion of "1292 alone"
    # here was WRONG and would have failed the very first live run.
    rc, out = chip.run(["descriptor", "read", "device-type-list", node,
                        str(ep)], timeout=30)
    types = parse_device_types(out)
    s.check("3.27 device type list carries 0x050C (1292), the composed "
            "0x0510 sensor (1296) and 0x050D DEM (1293)",
            rc == 0 and 1292 in types and 1296 in types and 1293 in types,
            tag="P3")

    # Server list: the full cluster surface the same three helpers above
    # actually create, traced the same way. EnergyEvse/EnergyEvseMode
    # from energy_evse::add(); PowerTopology/ElectricalPowerMeasurement/
    # ElectricalEnergyMeasurement from mt_graft_electrical_sensor(ep,
    # true, ...) (with_eem=true, so EEM is included); DeviceEnergyManagement/
    # DeviceEnergyManagementMode from mt_add_dem_triple() (DEM itself is
    # already created bare by energy_evse::add()'s own SDK path; the
    # triple's create() call returns the SAME cluster and only attaches
    # the delegate, so there is one DeviceEnergyManagement entry, not
    # two). No Identify: unlike the meter, mk_energy_evse never hand-adds
    # one, and energy_evse::add() does not either.
    rc, out = chip.run(["descriptor", "read", "server-list", node,
                        str(ep)], timeout=30)
    servers = parse_accepted_command_list(out)
    s.check("3.27 server list carries EnergyEvse (153), EnergyEvseMode "
            "(157), DeviceEnergyManagement (152), "
            "DeviceEnergyManagementMode (159), PowerTopology (156), "
            "ElectricalPowerMeasurement (144) and "
            "ElectricalEnergyMeasurement (145)",
            rc == 0 and 153 in servers and 157 in servers
            and 152 in servers and 159 in servers and 156 in servers
            and 144 in servers and 145 in servers, tag="P3")

    # ---- SOC-variant rule, the POSITIVE arm (variant 0: SoC mandatory)
    # ---- the NEGATIVE arm (variant 1, no SOC) is Phase 1's staged
    # t_row_evse_meter_staged. mt_evse_targets_apply_locked()'s pass 1
    # (mt_evse.cpp) enforces this identically on the AT path and the
    # fabric SetTargets path (task 4's review finding).
    res, _ = link.command("AT+MTROW=%d,1,0,2,480,,25000000" % ep)
    s.check("3.27 stage a target with SoC absent -> OK (staging never "
            "enforces the SOC-variant rule; only apply does)",
            res == 0, tag="P3")
    res, _ = link.command("AT+MTROWAPPLY=%d,1,1" % ep)
    s.check("3.27 apply: SoC absent on the SOC-feature endpoint -> "
            "+MTERR:1", res == 1, tag="P3")
    # A failed apply leaves the stage active (cmd_mtrowapply's own
    # comment): discard it explicitly, unscored, so it cannot bleed into
    # the merge-by-day rows below.
    link.command("AT+MTROWCLEAR=%d,1" % ep)

    # ---- merge by day: the case a wholesale-replace implementation
    # passes every naive apply-then-read test and fails only this one
    # (design spec 8.4 item 1a) ----
    res, _ = link.command("AT+MTROW=%d,1,0,2,480,80,25000000" % ep)
    s.check("3.27 merge-by-day: stage a Monday target -> OK", res == 0,
            tag="P3")
    res, _ = link.command("AT+MTROW=%d,1,1,4,600,80,10000000" % ep)
    s.check("3.27 merge-by-day: stage a Tuesday target -> OK", res == 0,
            tag="P3")
    res, _ = link.command("AT+MTROWAPPLY=%d,1,2" % ep)
    s.check("3.27 merge-by-day: apply Monday+Tuesday -> OK", res == 0,
            tag="P3")
    res, lines = cmd_retry(link, "AT+MTROWGET=%d,1" % ep)
    s.check("3.27 merge-by-day: readback carries both targets",
            res == 0 and lines == ["+MTROW:0,2,2,480,80,25000000",
                                   "+MTROW:1,2,4,600,80,10000000"],
            tag="P3")
    res, _ = link.command("AT+MTROW=%d,1,0,2,900,80,5000000" % ep)
    s.check("3.27 merge-by-day: stage a REPLACEMENT Monday target -> OK",
            res == 0, tag="P3")
    res, _ = link.command("AT+MTROWAPPLY=%d,1,1" % ep)
    s.check("3.27 merge-by-day: apply Monday-only -> OK", res == 0,
            tag="P3")
    res, lines = cmd_retry(link, "AT+MTROWGET=%d,1" % ep)
    s.check("3.27 merge-by-day: Monday REPLACED, Tuesday SURVIVES "
            "untouched (subtract_days() only ever removes the day bits "
            "the CURRENT apply names)",
            res == 0 and lines == ["+MTROW:0,2,4,600,80,10000000",
                                   "+MTROW:1,2,2,900,80,5000000"],
            tag="P3")

    # ---- a full 70-row schedule, written, applied and read back row for
    # row (design spec 8.4 item 1) ----
    res, _ = link.command("AT+MTROWAPPLY=%d,1,0" % ep)
    s.check("3.27 clear before the full-schedule test -> OK", res == 0,
            tag="P3")
    day_bits_list = [1, 2, 4, 8, 16, 32, 64]  # 7 distinct single-bit days
    idx = 0
    stage_ok = True
    for day in day_bits_list:
        for t in range(10):
            minutes = t * 120  # 0..1080, all inside 0..1439
            energy = (day * 100 + t) * 1000
            cmd = "AT+MTROW=%d,1,%d,%d,%d,80,%d" % (ep, idx, day, minutes,
                                                     energy)
            if link.command(cmd)[0] != 0:
                stage_ok = False
            idx += 1
    s.check("3.27 all 70 rows staged -> OK", stage_ok, tag="P3")
    res, _ = link.command("AT+MTROWAPPLY=%d,1,70" % ep)
    s.check("3.27 apply the full 70-row schedule -> OK", res == 0,
            tag="P3")
    res, lines = cmd_retry(link, "AT+MTROWGET=%d,1" % ep)
    expect = []
    idx = 0
    for day in day_bits_list:
        for t in range(10):
            minutes = t * 120
            energy = (day * 100 + t) * 1000
            expect.append("+MTROW:%d,70,%d,%d,80,%d"
                          % (idx, day, minutes, energy))
            idx += 1
    s.check("3.27 the full 70-row schedule reads back row for row",
            res == 0 and lines == expect, tag="P3")
    res, _ = link.command("AT+MTROWAPPLY=%d,1,0" % ep)
    s.check("3.27 clear the full schedule at step end -> OK", res == 0,
            tag="P3")

    # ---- Disable and EnableCharging: ordinary scalar-tail adjudicated
    # forwards (0x0099 commands 1 and 2), unlike SetTargets untouched by
    # this round's row-bearing machinery. Neither writes an attribute on
    # allow (task 6 report section 4: the host is the single authority on
    # what the hardware did), so there is no attribute readback here. ----
    responder = CmdResponder(link)

    # EnableCharging allow: null chargingEnabledUntil renders as an
    # EMPTY tail field, not a presence mask (task 6 report: the empty
    # token occupies the position the value would have, the
    # SetCookingParameters interior-gap convention, not Boost's mask).
    handle = invoke_chip(ctx, ["energyevse", "enable-charging", "null",
                              "6000", "32000", node, str(ep),
                              "--timedInteractionTimeoutMs",
                              EVSE_TIMED_INVOKE_MS], timeout=30)
    fwd = responder.expect(cluster=153, command=2, verdict=1,
                           payload=[None, 6000, 32000], timeout=5.0)
    s.check("3.27 EnableCharging allow: forward answered, null "
            "chargingEnabledUntil rendered as an empty tail field",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.27 EnableCharging allow: chip-tool exits 0 (Success)",
            rc == 0, tag="P3")

    # EnableCharging deny: a non-null chargingEnabledUntil renders
    # verbatim in the same tail position.
    handle = invoke_chip(ctx, ["energyevse", "enable-charging", "1800",
                              "6000", "32000", node, str(ep),
                              "--timedInteractionTimeoutMs",
                              EVSE_TIMED_INVOKE_MS], timeout=30)
    fwd = responder.expect(cluster=153, command=2, verdict=0,
                           payload=[1800, 6000, 32000], timeout=5.0)
    s.check("3.27 EnableCharging deny: forward answered, non-null "
            "chargingEnabledUntil (1800) rendered verbatim",
            fwd is not None, tag="P3")
    rc, out = handle.join(30)
    s.check("3.27 EnableCharging deny: chip-tool reports Failure "
            "(rc != 0)", rc != 0, tag="P3")
    s.check("3.27 EnableCharging deny: wire status 0x1 Failure",
            parse_status(out) == 0x1, tag="P3")

    # Disable allow: no tail at all (the four-field line, payload-less).
    handle = invoke_chip(ctx, ["energyevse", "disable", node, str(ep),
                              "--timedInteractionTimeoutMs",
                              EVSE_TIMED_INVOKE_MS], timeout=30)
    fwd = responder.expect(cluster=153, command=1, verdict=1, timeout=5.0)
    s.check("3.27 Disable allow: forward answered, no tail",
            fwd is not None and fwd["fields"] == [], tag="P3")
    rc, out = handle.join(30)
    s.check("3.27 Disable allow: chip-tool exits 0 (Success)", rc == 0,
            tag="P3")

    # Disable deny.
    handle = invoke_chip(ctx, ["energyevse", "disable", node, str(ep),
                              "--timedInteractionTimeoutMs",
                              EVSE_TIMED_INVOKE_MS], timeout=30)
    fwd = responder.expect(cluster=153, command=1, verdict=0, timeout=5.0)
    s.check("3.27 Disable deny: forward answered", fwd is not None,
            tag="P3")
    rc, out = handle.join(30)
    s.check("3.27 Disable deny: chip-tool reports Failure (rc != 0)",
            rc != 0, tag="P3")
    s.check("3.27 Disable deny: wire status 0x1 Failure",
            parse_status(out) == 0x1, tag="P3")


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
            # A capped run declares a PHASE3_COMPOSITION PREFIX, so every
            # step whose targets sit above the cap addresses endpoints
            # that do not exist. That is not a broken precondition and
            # not a failure: it is the same "architecturally does not
            # apply to this run" shape step_2_13 has on a WiFi image, so
            # it lands in not_applicable() and does not tip the exit
            # code. max_ep is a required key on every entry
            # (test_every_phase3_step_declares_max_ep), because a step
            # that silently defaulted to 0 would RUN against missing
            # endpoints and fail for the wrong reason.
            cap = getattr(ctx, "max_endpoints", None)
            if cap is not None and step["max_ep"] > cap:
                ctx.suite.not_applicable(
                    name, "composition capped at %d endpoints: this step "
                          "targets endpoint %d" % (cap, step["max_ep"]))
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
    {"name": "3.1 compose + boot-rebuild pin", "max_ep": 0,
     "fn": step_3_1_compose},
    {"name": "3.2 endpoint-dependent grammar", "max_ep": 11,
     "fn": step_3_2_grammar,
     "requires": ["3.1 compose + boot-rebuild pin"]},
    {"name": "3.2b AT+MTROW round trip", "max_ep": 28,
     "fn": step_3_2b_row_round_trip,
     "requires": ["3.1 compose + boot-rebuild pin"]},
    {"name": "3.3 self-test wedge reproduction", "max_ep": 5,
     "fn": step_3_3_selftest_wedge,
     "requires": ["3.1 compose + boot-rebuild pin"]},
    {"name": "3.4 store grammar edges", "max_ep": 4, "fn": step_3_4_stores,
     "requires": ["3.1 compose + boot-rebuild pin"]},
    {"name": "3.5 commission", "max_ep": 0, "fn": step_3_5_commission,
     "requires": ["3.4 store grammar edges"]},
    {"name": "3.6 valve", "max_ep": 2, "fn": step_3_6_valve,
     "requires": ["3.5 commission"]},
    {"name": "3.7 mode select", "max_ep": 3, "fn": step_3_7_modes,
     "requires": ["3.5 commission"]},
    {"name": "3.8 chime", "max_ep": 4, "fn": step_3_8_chime,
     "requires": ["3.5 commission"]},
    {"name": "3.9 operational state", "max_ep": 7, "fn": step_3_9_opstate,
     "requires": ["3.5 commission"]},
    {"name": "3.10 smoke/CO alarm", "max_ep": 5, "fn": step_3_10_smoke,
     "requires": ["3.5 commission"]},
    {"name": "3.11 power source", "max_ep": 6, "fn": step_3_11_power,
     "requires": ["3.5 commission"]},
    {"name": "3.12 lock, switch, temp levels", "max_ep": 10,
     "fn": step_3_12_lock_switch_levels, "requires": ["3.5 commission"]},
    {"name": "3.15 robotic vacuum cleaner", "max_ep": 12, "fn": step_3_15_rvc,
     "requires": ["3.5 commission"]},
    {"name": "3.16 microwave oven", "max_ep": 13, "fn": step_3_16_microwave,
     "requires": ["3.5 commission"]},
    {"name": "3.17 composed refrigerator", "max_ep": 16,
     "fn": step_3_17_composed_fridge,
     "requires": ["3.5 commission"]},
    {"name": "3.18 oven cavity", "max_ep": 18, "fn": step_3_18_oven_cavity,
     "requires": ["3.5 commission"]},
    {"name": "3.19 cook surface", "max_ep": 20, "fn": step_3_19_cook_surface,
     "requires": ["3.5 commission"]},
    {"name": "3.20 electrical sensor", "max_ep": 21,
     "fn": step_3_20_electrical_sensor,
     "requires": ["3.5 commission"]},
    {"name": "3.21 electrical meter", "max_ep": 22,
     "fn": step_3_21_electrical_meter,
     "requires": ["3.5 commission"]},
    {"name": "3.22 water heater", "max_ep": 23, "fn": step_3_22_water_heater,
     "requires": ["3.5 commission"]},
    {"name": "3.23 heat pump", "max_ep": 24, "fn": step_3_23_heat_pump,
     "requires": ["3.5 commission"]},
    {"name": "3.24 solar power", "max_ep": 25, "fn": step_3_24_solar_power,
     "requires": ["3.5 commission"]},
    {"name": "3.25 battery storage", "max_ep": 26,
     "fn": step_3_25_battery_storage,
     "requires": ["3.5 commission"]},
    {"name": "3.26 device energy management", "max_ep": 27,
     "fn": step_3_26_dem,
     "requires": ["3.5 commission"]},
    {"name": "3.27 energy evse", "max_ep": 28, "fn": step_3_27_evse,
     "requires": ["3.5 commission"]},
    {"name": "3.14 root-endpoint URC sweep", "max_ep": 0,
     "fn": step_3_14_root_urc_sweep},
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


def parse_parts_list(text):
    """Every entry in a chip-tool `[n]: <int>` list read where each
    element is a PLAIN unsigned integer, as ints: the shape Descriptor's
    `PartsList` (`list[endpoint-no]`) uses.

    CONFIRMED (composed appliance round, HELD on the bench, TESTING.md
    8.8): derived from the pinned SDK's own print path the same way
    `parse_indexed_list` was, then matched the live wire captures on the
    first attempt on both transports (the 2026-08-10 WiFi and Thread
    bench sessions, tasks 11/12; zero harness changes in either).
    `DataModelLogger.cpp`'s `PartsList` case decodes a
    `DecodableList<chip::EndpointId>` and hands it to the generic
    `DecodableList<T>` template in `DataModelLogger.h` (:119-140), which
    labels each entry `"[i]"`; `EndpointId` is an integral typedef
    (uint16), so each element routes to the integral `LogValue` overload
    (:92-99), which prints `std::to_string(value)`: plain decimal, no
    `0x` prefix, no parenthesised name. `ServerList` is NOT this shape:
    it goes through `LogClusterId` (`DataModelLogger.h`:162-183), which
    appends `" (" + ClusterIdToText(id) + ")"`, i.e. exactly the
    `[n]: <id> (Name)` form `parse_accepted_command_list` already parses,
    so server-list reads reuse that parser instead. Kept separate from
    `parse_indexed_list` (the list[string] sibling) so a non-numeric
    entry in a numeric read is a parse miss (empty list, failed check)
    rather than a silently-accepted string. Empty list on no match,
    never raises."""
    return [int(m) for m in
            re.findall(r"\[\d+\]:\s*(\d+)\s*$", text, re.MULTILINE)]


def parse_device_types(text):
    """Every device type id in a Descriptor `DeviceTypeList` chip-tool
    read, in order, as ints: step_3_23's triple-identity check (heat
    pump + power source + composed electrical sensor on one endpoint).

    INFERENCE (energy round B, awaiting the bench): derived from the
    pinned generated DataModelLogger.cpp's `DeviceTypeStruct` LogValue
    overload, which prints each entry's deviceType as
    `std::to_string(value.deviceType) + " (" + DeviceTypeIdToText(...) +
    ")"` under the label `DeviceType`, i.e. `DeviceType: 777 (Heat
    Pump)`, then a separate `Revision:` line this regex cannot match
    (no opening parenthesis follows its value). The `\\(` requirement is
    parse_accepted_command_list's shape discipline: a numeric that is
    not followed by a parenthesised name is a parse miss, not a silent
    acceptance. Empty list on no match, never raises."""
    return [int(m) for m in
            re.findall(r"DeviceType:\s*(\d+)\s*\(", text)]


def parse_notify_active(text):
    """Every `Active:` bitmap value chip-tool prints for a
    `RefrigeratorAlarm` `Notify` event read, in event order, as ints:
    the alarms that BECAME active in that event (the spec's
    becameActive semantics for the `Active` field).

    CONFIRMED (composed appliance round, HELD on the bench, TESTING.md
    8.8): derived from the pinned SDK's own print path, then matched the
    live wire captures on the first attempt on both transports (the
    2026-08-10 WiFi and Thread bench sessions, tasks 11/12; zero harness
    changes in either). `DataModelLogger.cpp`'s
    `RefrigeratorAlarm::Events::Notify` `LogValue` overload (:9713-9760)
    prints the four fields with the labels `Active`, `Inactive`,
    `State`, `Mask`; each is a `BitMask<AlarmBitmap>`, routed through
    the `BitFlags` `LogValue` template (`DataModelLogger.h`), which
    prints `value.Raw()` via the integral overload: plain decimal. The
    regex is case-sensitive, so `Inactive:` (lowercase `a` after `In`)
    can never match it. Empty list on no match, never raises."""
    return [int(m) for m in re.findall(r"Active:\s*(\d+)", text)]


def parse_mode_tag_values(text):
    """Every `ModeTagStruct.Value` chip-tool prints for a `ModeBase`
    `SupportedModes` read (`RvcRunMode`/`RvcCleanMode`/`MicrowaveOvenMode`),
    in the order the mode entries appear, one value per mode: this
    firmware's `GetModeTagsByIndex()` (`main.cpp`) always publishes exactly
    one tag per mode (`tags.reduce_size(1)`), never a multi-tag list.

    CONFIRMED (RVC + Microwave batch harness task, HELD on the bench,
    TESTING.md 8.7): derived the same way `parse_indexed_list` was, from
    the pinned SDK's own print path, then matched the live wire capture on
    the first attempt on both transports: TESTING.md 8.7 (WiFi) and the
    Thread bench session's task-10-report.md section 5.2 (Thread).
    `DataModelLogger.cpp`'s generic `chip::app::Clusters::detail::Structs::
    ModeTagStruct` `LogValue` overload (shared by all three ModeBase-derived
    clusters, distinct from `ModeSelect`'s own `SemanticTags` struct) prints
    `"Value: <u16>"` for the tag's `value` field and nothing at all for
    `mfgCode`, since it is `chip::Optional<VendorId>` and the generic
    `Optional<T>` `LogValue` template in `DataModelLogger.h` emits no line
    when a value is absent -- confirmed against `main.cpp`:
    `GetModeTagsByIndex()` only ever writes `tags[0].value`, `mfgCode` is
    never touched. So a mode's tag block reads `"ModeTags: 1 entries"` then
    one `"[1]: {"` `"Value: <tag>"` `"}"`, no `MfgCode:` line to confuse the
    regex with. Empty list on no match, never raises."""
    return [int(m) for m in re.findall(r"\bValue:\s*(\d+)", text)]


def parse_active_power_reports(text):
    """Every ActivePower value chip-tool prints, in order, as ints:
    step_3_21's subscription report stream (and any one-shot read).

    INFERENCE (energy round A, awaiting the bench): derived from the
    pinned SDK's own print path the same way parse_indexed_list was.
    The generated DataModelLogger.cpp's ElectricalPowerMeasurement
    ActivePower case decodes DataModel::Nullable<int64_t> and prints it
    with the label "ActivePower" through the Nullable LogValue template
    (DataModelLogger.h), which prints the literal "null" for a null
    value (no digits: a null report is invisible here, the same
    invisibility parse_onoff_reports gives heartbeat reports) and
    otherwise routes to the integral overload's std::to_string(value):
    plain decimal, full 64-bit width, minus sign included. The regex is
    case-sensitive and neither "ReactivePower:" nor "ApparentPower:"
    contains the "ActivePower:" substring, so the sibling attributes
    can never match. Deliberately NOT end-anchored: Subscriber output
    bypasses the central ANSI strip (reports()'s F1 note), and an
    end-anchored regex would break on a trailing SGR escape. Empty list
    on no match, never raises."""
    return [int(m) for m in re.findall(r"ActivePower:\s*(-?\d+)", text)]


def parse_energy_values(text):
    """Every EnergyMeasurementStruct `Energy:` field chip-tool prints,
    in order, as ints. One parser serves both shapes step_3_21 reads:
    a CumulativeEnergyImported/-Exported attribute read (the struct
    directly) and a CumulativeEnergyMeasured event read (the struct
    nested under the event's EnergyImported/EnergyExported fields).

    INFERENCE (energy round A, awaiting the bench): derived from the
    pinned generated DataModelLogger.cpp's EnergyMeasurementStruct
    LogValue overload, which prints "Energy: <i64>" via the integral
    overload (plain decimal, std::to_string), then StartTimestamp/
    EndTimestamp/StartSystime/EndSystime and ApparentEnergy/
    ReactiveEnergy; every one of those trailing fields is an
    Optional<T>, and the generic Optional LogValue template emits NO
    line for an absent value (the same silence parse_mode_tag_values
    relies on for mfgCode), so only the fields the firmware actually
    sets can print, and none of them is named Energy. \\bEnergy: cannot
    match the sibling labels: "ApparentEnergy:"/"ReactiveEnergy:" have
    a word character before the E (no \\b boundary there), and
    "EnergyImported:"/"EnergyExported:" continue past "Energy" before
    their colon. Empty list on no match, never raises."""
    return [int(m) for m in re.findall(r"\bEnergy:\s*(-?\d+)", text)]


def parse_power_adjust_entries(text):
    """Every `PowerAdjustStruct` entry in a chip-tool
    `PowerAdjustmentCapability` read, in order, as
    `(minPower, maxPower, minDuration, maxDuration)` tuples: step_3_26's
    AT+MTDEMCAP round trip (spec 3.26).

    INFERENCE (energy round C1, awaiting the bench): derived from the
    pinned generated DataModelLogger.cpp's `PowerAdjustStruct` LogValue
    overload, which prints the four fields with the labels `MinPower`,
    `MaxPower`, `MinDuration`, `MaxDuration` in that order, each through
    the integral overload's std::to_string (plain decimal, full 64-bit
    width, minus sign included on the two powers); the entries sit under
    the `PowerAdjustCapability` list of the enclosing
    `PowerAdjustCapabilityStruct`, whose only other field is `Cause`
    (parse_cause_values below). The four labels are collected
    INDEPENDENTLY and zipped rather than matched as four consecutive
    lines, so no assumption about interior blank/prefix lines is baked
    in; a length disagreement between the four means the capture is not
    the shape this parser understands and yields an empty list rather
    than a mis-zipped one. `MinDuration`/`MaxDuration` are also
    `SlotStruct` field names, but Forecast is permanently null in C1
    (main.cpp) and is never read here.

    All four labels are `\\b`-anchored, parse_cause_values' discipline:
    without it `AbsMinPower:`/`AbsMaxPower:` satisfy the two power
    patterns (no word boundary sits between `Abs` and `Min`), so any
    capture carrying the DEM power envelope beside a capability, or a
    whole-cluster attribute dump, would yield a phantom or mis-zipped
    entry. Empty list on no match, never raises."""
    mins = re.findall(r"\bMinPower:\s*(-?\d+)", text)
    maxs = re.findall(r"\bMaxPower:\s*(-?\d+)", text)
    mind = re.findall(r"\bMinDuration:\s*(\d+)", text)
    maxd = re.findall(r"\bMaxDuration:\s*(\d+)", text)
    if not (len(mins) == len(maxs) == len(mind) == len(maxd)):
        return []
    return [(int(a), int(b), int(c), int(d))
            for a, b, c, d in zip(mins, maxs, mind, maxd)]


def parse_cause_values(text):
    """Every `Cause:` value chip-tool prints, in order, as ints. One
    parser serves both shapes step_3_25/step_3_26 read: the
    `PowerAdjustCapabilityStruct`'s `cause` field
    (`PowerAdjustReasonEnum`: 0 NoAdjustment, 1
    LocalOptimizationAdjustment, 2 GridOptimizationAdjustment) and the
    `PowerAdjustEnd` event's `cause` field (`CauseEnum`: 0
    NormalCompletion, 4 Cancelled). The two spaces never appear in one
    capture, so one parser is honest here; the caller names which it
    reads.

    INFERENCE (energy round C1, awaiting the bench): both structures'
    LogValue overloads in the pinned generated DataModelLogger.cpp print
    the field with the label `Cause`, and both fields are enums, which
    the DataModelLogger.h enum template routes to
    `chip::to_underlying()` and then the integral overload's
    std::to_string: plain decimal, no parenthesised name. The `\\b`
    keeps `AdjustmentCause`-style compounds out (none is printed on
    these paths today, but the boundary is free). Empty list on no
    match, never raises."""
    return [int(m) for m in re.findall(r"\bCause:\s*(\d+)", text)]


def parse_esa_state_reports(text):
    """Every `ESAState` value chip-tool prints, in order, as ints:
    step_3_26's subscription report stream (and any one-shot read).

    INFERENCE (energy round C1, awaiting the bench): the generated
    DataModelLogger.cpp's DeviceEnergyManagement ESAState case decodes
    `ESAStateEnum` and prints it with the label `ESAState` through the
    enum template into the integral overload's std::to_string, the
    parse_active_power_reports derivation on this round's cluster. The
    regex is case-sensitive and no sibling label contains the
    `ESAState:` substring. Deliberately NOT end-anchored: Subscriber
    output bypasses the central ANSI strip (reports()'s F1 note), and an
    end-anchored regex would break on a trailing SGR escape. Empty list
    on no match, never raises."""
    return [int(m) for m in re.findall(r"ESAState:\s*(-?\d+)", text)]


def parse_change_to_mode_status(text):
    """The `status` field chip-tool prints for a `ModeBase`
    `ChangeToModeResponse` (`RvcRunMode`/`RvcCleanMode`'s `ChangeToMode`
    command), as an int: `0` `kSuccess`, `1` `kUnsupportedMode`, `2`
    `kGenericFailure`, `3` `kInvalidInMode` (`AT_MT_SPEC.md` 3.20.1).

    CONFIRMED (RVC + Microwave batch harness task, HELD on the bench,
    TESTING.md 8.7): derived from `DataModelLogger.cpp`'s per-cluster `LogValue`
    overload for `RvcRunMode::Commands::ChangeToModeResponse` (and
    `RvcCleanMode`'s identical sibling), which prints `"status: <int>"`
    (plain decimal, no `0x` prefix and no enclosing parenthesised name) and
    a separate `"statusText: ..."` line. This is `ModeBase`'s own status
    space, not the interaction-model `StatusIB` `parse_status`'s
    `"status = 0x.."` branch reads: a denied `ChangeToMode` is still a
    `StatusIB` `Success` (the SDK always returns a `ChangeToModeResponse`;
    the verdict lives inside it, not at the `StatusIB` level), so
    `parse_status` would find nothing here. The regex requires the literal
    `"status:"` (colon immediately after the word), which does not match
    `"statusText:"` (no colon after `"status"` there), so the two lines
    cannot be confused. Last match wins, so a capture that also contains an
    earlier attribute read still returns the command response's own value.
    None on no match, never raises. Matched the live wire capture on the
    first attempt on both transports: TESTING.md 8.7 (WiFi) and the Thread
    bench session's task-10-report.md section 5.2 (Thread)."""
    vals = re.findall(r"\bstatus:\s*(\d+)", text)
    return int(vals[-1]) if vals else None


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
    written by a test.

    cluster/attribute/parser (energy round A, task 5) parameterize the
    subscription target: step_3_21 subscribes to
    electricalpowermeasurement/active-power with
    parse_active_power_reports. The defaults keep the original OnOff
    shape byte-identical, so Phase 2's 2.4 and every existing consumer
    are unchanged. A parser passed here must not be end-anchored (see
    reports()'s F1 note: this output bypasses the central ANSI
    strip)."""

    def __init__(self, chip, node_id, endpoint=1, min_s=0, max_s=5,
                 popen=None, cluster="onoff", attribute="on-off",
                 parser=None):
        self.chip = chip
        self.argv = [chip.binary, "interactive", "start",
                     "--storage-directory", chip.storage_dir]
        self.subscribe_cmd = ("%s subscribe %s %d %d 0x%X %d"
                              % (cluster, attribute, min_s, max_s,
                                 node_id, endpoint))
        self.parser = parser or parse_onoff_reports
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
                return self.parser(f.read())
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


def t_mttransport(link):
    """AT+MTTRANSPORT (AT_MT_SPEC.md 3.12.2) exists only on the combined
    image (build_combined): build_wifi and build_thread never register
    it, so every form of the command, query or set, well-formed or not,
    reaches the ordinary unknown-command path and answers +MTERR:8, the
    same code register_phase1_negative's MTBOGUS row gets. The spec is
    explicit that there is no combined-image-only stub that reports one
    fixed transport, so absence is checked for the set forms too, not
    just the query.

    Same design decision t_mtthread_query_shape carries (task-3 report,
    round 0.11.0, this file further down): Phase 1 has one TESTS list
    for every image, add_test's contract is fn(link) with no context
    object, so a row whose correct answer differs by image detects its
    own image from the command itself. Querying AT+MTTRANSPORT? to find
    out whether AT+MTTRANSPORT exists at all is well-defined, not
    circular: the single-transport answer, +MTERR:8, is what "it does
    not exist" IS.

    The two set-form negatives expect +MTERR:1 for an out-of-range or an
    empty value, never a bare ERROR: AT+MTEVT= and AT+MTSWITCH= (both
    plain scalar setters, register_phase1_negative below) are already
    pinned to +MTERR:1 on an empty argument, and 3.12.2 reads "validates
    the argument (+MTERR:1 for anything else)" with no carve-out for an
    empty one."""
    res, lines = link.command("AT+MTTRANSPORT?")
    if res == 8:
        # Single-transport image: the command does not exist in any
        # form, not just the query.
        return (link.command("AT+MTTRANSPORT=WIFI")[0] == 8
                and link.command("AT+MTTRANSPORT=")[0] == 8)
    if res != 0 or not lines:
        return False
    if not re.fullmatch(r"\+MTTRANSPORT:(WIFI|THREAD),(WIFI|THREAD)",
                        lines[0]):
        return False
    if link.command("AT+MTTRANSPORT=BLUETOOTH")[0] != 1:
        return False
    return link.command("AT+MTTRANSPORT=")[0] == 1


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
                "build_wifi --port <port> --bridge espnow")
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
    # "N/A" is a distinct status from "SKIP" (bench defect C's controller
    # ruling): both are recorded so neither is hidden, but a status diff
    # against an old baseline can tell "this stopped running" (SKIP, a
    # real regression signal) from "this transport never applied" (N/A,
    # expected and stable) at a glance.
    data = {
        "header": header,
        "results": dict(
            [(name, "PASS" if ok else "FAIL")
             for name, ok, _ in suite.results]
            + [(name, "SKIP") for name, _ in suite.skipped]
            + [(name, "N/A") for name, _ in suite.na]),
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
    p("MTTRANSPORT: query and set, combined image only (3.12.2)",
      t_mttransport)
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
    split (superpowers/specs/2026-08-08-c5-regression-harness-t5-design.md
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
    # MTALARM=1,0,0 no longer lives here: the composed appliance round
    # migrated field 0 (it is a legal RefrigeratorAlarm bit now, so it
    # passes cmd_mtalarm()'s union gate and the rejection moved into the
    # bridge), which also makes the row rig-dependent (+MTERR:3 on the
    # single-light rig). See register_phase1_t7_negative.
    n("MTALARM=1,12,0 -> +MTERR:1 (field outside the union bound 0..11)",
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


def register_phase1_t6_negative():
    """RVC + Microwave batch, harness task: the state-safe subset of the
    new grammar TESTING.md 6.2 gained in tasks 2 and 3 of this batch (the
    cluster-aware AT+MTMODES form, task 2, corrected wording at 2da284f;
    the AT+MTOPSTATE extended-state range, task 3), the same
    order-independent-only split register_phase1_t5_negative established:
    a row lands here only when it is rejected inside the handler itself,
    before mt_at.c looks up any endpoint, per TESTING.md's own "first N
    rows are order-independent" callouts for each table. Everything else
    (the +MTERR:2/3 lookups, the OK storage/read-back rows) needs a real
    RVC or microwave endpoint from a declared composition and belongs to
    Phase 3 (step_3_15_rvc / step_3_16_microwave), named where TESTING.md
    itself says so.

    A sibling of register_phase1_t5_negative, not an addition to it: this
    batch's rows are a distinct project from the seven-type-batch grammar
    completion that function covers, so keeping them in their own
    function keeps each one's docstring an accurate description of what
    it actually registers."""
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    # AT+MTMODES cluster-aware form (TESTING.md 6.2, RVC + Microwave batch
    # task 2): the first four rows of that table are order-independent,
    # rejected by cmd_mtmodes()'s own form disambiguation and triple
    # parsing before any endpoint lookup runs. ep 1 (a light in every
    # composition this suite declares) stands in for "any endpoint": none
    # of these four rows ever reaches the lookup that would care which
    # endpoint it is.
    n('MTMODES=1,84 -> +MTERR:1 (no second comma follows 84 at all, so '
      'the parser cannot even disambiguate the form)',
      expect_err("AT+MTMODES=1,84", 1))
    n('MTMODES=1,84,0,"x" -> +MTERR:1 (<tag> missing: no comma '
      'terminates a <tag> token before "x"\'s opening quote)',
      expect_err('AT+MTMODES=1,84,0,"x"', 1))
    n('MTMODES=1,84,0,zz,"x" -> +MTERR:1 (<tag> not numeric)',
      expect_err('AT+MTMODES=1,84,0,zz,"x"', 1))
    n('MTMODES=1,84,0,70000,"x" -> +MTERR:1 (<tag> above 0xFFFF)',
      expect_err('AT+MTMODES=1,84,0,70000,"x"', 1))
    # MTMODES=99,84,... (+MTERR:2), the light-ep/RvcRunMode-absent and
    # rvc-ep/wrong-cluster +MTERR:3 pair, and every OK row (explicit
    # tags, tag-0 defaults on RvcRunMode/RvcCleanMode/MicrowaveOvenMode,
    # comma inside a label) all need a known composition carrying the
    # RVC or microwave device type: Phase 3 (step_3_15_rvc /
    # step_3_16_microwave).

    # AT+MTOPSTATE extended-state range (TESTING.md 6.2, RVC + Microwave
    # batch task 3): 0x43/kEmptyingDustBin is a real RvcOperationalState
    # enumerator, but outside this command's union entirely (the union
    # only publishes 0x40-0x42), so it is rejected the same
    # order-independent way as the existing =1,3/=1,4 rows
    # register_phase1_t5_negative already carries, before any endpoint
    # lookup.
    n("MTOPSTATE=1,0x43 -> +MTERR:1 (kEmptyingDustBin exists in "
      "RvcOperationalState's own enum but outside this command's union "
      "entirely)", expect_err("AT+MTOPSTATE=1,0x43", 1))
    # AT+MTOPSTATE=<washer ep>,0x40 and its decimal alias 64 (+MTERR:1,
    # legal union member but illegal on the plain OperationalState
    # cluster) and every AT+MTOPSTATE=<rvc ep>,{0x40,0x41,0x42,1} OK row
    # need a known composition: Phase 3 (step_3_15_rvc).


register_phase1_t6_negative()


def mtep_staging_negative(setup_cmds, cmd, want=1):
    """One AT+MTEP staging-time negative (composed appliance round, task
    6; TESTING.md 6.2's parenting table): run the staging prelude (each
    command must answer OK: AT+MTEPCLEAR opens the session, an optional
    AT+MTEP=0x0100 gives wrong-parent-type rows a staged light at index
    0), then the negative row itself, expecting +MTERR:<want>. Every row
    opens its OWN session, so the rows stay order-independent the way
    every other Phase 1 registration is.

    cmd_mtep() validates <parent_idx> and mt_devtype_parent_ok() entirely
    against s_staged, before any composition is applied (TESTING.md 6.2's
    "pure-form/staging-time negatives" note), so nothing here touches the
    live composition. A trailing AT+MTEPCLEAR (unscored, best-effort)
    empties the session again so no staged entry leaks into a later
    test's staging assumptions; staging is RAM-only, so even a skipped
    trailer cannot survive a reset."""
    def fn(link):
        for c in setup_cmds:
            if link.command(c)[0] != 0:
                return False
        res, _ = link.command(cmd)
        ok = res == want
        link.command("AT+MTEPCLEAR")  # unscored: leave the session empty
        return ok
    return fn


def register_phase1_t7_negative():
    """Composed appliance round, task 6: the state-safe rows TESTING.md
    6.2 gained from firmware tasks 2-5 of that round (the AT+MTEP
    parenting grammar, task 2 plus task 5's two Cook Surface rows; the
    AT+MTALARM field-0 migration, task 3), transcribed from the tables,
    not re-derived. The same order-independent-only split
    register_phase1_t5_negative established, with one deliberate
    stretch: the MTALARM migration row is not handler-internal any more
    (the rejection moved into the bridge's cluster lookup), but
    TESTING.md 6.2 pins its answer ON THE SINGLE-LIGHT RIG (+MTERR:3, ep
    1 carries neither alarm cluster), which is exactly the bench
    standard state every Phase 1 run starts from, so it stays a Phase 1
    row by the table's own words. Its smoke-endpoint sibling
    (=<alarm ep>,0,0 -> +MTERR:1) needs the real alarm slot and lands in
    step_3_2_grammar.

    A sibling of register_phase1_t5_negative/t6, not an addition to
    either: this round's rows are their own project, same reasoning as
    t6's docstring."""
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    # AT+MTEP parenting grammar (TESTING.md 6.2, composed appliance round
    # tasks 2 and 5): all five rows are staging-session negatives; see
    # mtep_staging_negative's docstring. Rows 3 and 5 stage a light first
    # so index 0 exists and is of the WRONG device type; rows 1, 2 and 4
    # need only the empty session.
    n("MTEPCLEAR then MTEP=0x0100,0,0 -> +MTERR:1 (parent index 0 out of "
      "range: nothing staged yet)",
      mtep_staging_negative(["AT+MTEPCLEAR"], "AT+MTEP=0x0100,0,0"))
    n("MTEP=0x0100,0,zz -> +MTERR:1 (parent index not numeric)",
      mtep_staging_negative(["AT+MTEPCLEAR"], "AT+MTEP=0x0100,0,zz"))
    n("MTEP=0x0071,0,0 under a light -> +MTERR:1 (cabinet parent must be "
      "a fridge or an oven)",
      mtep_staging_negative(["AT+MTEPCLEAR", "AT+MTEP=0x0100"],
                            "AT+MTEP=0x0071,0,0"))
    n("MTEP=0x0077 unparented -> +MTERR:1 (cook surface REQUIRES a "
      "cooktop parent)",
      mtep_staging_negative(["AT+MTEPCLEAR"], "AT+MTEP=0x0077"))
    n("MTEP=0x0077,0,0 under a light -> +MTERR:1 (cook surface parent "
      "must be a cooktop)",
      mtep_staging_negative(["AT+MTEPCLEAR", "AT+MTEP=0x0100"],
                            "AT+MTEP=0x0077,0,0"))

    # AT+MTALARM field-0 migration (TESTING.md 6.2, composed appliance
    # round task 3): field 0 passes cmd_mtalarm()'s union gate now (a
    # legal RefrigeratorAlarm bit), so on the single-light rig the
    # rejection comes from the bridge's cluster lookup, +MTERR:3, where
    # it used to be the handler's own +MTERR:1.
    n("MTALARM=1,0,0 -> +MTERR:3 (migrated: field 0 passes the union "
      "gate; the single-light rig's ep 1 carries neither alarm cluster)",
      expect_err("AT+MTALARM=1,0,0", 3))


register_phase1_t7_negative()


def register_phase1_t8_negative():
    """Energy round A (0.8.0), task 5: the state-safe rows TESTING.md
    6.2 gained from the round's firmware tasks (the AT+MTMEAS table's
    pure-form negatives, tasks 1/3; the AT+MTATTR 64-bit boundary
    table's Phase-1 rows, task 1), transcribed from the tables, not
    re-derived, the same order-independent split
    register_phase1_t5/t6/t7 established.

    The MTMEAS rows through the leading-minus row are rejected inside
    cmd_mtmeas()'s own shape and parse gates before any endpoint lookup
    runs (the signedness table needs only the <cluster> token,
    TESTING.md's own "first eight negative rows are order-independent"
    callout), so ep 1 stands in for "any endpoint" exactly as t6's
    MTMODES rows use it. The three MTATTR rows are the boundary table's
    Phase-1 column, pinned by the table ON THE SINGLE-LIGHT RIG (ep 1 =
    OnOff light): Task 1's watch item (=1,... rows depend on ep 1
    existing) is honoured by the same rig-precondition reading
    register_phase1_t7's MTALARM row documents, since Phase 1's
    contract IS the standard single-0x0100 bench state
    (restore_standard_state's own target). All three are state-safe:
    each is refused before any write happens (the round's deliberate
    precedence change makes lookup errors precede bad-value, which is
    why the signedness fetch on ep 1 must succeed first).

    A sibling of t5/t6/t7, not an addition to any of them: this round's
    rows are their own project, same reasoning as t6's docstring."""
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    # AT+MTMEAS pure-form rows (TESTING.md 6.2, energy round A; spec
    # 3.25). The +MTERR:2/3 lookups, the unknown-field and
    # Frequency-range rows, and the two OK push rows all need the
    # measurement slots (21-22): Phase 3 (step_3_20_electrical_sensor /
    # step_3_21_electrical_meter), where TESTING.md itself sends them.
    n("MTMEAS? -> ERROR (query form on a set-only command)",
      expect_err("AT+MTMEAS?", -1))
    n("MTMEAS no args -> ERROR (exec form)", expect_err("AT+MTMEAS", -1))
    n("MTMEAS=1,144 -> +MTERR:1 (no pairs: fewer than 4 parameters)",
      expect_err("AT+MTMEAS=1,144", 1))
    n("MTMEAS=1,144,0 -> +MTERR:1 (odd tail: field 0 has no value)",
      expect_err("AT+MTMEAS=1,144,0", 1))
    n("MTMEAS=1,144,zz,5 -> +MTERR:1 (field not numeric)",
      expect_err("AT+MTMEAS=1,144,zz,5", 1))
    n("MTMEAS=1,144,0,zz -> +MTERR:1 (value not numeric)",
      expect_err("AT+MTMEAS=1,144,0,zz", 1))
    n("MTMEAS eight pairs -> +MTERR:1 (18 tokens overflow "
      "at_split_args' 2 + 2 * MT_MEAS_MAX_PAIRS bound)",
      expect_err("AT+MTMEAS=1,144,0,1,1,1,2,1,3,1,4,1,5,1,6,1,0,1", 1))
    n("MTMEAS=1,145,0,-1 -> +MTERR:1 (minus on an unsigned energy "
      "counter: rejected at parse, before any endpoint lookup)",
      expect_err("AT+MTMEAS=1,145,0,-1", 1))

    # AT+MTATTR 64-bit boundary rows (TESTING.md 6.2's boundary table,
    # Phase-1 column; spec 3.8). The two full-width OK rows are Phase 3
    # (step_3_20's Breadcrumb pair; the i64 row has no reachable target,
    # see TESTING.md's note under the table).
    n("MTATTR=1,6,0,-1 -> +MTERR:1 (minus on an unsigned attribute: "
      "pre-round-A this wrapped through strtoul and wrote true)",
      expect_err("AT+MTATTR=1,6,0,-1", 1))
    n("MTATTR=1,6,0,92233720368547758080 -> +MTERR:1 (literal "
      "overflows 64 bits: rejected at parse, ERANGE)",
      expect_err("AT+MTATTR=1,6,0,92233720368547758080", 1))
    n("MTATTR=1,6,0xFFFC,4294967296 -> +MTERR:1 (2^32 into the u32 "
      "FeatureMap: the width gate, not silent truncation to 0)",
      expect_err("AT+MTATTR=1,6,0xFFFC,4294967296", 1))


register_phase1_t8_negative()


def t_meas_staged_wh_min(link):
    """Energy round B: the 0x94 feature-gate rows need a real VARIANT-1
    (minimal) water heater endpoint, which the Phase 3 composition
    deliberately does not carry (its slot 23 is the FULL variant, and
    the two variants are one device type, so both cannot ride one
    composition without burning a second slot for three rows). Staged
    here instead, the design spec section 5's Phase 1 assignment:
    declare a scratch composition of the standard light plus a minimal
    water heater (AT+MTEP=0x050F,1 -> endpoint 2), AT+MTEPAPPLY (which
    persists and REBOOTS, spec 3.9), verify the composition actually
    took effect via AT+MTEP? (the known-start-state rule: a check that
    cannot observe its variable is not a check; without this read a
    failed apply would still answer +MTERR:2/3 and fake a pass), run
    the three rows, and restore the single-light standard state in a
    finally block so the rest of Phase 1's single-light contract holds
    no matter which stage failed. Restore is by re-stage + re-apply,
    NOT AT+MTFRESET: Phase 1 must not destroy fabric state that is none
    of its business.

    The three rows (TESTING.md 6.2, AT_MT_SPEC.md 3.25):
      - field 3 (TankVolume) push -> +MTERR:3: the minimal variant has
        no EnergyManagement feature, so the field is not served, the
        same data-model code as a missing cluster.
      - field 4 (EstimatedHeatRequired) with a NEGATIVE value ->
        +MTERR:3, not +MTERR:1: the ledger's precedence pin. The gate
        check runs before the range check in mt_meas_whm_apply()'s
        validate pass, so "this endpoint does not serve that data"
        outranks "that value is out of range". A regression that
        reordered them would answer 1 here and this row would catch it.
      - field 9 -> +MTERR:1: unknown to 0x94 even on an endpoint that
        DOES carry the cluster (the bridge's validate pass), the
        gate-independent control row.

    One composite Phase 1 row rather than three: each row needs the
    same two reboots (apply + restore), and Phase 1 rows are
    order-independent by contract, so three separate rows would each
    have to stage and restore on their own, six reboots for no added
    coverage. Sub-stage failures print a diagnosis line so the single
    FAIL is attributable."""
    def diag(msg):
        print("    (staged wh-min: %s)" % msg)

    def apply_and_wait():
        link.drain(0.3)
        if link.command("AT+MTEPAPPLY", timeout=5.0)[0] != 0:
            return False
        return link.await_urc(r"\+MTREADY$", timeout=15.0) is not None

    ok = True
    try:
        if not stage_composition(link, ["0x0100", "0x050F,1"]):
            diag("staging the scratch composition failed")
            return False
        if not apply_and_wait():
            diag("AT+MTEPAPPLY or the +MTREADY wait failed")
            return False
        res, lines = cmd_retry(link, "AT+MTEP?")
        if res != 0 or lines != ["+MTEP:0,1,0x0100",
                                 "+MTEP:1,2,0x050F,1"]:
            diag("composition readback wrong: %r" % lines)
            ok = False
        else:
            for cmd, want in (("AT+MTMEAS=2,148,3,200", 3),
                              ("AT+MTMEAS=2,148,4,-5", 3),
                              ("AT+MTMEAS=2,148,9,1", 1)):
                res, _ = link.command(cmd)
                if res != want:
                    diag("%s answered %d, wanted +MTERR:%d"
                         % (cmd, res, want))
                    ok = False
    finally:
        restored = (stage_composition(link, ["0x0100"])
                    and apply_and_wait())
        if restored:
            res, lines = cmd_retry(link, "AT+MTEP?")
            restored = res == 0 and lines == ["+MTEP:0,1,0x0100"]
        if not restored:
            diag("RESTORE FAILED: bench is not in the single-light "
                 "standard state")
            ok = False
    return ok


def register_phase1_t9_negative():
    """Energy round B (0.9.0), task 4: the state-safe rows TESTING.md
    6.2 gained from the round's firmware tasks (the AT+MTMEAS 0x94
    family, tasks 1-2), transcribed from the tables, not re-derived,
    the same order-independent split register_phase1_t5/t6/t7/t8
    established.

    The two pure-form rows are rejected inside cmd_mtmeas()'s own shape
    and parse gates before any endpoint lookup runs: the odd-tail row
    by the pair-count gate (cluster-independent, so 148 rides the same
    gate 144 already proved) and the leading-minus row by the
    signedness table, which needs only the <cluster> token (of the 0x94
    family only field 4 is signed, so a minus on field 0 dies at parse,
    TESTING.md's own row note). ep 1 stands in for "any endpoint"
    exactly as t8's MTMEAS rows use it.

    The staged variant-1 water heater row (t_meas_staged_wh_min) is the
    deliberate exception to "state-safe only", with the exception
    CONTAINED: it reboots the device twice (apply + restore) but
    restores the single-light standard state in a finally block, so
    every other Phase 1 row's contract survives it; see its docstring
    for why its three checks cannot live anywhere else (the Phase 3
    composition carries the FULL variant at slot 23, and the +MTERR:3
    gate rows need the MINIMAL one). The full-variant siblings of its
    rows (+MTERR:1 on field 9 and on a negative field 4, where the gate
    passes and the range check answers) live in Phase 3's
    step_3_22_water_heater, where TESTING.md 6.2 sends them.

    A sibling of t5/t6/t7/t8, not an addition to any of them: this
    round's rows are their own project, same reasoning as t6's
    docstring."""
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    n("MTMEAS=1,148,0 -> +MTERR:1 (0x94 odd tail: field 0 has no value, "
      "the cluster-independent pair gate)",
      expect_err("AT+MTMEAS=1,148,0", 1))
    n("MTMEAS=1,148,0,-1 -> +MTERR:1 (minus on an unsigned 0x94 field: "
      "only field 4 is signed, rejected at parse before any endpoint "
      "lookup)", expect_err("AT+MTMEAS=1,148,0,-1", 1))
    n("MTMEAS staged variant-1 water heater: gate rows (+MTERR:3, gate "
      "outranks range), unknown field, then restore",
      t_meas_staged_wh_min)


register_phase1_t9_negative()


def t_staged_variant1_energy_c1(link):
    """Energy round C1: the variant-1 refusal rows for all THREE of this
    round's device types need real variant-1 endpoints, which the Phase 3
    composition deliberately does not carry (its slots 25-27 are the full
    variants, and the two variants of one device type cannot ride one
    composition without burning a slot each for a handful of rows).
    Staged here instead, the t_meas_staged_wh_min pattern and the design
    spec section 5 assignment: declare a scratch composition of the
    standard light plus one variant-1 endpoint per type (endpoints 2, 3
    and 4), AT+MTEPAPPLY (which persists and REBOOTS, spec 3.9), VERIFY
    the composition actually took effect via AT+MTEP? (the
    known-start-state rule: a check that cannot observe its variable is
    not a check), run the rows, and restore the single-light standard
    state in a finally block so the rest of Phase 1's single-light
    contract holds no matter which stage failed. Restore is by re-stage
    plus re-apply, NOT AT+MTFRESET: Phase 1 must not destroy fabric state
    that is none of its business.

    ONE composite row carrying all three types rather than three rows,
    for the reason t_meas_staged_wh_min gives and more strongly: each
    staged row costs the same two reboots, so three would cost six for
    no added coverage. Sub-stage failures print a diagnosis line so the
    single FAIL stays attributable.

    The rows (TESTING.md 6.2/6.4, AT_MT_SPEC.md 3.25/3.26):
      - ep 2, solar variant 1: an ENERGY push answers +MTERR:3 (the
        no-EEM current-clamp shape), while a POWER push on the same
        endpoint answers OK. The pair is the point: variant 1 keeps the
        sensor graft and loses only ElectricalEnergyMeasurement, so a
        thunk regression that dropped the whole graft would turn the
        control row red instead of passing quietly.
      - ep 3, battery variant 1: both DEM surfaces answer +MTERR:3, the
        cluster-not-carried code, because variant 1 omits the DEM triple
        the SDK build bolts on unconditionally.
      - ep 4, DEM variant 1 (FeatureMap 0, the report-only ESA):
        AT+MTDEMCAP answers +MTERR:4, the ATTRIBUTE-missing code, NOT
        +MTERR:3 -- the cluster is there and only the PowerAdjustment
        feature (and with it the PowerAdjustmentCapability attribute) is
        absent. Its control row is a 0x0098 push, which answers OK on
        both variants because the five ESA attributes are Instance-served
        either way (design spec 2.4). That contrast is exactly what
        separates code 4 from code 3 here.

    NOT COVERED HERE, and it has no home in this harness: the design
    spec's "a chip-tool power-adjust-request against DEM variant 1
    answers UnsupportedCommand" probe. Phase 1 test functions receive
    only the AT link (add_test's contract) and Phase 1 runs with no
    commissioned fabric and no controller at all, while Phase 3's
    composition is fixed at 27 variant-0 slots, so no phase can host a
    controller invoke against a variant-1 DEM endpoint. The nearest
    executable sibling IS covered: step_3_26 pins the variant-0
    AcceptedCommandList at exactly {0, 1}, so a feature bit appearing or
    vanishing changes an asserted list. Flagged in the task-4 report."""
    def diag(msg):
        print("    (staged energy C1 variant 1: %s)" % msg)

    def apply_and_wait():
        link.drain(0.3)
        if link.command("AT+MTEPAPPLY", timeout=5.0)[0] != 0:
            return False
        return link.await_urc(r"\+MTREADY$", timeout=15.0) is not None

    staged = ["0x0100", "0x0017,1", "0x0018,1", "0x050D,1"]
    expect = ["+MTEP:0,1,0x0100", "+MTEP:1,2,0x0017,1",
              "+MTEP:2,3,0x0018,1", "+MTEP:3,4,0x050D,1"]
    ok = True
    try:
        if not stage_composition(link, staged):
            diag("staging the scratch composition failed")
            return False
        if not apply_and_wait():
            diag("AT+MTEPAPPLY or the +MTREADY wait failed")
            return False
        res, lines = cmd_retry(link, "AT+MTEP?")
        if res != 0 or lines != expect:
            diag("composition readback wrong: %r" % lines)
            ok = False
        else:
            for cmd, want in (("AT+MTMEAS=2,145,0,1500000", 3),
                              ("AT+MTMEAS=2,144,0,230000", 0),
                              ("AT+MTMEAS=3,152,0,5", 3),
                              ("AT+MTDEMCAP=3,1,0", 3),
                              ("AT+MTDEMCAP=4,1,0", 4),
                              ("AT+MTMEAS=4,152,0,5", 0)):
                res, _ = link.command(cmd)
                if res != want:
                    diag("%s answered %d, wanted %s"
                         % (cmd, res, "OK" if want == 0
                            else "+MTERR:%d" % want))
                    ok = False
    finally:
        restored = (stage_composition(link, ["0x0100"])
                    and apply_and_wait())
        if restored:
            res, lines = cmd_retry(link, "AT+MTEP?")
            restored = res == 0 and lines == ["+MTEP:0,1,0x0100"]
        if not restored:
            diag("RESTORE FAILED: bench is not in the single-light "
                 "standard state")
            ok = False
    return ok


def register_phase1_t10_negative():
    """Energy round C1 (0.10.0), task 4: the state-safe rows TESTING.md
    6.2 gained from the round's firmware tasks (the AT+MTMEAS 0x0098
    family and the whole AT+MTDEMCAP grammar, both task 2; both tables
    live in 6.2, there is no 6.4), transcribed from the tables, not
    re-derived, the same order-independent split
    register_phase1_t5/t6/t7/t8/t9 established.

    Why every row below is safe on the single-light bench:

    - The 0x0098 MTMEAS rows die inside cmd_mtmeas()'s own shape and
      parse gates before any endpoint lookup runs (the odd-tail row by
      the cluster-independent pair gate, the four leading-minus rows by
      the signedness table, which needs only the <cluster> token: of the
      0x0098 family only fields 3, 4 and 6 are signed). ep 1 stands in
      for "any endpoint" exactly as t8's and t9's MTMEAS rows use it.
    - The ONE row that deliberately reaches the endpoint lookup is the
      signed-field contrast row (field 3 with a minus), which answers
      +MTERR:3 rather than 1 precisely because the minus is legal there.
      It is the mirror image of its four siblings and needs ep 1 to be a
      light, the same rig precondition register_phase1_t7's MTALARM row
      documents; it is state-safe because the lookup refuses before
      anything is applied.
    - The MTDEMCAP rows all die in cmd_mtdemcap() before the bridge:
      the four bare-ERROR forms by the arity/type gates, the n=5 row by
      the <n> range check (which deliberately outranks the count check,
      mt_at.c), and the four value rows by the per-entry integer parses.
      Everything semantic (the cause enum range, the interval ordering,
      the PowerAdjustment feature gate) is the bridge's business and
      therefore needs a real DEM endpoint: those rows live in Phase 3's
      step_3_26_dem, where TESTING.md sends them.

    A sibling of t5/t6/t7/t8/t9, not an addition to any of them: this
    round's rows are their own project, same reasoning as t6's
    docstring."""
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    # AT+MTMEAS 0x0098 rows (TESTING.md 6.2, energy round C1; spec 3.25).
    n("MTMEAS=1,152,0 -> +MTERR:1 (0x98 odd tail: field 0 has no value, "
      "the cluster-independent pair gate)",
      expect_err("AT+MTMEAS=1,152,0", 1))
    n("MTMEAS=1,152,0,-1 -> +MTERR:1 (minus on ESAType, an unsigned "
      "enum8: rejected at parse before any endpoint lookup)",
      expect_err("AT+MTMEAS=1,152,0,-1", 1))
    n("MTMEAS=1,152,1,-1 -> +MTERR:1 (minus on ESACanGenerate, a bool)",
      expect_err("AT+MTMEAS=1,152,1,-1", 1))
    n("MTMEAS=1,152,2,-1 -> +MTERR:1 (minus on ESAState, an unsigned "
      "enum8)", expect_err("AT+MTMEAS=1,152,2,-1", 1))
    n("MTMEAS=1,152,5,-1 -> +MTERR:1 (minus on OptOutState, an unsigned "
      "enum8)", expect_err("AT+MTMEAS=1,152,5,-1", 1))
    n("MTMEAS=1,152,3,-5000000000 -> +MTERR:3 (AbsMinPower IS signed, so "
      "the minus parses and the light's missing cluster answers: the "
      "signedness table's positive control)",
      expect_err("AT+MTMEAS=1,152,3,-5000000000", 3))

    # AT+MTDEMCAP grammar rows (TESTING.md 6.2, energy round C1; spec
    # 3.26). The +MTERR:2/3/4 lookups, the cause-range and interval-order
    # rows and the OK pushes all need a real DEM endpoint: Phase 3
    # (step_3_26_dem) and the staged variant-1 row below.
    n("MTDEMCAP? -> ERROR (query form on a set-only command)",
      expect_err("AT+MTDEMCAP?", -1))
    n("MTDEMCAP no args -> ERROR (exec form)", expect_err("AT+MTDEMCAP", -1))
    n("MTDEMCAP=1 -> ERROR (fewer than the three mandatory parameters)",
      expect_err("AT+MTDEMCAP=1", -1))
    n("MTDEMCAP=1,1,2,1000,5000,60,3600 -> ERROR (7 parameters contradict "
      "n=2's required 11: <n> declares the command's own form)",
      expect_err("AT+MTDEMCAP=1,1,2,1000,5000,60,3600", -1))
    n("MTDEMCAP=1,1,0,1000 -> ERROR (n=0 declares exactly 3 parameters, "
      "a fourth is junk)", expect_err("AT+MTDEMCAP=1,1,0,1000", -1))
    n("MTDEMCAP=1,zz,0 -> +MTERR:1 (cause not numeric: the parses run "
      "before the arity check, so the value code wins here)",
      expect_err("AT+MTDEMCAP=1,zz,0", 1))
    n("MTDEMCAP n=5 -> +MTERR:1 (a well-formed count above the 4-entry "
      "bound: the range check outranks the arity check)",
      expect_err("AT+MTDEMCAP=1,1,5,1,2,3,4,1,2,3,4,1,2,3,4,1,2,3,4,"
                 "1,2,3,4", 1))
    n("MTDEMCAP=1,1,1,-2,5000,zz,60 -> +MTERR:1 (minDuration not "
      "numeric; the minus on minPower is fine, powers are signed)",
      expect_err("AT+MTDEMCAP=1,1,1,-2,5000,zz,60", 1))
    n("MTDEMCAP=1,1,1,92233720368547758080,5000,60,3600 -> +MTERR:1 "
      "(minPower literal overflows 64 bits: rejected at parse, ERANGE)",
      expect_err("AT+MTDEMCAP=1,1,1,92233720368547758080,5000,60,3600", 1))
    n("MTDEMCAP=1,1,1,1000,5000,60,4294967296 -> +MTERR:1 (maxDuration "
      "above uint32: parse_u64's ERANGE-checked bound, where parse_u's "
      "32-bit strtoul would have clamped silently on the target)",
      expect_err("AT+MTDEMCAP=1,1,1,1000,5000,60,4294967296", 1))
    n("MTDEMCAP=1,1,1,1000,5000,-1,3600 -> +MTERR:1 (minus on an "
      "unsigned duration)",
      expect_err("AT+MTDEMCAP=1,1,1,1000,5000,-1,3600", 1))

    n("MTDEMCAP/MTMEAS staged variant-1 solar, battery and DEM: the "
      "cluster-missing and attribute-missing rows with their OK "
      "controls, then restore", t_staged_variant1_energy_c1)


register_phase1_t10_negative()


# Thread role API round (0.11.0), task 3: the decoded RoutingRoleEnum
# grammar AT_MT_SPEC.md 3.27 defines for AT+MTTHREAD? and reuses verbatim
# for +MTEVT:28's payload (spec 3.11: "the identical decoded token").
# Shared by t_mtthread_query_shape below (Phase 1) and
# step_2_3_commission's Thread-role observation (Phase 2, bench defect C
# moved it there from a dedicated post-reboot row) so the two checks
# cannot silently drift onto different grammars.
MT_THREAD_ROLE_TOKENS = (
    "UNSPECIFIED", "UNASSIGNED", "SLEEPY_END_DEVICE", "END_DEVICE",
    "REED", "ROUTER", "LEADER",
)

# The subset of MT_THREAD_ROLE_TOKENS that means "on a mesh": everything
# except UNSPECIFIED (interface down) and UNASSIGNED (up but detached).
# step_2_13_thread_reboot_reattach's ends-attached AT+MTTHREAD? read uses
# this to prove a REJOIN happened, not merely a bring-up (review finding
# F1): a device that came back with no Thread dataset would still emit
# SOME role token, but it could never land here.
MT_THREAD_ATTACHED_ROLES = (
    "SLEEPY_END_DEVICE", "END_DEVICE", "REED", "ROUTER", "LEADER",
)

# A future SDK addition outside the known set degrades to the raw decimal
# (design spec 2.1, mt_at.c's role_tok fallback), so the grammar admits
# \d+ alongside the named tokens rather than treating an unknown number as
# malformed.
MT_THREAD_ROLE_TOKEN_RE = re.compile(
    r"(?:" + "|".join(MT_THREAD_ROLE_TOKENS) + r"|\d+)")

# mt_at.c's cmd_mtthread() renders channel decimal, the three ids
# 0x-prefixed upper-case hex at their natural byte width (panid 4 digits,
# extpanid 16, partitionid 8; snprintf's "%04lX"/"%016llX"/"%08lX"), and
# each of the four stays the empty string when its has_* flag is clear
# (null-means-unknown, never 0). The name is always double-quoted, with
# `"` and `\` escaped.
MT_THREAD_LINE_RE = re.compile(
    r"\+MTTHREAD:" + MT_THREAD_ROLE_TOKEN_RE.pattern + r","
    r"[01],"
    r"\d*,"
    r"(?:0x[0-9A-F]{4})?,"
    r"(?:0x[0-9A-F]{16})?,"
    r"(?:0x[0-9A-F]{8})?,"
    r'"(?:[^"\\]|\\.)*"')


def t_mtthread_query_shape(link):
    """AT+MTTHREAD? (design spec 2026-08-12 section 2.1, AT_MT_SPEC.md
    3.27) answers differently depending on the image: a WiFi image has no
    ThreadNetworkDiagnostics cluster on endpoint 0 at all and answers
    +MTERR:8, never a bare ERROR; a Thread image answers OK with a
    decoded role line.

    Design decision (task-3 report): Phase 1 has no per-image test list of
    its own. add_test's whole contract is fn(link) with no context object,
    and both committed baselines (wifi.json, thread.json) run the exact
    same TESTS list and simply record whatever the flashed image gives
    back (capture_header does the same self-detection from AT+MTNET? to
    label the report header). This is the first row whose CORRECT answer
    differs by image rather than its recorded value only, so rather than
    inventing a Phase-1-wide transport-gating mechanism this row detects
    its own image the same way capture_header and phase2_gate already do
    and asserts the behaviour that image is contracted to give. The
    set-form row (AT+MTTHREAD=1 -> ERROR) needed no such split: it is
    identical on both images, so it stays a plain expect_err row in
    register_phase1_t11_negative below.

    Only the line's SHAPE is asserted on a Thread image, never the
    observed role, ids or name: those are the rig's Thread network
    identity, and B181 treats dataset material as credentials, so no
    observed value may reach a committed baseline through this check."""
    res, lines = link.command("AT+MTNET?")
    if res != 0 or not lines:
        return False
    m = re.match(r"\+MTNET:(WIFI|THREAD),", lines[0])
    if not m:
        return False
    if m.group(1) == "WIFI":
        res, _ = link.command("AT+MTTHREAD?")
        return res == 8
    res, lines = link.command("AT+MTTHREAD?")
    return (res == 0 and bool(lines) and
           MT_THREAD_LINE_RE.fullmatch(lines[0]) is not None)


def register_phase1_t11_negative():
    """Thread role API round (0.11.0), task 3: the two state-safe rows
    TESTING.md 6.2 gains from the round's firmware tasks 1 and 2
    (AT+MTTHREAD?, spec 3.27; +MTEVT:28, spec 3.11), a sibling of
    register_phase1_t5/t6/t7/t8/t9/t10 in the same order-independent
    split.

    Both rows are safe on the single-light bench and need no endpoint:
    cmd_mtthread() answers from endpoint 0's ThreadNetworkDiagnostics
    cluster or its absence, never from anything Phase 1's standard
    composition declares.

    - The set-form row is identical on both images (a bare ERROR, the
      standing "wrong command form" division): AT+MTTHREAD? is
      query-only, so AT+MTTHREAD=1 dies in the parser before the image is
      even asked, and one row covers both.
    - The query row is the one whose answer differs by image (+MTERR:8 on
      WiFi, a shape-asserted OK line on Thread); t_mtthread_query_shape
      carries both branches, since Phase 1 has no other place to put an
      image-conditional row (see that function's docstring for the design
      decision this round needed).

    The Phase 2 half of this round (the +MTEVT:28 observation, which
    needs a real commissioning cycle and so cannot be bench-safe on
    Phase 1's single-light rig) lives inside step_2_3_commission's own
    Thread-only branch (bench defect C moved it there from a dedicated
    post-reboot row: the reboot path turned out to have no observable
    event window at all)."""
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")
    n("MTTHREAD=1 -> ERROR (query-only command, both images)",
      expect_err("AT+MTTHREAD=1", -1))
    n("MTTHREAD? shape by image (+MTERR:8 on WiFi, decoded line on "
      "Thread)", t_mtthread_query_shape)


register_phase1_t11_negative()


def _mtrow_stage_then_clear(cmd, ep=1, kind=1):
    """AT+MTROW succeeds (OK) and is immediately cleared with
    AT+MTROWCLEAR, so the single global s_row_stage returns to inactive
    for every later Phase 1 row: the t_evt_mask_set_read_restore
    discipline ("the one piece of state Phase 1 may touch, restored
    immediately"), applied to the one piece of RAM state AT+MTROW
    touches. Every row below that expects OK from a stage uses this
    instead of a bare expect_ok, so no successful stage row can ever
    leak into a later, unrelated AT+MTROWCLEAR/AT+MTROWAPPLY row."""
    def fn(link):
        if link.command(cmd)[0] != 0:
            return False
        return link.command("AT+MTROWCLEAR=%d,%d" % (ep, kind))[0] == 0
    return fn


def t_row_evse_meter_staged(link):
    """Energy round C2, task 13: the AT+MTROW family and AT+MTMETERID
    rows that need a REAL endpoint but not a controller, staged the
    t_meas_staged_wh_min / t_staged_variant1_energy_c1 way: a scratch
    composition (standard light + EVSE variant 1, no SOC feature, ep 2 +
    the utility meter, ep 3), AT+MTEPAPPLY (persists and reboots), VERIFY
    the composition actually took effect via AT+MTEP?, run every row,
    then restore the single-light standard state in a finally block.

    Why these three things are staged together in ONE reboot cycle
    rather than three: each staged Phase 1 row costs two reboots (apply
    + restore), and none of the checks below touch each other's state,
    so bundling saves four reboots for identical coverage (the same
    reasoning t_meas_staged_wh_min and t_staged_variant1_energy_c1 give
    for their own bundling).

    Why EVSE variant 1 rather than variant 0 here: this function's job is
    the AT+MTROWAPPLY count-0 semantics (task 2's review caught two
    separate defects in opposite directions here, task 6 report's ledger
    entry) and the SOC-variant rule's NEGATIVE arm (no SOC feature: a
    target may only claim SoC 100 or omit it). The POSITIVE arm (SOC
    feature: SoC is mandatory) runs against the persistent Phase 3 slot
    (variant 0, PHASE3_COMPOSITION 28), where the round's own full
    70-row-schedule and merge-by-day proofs also live: the design spec's
    own pool table earmarked exactly ONE new permanent Phase 3 endpoint
    for this round (sdkconfig.defaults's "one for C2's EVSE" comment,
    MT_COMP_MAX_ENDPOINTS 28 with 27 already declared), so a second
    permanent EVSE endpoint plus a permanent meter endpoint would need a
    composition-cap raise this task's file list (test/mt_regression.py,
    test/test_mt_regression.py, test/baselines/*) does not include. The
    utility meter needs no controller for anything round C2 added (it is
    an attributes-only cluster with no commands at all, and AT+MTATTR
    reads through the same ember/Instance path a real Matter read would),
    so staging it here rather than spending the one free composition slot
    on it costs nothing this round's own checks need.

    The meter's attributes no longer answering a bare ERROR (design spec
    6.1) is the direct, AT-only test of this round's dead-shell fix:
    before it, MeterIdentification's Instance AttrAccess was declared but
    never registered, so esp_matter::attribute::get_val()'s call into the
    real ReadAttribute() dispatch failed outright, regardless of what
    AT+MTMETERID had pushed. AT+MTATTR's own pipeline only converts
    integer-carrying types past that point, so the three char_strings
    answer +MTERR:5 once the Instance runs, not a value; only MeterType
    (an enum) can prove the stronger "answers a value" claim at this
    layer. PowerThreshold, the struct, is NOT part of that proof: the SDK
    registers it ARRAY-typed and get_val() refuses ARRAY before the read
    dispatch, so it never reaches the Instance in either direction and
    answers +MTERR:5 unconditionally (B265, firmware 0.12.0; before that
    it answered a bare ERROR and the bench failed this row on a correct
    device). The full five-attribute value proof is a controller-read
    bench item, the only path that can carry a string or a struct back at
    all."""
    def diag(msg):
        print("    (staged EVSE/meter c2: %s)" % msg)

    def apply_and_wait():
        link.drain(0.3)
        if link.command("AT+MTEPAPPLY", timeout=5.0)[0] != 0:
            return False
        return link.await_urc(r"\+MTREADY$", timeout=15.0) is not None

    ok = True
    try:
        if not stage_composition(link, ["0x0100", "0x050C,1", "0x0511"]):
            diag("staging the scratch composition failed")
            return False
        if not apply_and_wait():
            diag("AT+MTEPAPPLY or the +MTREADY wait failed")
            return False
        res, lines = cmd_retry(link, "AT+MTEP?")
        if res != 0 or lines != ["+MTEP:0,1,0x0100", "+MTEP:1,2,0x050C,1",
                                 "+MTEP:2,3,0x0511"]:
            diag("composition readback wrong: %r" % lines)
            return False

        # ---- AT+MTROWAPPLY count-0, case (a): nothing staged ----
        # The documented clear request must succeed even when nothing was
        # ever staged for this (ep, kind) (task 1's review: a guard that
        # rejected this used to answer +MTERR:1 instead of clearing).
        res, _ = link.command("AT+MTROWAPPLY=2,1,0")
        if res != 0:
            diag("count-0 case (a), nothing staged: apply answered %r"
                 % res)
            ok = False
        res, lines = cmd_retry(link, "AT+MTROWGET=2,1")
        if res != 0 or lines != []:
            diag("count-0 case (a): readback not empty: %r" % lines)
            ok = False

        # ---- AT+MTROWAPPLY count-0, case (b): rows ARE staged ----
        # The exact repro task 2's review caught: stage two rows, apply
        # count 0, and the two staged rows must be ABANDONED, not
        # committed. A wholesale-replace implementation (or the original,
        # buggy "if matches, pass the stage through unchanged" reading)
        # passes every naive apply-then-read test and fails only this one.
        if link.command("AT+MTROW=2,1,0,2,480,,25000000")[0] != 0:
            diag("count-0 case (b): staging row 0 failed")
            ok = False
        if link.command("AT+MTROW=2,1,1,4,600,,10000000")[0] != 0:
            diag("count-0 case (b): staging row 1 failed")
            ok = False
        res, _ = link.command("AT+MTROWAPPLY=2,1,0")
        if res != 0:
            diag("count-0 case (b): apply answered %r" % res)
            ok = False
        res, lines = cmd_retry(link, "AT+MTROWGET=2,1")
        if res != 0 or lines != []:
            diag("count-0 case (b): the two staged rows were committed "
                 "instead of abandoned: %r" % lines)
            ok = False

        # ---- SOC-variant rule, the NEGATIVE arm (no SOC feature) ----
        # mt_evse_targets_apply_locked()'s pass 1 (mt_evse.cpp) enforces
        # this on the AT path too, not only on a fabric SetTargets: task
        # 4's review found the AT path could otherwise install a schedule
        # a controller's SetTargets would have been refused.
        if link.command("AT+MTROW=2,1,0,2,480,50,25000000")[0] != 0:
            diag("SOC-variant negative: staging soc=50 failed")
            ok = False
        res, _ = link.command("AT+MTROWAPPLY=2,1,1")
        if res != 1:
            diag("SOC-variant negative: soc=50 on a no-SOC endpoint "
                 "answered %r, wanted +MTERR:1" % res)
            ok = False
        if link.command("AT+MTROWCLEAR=2,1")[0] != 0:
            diag("SOC-variant negative: clearing the rejected stage "
                 "failed")
            ok = False
        # SoC 100 ("charge it completely") IS legal on a no-SOC endpoint.
        if link.command("AT+MTROW=2,1,0,2,480,100,25000000")[0] != 0:
            diag("SOC-variant negative: staging soc=100 failed")
            ok = False
        res, _ = link.command("AT+MTROWAPPLY=2,1,1")
        if res != 0:
            diag("SOC-variant negative: soc=100 answered %r, wanted OK"
                 % res)
            ok = False
        # SoC absent is legal too.
        if link.command("AT+MTROW=2,1,0,2,480,,25000000")[0] != 0:
            diag("SOC-variant negative: staging soc-absent failed")
            ok = False
        res, _ = link.command("AT+MTROWAPPLY=2,1,1")
        if res != 0:
            diag("SOC-variant negative: soc-absent answered %r, wanted "
                 "OK" % res)
            ok = False
        # Clear back to empty so this scratch endpoint carries nothing
        # into its next reboot (none is expected before AT+MTFRESET, but
        # leaving a real schedule behind in NVS is exactly the kind of
        # residue this discipline exists to avoid).
        res, _ = link.command("AT+MTROWAPPLY=2,1,0")
        if res != 0:
            diag("final clear answered %r" % res)
            ok = False

        # ---- the utility meter: identity push, then the dead-shell
        # fix's direct test ----
        res, _ = link.command(
            'AT+MTMETERID=3,1,"POD-1","SN-123","V1.0",1500000,2000000,1')
        if res != 0:
            diag("meter identity push answered %r" % res)
            ok = False
        # MeterIdentification 0x0B06: MeterType(0), PointOfDelivery(1),
        # MeterSerialNumber(2), ProtocolVersion(3), PowerThreshold(4).
        # esp_matter::attribute::get_val() (mt_matter_attr_read's own
        # path, main.cpp) calls through to the REAL CHIP data-model
        # provider's ReadAttribute(), the same dispatch a Matter wire
        # read uses, so it DOES reach Instance::Read() once the round's
        # fix registers one. Before the fix nothing implements Read() at
        # all and ReadAttribute() answers failure outright, which
        # mt_matter_attr_read() cannot distinguish from any other
        # unclassified bridge failure and so reports as a bare ERROR
        # (MT_ATTR_ERR_FAILED has no entry in attr_err_to_mterr()'s
        # switch, mt_at.c).
        #
        # AT+MTATTR's OWN pipeline past that point only converts
        # integer-carrying types (attr_val_to_i64(), main.cpp); the
        # cluster's own XML types PointOfDelivery/MeterSerialNumber/
        # ProtocolVersion char_string and PowerThreshold a struct, none
        # of which attr_val_to_i64() can represent, so a SUCCESSFUL read
        # of any of those four still answers +MTERR:5 (MT_ATTR_ERR_TYPE),
        # not a value: that is a real, permanent limit of AT+MTATTR (the
        # project's own "AT+MTATTRX for opaque types is specified but
        # unimplemented"), not evidence the fix regressed. Only MeterType
        # (an enum, genuinely integer-carrying) can prove the stronger
        # claim. The full "all five answer values" proof needs a real
        # controller read of each attribute by name (the bench
        # procedure), which is the only path that can carry a string or a
        # struct back at all.
        #
        # B265, and why these four assert +MTERR:5 EXACTLY rather than
        # "not a bare ERROR". The C2 WiFi bench found the loose form both
        # too weak and, for attribute 4, entirely without discriminating
        # power:
        #   - Attributes 1-3 (the char_strings) reach Instance::Read(),
        #     come back fine, and fail only in attr_val_to_i64(), so they
        #     answer +MTERR:5. Before the dead-shell fix they answered a
        #     bare ERROR. +MTERR:5 is therefore the exact post-fix
        #     answer, and asserting it pins the fix harder than "not -1"
        #     did: a future regression that made the read fail some OTHER
        #     way would slip through "not a bare ERROR" and is caught
        #     here.
        #   - Attribute 4 (PowerThreshold, a struct) NEVER reaches the
        #     Instance at all, before the fix or after it. esp-matter
        #     registers it ESP_MATTER_VAL_TYPE_ARRAY (a struct has no
        #     esp_matter_attr_val_t representation), and get_val()
        #     refuses ARRAY BEFORE the ReadAttribute() dispatch
        #     (esp_matter_data_model.cpp). The bench captured the C6
        #     console during each read: attributes 0-3 each logged one
        #     "Meter Indication read attr N" line, attribute 4 logged
        #     nothing. So the old "not a bare ERROR" assertion could
        #     never have told a reached Instance from an unreached one
        #     for this attribute: its answer is unconditional. It was an
        #     unsound inference, and it failed on a CORRECT device.
        #     Firmware 0.12.0 maps that ARRAY refusal to +MTERR:5
        #     (mt_matter_attr_read(), B265), which is what the code
        #     means, so all four now answer the same code for two
        #     different reasons and AT_MT_SPEC.md 3.9's claim is true as
        #     written. This row pins the error contract; it does NOT pin
        #     the dead-shell fix for attribute 4, and nothing at this
        #     layer can.
        res, lines = link.command("AT+MTATTR=3,2822,0")
        if res != 0:
            diag("MeterType read answered %r, wanted a value" % res)
            ok = False
        elif not lines or not re.fullmatch(r"\+MTATTR:3,2822,0,1", lines[0]):
            diag("MeterType read: %r, wanted MeterType=1 (Private)"
                 % lines)
            ok = False
        for attr, name in ((1, "PointOfDelivery"), (2, "MeterSerialNumber"),
                           (3, "ProtocolVersion")):
            res, _ = link.command("AT+MTATTR=3,2822,%d" % attr)
            if res != 5:
                diag("%s read answered %r, wanted +MTERR:5 (a bare ERROR "
                     "means the Instance was never reached, the "
                     "dead-shell bug; anything else is a new failure)"
                     % (name, res))
                ok = False
        res, _ = link.command("AT+MTATTR=3,2822,4")
        if res != 5:
            diag("PowerThreshold read answered %r, wanted +MTERR:5 "
                 "(get_val()'s ARRAY refusal mapped to MT_ATTR_ERR_TYPE, "
                 "B265); this row does not test the dead-shell fix, "
                 "which cannot be reached for a struct attribute" % res)
            ok = False
    finally:
        restored = (stage_composition(link, ["0x0100"]) and apply_and_wait())
        if restored:
            res, lines = cmd_retry(link, "AT+MTEP?")
            restored = res == 0 and lines == ["+MTEP:0,1,0x0100"]
        if not restored:
            diag("RESTORE FAILED: bench is not in the single-light "
                 "standard state")
            ok = False
    return ok


def t_row_meter_pool_exhaustion(link):
    """Energy round C2: MT_METER_MAX is 2 (mt_matter.h), and declaring a
    THIRD Electrical Utility Meter must abort the composition rebuild
    without silently building a third, broken endpoint. Fix round 2
    (task 8 report) moved the pool reservation to the FIRST statement of
    mk_electrical_utility_meter(), before electrical_utility_meter::
    create() runs, so the third meter's own endpoint is never created at
    all: nothing is orphaned, unlike the fix round 1 shape the review
    caught.

    What is and is not observable over AT: mt_matter_record_endpoint()
    only runs for a thunk that returned an endpoint id, so the boot log's
    "composition rebuilt: N endpoint(s)" and this test's AT+MTEP? readback
    both stop at the light plus the first TWO meters (endpoints 1-3), even
    though FOUR entries (light + three meters) were declared and stored.
    A controller-visible orphan (an endpoint AT+MTEP? cannot see but
    provider::Endpoints() can) is what the bench's chip-tool parts-list
    read (report section on bench items) checks for; this Phase 1 row is
    the AT-only half: readback shows exactly the successful PREFIX, never
    the declared count and never zero."""
    def diag(msg):
        print("    (staged meter pool exhaustion: %s)" % msg)

    def apply_and_wait():
        link.drain(0.3)
        if link.command("AT+MTEPAPPLY", timeout=5.0)[0] != 0:
            return False
        return link.await_urc(r"\+MTREADY$", timeout=15.0) is not None

    ok = True
    try:
        staged = ["0x0100", "0x0511", "0x0511", "0x0511"]
        if not stage_composition(link, staged):
            diag("staging the 4-entry scratch composition failed")
            return False
        if not apply_and_wait():
            diag("AT+MTEPAPPLY or the +MTREADY wait failed")
            return False
        res, lines = cmd_retry(link, "AT+MTEP?")
        expect = ["+MTEP:0,1,0x0100", "+MTEP:1,2,0x0511", "+MTEP:2,3,0x0511"]
        if res != 0 or lines != expect:
            diag("readback after pool exhaustion: %r, wanted the "
                 "light-plus-two-meters PREFIX %r (not the declared "
                 "4-entry list, and not empty)" % (lines, expect))
            ok = False
    finally:
        restored = (stage_composition(link, ["0x0100"]) and apply_and_wait())
        if restored:
            res, lines = cmd_retry(link, "AT+MTEP?")
            restored = res == 0 and lines == ["+MTEP:0,1,0x0100"]
        if not restored:
            diag("RESTORE FAILED: bench is not in the single-light "
                 "standard state")
            ok = False
    return ok


def register_phase1_t12_negative():
    """Energy round C2 (0.12.0), task 13: the AT+MTROW family
    (AT+MTROW, AT+MTROWCLEAR, AT+MTROWAPPLY, AT+MTROWGET) and
    AT+MTMETERID grammar rows design spec 8.2 asks for: arity, every
    range boundary, the empty-field convention, every error code in the
    spec's error table (2.5/6.2), and the bare-ERROR-versus-+MTERR
    division at each boundary, transcribed against main/mt_at.c's own
    handler comments rather than re-derived, the same discipline
    register_phase1_t5..t11 established.

    ep 1 (the standard rig's OnOff light) stands in for "any endpoint"
    throughout, the register_phase1_t7/t8's role for it: AT+MTROW/
    AT+MTROWCLEAR never touch the live data model at all (pure RAM
    staging, mt_rows.c's file comment), so ep's value is irrelevant to
    every one of their rows below. AT+MTROWAPPLY's count-0 form and
    AT+MTROWGET's unqualified read DO reach the live model, and ep 1
    there answers +MTERR:4 (endpoint exists, no EnergyEvse/
    MeterIdentification cluster), which doubles as a DIFFERENTIAL signal
    proving a value-level check passed: AT+MTMETERID's 64-byte-string-
    accepted row and its comma-survives row both use "+MTERR:4 on ep 1"
    as their positive evidence, since the alternative (some other
    endpoint that carries the cluster) would need a Phase 3 slot these
    rows do not need and should not spend one on.

    ep 99 always answers +MTERR:2 (unknown endpoint): register_phase1_
    t5's own convention.

    Two rows need a REAL EVSE/meter endpoint (the AT+MTROWAPPLY count-0
    semantics and the meter's dead-shell fix) and are staged the
    t_meas_staged_wh_min way, in t_row_evse_meter_staged and
    t_row_meter_pool_exhaustion above: see their docstrings for why a
    scratch composition rather than a Phase 3 slot, and why the two are
    kept as separate reboot cycles (the pool-exhaustion row's composition
    is deliberately abnormal -- comp.count desyncs from what was staged
    -- and mixing that into the same scratch session as the ordinary-path
    rows above would make a diagnosis harder to attribute)."""
    n = lambda name, fn: add_test(1, name, fn, tag="AT-")

    # ==== AT+MTROW=<ep>,<kind>,<idx>,<field>[,<field>...] ====
    # Kind 1 (MT_ROW_KIND_EVSE_TARGET) needs exactly 4 field tokens: day
    # bitmap (1..0x7F, mandatory), minutes past midnight (0..1439,
    # mandatory), target SoC (0..100, optional), added energy (0..
    # INT64_MAX, optional; at least one of the last two is required).
    n("MTROW? -> ERROR (query form on a set-only command)",
      expect_err("AT+MTROW?", -1))
    n("MTROW no args -> ERROR (exec form)", expect_err("AT+MTROW", -1))
    n("MTROW=1,1 -> ERROR (fewer than <ep>,<kind>,<idx>)",
      expect_err("AT+MTROW=1,1", -1))
    n("MTROW=1,1,0,2,480,50 -> ERROR (3 field tokens, kind 1 needs "
      "exactly 4: arity, not a value error)",
      expect_err("AT+MTROW=1,1,0,2,480,50", -1))
    n("MTROW=1,1,0,2,480,50,25000000,1 -> ERROR (5 field tokens, one "
      "too many)",
      expect_err("AT+MTROW=1,1,0,2,480,50,25000000,1", -1))
    n("MTROW 12 tokens -> ERROR (at_split_args overflow, "
      "MT_ROW_MAX_TOKENS is 11)",
      expect_err("AT+MTROW=" + ",".join(str(i) for i in range(12)), -1))
    n("MTROW=zz,1,0,2,480,,25000000 -> +MTERR:1 (ep not numeric)",
      expect_err("AT+MTROW=zz,1,0,2,480,,25000000", 1))
    n("MTROW=70000,1,0,2,480,,25000000 -> +MTERR:1 (ep > 0xFFFF)",
      expect_err("AT+MTROW=70000,1,0,2,480,,25000000", 1))
    n("MTROW=1,zz,0 -> +MTERR:1 (kind not numeric)",
      expect_err("AT+MTROW=1,zz,0", 1))
    n("MTROW=1,300,0 -> +MTERR:3 (kind > 0xFF)",
      expect_err("AT+MTROW=1,300,0", 3))
    n("MTROW=1,2,0 -> +MTERR:3 (kind 2 is not a registered row kind)",
      expect_err("AT+MTROW=1,2,0", 3))
    n("MTROW=1,1,zz,2,480,,25000000 -> +MTERR:1 (idx not numeric)",
      expect_err("AT+MTROW=1,1,zz,2,480,,25000000", 1))
    n("MTROW=1,1,70000,2,480,,25000000 -> +MTERR:1 (idx > 0xFFFF)",
      expect_err("AT+MTROW=1,1,70000,2,480,,25000000", 1))
    n("MTROW=1,1,70,2,480,,25000000 -> +MTERR:1 (idx 70 is at kind 1's "
      "max_rows, valid range is 0..69)",
      expect_err("AT+MTROW=1,1,70,2,480,,25000000", 1))
    n("MTROW=1,1,0,,480,,25000000 -> +MTERR:1 (day bitmap is mandatory, "
      "an empty token is not an absent-optional here)",
      expect_err("AT+MTROW=1,1,0,,480,,25000000", 1))
    n("MTROW=1,1,0,zz,480,,25000000 -> +MTERR:1 (day bitmap not numeric)",
      expect_err("AT+MTROW=1,1,0,zz,480,,25000000", 1))
    n("MTROW=1,1,0,0,480,,25000000 -> +MTERR:1 (day bitmap 0, below the "
      "1..0x7F range)",
      expect_err("AT+MTROW=1,1,0,0,480,,25000000", 1))
    n("MTROW=1,1,0,128,480,,25000000 -> +MTERR:1 (day bitmap 0x80, "
      "above the 1..0x7F range)",
      expect_err("AT+MTROW=1,1,0,128,480,,25000000", 1))
    n("MTROW=1,1,0,2,1440,,25000000 -> +MTERR:1 (minutes past midnight "
      "1440, above the 0..1439 range)",
      expect_err("AT+MTROW=1,1,0,2,1440,,25000000", 1))
    n("MTROW=1,1,0,2,-1,,25000000 -> +MTERR:1 (minutes -1: sign always "
      "parses, then the range check rejects it)",
      expect_err("AT+MTROW=1,1,0,2,-1,,25000000", 1))
    n("MTROW=1,1,0,2,zz,,25000000 -> +MTERR:1 (minutes not numeric)",
      expect_err("AT+MTROW=1,1,0,2,zz,,25000000", 1))
    n("MTROW=1,1,0,2,480,101,25000000 -> +MTERR:1 (SoC 101, above the "
      "0..100 range)",
      expect_err("AT+MTROW=1,1,0,2,480,101,25000000", 1))
    n("MTROW=1,1,0,2,480,-1,25000000 -> +MTERR:1 (SoC -1, below 0)",
      expect_err("AT+MTROW=1,1,0,2,480,-1,25000000", 1))
    n("MTROW=1,1,0,2,480,,-1 -> +MTERR:1 (added energy -1, below 0: an "
      "unsigned field, sign always parses, then range rejects)",
      expect_err("AT+MTROW=1,1,0,2,480,,-1", 1))
    n("MTROW=1,1,0,2,480,zz, -> +MTERR:1 (SoC not numeric)",
      expect_err("AT+MTROW=1,1,0,2,480,zz,", 1))
    n("MTROW=1,1,0,2,480,, -> +MTERR:1 (SoC and added energy both "
      "absent: the kind-specific \"at least one\" rule)",
      expect_err("AT+MTROW=1,1,0,2,480,,", 1))
    n("MTROW=1,1,0,2,480,,25000000 -> OK (SoC absent, added energy "
      "present: the empty-field convention), then AT+MTROWCLEAR -> OK",
      _mtrow_stage_then_clear("AT+MTROW=1,1,0,2,480,,25000000"))
    n('MTROW=1,1,0,2,480,80, -> OK (SoC present, added energy absent), '
      "then AT+MTROWCLEAR -> OK",
      _mtrow_stage_then_clear("AT+MTROW=1,1,0,2,480,80,"))
    n("MTROW=1,1,0,127,480,,1 -> OK (day bitmap at its max, 0x7F), "
      "then AT+MTROWCLEAR -> OK",
      _mtrow_stage_then_clear("AT+MTROW=1,1,0,127,480,,1"))

    # ==== AT+MTROWCLEAR=<ep>,<kind> ====
    n("MTROWCLEAR? -> ERROR (query form)", expect_err("AT+MTROWCLEAR?", -1))
    n("MTROWCLEAR no args -> ERROR (exec form)",
      expect_err("AT+MTROWCLEAR", -1))
    n("MTROWCLEAR=1 -> ERROR (one token, fewer than <ep>,<kind>)",
      expect_err("AT+MTROWCLEAR=1", -1))
    n("MTROWCLEAR=1,1,0 -> ERROR (three tokens, one too many)",
      expect_err("AT+MTROWCLEAR=1,1,0", -1))
    n("MTROWCLEAR=zz,1 -> +MTERR:1 (ep not numeric)",
      expect_err("AT+MTROWCLEAR=zz,1", 1))
    n("MTROWCLEAR=70000,1 -> +MTERR:1 (ep > 0xFFFF)",
      expect_err("AT+MTROWCLEAR=70000,1", 1))
    n("MTROWCLEAR=1,zz -> +MTERR:1 (kind not numeric)",
      expect_err("AT+MTROWCLEAR=1,zz", 1))
    n("MTROWCLEAR=1,300 -> +MTERR:3 (kind > 0xFF)",
      expect_err("AT+MTROWCLEAR=1,300", 3))
    n("MTROWCLEAR=1,2 -> +MTERR:3 (kind 2 is not registered)",
      expect_err("AT+MTROWCLEAR=1,2", 3))
    n("MTROWCLEAR=1,1 -> +MTERR:1 (nothing staged for this ep/kind: "
      "mt_rows_clear() refuses rather than silently no-opping)",
      expect_err("AT+MTROWCLEAR=1,1", 1))

    # ==== AT+MTROWAPPLY=<ep>,<kind>,<count>, the pure-form/lookup-free
    # rows only: the count-0 special cases need a real EVSE endpoint
    # (t_row_evse_meter_staged above) because mt_matter_rows_apply() is
    # called unconditionally on that path, even when nothing matched. ====
    n("MTROWAPPLY? -> ERROR (query form)", expect_err("AT+MTROWAPPLY?", -1))
    n("MTROWAPPLY no args -> ERROR (exec form)",
      expect_err("AT+MTROWAPPLY", -1))
    n("MTROWAPPLY=1,1 -> ERROR (two tokens, fewer than <ep>,<kind>,"
      "<count>)", expect_err("AT+MTROWAPPLY=1,1", -1))
    n("MTROWAPPLY=1,1,0,5 -> ERROR (four tokens, one too many)",
      expect_err("AT+MTROWAPPLY=1,1,0,5", -1))
    n("MTROWAPPLY=70000,1,0 -> +MTERR:1 (ep > 0xFFFF, checked before "
      "kind or count)",
      expect_err("AT+MTROWAPPLY=70000,1,0", 1))
    n("MTROWAPPLY=1,300,0 -> +MTERR:3 (kind > 0xFF)",
      expect_err("AT+MTROWAPPLY=1,300,0", 3))
    n("MTROWAPPLY=1,2,0 -> +MTERR:3 (kind 2 is not registered)",
      expect_err("AT+MTROWAPPLY=1,2,0", 3))
    n("MTROWAPPLY=1,1,70000 -> +MTERR:1 (count > 0xFFFF)",
      expect_err("AT+MTROWAPPLY=1,1,70000", 1))
    n("MTROWAPPLY=1,1,zz -> +MTERR:1 (count not numeric)",
      expect_err("AT+MTROWAPPLY=1,1,zz", 1))
    n("MTROWAPPLY=1,1,5 -> +MTERR:1 (nonzero count, nothing staged for "
      "this ep/kind: cannot possibly match, the same value error a "
      "mismatched count against a real stage gives)",
      expect_err("AT+MTROWAPPLY=1,1,5", 1))
    # The two lookup errors mt_matter_rows_apply() itself can answer, once
    # a matching, well-formed stage reaches the bridge: staging under the
    # SAME ep the apply names makes "matches" true without needing any
    # real endpoint at all (mt_rows_stage() never checks ep against the
    # composition), so these need no Phase 3 slot. A failed apply leaves
    # the stage untouched (cmd_mtrowapply's own comment), hence the
    # explicit AT+MTROWCLEAR after each.
    n("MTROWAPPLY=1,1,1 -> +MTERR:4 (stage matches, reaches the bridge: "
      "ep 1 carries no EnergyEvse cluster), then AT+MTROWCLEAR -> OK",
      lambda link: (
          link.command("AT+MTROW=1,1,0,2,480,,25000000")[0] == 0
          and link.command("AT+MTROWAPPLY=1,1,1")[0] == 4
          and link.command("AT+MTROWCLEAR=1,1")[0] == 0))
    n("MTROWAPPLY=99,1,1 -> +MTERR:2 (stage matches, reaches the bridge: "
      "ep 99 is unknown), then AT+MTROWCLEAR -> OK",
      lambda link: (
          link.command("AT+MTROW=99,1,0,2,480,,25000000")[0] == 0
          and link.command("AT+MTROWAPPLY=99,1,1")[0] == 2
          and link.command("AT+MTROWCLEAR=99,1")[0] == 0))

    # ==== AT+MTROWGET=<ep>,<kind>[,<idx>][,<seq>] ====
    n("MTROWGET? -> ERROR (query form)", expect_err("AT+MTROWGET?", -1))
    n("MTROWGET no args -> ERROR (exec form)",
      expect_err("AT+MTROWGET", -1))
    n("MTROWGET=1 -> ERROR (one token, fewer than the two-token minimum)",
      expect_err("AT+MTROWGET=1", -1))
    n("MTROWGET=1,1,0,5,9 -> ERROR (five tokens, at_split_args overflow "
      "against the fixed 4-token array)",
      expect_err("AT+MTROWGET=1,1,0,5,9", -1))
    n("MTROWGET=1,1, -> ERROR (empty third token with no fourth: the "
      "bulk form's own spelling is a bare two-token line, not a "
      "long-hand empty idx)",
      expect_err("AT+MTROWGET=1,1,", -1))
    n("MTROWGET=zz,1 -> +MTERR:1 (ep not numeric)",
      expect_err("AT+MTROWGET=zz,1", 1))
    n("MTROWGET=70000,1 -> +MTERR:1 (ep > 0xFFFF)",
      expect_err("AT+MTROWGET=70000,1", 1))
    n("MTROWGET=1,zz -> +MTERR:1 (kind not numeric)",
      expect_err("AT+MTROWGET=1,zz", 1))
    n("MTROWGET=1,300 -> +MTERR:3 (kind > 0xFF)",
      expect_err("AT+MTROWGET=1,300", 3))
    n("MTROWGET=1,2 -> +MTERR:3 (kind 2 is not registered)",
      expect_err("AT+MTROWGET=1,2", 3))
    n("MTROWGET=1,1,zz -> +MTERR:1 (idx not numeric, single-row form)",
      expect_err("AT+MTROWGET=1,1,zz", 1))
    n("MTROWGET=1,1,70000 -> +MTERR:1 (idx > 0xFFFF)",
      expect_err("AT+MTROWGET=1,1,70000", 1))
    n("MTROWGET=99,1 -> +MTERR:2 (unknown ep, unqualified bulk form)",
      expect_err("AT+MTROWGET=99,1", 2))
    n("MTROWGET=1,1 -> +MTERR:4 (ep 1 carries no EnergyEvse cluster, "
      "unqualified bulk form)",
      expect_err("AT+MTROWGET=1,1", 4))
    n("MTROWGET=1,1,0 -> +MTERR:4 (same lookup, single-row form)",
      expect_err("AT+MTROWGET=1,1,0", 4))
    n("MTROWGET=1,1,,0 -> +MTERR:1 (seq 0 is the idle marker, never a "
      "legitimate seq, rejected before any pending-set check)",
      expect_err("AT+MTROWGET=1,1,,0", 1))
    n("MTROWGET=1,1,,12345 -> +MTERR:1 (seq well-formed but nothing is "
      "pending for this ep/kind on the single-light rig)",
      expect_err("AT+MTROWGET=1,1,,12345", 1))
    n("MTROWGET=1,1,0,12345 -> +MTERR:1 (same, single-row qualified form)",
      expect_err("AT+MTROWGET=1,1,0,12345", 1))
    n("MTROWGET=1,1,,4294967296 -> +MTERR:1 (seq above uint32)",
      expect_err("AT+MTROWGET=1,1,,4294967296", 1))
    n("MTROWGET=1,1,,zz -> +MTERR:1 (seq not numeric)",
      expect_err("AT+MTROWGET=1,1,,zz", 1))

    # ==== AT+MTMETERID=<ep>,<type>,"<pod>","<serial>","<protocol>",
    # <pwr>,<apparent>,<src> ====
    n("MTMETERID? -> ERROR (query form)", expect_err("AT+MTMETERID?", -1))
    n("MTMETERID no args -> ERROR (exec form)",
      expect_err("AT+MTMETERID", -1))
    n("MTMETERID=1 -> ERROR (no comma at all: <type> structurally "
      "missing)", expect_err("AT+MTMETERID=1", -1))
    n('MTMETERID=zz,0,"A","B","C",100,, -> +MTERR:1 (ep not numeric)',
      expect_err('AT+MTMETERID=zz,0,"A","B","C",100,,', 1))
    n('MTMETERID=70000,0,"A","B","C",100,, -> +MTERR:1 (ep > 0xFFFF)',
      expect_err('AT+MTMETERID=70000,0,"A","B","C",100,,', 1))
    n('MTMETERID=99,0,"A","B","C",100,, -> +MTERR:2 (unknown ep)',
      expect_err('AT+MTMETERID=99,0,"A","B","C",100,,', 2))
    n('MTMETERID=1,zz,"A","B","C",100,, -> +MTERR:1 (type not numeric)',
      expect_err('AT+MTMETERID=1,zz,"A","B","C",100,,', 1))
    n('MTMETERID=1,3,"A","B","C",100,, -> +MTERR:1 (type 3, above '
      "MeterTypeEnum's 0..2)",
      expect_err('AT+MTMETERID=1,3,"A","B","C",100,,', 1))
    n('MTMETERID=1,0,A,"B","C",100,, -> ERROR (pod has no opening '
      'quote: the position calls for a quoted string, form)',
      expect_err('AT+MTMETERID=1,0,A,"B","C",100,,', -1))
    n('MTMETERID=1,0,"ABC -> ERROR (unterminated quote: the parser can '
      "never establish where the field ends, form)",
      expect_err('AT+MTMETERID=1,0,"ABC', -1))
    n('MTMETERID=1,0,"A"B","C","D",100,, -> +MTERR:1 (junk after the '
      "closing quote, exactly what an embedded raw quote produces: the "
      "boundary WAS found, so this is a value problem, not a shape one)",
      expect_err('AT+MTMETERID=1,0,"A"B","C","D",100,,', 1))
    n('MTMETERID comma survives inside a quoted pod -> +MTERR:4 (ep 1 '
      "wrong cluster: reaching the bridge at all proves the comma did "
      "NOT split the token)",
      expect_err('AT+MTMETERID=1,0,"Meter, Point","S","P",100,,', 4))
    n('MTMETERID 64-byte pod -> +MTERR:4 (ep 1 wrong cluster: reaching '
      "the bridge proves the length was accepted)",
      expect_err('AT+MTMETERID=1,0,"' + "A" * 64 + '","S","P",100,,', 4))
    n('MTMETERID 65-byte pod -> +MTERR:1 (one byte over '
      "MT_METERID_MAX_STR, caught before the bridge)",
      expect_err('AT+MTMETERID=1,0,"' + "A" * 65 + '","S","P",100,,', 1))
    n('MTMETERID non-printable byte in pod -> +MTERR:1 (0x01 is outside '
      "0x20..0x7E)",
      expect_err('AT+MTMETERID=1,0,"A\x01B","S","P",100,,', 1))
    n('MTMETERID=1,0,"A","B","C",,, -> +MTERR:1 (pwr and apparent both '
      'absent: PowerThresholdStruct\'s "choice b")',
      expect_err('AT+MTMETERID=1,0,"A","B","C",,,', 1))
    n('MTMETERID pwr only present -> +MTERR:4 (ep 1 wrong cluster: '
      "reaching the bridge proves choice-b accepted a single value)",
      expect_err('AT+MTMETERID=1,0,"A","B","C",100,,', 4))
    n('MTMETERID apparent only present -> +MTERR:4 (same, the other '
      "half of choice-b)",
      expect_err('AT+MTMETERID=1,0,"A","B","C",,200,', 4))
    n('MTMETERID=1,0,"A","B","C",100,200 -> ERROR (no separator comma '
      "before <src> at all: structurally missing, form)",
      expect_err('AT+MTMETERID=1,0,"A","B","C",100,200', -1))
    n('MTMETERID=1,0,"A","B","C",100,200, -> +MTERR:4 (ep 1 wrong '
      "cluster: the trailing comma makes an empty <src> legal, null)",
      expect_err('AT+MTMETERID=1,0,"A","B","C",100,200,', 4))
    n('MTMETERID=1,0,"A","B","C",100,200,3 -> +MTERR:1 (src 3, above '
      "PowerThresholdSourceEnum's 0..2)",
      expect_err('AT+MTMETERID=1,0,"A","B","C",100,200,3', 1))
    n('MTMETERID=1,0,"A","B","C",100,200,zz -> +MTERR:1 (src not '
      "numeric)",
      expect_err('AT+MTMETERID=1,0,"A","B","C",100,200,zz', 1))

    # ==== AT+MTMEAS cluster 0x0099 (153, EnergyEvse) ====
    # Added by round C2's FINAL REVIEW: the round shipped a whole new
    # AT+MTMEAS cluster branch (mt_at.c's mt_meas_field_signed() 0x0099
    # arm, mt_matter_evse_set(), mt_evse.cpp's nineteen-field table) with
    # NO Phase 1 row and no Phase 3 push. Its four sibling clusters
    # (0x0091, 0x0094, 0x0098, 0x0090/0x0091's pure-form rows) each got
    # this treatment in their own round; this closes the gap.
    #
    # ep 1 (the standard rig's OnOff light) is "any endpoint" here, the
    # register_phase1_t8/t9/t10 convention, and the rows split into two
    # groups that prove different things:
    #
    #   - The SIGN rows die inside cmd_mtmeas()'s own parse, BEFORE the
    #     endpoint lookup ever runs (parse_i64 is handed
    #     mt_meas_field_signed(cluster, field) and refuses a leading
    #     minus on an unsigned field), so they answer +MTERR:1 on any
    #     endpoint at all and are state-safe on the single-light rig.
    #   - The rows on fields the table marks SIGNED parse cleanly and
    #     reach mt_matter_meas_set(), where ep 1 carries no EnergyEvse
    #     cluster and answers +MTERR:3. That +MTERR:3 is the POSITIVE
    #     evidence: getting that far proves the minus was accepted, the
    #     same differential trick t10's AbsMinPower row uses for 0x0098.
    #
    # The signedness split is transcribed from mt_at.c's 0x0099 arm, not
    # guessed: SIGNED are 4 (CircuitCapacity), 5 (MinChargeCurrent), 6
    # (MaxChargeCurrent), 7 (UserMaximumChargeCurrent), 11
    # (NextChargeRequiredEnergy), 15 (BatteryCapacity) and 18
    # (SessionEnergyCharged); everything else in 0..18 is unsigned, and a
    # field id ABOVE 18 falls through to the function's signed default.
    n("MTMEAS=1,153,0 -> +MTERR:1 (0x99 odd tail: field 0 has no value, "
      "the shape check that runs before any endpoint lookup)",
      expect_err("AT+MTMEAS=1,153,0", 1))
    n("MTMEAS=1,153,0,-1 -> +MTERR:1 (minus on State, an unsigned enum8)",
      expect_err("AT+MTMEAS=1,153,0,-1", 1))
    n("MTMEAS=1,153,1,-1 -> +MTERR:1 (minus on SupplyState, an unsigned "
      "enum8)",
      expect_err("AT+MTMEAS=1,153,1,-1", 1))
    n("MTMEAS=1,153,3,-1 -> +MTERR:1 (minus on ChargingEnabledUntil, an "
      "unsigned u32 epoch)",
      expect_err("AT+MTMEAS=1,153,3,-1", 1))
    n("MTMEAS=1,153,8,-1 -> +MTERR:1 (minus on RandomizationDelay"
      "Window, an unsigned u32)",
      expect_err("AT+MTMEAS=1,153,8,-1", 1))
    n("MTMEAS=1,153,14,-1 -> +MTERR:1 (minus on StateOfCharge, an "
      "unsigned u8)",
      expect_err("AT+MTMEAS=1,153,14,-1", 1))
    n("MTMEAS=1,153,4,-5000000000 -> +MTERR:3 (CircuitCapacity IS "
      "signed, so the minus AND the 64-bit width both parse and the "
      "call reaches the bridge, where ep 1 carries no EnergyEvse "
      "cluster: the +MTERR:3 is the positive evidence)",
      expect_err("AT+MTMEAS=1,153,4,-5000000000", 3))
    n("MTMEAS=1,153,15,-1 -> +MTERR:3 (BatteryCapacity is signed too, "
      "the same differential)",
      expect_err("AT+MTMEAS=1,153,15,-1", 3))
    n("MTMEAS=1,153,18,5000000000 -> +MTERR:3 (SessionEnergyCharged "
      "past 32 bits parses, the 64-bit value pipeline: same "
      "differential)",
      expect_err("AT+MTMEAS=1,153,18,5000000000", 3))
    n("MTMEAS=1,153,0,1 -> +MTERR:3 (a perfectly well formed push at a "
      "light endpoint: cluster-not-present, not endpoint-not-present)",
      expect_err("AT+MTMEAS=1,153,0,1", 3))
    n("MTMEAS=99,153,0,1 -> +MTERR:2 (unknown endpoint outranks the "
      "cluster check)",
      expect_err("AT+MTMEAS=99,153,0,1", 2))
    n("MTMEAS=1,153,zz,1 -> +MTERR:1 (field token not numeric, before "
      "the signedness lookup can be consulted at all)",
      expect_err("AT+MTMEAS=1,153,zz,1", 1))
    n("MTMEAS=1,153,300,1 -> +MTERR:1 (field id above u8)",
      expect_err("AT+MTMEAS=1,153,300,1", 1))

    n("MTROWAPPLY count-0, both directions, on a real EVSE endpoint "
      "(case a: nothing staged; case b: two rows staged, must be "
      "abandoned not committed); SOC-variant rule negative arm; meter "
      "identity push + AT+MTATTR readback (the dead-shell fix)",
      t_row_evse_meter_staged)
    n("Utility meter pool exhaustion (MT_METER_MAX=2): a third meter "
      "aborts the rebuild and AT+MTEP? shows exactly the successful "
      "prefix, never the declared count and never empty",
      t_row_meter_pool_exhaustion)


register_phase1_t12_negative()


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
    {"name": "2.13 Thread reattach survives a non-factory reboot",
     "fn": step_2_13_thread_reboot_reattach,
     "requires": ["2.3 commission ble-wifi"]},
    {"name": "2.14 transport switch", "fn": step_2_14_transport_switch,
     "requires": ["2.3 commission ble-wifi"]},
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
    tip this on their own (T3 final review finding 1b), and NEITHER must
    suite.na (bench defect C's controller ruling): a step reporting
    itself architecturally not applicable to this run (e.g. a WiFi
    transport has no Thread mesh for step_2_13 to reattach to) is a fact
    about the environment, not a truncation or an operator choice, and a
    clean WiFi run with zero failures must exit 0 to remain a usable pass
    signal. suite.skipped is unaffected: a real precondition-broken skip
    still tips this, on purpose. Kept as its own function, not inlined in
    main(), so a self-test can call the real return path instead of
    re-deriving the same boolean expression."""
    return 1 if (suite.failed or suite.skipped or truncated) else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    # NO default, deliberately. /dev/ttyACM0 was the default until the C2
    # bench, and on this rig that node is the ZBT-2 Thread RCP: an AT write
    # into its live Spinel link kills otbr-agent within about four seconds,
    # which is exactly what happened. A comment warning about a trap does
    # not remove the trap, so the run now refuses rather than guessing
    # (module docstring, TESTING.md section 2).
    ap.add_argument("--port", default=os.environ.get("MT_PORT"),
                    help="AT link, REQUIRED; use a /dev/serial/by-id path, "
                         "never /dev/ttyACM<n> (see the module docstring)")
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
    ap.add_argument("--max-endpoints", type=int, default=None,
                    help="Phase 3 only: declare the first N entries of "
                         "PHASE3_COMPOSITION instead of all of them, and "
                         "report every step that targets an endpoint above "
                         "N as not-applicable. Exists for the combined "
                         "WiFi+Thread image, whose measured endpoint cap is "
                         "below the full table (task-5-report.md)")
    args = ap.parse_args(argv)
    if args.baseline and args.phase == 2 and not (
            args.include_slow and args.include_manual):
        ap.error("--baseline with --phase 2 requires --include-slow and "
                 "--include-manual: a committed baseline must not contain "
                 "gated-out entries")
    # Same shape as the gate above, and for the same reason: a request that
    # cannot mean anything must be refused at the door rather than exiting
    # 0 having measured something else. --max-endpoints 0 would declare an
    # empty composition, a negative would silently truncate from the far
    # end (Python slicing), and anything above the table's length would
    # claim a cap the run never applied.
    if args.max_endpoints is not None:
        if args.phase != 3:
            ap.error("--max-endpoints applies to --phase 3 only: it "
                     "truncates PHASE3_COMPOSITION, and no other phase "
                     "declares one")
        if not 1 <= args.max_endpoints <= len(PHASE3_COMPOSITION):
            ap.error("--max-endpoints must be between 1 and %d (the length "
                     "of PHASE3_COMPOSITION); got %d"
                     % (len(PHASE3_COMPOSITION), args.max_endpoints))
    # After the baseline gate on purpose: the self-test for that gate calls
    # main() without a port, and it must keep exiting for its own reason.
    if not args.port:
        ap.error("--port is required (or MT_PORT in the environment): pass a "
                 "/dev/serial/by-id path, never /dev/ttyACM<n>, which moves "
                 "across re-enumeration and may be another device entirely")

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
            # A device that answers AT+MTTRANSPORT? at all IS the combined
            # image (spec 3.12.2: build_wifi/build_thread never register
            # it, so it reaches the unknown-command path and answers
            # +MTERR:8 there, never OK).
            res, _ = link.command("AT+MTTRANSPORT?")
            ctx.image = "combined" if res == 0 else transport.lower()
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
            if args.max_endpoints is not None:
                ctx.max_endpoints = args.max_endpoints
                ctx.composition = list(
                    PHASE3_COMPOSITION[:args.max_endpoints])
                header["max_endpoints"] = args.max_endpoints
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
