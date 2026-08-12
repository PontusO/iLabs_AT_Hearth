#!/usr/bin/env python3
"""Hardware-free self-test for the T1 regression harness.

Exercises ATLink against a scripted fake transport, so the result
mapping and URC handling are pinned without a board on the desk.
"""

import contextlib
import io
import json
import os
import re
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mt_regression import ATLink, cmd_retry


class FakeTransport:
    """Stream double: read() drains a scripted rx buffer, write() records
    and can trigger a scripted reply via on_write.

    feed_line() and expect_write() (added for CmdResponder's self-test)
    give a second, more targeted scripting style: feed_line() preloads a
    line as if it had already arrived on the wire (a +MTCMD URC waiting
    to be pumped), and expect_write() scripts a one-shot exact-match
    reply to a specific write, which on_write's single global callback
    cannot express when a test needs different replies to different
    writes in the same run."""

    def __init__(self, on_write=None):
        self.rx = b""
        self.tx = b""
        self.writes = []
        self.on_write = on_write
        self._scripted = []

    def read(self, n=1):
        chunk, self.rx = self.rx[:n], self.rx[n:]
        return chunk

    def write(self, data):
        self.tx += data
        self.writes.append(data)
        for i, (expected, reply) in enumerate(self._scripted):
            if expected == data:
                self._scripted.pop(i)
                self.rx += reply
                return len(data)
        if self.on_write:
            self.rx += self.on_write(data)
        return len(data)

    def feed_line(self, line):
        """Preload a line, terminator added, as if already on the wire."""
        self.rx += line.encode("ascii") + b"\r\n"

    def expect_write(self, data, then_lines=()):
        """Script a one-shot reply: the next write() that matches data
        byte-for-byte (caller supplies the full wire form, CRLF
        included) appends then_lines, CRLF-joined, to rx."""
        expected = data.encode("ascii") if isinstance(data, str) else data
        reply = b"".join(l.encode("ascii") + b"\r\n" for l in then_lines)
        self._scripted.append((expected, reply))


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


from mt_regression import (Suite, capture_header, write_baseline, phase0,
                           otctl_run, parse_dataset)


class TestSuite(unittest.TestCase):
    def test_check_scores_and_prints(self):
        s = Suite()
        self.assertTrue(s.check("MTVER? emits +MTVER:", True))
        self.assertFalse(s.check("MTCOMMISSION=901 -> +MTERR:1", False, tag="AT-"))
        self.assertEqual(s.failed, 1)
        self.assertEqual(s.results[0], ("MTVER? emits +MTVER:", True, "AT+"))
        self.assertEqual(s.results[1],
                         ("MTCOMMISSION=901 -> +MTERR:1", False, "AT-"))


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


from mt_regression import add_test, TESTS, main, exit_code
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
        # T3's run_phase2 calls recover_after_abort() on StepAbort, which
        # issues AT+MTRESET: link=None would crash there, so this needs a
        # real (fake) link even though these tests predate gates/requires.
        s = Suite()
        return Phase2Context(link=FakeLink(), chip=None, suite=s,
                             opts=None), s

    def test_abort_skips_the_rest(self):
        calls = []

        def ok_step(ctx):
            calls.append("ok")

        def bad_step(ctx):
            raise StepAbort("device did not come back")

        def never_step(ctx):
            calls.append("never")

        saved = list(PHASE2_STEPS)
        PHASE2_STEPS[:] = [{"name": "one", "fn": ok_step},
                           {"name": "two", "fn": bad_step},
                           {"name": "three", "fn": never_step}]
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
        PHASE2_STEPS[:] = [{"name": "one", "fn": lambda ctx: calls.append(1)},
                          {"name": "two", "fn": lambda ctx: calls.append(2)}]
        try:
            ctx, _ = self._ctx()
            run_phase2(ctx)
        finally:
            PHASE2_STEPS[:] = saved
        self.assertEqual(calls, [1, 2])

    def test_captures_composition_before_the_loop(self):
        """T3 Task 9: run_phase2 must read AT+MTEP? once, before any step
        runs, so cleanup (Task 10) can compare against it later even if
        every step is gated out or the chain aborts on step one."""
        link = FakeLink({"AT+MTEP?": (0, ["+MTEP:0,1,0x0100"])})
        s = Suite()
        ctx = Phase2Context(link=link, chip=None, suite=s, opts=None)
        saved = list(PHASE2_STEPS)
        PHASE2_STEPS[:] = []
        try:
            run_phase2(ctx)
        finally:
            PHASE2_STEPS[:] = saved
        self.assertEqual(ctx.composition, ["+MTEP:0,1,0x0100"])

    def test_unscripted_mtep_query_leaves_composition_empty(self):
        """FakeLink's default reply for a command absent from its dict is
        (0, []) (Task 6 finding): every pre-Task-9 test's FakeLink lacks
        AT+MTEP? entirely, so this is what they all now get, harmlessly."""
        link = FakeLink()
        s = Suite()
        ctx = Phase2Context(link=link, chip=None, suite=s, opts=None)
        saved = list(PHASE2_STEPS)
        PHASE2_STEPS[:] = []
        try:
            run_phase2(ctx)
        finally:
            PHASE2_STEPS[:] = saved
        self.assertEqual(ctx.composition, [])


class TestAddTestDuplicateGuard(unittest.TestCase):
    """A duplicate (phase, name) would silently collapse a baseline
    results key (write_baseline keys by name), so it must raise instead
    of registering quietly."""

    def test_duplicate_name_raises(self):
        saved = list(TESTS)
        try:
            add_test(99, "__dup_test__", lambda link: True)
            with self.assertRaises(ValueError):
                add_test(99, "__dup_test__", lambda link: True)
        finally:
            TESTS[:] = saved


class FlakySerial:
    """serial.Serial double for main(): answers Phase 0's three commands
    from a canned script, then raises OSError on the first read once the
    link has gone quiet, simulating a lost link mid-run (design spec
    item 6.6/§6, TESTING.md §6). The raise is armed only after AT+CGMR's
    reply is written, so Phase 0's own polling never trips it."""

    HEALTHY = {
        b"AT\r\n": b"OK\r\n",
        b"AT+CGMM\r\n": b"ESP32-C6 Hearth\r\nOK\r\n",
        b"AT+CGMR\r\n": b"0.1.0\r\nOK\r\n",
    }

    def __init__(self, port, baudrate, timeout=0.05):
        self.rx = b""
        self.armed = False

    def write(self, data):
        if data == b"AT+CGMR\r\n":
            self.armed = True
        self.rx += self.HEALTHY.get(data, b"")
        return len(data)

    def read(self, n=1):
        if self.rx:
            chunk, self.rx = self.rx[:n], self.rx[n:]
            return chunk
        if self.armed:
            raise OSError("simulated link loss")
        return b""

    def close(self):
        pass


class TestMainSurvivesLostLink(unittest.TestCase):
    """Design spec §6: a serial disconnect mid-run must still print the
    summary. main() only imports pyserial function-locally, so a fake
    'serial' module in sys.modules is enough to drive main() end to end
    without a real port or pyserial installed."""

    def test_lost_link_still_prints_summary(self):
        fake_serial = types.ModuleType("serial")

        class FakeSerialException(Exception):
            pass

        fake_serial.SerialException = FakeSerialException
        fake_serial.Serial = FlakySerial
        had_serial = "serial" in sys.modules
        saved_serial = sys.modules.get("serial")
        sys.modules["serial"] = fake_serial
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = main(["--port", "/dev/fake", "--phase", "1"])
        finally:
            if had_serial:
                sys.modules["serial"] = saved_serial
            else:
                del sys.modules["serial"]
        text = out.getvalue()
        self.assertIn("(link lost:", text)
        self.assertIn("===== RESULT:", text)
        # T2 contract change (design spec section 3): a truncated run must
        # not exit 0, even when nothing that ran failed.
        self.assertEqual(rc, 1)


class TestMainBaselineRefusalGate(unittest.TestCase):
    """Design spec's self-test inventory (T3 final review finding 1a):
    --baseline with --phase 2 must refuse before any serial or
    chip-tool activity runs, when --include-slow and --include-manual
    are not both given, so a partial baseline is never written to disk
    let alone reaching hardware. ap.error() exits 2 via argparse at
    argument-parsing time, well before main() imports pyserial or
    touches ChipTool, so no fake transport is needed here."""

    def test_baseline_without_gates_refuses_before_any_io(self):
        with tempfile.TemporaryDirectory() as d:
            baseline_path = os.path.join(d, "baseline.json")
            with contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    main(["--phase", "2", "--baseline", baseline_path])
            self.assertEqual(cm.exception.code, 2)
            self.assertFalse(os.path.exists(baseline_path))


class TestExitCode(unittest.TestCase):
    """T3 final review finding 1b: the design spec's self-test inventory
    expects main()'s exit expression pinned against a suite of only
    gated entries. exit_code() is the production function main()
    returns through (mt_regression.py), so calling it directly here
    means a regression that folds suite.gated into the expression fails
    this test, rather than only a hand-derived copy of the formula."""

    def test_gated_only_suite_exits_zero(self):
        s = Suite()
        s.check("a", True)
        s.gate_skip("2.10 window expiry", "include_slow")
        s.gate_skip("2.9 cold boot", "include_manual")
        self.assertEqual(exit_code(s, truncated=False), 0)

    def test_failed_check_exits_nonzero(self):
        s = Suite()
        s.check("a", False)
        self.assertEqual(exit_code(s, truncated=False), 1)

    def test_truncation_alone_exits_nonzero(self):
        s = Suite()
        s.check("a", True)
        self.assertEqual(exit_code(s, truncated=True), 1)


from mt_regression import (ChipTool, parse_setup_payload,
                           parse_onoff_reports, parse_onoff_read)

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "fixtures")


def fixture(name):
    with open(os.path.join(FIXTURES, name), "r", errors="replace") as f:
        return f.read()


class FakeChipRunner:
    """Scripted stand-in for ChipTool's subprocess runner. on_call, when
    given, is invoked with each argv before the scripted result is
    returned: T3 step 2.7 uses this to release the commissioning URCs at
    the moment the `pairing` invocation actually happens, rather than
    pre-seeding them at construction (N41 item 2), so a test that deletes
    the pairing call can actually fail the URC assertions instead of
    trivially passing them."""

    def __init__(self, script, on_call=None):
        self.script = list(script)
        self.calls = []
        self.on_call = on_call

    def __call__(self, argv, timeout):
        self.calls.append((argv, timeout))
        if self.on_call is not None:
            self.on_call(argv)
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

    def test_default_runner_timeout_is_scored_not_raised(self):
        """A hung chip-tool must surface as a nonzero rc so the step
        machinery scores it and skip semantics run; TimeoutExpired is a
        SubprocessError, which nothing above the runner catches."""
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool(sys.executable, d)
            rc, out = chip.run(["-c", "import time; time.sleep(10)"],
                               timeout=0.5)
        self.assertNotEqual(rc, 0)
        self.assertIn("chip-tool timed out", out)

    def test_run_payload_family_omits_storage(self):
        # The real binary's payload subcommands take exactly one positional
        # argument and no options: appending --storage-directory makes them
        # fail with "Wrong arguments number" (found on hardware, Task 11).
        runner = FakeChipRunner([(0, "parsed")])
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/chip-tool", d, runner=runner)
            rc, out = chip.run(["payload", "parse-setup-payload",
                                "MT:Y.K9042C00KA0648G00"])
        self.assertEqual((rc, out), (0, "parsed"))
        argv, _ = runner.calls[0]
        self.assertNotIn("--storage-directory", argv)
        self.assertEqual(argv[-1], "MT:Y.K9042C00KA0648G00")

    def test_wipe_storage_removes_files_keeps_dir(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "chip_tool_config.ini"), "w").close()
            chip = ChipTool("/bin/chip-tool", d)
            chip.wipe_storage()
            self.assertTrue(os.path.isdir(d))
            self.assertEqual(os.listdir(d), [])

    def test_default_runner_strips_ansi_sgr_from_real_output(self):
        """Task-7 fix F1: the pinned chip-tool colours every line even
        when stdout is a pipe, not a TTY, so a raw capture line ends
        '...value\\x1b[0m' (task-7-report.md section 5, defect H1: nine
        of the thirteen live-bench failures). The strip belongs in the
        runner every ChipTool.run() call goes through by default, not in
        each parser, so this spawns a REAL subprocess (python, not
        chip-tool -- no bench, no serial port, no chip-tool execution)
        that writes a byte-exact excerpt of chiptool-capture-run5.txt's
        valve CurrentState read (test/fixtures/t5/
        valve-current-state-raw.txt, task-7-evidence, escapes included)
        to stdout, then checks the escapes are gone by the time
        ChipTool.run() returns -- through the production
        _subprocess_runner path, no mock involved. Removing the strip's
        call site fails this test the same way it failed nine checks on
        the bench."""
        raw = fixture(os.path.join("t5", "valve-current-state-raw.txt"))
        self.assertIn("\x1b[", raw)  # sanity: the fixture really is raw
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool(sys.executable, d)
            rc, out = chip.run(["-c",
                                "import sys; sys.stdout.write(%r)" % raw])
        self.assertEqual(rc, 0)
        self.assertNotIn("\x1b[", out)
        self.assertEqual(parse_int_attr(out), 1)


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


from mt_regression import parse_fabric_index


class TestParseFabricIndex(unittest.TestCase):
    def test_finds_index_for_node(self):
        text = fixture("chiptool_read_fabrics.txt")
        idx = parse_fabric_index(text, 0x4846)
        # literal value the fixture contains (Task 1 finding (b): index 2)
        self.assertEqual(idx, 2)

    def test_absent_node_is_none(self):
        text = fixture("chiptool_read_fabrics.txt")
        self.assertIsNone(parse_fabric_index(text, 0xDEAD))


from mt_regression import Subscriber


class TestSubscriber(unittest.TestCase):
    def _chip(self, d):
        return ChipTool("/bin/chip-tool", d)

    def test_argv_shape(self):
        # Interactive mode is not a choice: the real binary's one-shot
        # subscribe exits about 3 s after the priming report (Task 11),
        # so only an interactive session can observe change reports.
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845)
        self.assertEqual(sub.argv,
                         ["/bin/chip-tool", "interactive", "start",
                          "--storage-directory", d])
        self.assertEqual(sub.subscribe_cmd,
                         "onoff subscribe on-off 0 5 0x4845 1")

    def test_parameterized_target_shape(self):
        # Energy round A, task 5: step_3_21's ActivePower subscription.
        # The cluster/attribute verbs are the pinned binary's own
        # (chip-tool electricalpowermeasurement subscribe lists
        # active-power); defaults above stay byte-identical.
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845, endpoint=22,
                             cluster="electricalpowermeasurement",
                             attribute="active-power",
                             parser=parse_active_power_reports)
        self.assertEqual(
            sub.subscribe_cmd,
            "electricalpowermeasurement subscribe active-power "
            "0 5 0x4845 22")

    def test_parameterized_parser_reads_reports(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845, endpoint=22,
                             cluster="electricalpowermeasurement",
                             attribute="active-power",
                             parser=parse_active_power_reports)
            with open(sub.out_path, "w") as f:
                f.write("CHIP:TOO:   ActivePower: 4294967297\n")
            self.assertEqual(sub.reports(), [4294967297])
            base = 1
            self.assertFalse(sub.wait_new_report(base, 0.3))
            with open(sub.out_path, "a") as f:
                f.write("CHIP:TOO:   ActivePower: 99590\n")
            self.assertTrue(sub.wait_new_report(base, 0.5))

    def test_start_sends_subscribe_line(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845)
            sub.argv = [sys.executable, "-u", "-c",
                        "import sys; line = sys.stdin.readline();"
                        "print('GOT ' + line.strip());"
                        "print('CHIP:TOO:   OnOff: TRUE');"
                        "import time; time.sleep(30)"]
            self.assertTrue(sub.start(settle=5.0))
            with open(sub.out_path, errors="replace") as f:
                self.assertIn("GOT " + sub.subscribe_cmd, f.read())
            sub.stop()

    def test_stop_quits_gracefully(self):
        """quit() on stdin must end the session without SIGTERM, so the
        interactive process can flush and release its ini storage."""
        with tempfile.TemporaryDirectory() as d:
            sub = Subscriber(self._chip(d), 0x4845)
            sub.argv = [sys.executable, "-u", "-c",
                        "import sys; print('CHIP:TOO:   OnOff: TRUE');"
                        "[sys.exit(0) for l in sys.stdin"
                        " if l.startswith('quit')]"]
            self.assertTrue(sub.start(settle=5.0))
            sub.stop()
            self.assertEqual(sub._proc.returncode, 0)

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


class TestOtCtl(unittest.TestCase):
    def test_run_argv_and_result(self):
        calls = []
        def runner(argv, timeout):
            calls.append((argv, timeout))
            return 0, "hex\nDone\n"
        rc, out = otctl_run(["dataset", "active", "-x"], "/x/ot-ctl",
                            runner=runner)
        self.assertEqual(rc, 0)
        self.assertEqual(calls[0][0], ["/x/ot-ctl", "dataset", "active", "-x"])

    def test_parse_dataset_fixture(self):
        text = fixture("otctl_dataset_active.txt")
        ds = parse_dataset(text)
        self.assertIsNotNone(ds)
        # Tighten to the literal hex from the fixture. This is a
        # SYNTHETIC dataset (bug B181): it replaced a real bench capture
        # that leaked live Thread NetworkKey/PSKc material into the
        # public repo. NetworkKey and PSKc here are the fixed
        # placeholders 00112233445566778899aabbccddeeff and
        # ffeeddccbbaa99887766554433221100; the parser needs TLV shape,
        # not authenticity.
        self.assertEqual(ds,
                         "0e08000000000001000000030000194a0300000b35060004001fffe0020811223344556677880708fd11223344556677051000112233445566778899aabbccddeeff030c53796e7468657469634e65740102abcd0410ffeeddccbbaa998877665544332211000c0402a0f7f8")

    def test_parse_dataset_garbage_is_none(self):
        self.assertIsNone(parse_dataset("Error 35: InvalidState\nDone\n"))

    def test_run_timeout_label_is_ot_ctl(self):
        # Verify the default runner labels timeouts as "ot-ctl timed out".
        # A real process that sleeps past the timeout, not /bin/true with
        # a microsecond budget: the latter is a race on loaded machines,
        # since /bin/true can exit before the timeout fires at all.
        rc, out = otctl_run(["-c", "import time; time.sleep(10)"],
                            sys.executable, timeout=0.5)
        self.assertNotEqual(rc, 0)
        self.assertIn("ot-ctl timed out", out)


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

    def test_drain_clears_stale_ready_that_would_satisfy_await(self):
        """A gutted drain() must fail this: the stale +MTREADY would
        satisfy the await that follows it."""
        link = FakeLink(commands={"AT+MTRESET": (0, [])},
                        stale_urcs=["+MTREADY"])
        link.drain()
        self.assertIsNone(link.await_urc(r"\+MTREADY$", timeout=0.05))

    def test_parse_setup_payload_short_discriminator_fallback(self):
        text = "Passcode: 20202021\nDiscriminator value: 3840\n"
        self.assertEqual(parse_setup_payload(text), (20202021, 3840))


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


from mt_regression import phase2_gate, GATE_REFERENCE_QR


class TestPhase2Gate(unittest.TestCase):
    def _args(self, **kw):
        base = {"ssid": "net", "psk": "secret"}
        base.update(kw)
        return types.SimpleNamespace(**base)

    @staticmethod
    def _wifi_link():
        return FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:WIFI,0,0,0"])})

    def test_missing_creds_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(ssid=None), self._wifi_link())
        self.assertIn("MT_SSID", problem)
        self.assertEqual(transport, "WIFI")
        self.assertIsNone(dataset)

    def test_missing_binary_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool(os.path.join(d, "nope"), d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(), self._wifi_link())
        self.assertIn("chip-tool", problem)
        self.assertEqual(transport, "WIFI")

    def test_unparseable_reference_payload_abort(self):
        runner = FakeChipRunner([(0, "garbage")])
        with tempfile.TemporaryDirectory() as d:
            binary = os.path.join(d, "chip-tool")
            open(binary, "w").close()
            os.chmod(binary, 0o755)
            chip = ChipTool(binary, d, runner=runner)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(), self._wifi_link())
        self.assertIn("parse", problem)
        self.assertEqual(transport, "WIFI")

    def test_healthy_gate_passes(self):
        runner = FakeChipRunner(
            [(0, fixture("chiptool_parse_setup_payload.txt"))])
        with tempfile.TemporaryDirectory() as d:
            binary = os.path.join(d, "chip-tool")
            open(binary, "w").close()
            os.chmod(binary, 0o755)
            chip = ChipTool(binary, d, runner=runner)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(), self._wifi_link())
        self.assertIsNone(problem)
        self.assertEqual(transport, "WIFI")
        self.assertIsNone(dataset)
        self.assertEqual(runner.calls[0][0][1:3],
                         ["payload", "parse-setup-payload"])

    def test_missing_openocd_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            with mock.patch("mt_regression.shutil.which",
                            return_value=None):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(), self._wifi_link())
        self.assertIn("openocd", problem)
        self.assertIsNone(transport)
        self.assertIsNone(dataset)


class TestPhase2GateTransport(unittest.TestCase):
    """T4 task 3: the gate detects WiFi vs Thread from the device (or
    takes --transport as an override) and branches: WiFi needs
    credentials, Thread needs a live otbr-agent and a dataset."""

    def _args(self, **kw):
        base = {"ssid": "net", "psk": "secret", "transport": None,
                "dataset": None, "ot_ctl": "/fake/ot-ctl"}
        base.update(kw)
        return types.SimpleNamespace(**base)

    @staticmethod
    def _healthy_chip(d):
        runner = FakeChipRunner(
            [(0, fixture("chiptool_parse_setup_payload.txt"))])
        binary = os.path.join(d, "chip-tool")
        open(binary, "w").close()
        os.chmod(binary, 0o755)
        return ChipTool(binary, d, runner=runner)

    def test_wifi_detected_requires_credentials(self):
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:WIFI,0,0,0"])})
        with tempfile.TemporaryDirectory() as d:
            chip = self._healthy_chip(d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(ssid=None, psk=None), link)
        self.assertIn("MT_SSID", problem)
        self.assertEqual(transport, "WIFI")
        self.assertIsNone(dataset)

    def test_thread_detected_skips_credentials_requires_otbr(self):
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:THREAD,0,0,0"])})
        ds_text = fixture("otctl_dataset_active.txt")

        def fake_otctl(cmd_args, binary):
            self.assertEqual(binary, "/fake/ot-ctl")
            if cmd_args == ["state"]:
                return 0, "leader\r\nDone\r\n"
            if cmd_args == ["dataset", "active", "-x"]:
                return 0, ds_text
            raise AssertionError("unexpected ot-ctl call: %r" % (cmd_args,))

        with tempfile.TemporaryDirectory() as d:
            chip = self._healthy_chip(d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(ssid=None, psk=None), link,
                    otctl=fake_otctl)
        self.assertIsNone(problem)
        self.assertEqual(transport, "THREAD")
        self.assertEqual(dataset, parse_dataset(ds_text))

    def test_thread_dead_otbr_aborts(self):
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:THREAD,0,0,0"])})

        def fake_otctl(cmd_args, binary):
            return 1, "Error: no such device\n"

        with tempfile.TemporaryDirectory() as d:
            chip = self._healthy_chip(d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(), link, otctl=fake_otctl)
        self.assertIsNotNone(problem)
        self.assertIn("otbr-agent", problem)
        self.assertEqual(transport, "THREAD")
        self.assertIsNone(dataset)

    def test_thread_bad_role_aborts(self):
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:THREAD,0,0,0"])})

        def fake_otctl(cmd_args, binary):
            if cmd_args == ["state"]:
                return 0, "disabled\r\nDone\r\n"
            raise AssertionError("dataset should not be fetched")

        with tempfile.TemporaryDirectory() as d:
            chip = self._healthy_chip(d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(), link, otctl=fake_otctl)
        self.assertIsNotNone(problem)
        self.assertIn("down", problem)
        self.assertEqual(transport, "THREAD")
        self.assertIsNone(dataset)

    def test_thread_empty_role_aborts_without_traceback(self):
        """rc 0 with empty stdout (a live otbr-agent that answered nothing
        useful) must reach the named 'Thread network is down' abort, not
        raise IndexError out of the splitlines()[0] parse."""
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:THREAD,0,0,0"])})

        def fake_otctl(cmd_args, binary):
            if cmd_args == ["state"]:
                return 0, ""
            raise AssertionError("dataset should not be fetched")

        with tempfile.TemporaryDirectory() as d:
            chip = self._healthy_chip(d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(), link, otctl=fake_otctl)
        self.assertIsNotNone(problem)
        self.assertIn("down", problem)
        self.assertEqual(transport, "THREAD")
        self.assertIsNone(dataset)

    def test_dataset_override_skips_otctl_fetch(self):
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:THREAD,0,0,0"])})

        def fake_otctl(cmd_args, binary):
            if cmd_args == ["state"]:
                return 0, "leader\r\nDone\r\n"
            raise AssertionError("dataset fetch should be skipped: %r"
                                % (cmd_args,))

        with tempfile.TemporaryDirectory() as d:
            chip = self._healthy_chip(d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(dataset="deadbeef"), link,
                    otctl=fake_otctl)
        self.assertIsNone(problem)
        self.assertEqual(transport, "THREAD")
        self.assertEqual(dataset, "deadbeef")

    def test_transport_override_beats_detection(self):
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:WIFI,0,0,0"])})
        ds_text = fixture("otctl_dataset_active.txt")

        def fake_otctl(cmd_args, binary):
            if cmd_args == ["state"]:
                return 0, "leader\r\nDone\r\n"
            if cmd_args == ["dataset", "active", "-x"]:
                return 0, ds_text
            raise AssertionError("unexpected ot-ctl call: %r" % (cmd_args,))

        with tempfile.TemporaryDirectory() as d:
            chip = self._healthy_chip(d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase2_gate(
                    chip, self._args(transport="THREAD"), link,
                    otctl=fake_otctl)
        self.assertIsNone(problem)
        self.assertEqual(transport, "THREAD")
        self.assertEqual(dataset, parse_dataset(ds_text))
        self.assertNotIn("AT+MTNET?", link.sent)


from mt_regression import phase3_gate, _transport_gate


class TestPhase3Gate(unittest.TestCase):
    """T5 task 4: phase3_gate must refuse in exactly the same shapes
    phase2_gate does, since both are now thin wrappers over the same
    _transport_gate body. Mirrors TestPhase2Gate's cases one for one
    rather than re-deriving them, so a divergence in the extraction
    shows up as a phase3_gate test failing while phase2_gate's own
    (unmodified) tests keep passing."""

    def _args(self, **kw):
        base = {"ssid": "net", "psk": "secret"}
        base.update(kw)
        return types.SimpleNamespace(**base)

    @staticmethod
    def _wifi_link():
        return FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:WIFI,0,0,0"])})

    def test_missing_creds_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase3_gate(
                    chip, self._args(ssid=None), self._wifi_link())
        self.assertIn("MT_SSID", problem)
        self.assertEqual(transport, "WIFI")
        self.assertIsNone(dataset)

    def test_missing_binary_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool(os.path.join(d, "nope"), d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase3_gate(
                    chip, self._args(), self._wifi_link())
        self.assertIn("chip-tool", problem)
        self.assertEqual(transport, "WIFI")

    def test_missing_openocd_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            with mock.patch("mt_regression.shutil.which", return_value=None):
                problem, transport, dataset = phase3_gate(
                    chip, self._args(), self._wifi_link())
        self.assertIn("openocd", problem)
        self.assertIsNone(transport)
        self.assertIsNone(dataset)

    def test_healthy_gate_passes(self):
        runner = FakeChipRunner(
            [(0, fixture("chiptool_parse_setup_payload.txt"))])
        with tempfile.TemporaryDirectory() as d:
            binary = os.path.join(d, "chip-tool")
            open(binary, "w").close()
            os.chmod(binary, 0o755)
            chip = ChipTool(binary, d, runner=runner)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase3_gate(
                    chip, self._args(), self._wifi_link())
        self.assertIsNone(problem)
        self.assertEqual(transport, "WIFI")
        self.assertIsNone(dataset)

    def test_thread_detected_skips_credentials_requires_otbr(self):
        link = FakeLink(commands={"AT+MTNET?": (0, ["+MTNET:THREAD,0,0,0"])})
        ds_text = fixture("otctl_dataset_active.txt")

        def fake_otctl(cmd_args, binary):
            if cmd_args == ["state"]:
                return 0, "leader\r\nDone\r\n"
            if cmd_args == ["dataset", "active", "-x"]:
                return 0, ds_text
            raise AssertionError("unexpected ot-ctl call: %r" % (cmd_args,))

        with tempfile.TemporaryDirectory() as d:
            runner = FakeChipRunner(
                [(0, fixture("chiptool_parse_setup_payload.txt"))])
            binary = os.path.join(d, "chip-tool")
            open(binary, "w").close()
            os.chmod(binary, 0o755)
            chip = ChipTool(binary, d, runner=runner)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem, transport, dataset = phase3_gate(
                    chip, self._args(ssid=None, psk=None,
                                     transport=None, dataset=None,
                                     ot_ctl="/fake/ot-ctl"),
                    link, otctl=fake_otctl)
        self.assertIsNone(problem)
        self.assertEqual(transport, "THREAD")
        self.assertEqual(dataset, parse_dataset(ds_text))

    def test_delegates_to_the_same_transport_gate_body(self):
        """The extraction's own guard: both wrappers must produce the
        identical result for the identical input, since they are meant
        to be indistinguishable to a caller."""
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                r2 = phase2_gate(chip, self._args(ssid=None),
                                 self._wifi_link())
                r3 = phase3_gate(chip, self._args(ssid=None),
                                 self._wifi_link())
        self.assertEqual(r2, r3)


from mt_regression import swd_reset, operator_power_cycle


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


from mt_regression import make_relink, flush_print


class TestFlushPrint(unittest.TestCase):
    def test_default_prompt_printer_flushes(self):
        """The operator prompt must reach a tee'd log immediately:
        python stdout is block-buffered when piped, and an unflushed
        prompt cost a whole hardware run (the operator was never told
        to unplug)."""
        class Recorder:
            def __init__(self):
                self.writes = []
                self.flushes = 0

            def write(self, s):
                self.writes.append(s)

            def flush(self):
                self.flushes += 1

        rec = Recorder()
        with contextlib.redirect_stdout(rec):
            flush_print("unplug now")
        self.assertIn("unplug now", "".join(rec.writes))
        self.assertGreater(rec.flushes, 0)

    def test_operator_power_cycle_default_printer_is_flush_print(self):
        defaults = operator_power_cycle.__defaults__
        self.assertIs(defaults[0], flush_print)

    def test_operator_power_cycle_default_window_covers_relay(self):
        """60 s was eaten by the prompt relay chain on a real run; the
        window must budget for a notification round-trip plus a human."""
        self.assertGreaterEqual(operator_power_cycle.__defaults__[3], 180.0)


class TestCaptureHeaderClosedPort(unittest.TestCase):
    def test_capture_header_tolerates_dead_link(self):
        """After an abort whose relink left the port closed, main still
        calls capture_header; it must degrade to an incomplete header,
        not a misleading '(link lost)' tail."""
        class DeadLink:
            def command(self, *a, **k):
                raise OSError("port closed")

        header = {"port": "/dev/fake"}
        capture_header(DeadLink(), header)
        self.assertEqual(header, {"port": "/dev/fake"})


class TestMakeRelink(unittest.TestCase):
    """Direct coverage for the reopen path, added after the first T3
    hardware run: the USB CDC enumeration bounced after the SWD reset,
    the first open succeeded, and the next read died mid-step as a
    bogus link loss. The relink must catch the bounce itself."""

    class _QuietPort:
        """Open port that stays silent: the pump window passes clean."""
        def __init__(self, rx=b""):
            self.rx = rx
            self.closed = False

        def read(self, n=1):
            chunk, self.rx = self.rx[:n], self.rx[n:]
            return chunk

        def write(self, data):
            return len(data)

        def close(self):
            self.closed = True

    def _mod(self, opens):
        """Fake serial module: opens is a list whose items are either a
        transport (returned) or an Exception instance (raised)."""
        seq = list(opens)

        def serial_ctor(path, baud, timeout):
            item = seq.pop(0)
            if isinstance(item, Exception):
                raise item
            return item
        return types.SimpleNamespace(Serial=serial_ctor,
                                     SerialException=OSError)

    def _relink(self, link, mod, path_exists=lambda p: True):
        return make_relink(link, "/dev/fake", settle=0.0, pump=0.05,
                           deadline_s=1.0, path_exists=path_exists,
                           sleep=lambda s: None, serial_mod=mod)

    def test_action_failure_returns_without_opening(self):
        link, _ = link_with_reply(b"")
        relink = self._relink(link, self._mod([]))  # ctor would IndexError
        ok, detail = relink(lambda: (False, "boom"))
        self.assertEqual((ok, detail), (False, "boom"))

    def test_bounced_open_is_retried(self):
        link, _ = link_with_reply(b"")
        good = self._QuietPort()
        mod = self._mod([OSError("enumeration bounce"), good])
        ok, _ = self._relink(link, mod)(lambda: (True, ""))
        self.assertTrue(ok)
        self.assertIs(link.t, good)

    def test_dead_first_read_is_caught_and_reopened(self):
        """The Task 11 failure shape: open succeeds, first read dies.
        The pump must catch it inside relink, close the corpse, and
        retry, instead of letting a step's await crash the run."""
        link, _ = link_with_reply(b"")
        dead = self._QuietPort()
        dead.read = lambda n=1: (_ for _ in ()).throw(
            OSError("returned no data"))
        good = self._QuietPort()
        mod = self._mod([dead, good])
        ok, _ = self._relink(link, mod)(lambda: (True, ""))
        self.assertTrue(ok)
        self.assertTrue(dead.closed)
        self.assertIs(link.t, good)

    def test_early_urc_during_pump_is_queued_not_lost(self):
        link, _ = link_with_reply(b"")
        port = self._QuietPort(rx=b"+MTREADY\r\n")
        ok, _ = self._relink(link, self._mod([port]))(lambda: (True, ""))
        self.assertTrue(ok)
        self.assertEqual(link.await_urc(r"\+MTREADY$", timeout=0.05),
                         "+MTREADY")

    def test_path_never_back_reports_failure(self):
        link, _ = link_with_reply(b"")
        relink = self._relink(link, self._mod([]),
                              path_exists=lambda p: False)
        ok, detail = relink(lambda: (True, ""))
        self.assertFalse(ok)
        self.assertIn("did not come back", detail)


class TestOperatorPowerCycle(unittest.TestCase):
    def _run(self, presence, clock_step=0.3, unplug_timeout=1.0):
        """presence: list of booleans path_exists returns in order.
        clock is a counter-based fake (ticks by clock_step per call)
        instead of time.monotonic, so the unplug_timeout deadline is
        crossed after a handful of calls rather than a real wall-clock
        second: the sleep=lambda is a no-op, so a real clock would spin
        the CPU for the full timeout instead of returning instantly."""
        seq = iter(presence)
        counter = {"t": 0.0}
        def clock():
            counter["t"] += clock_step
            return counter["t"]
        return operator_power_cycle(
            "/dev/fake", printer=lambda *a: None,
            path_exists=lambda p: next(seq, presence[-1]),
            sleep=lambda s: None, unplug_timeout=unplug_timeout,
            clock=clock)

    def test_observed_cycle_passes(self):
        ok, _ = self._run([True, True, False])
        self.assertTrue(ok)

    def test_never_unplugged_fails(self):
        ok, detail = self._run([True] * 10000)
        self.assertFalse(ok)
        self.assertIn("not observed", detail)


from mt_regression import step_2_1_factory_fresh, step_2_2_codes_stable


class FakeLink:
    """ATLink double for Phase 2 step tests: scripted command replies
    (dict cmd -> (res, lines), or a list of those for sequential calls)
    and a scripted URC stream whose timestamps encode wire order. stale_urcs
    are seeded at construction and drain() clears them, letting fresh URCs
    (from urcs) arrive after AT+MTRESET. Pass no_reset=True to seed fresh
    URCs immediately (for tests that skip AT+MTRESET); otherwise URCs arrive
    only after drain() or AT+MTRESET (preserves test fidelity).

    urcs_on_command (dict cmd -> [urc, ...]) releases URCs the first time
    that exact command string is sent through command(), independent of
    the AT+MTRESET mechanics above: T3 step 2.7 uses this to tie
    +MTEVT:0 to AT+MTCOMMISSION=180, the way the real device's window-open
    URC actually arrives. push_urcs() is the same release mechanism for a
    trigger that isn't an AT command at all (step 2.7's pairing/1/3/4 URCs,
    released from a FakeChipRunner on_call hook when the chip-tool
    `pairing` invocation happens)."""

    def __init__(self, commands=None, urcs=None, stale_urcs=None, no_reset=False,
                 urcs_after_drain=None, urcs_on_command=None):
        self.commands = dict(commands or {})
        self.urcs = list(urcs or [])
        self.stale_urcs = list(stale_urcs or [])
        self.urcs_after_drain = list(urcs_after_drain or [])
        self.urcs_on_command = dict(urcs_on_command or {})
        self._released_on_command = set()
        self.no_reset = no_reset
        # Seed queue with stale URCs at low timestamps (0, 1, ...)
        self.urc_queue = [(float(i), u) for i, u in enumerate(self.stale_urcs)]
        self.urc_history = [(float(i), u) for i, u in enumerate(self.stale_urcs)]
        # If stale URCs exist, drain() must be called before fresh URCs are added
        self.needs_drain = bool(self.stale_urcs)
        # Track whether fresh URCs have been added to avoid duplicates
        self.fresh_urcs_added = False
        # Track whether urcs_after_drain have been released
        self.urcs_after_drain_released = False
        # Only seed fresh URCs immediately when no_reset=True (explicit opt-in)
        if no_reset and not self.stale_urcs and self.urcs:
            start_ts = max((ts for ts, _ in self.urc_queue), default=-1.0) + 1.0
            fresh_entries = [(start_ts + float(i), u)
                             for i, u in enumerate(self.urcs)]
            self.urc_queue.extend(fresh_entries)
            self.urc_history.extend(fresh_entries)
            self.fresh_urcs_added = True
        self.sent = []

    def command(self, cmd, expect=None, timeout=None):
        self.sent.append(cmd)
        v = self.commands.get(cmd, (0, []))
        if isinstance(v, list):
            v = v.pop(0) if v else (-2, [])
        # Append fresh URCs after AT+MTRESET (only if stale URCs were drained and fresh not already added)
        if cmd == "AT+MTRESET" and not self.needs_drain and not self.fresh_urcs_added:
            # Add fresh URCs with timestamps starting after current max
            start_ts = (max((ts for ts, _ in self.urc_queue), default=-1.0)
                       + 1.0)
            fresh_entries = [(start_ts + float(i), u)
                             for i, u in enumerate(self.urcs)]
            self.urc_queue.extend(fresh_entries)
            self.urc_history.extend(fresh_entries)
            self.fresh_urcs_added = True
        if (cmd in self.urcs_on_command
                and cmd not in self._released_on_command):
            self._released_on_command.add(cmd)
            self.push_urcs(self.urcs_on_command[cmd])
        return v

    def push_urcs(self, urcs):
        """Directly enqueue URCs after whatever is already queued, with
        timestamps that preserve arrival order. Honest release mechanism
        for triggers that never go through command() at all (e.g. a
        chip-tool invocation observed via FakeChipRunner.on_call)."""
        start_ts = (max((ts for ts, _ in self.urc_queue), default=-1.0)
                   + 1.0)
        entries = [(start_ts + float(i), u) for i, u in enumerate(urcs)]
        self.urc_queue.extend(entries)
        self.urc_history.extend(entries)

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
        # Always clear the queue: honest drain() semantics
        dropped = [u for _, u in self.urc_queue]
        self.urc_queue.clear()
        self.needs_drain = False  # Drain satisfies the requirement
        self.fresh_urcs_added = False  # Reset so AT+MTRESET can add them again
        # Release urcs_after_drain on the first drain() call
        if self.urcs_after_drain and not self.urcs_after_drain_released:
            start_ts = max((ts for ts, _ in self.urc_queue), default=-1.0) + 1.0
            after_drain_entries = [(start_ts + float(i), u)
                                   for i, u in enumerate(self.urcs_after_drain)]
            self.urc_queue.extend(after_drain_entries)
            self.urc_history.extend(after_drain_entries)
            self.urcs_after_drain_released = True
        return dropped


CODES = ("+MTCODES:MT:Y.K9042C00KA0648G00,34970112332", )


def fresh_ctx(link=None, chip=None, storage=None):
    if link is None:
        link = FakeLink()
    opts = types.SimpleNamespace(node_id=0x4845, ssid="net", psk="secret",
                                 storage=storage, include_slow=False,
                                 include_manual=False, keyword=None)
    return Phase2Context(link=link, chip=chip, suite=Suite(), opts=opts)


@contextlib.contextmanager
def patched_steps(steps):
    """Swap PHASE2_STEPS for the duration of the block, restoring after:
    the T3 gate/-k/requires tests exercise run_phase2() against a small
    scripted table instead of the real six-step Phase 2 chain."""
    saved = list(PHASE2_STEPS)
    PHASE2_STEPS[:] = steps
    try:
        yield
    finally:
        PHASE2_STEPS[:] = saved


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

    def test_drain_clears_stale_urcs(self):
        """Verify drain() correctly clears stale URCs that arrived before
        the reset, preventing them from satisfying post-reset awaits. This
        test seeds stale URCs (wrong event codes) and fresh URCs; drain()
        must remove stale ones so only fresh ones are found. Without drain(),
        the step fails to find the expected +MTREADY."""
        stale_urcs = ["+MTEVT:99", "+MTEVT:88"]  # wrong codes, from pre-reset
        fresh_urcs = ["+MTREADY", "+MTEVT:0"]  # correct codes, post-reset
        link = FakeLink(self.HAPPY, urcs=fresh_urcs, stale_urcs=stale_urcs)
        ctx = fresh_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_1_factory_fresh(ctx)
        # All checks pass: drain() cleared stale URCs, and the step found
        # the fresh +MTREADY and +MTEVT:0 that arrived after AT+MTRESET
        self.assertEqual(ctx.suite.failed, 0)
        self.assertEqual(ctx.qr, "MT:Y.K9042C00KA0648G00")
        self.assertEqual(ctx.manual, "34970112332")


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


from mt_regression import (step_2_3_commission, step_2_4_host_to_controller,
                           step_2_5_controller_to_host, pairing_argv)


class TestPairingArgv(unittest.TestCase):
    """T4: the pairing verb and its argv differ by transport; everything
    else in 2.3/2.11 is untouched. WIFI keeps the pre-T4 shape, THREAD's
    was pinned by the T4 Task 1 preflight (task-1-findings.md, finding
    (c))."""

    def _ctx(self, transport, dataset=None):
        ctx = fresh_ctx()
        ctx.transport = transport
        ctx.dataset = dataset
        ctx.passcode = 20202021
        ctx.discriminator = 3840
        return ctx

    def test_wifi_argv(self):
        argv = pairing_argv(self._ctx("WIFI"))
        self.assertEqual(argv[:2], ["pairing", "ble-wifi"])
        self.assertIn("0x4845", argv)

    def test_thread_argv(self):
        argv = pairing_argv(self._ctx("THREAD", dataset="0e08abc123"))
        self.assertEqual(argv[:2], ["pairing", "ble-thread"])
        self.assertIn("hex:0e08abc123", argv)
        self.assertNotIn(None, argv)


class TestStep23(unittest.TestCase):
    """T3 retrofit (N41 item 2): the commissioning URCs release via a
    FakeChipRunner on_call hook firing on the actual `pairing` invocation
    (TestStep27's pattern), not pre-seeded at construction, so a test that
    deletes the pairing call cannot pass the URC assertions by accident."""

    AT_OK = {
        "AT+MTFABRICS?": (0, ["+MTFABRICS:1"]),
        "AT+MTSTATE?": (0, ["+MTSTATE:2,1"]),
    }

    @staticmethod
    def _release_on_pairing(link, urcs):
        def on_call(argv):
            if "pairing" in argv:
                link.push_urcs(urcs)
        return on_call

    def _ctx(self, runner, urcs):
        link = FakeLink(dict(self.AT_OK))
        d = tempfile.mkdtemp()  # outlives the step; ChipTool.run mkdirs it
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        runner.on_call = self._release_on_pairing(link, urcs)
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

    def test_thread_transport_uses_ble_thread(self):
        """T4: ctx.transport = THREAD routes 2.3's pairing call through
        pairing_argv's ble-thread branch instead of ble-wifi, with no
        WiFi credentials in the argv."""
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (0, "CHIP:TOO: Device commissioning completed with success"),
        ])
        ctx, runner = self._ctx(runner,
                                ["+MTEVT:1", "+MTEVT:3", "+MTEVT:4"])
        ctx.transport = "THREAD"
        ctx.dataset = "0e08abc123"
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_3_commission(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        pairing_argv_used = runner.calls[1][0]
        self.assertIn("ble-thread", pairing_argv_used)
        self.assertIn("hex:0e08abc123", pairing_argv_used)
        self.assertNotIn(ctx.opts.ssid, pairing_argv_used)
        self.assertNotIn(ctx.opts.psk, pairing_argv_used)


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
        }, urcs_after_drain=urcs)
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


from mt_regression import step_2_7_second_fabric


class TestStep27(unittest.TestCase):
    """T3 2.7. Deliberate upgrade over TestStep23's pattern (N41 item 2):
    the pairing/1/3/4 URCs are released by a FakeChipRunner on_call hook
    firing on the actual `pairing` invocation, not pre-seeded at
    construction, so a step that skips the call cannot pass the URC
    checks anyway. See task-5-report.md for the deletion probe that
    proves this."""

    @staticmethod
    def _commands():
        # A fresh dict and fresh inner lists per call: FakeLink.command()
        # pops list-valued entries in place, so a dict literal reused
        # across test methods (a class attribute, shared for the whole
        # process) would let one test exhaust another's scripted replies.
        return {
            "AT+MTCOMMISSION=180": (0, []),
            "AT+MTSTATE?": [(0, ["+MTSTATE:1,1"]), (0, ["+MTSTATE:2,2"])],
            "AT+MTFABRICS?": [(0, ["+MTFABRICS:2"]), (0, ["+MTFABRICS:1"])],
        }

    def _ctx(self, runner):
        link = FakeLink(
            self._commands(), no_reset=True,
            urcs_on_command={"AT+MTCOMMISSION=180": ["+MTEVT:0"]})
        d = tempfile.mkdtemp()  # outlives the step; ChipTool.run mkdirs it
        ctx = fresh_ctx(link)
        ctx.chip2 = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx.passcode, ctx.discriminator = 20202021, 3840
        return ctx, link

    @staticmethod
    def _release_on_pairing(link):
        def on_call(argv):
            if "pairing" in argv:
                link.push_urcs(["+MTEVT:1", "+MTEVT:3", "+MTEVT:4"])
        return on_call

    def test_happy_path_counts(self):
        runner = FakeChipRunner([
            (0, "CHIP:TOO: Device commissioning completed with success"),
            (0, fixture("chiptool_read_fabrics.txt")),
            (0, "CHIP:TOO: NOCResponse"),
        ])
        ctx, link = self._ctx(runner)
        runner.on_call = self._release_on_pairing(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_7_second_fabric(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        pairing_argv = runner.calls[0][0]
        self.assertEqual(pairing_argv[1:3], ["pairing", "onnetwork-long"])
        self.assertIn("0x4846", pairing_argv)
        self.assertIn("20202021", pairing_argv)
        self.assertIn("3840", pairing_argv)
        remove_argv = runner.calls[2][0]
        self.assertEqual(remove_argv[1:3],
                         ["operationalcredentials", "remove-fabric"])
        self.assertIn("2", remove_argv)

    def test_pairing_failure_aborts(self):
        # No on_call release wired: a failed pairing produces no
        # commissioning URCs on real hardware either.
        runner = FakeChipRunner([(1, "CHIP:TOO: Run command failure")])
        ctx, _ = self._ctx(runner)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_2_7_second_fabric(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_missing_index_scores_fail_but_continues(self):
        runner = FakeChipRunner([
            (0, "CHIP:TOO: Device commissioning completed with success"),
            (0, "no matching fabric entries in this capture"),
        ])
        ctx, link = self._ctx(runner)
        runner.on_call = self._release_on_pairing(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_7_second_fabric(ctx)   # must not raise
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.7 fabric index found", failed_names)
        # no index means remove-fabric must never be attempted
        self.assertEqual(len(runner.calls), 2)


from mt_regression import step_2_8_warm_reboot


class TestStep28(unittest.TestCase):
    """T3 2.8: warm reboot over the SWD path (Task 1 finding (a), pinned
    on the bench: the by-id device path disappears and reappears across
    the reset, and +MTREADY was observed 1.84 s after reopen, n=1). The
    fake relink models exactly that: it stands in for ctx.relink, is
    called with the swd_reset action, and pushes +MTREADY into the link
    via FakeLink.push_urcs the way a real reboot's URC would arrive after
    the port comes back."""

    @staticmethod
    def _commands():
        # A fresh dict and fresh inner lists per call: FakeLink.command()
        # pops list-valued entries in place, so a dict literal reused
        # across test methods (a class attribute, shared for the whole
        # process) would let one test exhaust another's scripted replies.
        return {
            "AT+MTATTR=1,6,0,1": (0, []),
            # Pre-reboot read echoes the write; the post-reboot read
            # is 1 again: OnOff survives since the B63 fix (4410498),
            # and this step is its warm-reboot regression guard.
            "AT+MTATTR=1,6,0": [(0, ["+MTATTR:1,6,0,1"]),
                                (0, ["+MTATTR:1,6,0,1"])],
            "AT+MTFABRICS?": (0, ["+MTFABRICS:1"]),
            "AT+MTSTATE?": (0, ["+MTSTATE:2,1"]),
        }

    def test_happy_path(self):
        link = FakeLink(self._commands())
        relink_called = []
        swd_calls = []

        def fake_swd_runner(argv, **kw):
            swd_calls.append(argv)
            return types.SimpleNamespace(returncode=0, stdout="", stderr="")

        def relink(action):
            # T3 final review finding 4: must invoke the injected action
            # (like TestStep29's fake relink does), or step_2_8's real
            # swd_reset() call is never reached and this test cannot
            # notice it going missing.
            ok, detail = action()
            relink_called.append((ok, detail))
            if ok:
                link.push_urcs(["+MTREADY"])
            return ok, detail

        ctx = fresh_ctx(link)
        ctx.relink = relink
        ctx.swd_runner = fake_swd_runner
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_8_warm_reboot(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        self.assertTrue(relink_called)
        self.assertTrue(relink_called[0][0])
        self.assertTrue(swd_calls,
                        "step must reach swd_reset via ctx.swd_runner")

    def test_relink_failure_aborts(self):
        link = FakeLink(self._commands())

        def relink(action):
            return False, "port did not come back"

        ctx = fresh_ctx(link)
        ctx.relink = relink
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_2_8_warm_reboot(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_boot_window_urc_fails_the_guard(self):
        link = FakeLink(self._commands())

        def relink(action):
            link.push_urcs(["+MTREADY", "+MTEVT:0"])
            return True, ""

        ctx = fresh_ctx(link)
        ctx.relink = relink
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_8_warm_reboot(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.8 no boot window on commissioned device",
                      failed_names)


from mt_regression import step_2_9_cold_boot


class TestStep29(unittest.TestCase):
    """T3 2.9, the B4.3 boot-loop regression (design spec section 12.1):
    only a cold boot reaches the URC path that fires during
    esp_matter::start(), before the AT UART exists. While B63 stands
    nothing persists across any reboot, warm (2.8) or cold, so the
    StartUpOnOff-at-init path this step targets cannot currently arm;
    its present regression value is the commissioned device coming
    cleanly through a cold boot to +MTREADY (see B63, TESTING.md's 2.9
    caveat). ctx.relink here wraps ctx.power_cycler (not swd_reset,
    unlike 2.8), so the fake relink calls the injected action and
    propagates its (ok, detail) the way make_relink's real relink does,
    instead of assuming success outright: that is what lets the
    cycle-not-observed case reach StepAbort through the same code path
    as a real refused power cycle."""

    @staticmethod
    def _commands(post_boot_attr=0):
        # A fresh dict and fresh inner lists per call: FakeLink.command()
        # pops list-valued entries in place, so a dict literal reused
        # across test methods (a class attribute, shared for the whole
        # process) would let one test exhaust another's scripted replies
        # (the TestStep27 hazard TestStep28 already avoids).
        return {
            "AT+MTATTR=1,6,0,1": (0, []),
            "AT+MTATTR=1,6,0": [(0, ["+MTATTR:1,6,0,1"]),
                                (0, ["+MTATTR:1,6,0,%d" % post_boot_attr])],
            "AT+MTFABRICS?": (0, ["+MTFABRICS:1"]),
        }

    @staticmethod
    def _relink(link):
        def relink(action):
            ok, detail = action()
            if ok:
                link.push_urcs(["+MTREADY"])
            return ok, detail
        return relink

    def test_happy_path(self):
        link = FakeLink(self._commands(post_boot_attr=1))
        ctx = fresh_ctx(link)
        ctx.relink = self._relink(link)
        cycler_calls = []

        def power_cycler():
            cycler_calls.append(True)
            return True, ""

        ctx.power_cycler = power_cycler
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_9_cold_boot(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        self.assertTrue(cycler_calls)

    def test_value_lost_fails_the_guard(self):
        """A post-boot read of 0 is a B63 regression and must fail."""
        link = FakeLink(self._commands(post_boot_attr=0))
        ctx = fresh_ctx(link)
        ctx.relink = self._relink(link)
        ctx.power_cycler = lambda: (True, "")
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_9_cold_boot(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.9 value survived cold boot (B63 guard)",
                      failed_names)

    def test_cycle_not_observed_aborts(self):
        link = FakeLink(self._commands())
        ctx = fresh_ctx(link)
        ctx.relink = self._relink(link)
        ctx.power_cycler = lambda: (
            False, "power cycle not observed (device never vanished)")
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_2_9_cold_boot(ctx)
        self.assertGreater(ctx.suite.failed, 0)


from mt_regression import step_2_10_window_expiry, WINDOW_EXPIRY_RAISES_EVT5


class TestStep210(unittest.TestCase):
    """T3 2.10: window expiry, asserting the DE24 baseline the T3 Task 1
    bench findings pinned (task-1-findings.md, finding (c)): a window
    nobody attaches to closes with exactly one +MTEVT:4 and no +MTEVT:5,
    ~180 s after the OK, and AT+MTSTATE? reads 2,1 afterward. ctx.opts
    .expiry_wait stands in for the real ~200 s wait: FakeLink's awaits are
    synchronous queue scans, so with the URCs pre-released via
    urcs_on_command/push_urcs the waits return instantly regardless of the
    value passed, and the override just documents that the step never
    sleeps in this self-test."""

    @staticmethod
    def _commands():
        # Fresh dict per call: FakeLink.command() pops list-valued entries
        # in place (the TestStep27 hazard TestStep28/29 already avoid).
        return {
            "AT+MTCOMMISSION=180": (0, []),
            "AT+MTSTATE?": (0, ["+MTSTATE:2,1"]),
        }

    def test_constant_pinned_false(self):
        # T3 Task 1 finding (c): no +MTEVT:5 was observed in 200 s of
        # capture, only one +MTEVT:4 at ~180.2 s.
        self.assertFalse(WINDOW_EXPIRY_RAISES_EVT5)

    def test_happy_path_all_checks_pass(self):
        link = FakeLink(
            self._commands(), no_reset=True,
            urcs_on_command={"AT+MTCOMMISSION=180": ["+MTEVT:0", "+MTEVT:4"]})
        ctx = fresh_ctx(link)
        ctx.opts.expiry_wait = 0.1
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_10_window_expiry(ctx)
        self.assertEqual(ctx.suite.failed, 0)

    def test_duplicate_window_close_fails_exactly_one_check(self):
        link = FakeLink(
            self._commands(), no_reset=True,
            urcs_on_command={"AT+MTCOMMISSION=180": ["+MTEVT:0", "+MTEVT:4"]})
        # A second +MTEVT:4 seeded independently of the command trigger,
        # as if a duplicate close arrived during the 10 s watch.
        link.push_urcs(["+MTEVT:4"])
        ctx = fresh_ctx(link)
        ctx.opts.expiry_wait = 0.1
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_10_window_expiry(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.10 no duplicate window-close", failed_names)

    def test_state_not_2_1_fails_its_check(self):
        commands = self._commands()
        commands["AT+MTSTATE?"] = (0, ["+MTSTATE:1,1"])
        link = FakeLink(
            commands, no_reset=True,
            urcs_on_command={"AT+MTCOMMISSION=180": ["+MTEVT:0", "+MTEVT:4"]})
        ctx = fresh_ctx(link)
        ctx.opts.expiry_wait = 0.1
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_10_window_expiry(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.10 state back to 2 (operational)", failed_names)
        self.assertNotIn("2.10 window end reported (+MTEVT:4)", failed_names)


from mt_regression import step_2_11_two_resets


class TestStep211(unittest.TestCase):
    """T3 2.11: the two resets differ in exactly one respect (spec
    3.10, TESTING.md 2.11). AT+MTEP? is queried three times with
    different scripted replies (before the resets, after MTRESET,
    after MTFRESET), so it is list-valued like TestStep28/29's
    AT+MTATTR=1,6,0; +MTREADY is released per-command via
    urcs_on_command the way TestStep27 releases +MTEVT:0 on
    AT+MTCOMMISSION=180, and +MTEVT:3 is released by a FakeChipRunner
    on_call hook firing on the real `pairing` invocation (TestStep27's
    pattern), not pre-seeded, so a step that skipped the call could not
    pass the check by accident."""

    @staticmethod
    def _commands(before=("+MTEP:0,1,0x0100",), after_reset=None,
                  after_freset=()):
        # Fresh dict and fresh inner lists per call: FakeLink.command()
        # pops list-valued entries in place (the TestStep27 hazard
        # TestStep28/29/210 already avoid).
        if after_reset is None:
            after_reset = list(before)
        return {
            "AT+MTEP?": [(0, list(before)), (0, list(after_reset)),
                        (0, list(after_freset))],
            "AT+MTFABRICS?": [(0, ["+MTFABRICS:0"]), (0, ["+MTFABRICS:1"]),
                              (0, ["+MTFABRICS:0"])],
            "AT+MTRESET": (0, []),
            "AT+MTFRESET": (0, []),
        }

    def _ctx(self, runner, commands=None):
        link = FakeLink(
            commands or self._commands(),
            urcs_on_command={"AT+MTRESET": ["+MTREADY"],
                             "AT+MTFRESET": ["+MTREADY"]})
        d = tempfile.mkdtemp()  # outlives the step; ChipTool.run mkdirs it
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_ctx(link, chip=chip)
        ctx.passcode, ctx.discriminator = 20202021, 3840
        return ctx, link

    @staticmethod
    def _release_on_pairing(link):
        def on_call(argv):
            if "pairing" in argv:
                link.push_urcs(["+MTEVT:1", "+MTEVT:3"])
        return on_call

    def test_happy_path(self):
        runner = FakeChipRunner([
            (0, "CHIP:TOO: Device commissioning completed with success"),
        ])
        ctx, link = self._ctx(runner)
        runner.on_call = self._release_on_pairing(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_11_two_resets(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        pairing_argv = runner.calls[0][0]
        self.assertEqual(pairing_argv[1:3], ["pairing", "ble-wifi"])

    def test_composition_changed_by_reset_fails_survives_check(self):
        # A regression that made AT+MTRESET erase the composition, the
        # exact failure TESTING.md 2.11 exists to catch.
        commands = self._commands(after_reset=["+MTEP:0,1,0x0104"])
        runner = FakeChipRunner([
            (0, "CHIP:TOO: Device commissioning completed with success"),
        ])
        ctx, link = self._ctx(runner, commands=commands)
        runner.on_call = self._release_on_pairing(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_11_two_resets(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.11 composition survives MTRESET", failed_names)

    def test_recommission_failure_aborts(self):
        # No on_call release wired: a failed pairing produces no
        # commissioning URCs on real hardware either (TestStep27's
        # test_pairing_failure_aborts reasoning).
        runner = FakeChipRunner([(1, "CHIP:TOO: Run command failure")])
        ctx, _ = self._ctx(runner)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_2_11_two_resets(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_thread_transport_uses_ble_thread(self):
        """T4: same rewiring as 2.3; the re-commission call in 2.11 routes
        through pairing_argv's ble-thread branch on a THREAD ctx."""
        runner = FakeChipRunner([
            (0, "CHIP:TOO: Device commissioning completed with success"),
        ])
        ctx, link = self._ctx(runner)
        ctx.transport = "THREAD"
        ctx.dataset = "0e08abc123"
        runner.on_call = self._release_on_pairing(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_11_two_resets(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        pairing_argv_used = runner.calls[0][0]
        self.assertIn("ble-thread", pairing_argv_used)
        self.assertIn("hex:0e08abc123", pairing_argv_used)
        self.assertNotIn(ctx.opts.ssid, pairing_argv_used)
        self.assertNotIn(ctx.opts.psk, pairing_argv_used)


from mt_regression import step_2_12_rig_restore


class TestStep212(unittest.TestCase):
    """T3 2.12: absorbs the retired cleanup step's duties (TESTING.md
    2.12, spec 3.9 staging grammar). The MTEP staging round-trip is
    byte-identical on the bench (T3 Task 1 finding (d): capture, clear,
    stage, apply, readback all matched), so the strict `lines ==
    ctx.composition` equality in the step is correct as written, not a
    stand-in for a looser comparison."""

    COMPOSITION = ["+MTEP:0,1,0x0100"]

    @staticmethod
    def _commands(ep_readback=None, state="+MTSTATE:1,0",
                 fabrics="+MTFABRICS:0"):
        # Fresh dict per call (the TestStep27 exhaustion hazard the newer
        # TestStep classes already avoid).
        if ep_readback is None:
            ep_readback = list(TestStep212.COMPOSITION)
        return {
            "AT+MTSTATE?": (0, [state]),
            "AT+MTEPCLEAR": (0, []),
            "AT+MTEP=0x0100": (0, []),
            "AT+MTEPAPPLY": (0, []),
            "AT+MTEP?": (0, ep_readback),
            "AT+MTFABRICS?": (0, [fabrics]),
        }

    def _ctx(self, runner, commands=None):
        # +MTEVT:0 models the URC MTFRESET's reboot released in 2.11's
        # flow (Task 9's urcs_on_command pattern); +MTREADY releases on
        # AT+MTEPAPPLY the same way.
        link = FakeLink(commands or self._commands(), no_reset=True,
                        urcs=["+MTEVT:0"],
                        urcs_on_command={"AT+MTEPAPPLY": ["+MTREADY"]})
        d1, d2 = tempfile.mkdtemp(), tempfile.mkdtemp()
        open(os.path.join(d1, "f"), "w").close()
        open(os.path.join(d2, "f"), "w").close()
        chip = ChipTool("/bin/chip-tool", d1, runner=runner)
        chip2 = ChipTool("/bin/chip-tool", d2, runner=runner)
        ctx = fresh_ctx(link, chip=chip)
        ctx.chip2 = chip2
        ctx.composition = list(self.COMPOSITION)
        return ctx, d1, d2

    def test_happy_path(self):
        # The old node's storage was wiped mid-2.11, then re-commissioned,
        # then AT+MTFRESET killed the device side: the read must fail.
        runner = FakeChipRunner([(1, "CHIP:TOO: Run command failure")])
        ctx, d1, d2 = self._ctx(runner)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_12_rig_restore(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        self.assertEqual(os.listdir(d1), [])
        self.assertEqual(os.listdir(d2), [])

    def test_reachable_old_node_fails(self):
        runner = FakeChipRunner([(0, "CHIP:TOO:   OnOff: TRUE")])
        ctx, _, _ = self._ctx(runner)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_12_rig_restore(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.12 old node unreachable", failed_names)

    def test_readback_mismatch_fails(self):
        runner = FakeChipRunner([(1, "CHIP:TOO: Run command failure")])
        commands = self._commands(ep_readback=["+MTEP:0,1,0x0104"])
        ctx, _, _ = self._ctx(runner, commands=commands)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_12_rig_restore(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("cleanup composition restored", failed_names)

    def test_five_field_parent_lines_restage_verbatim(self):
        # Composed appliance round, task 6: the capture regex
        # (\+MTEP:\d+,\d+,(\S+)) takes EVERYTHING after the endpoint id,
        # so a five-field parented line's devtype, variant and parent
        # index all ride the capture and restage verbatim through
        # stage_composition's AT+MTEP=%s passthrough: parenting needed
        # no helper change, and this pins that fact against a future
        # "tidy-up" that narrows the capture to the devtype alone.
        composition = ["+MTEP:0,1,0x0070", "+MTEP:1,2,0x0071,0,0"]
        commands = self._commands(ep_readback=list(composition))
        commands["AT+MTEP=0x0070"] = (0, [])
        commands["AT+MTEP=0x0071,0,0"] = (0, [])
        runner = FakeChipRunner([(1, "CHIP:TOO: Run command failure")])
        ctx, _, _ = self._ctx(runner, commands=commands)
        ctx.composition = list(composition)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_12_rig_restore(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertIn("AT+MTEP=0x0070", ctx.link.sent)
        self.assertIn("AT+MTEP=0x0071,0,0", ctx.link.sent)


from mt_regression import step_2_6_root_urc_sweep


class TestStep26(unittest.TestCase):
    """T3 2.6, scored last: no `+MTATTR:0,...` URC anywhere in the whole
    run's history (TESTING.md 2.6)."""

    def test_clean_history_passes(self):
        link = FakeLink()
        link.urc_history = [(0.0, "+MTEVT:0"), (1.0, "+MTATTR:1,6,0,1"),
                            (2.0, "+MTREADY")]
        ctx = fresh_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_6_root_urc_sweep(ctx)
        self.assertEqual(ctx.suite.failed, 0)

    def test_root_endpoint_attr_urc_fails(self):
        link = FakeLink()
        link.urc_history = [(0.0, "+MTEVT:0"), (1.0, "+MTATTR:0,40,2,1"),
                            (2.0, "+MTREADY")]
        ctx = fresh_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_6_root_urc_sweep(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.6 no root-endpoint +MTATTR URC in the whole run",
                      failed_names)


class TestGateSkips(unittest.TestCase):
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

    def test_gate_skip_label_spells_the_real_flag(self):
        # T3 final review finding 5: run_phase2 passes "k=<kw>" for a
        # -k deselection, which is not a --flag; gate_skip must not run
        # it through the --flag.replace("_", "-") path meant for
        # --include-slow/--include-manual, or the printed label claims
        # a "--k=..." flag that does not exist.
        s = Suite()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            s.gate_skip("2.10 window expiry", "include_slow")
            s.gate_skip("2.5 controller to host", "k=2.4")
        out = buf.getvalue()
        self.assertIn("(gated: --include-slow)", out)
        self.assertIn("(gated: -k 2.4)", out)
        self.assertNotIn("--k=", out)

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
            with contextlib.redirect_stdout(io.StringIO()):
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
            with contextlib.redirect_stdout(io.StringIO()):
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
            with contextlib.redirect_stdout(io.StringIO()):
                run_phase2(ctx)
        self.assertIn("AT+MTRESET", ctx.link.sent)  # recovery reset issued
        self.assertEqual([n for n, _ in ctx.suite.skipped], ["after"])

    def test_recovery_survives_dead_link_and_still_wipes_storage(self):
        """Task 4 carried finding: make_relink's contract leaves the port
        CLOSED when its action fails, and step_2_8_warm_reboot raises
        StepAbort on that path. recover_after_abort's own AT+MTRESET then
        hits a closed transport (PortNotOpenError, a SerialException,
        which subclasses OSError), which must not propagate past
        run_phase2 nor skip the storage wipes that follow it."""

        class DeadLink:
            def command(self, *a, **kw):
                raise OSError("port closed")

        def boom(ctx):
            ctx.suite.check("boom", False, tag="P2")
            raise StepAbort("dead")

        steps = [{"name": "boom", "fn": boom}]
        d1, d2 = tempfile.mkdtemp(), tempfile.mkdtemp()
        open(os.path.join(d1, "f"), "w").close()
        open(os.path.join(d2, "f"), "w").close()
        ctx = fresh_ctx(link=DeadLink(), chip=ChipTool("/bin/chip-tool", d1))
        ctx.chip2 = ChipTool("/bin/chip-tool", d2)
        with patched_steps(steps):
            with contextlib.redirect_stdout(io.StringIO()):
                run_phase2(ctx)  # must not raise
        self.assertEqual(os.listdir(d1), [])
        self.assertEqual(os.listdir(d2), [])


from mt_regression import CmdResponder


class TestCmdResponder(unittest.TestCase):
    """T5 Task 2: CmdResponder answers a forwarded +MTCMD URC the way a
    host MCU would, except for seq 0 (notify-only), which spec 3.17 says
    the firmware itself rejects (+MTERR:1) if anyone tries."""

    def test_adjudicated_forward_answered(self):
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:7,3,96,0")
        tr.expect_write("AT+MTCMDRESP=7,1\r\n", then_lines=["OK"])
        r = CmdResponder(link)
        fwd = r.expect(cluster=96, command=0, verdict=1)
        self.assertEqual(fwd["seq"], 7)
        self.assertEqual(fwd["ep"], 3)
        self.assertIsNone(fwd["payload"])

    def test_notify_never_answered(self):
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:0,4,92,0")
        r = CmdResponder(link)
        fwd = r.expect_notify(cluster=92, command=0)
        self.assertEqual(fwd["seq"], 0)
        self.assertEqual(tr.writes, [])   # nothing sent, ever

    def test_five_field_payload_parsed(self):
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:9,4,1366,0,2")
        tr.expect_write("AT+MTCMDRESP=9,0\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=1366, command=0, verdict=0,
                                        payload=2)
        self.assertEqual(fwd["payload"], 2)

    def test_wrong_cluster_returns_none(self):
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:7,3,96,0")
        self.assertIsNone(CmdResponder(link).expect(
            cluster=6, command=0, verdict=1, timeout=0.3))

    def test_seq_zero_via_expect_still_never_answered(self):
        """expect() (the adjudicated path) must honor the seq-0 rule too:
        a notify-only forward reaching it by way of the general API must
        not be answered just because the caller used the wrong method."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:0,4,92,0")
        fwd = CmdResponder(link).expect(cluster=92, command=0, verdict=1)
        self.assertEqual(fwd["seq"], 0)
        self.assertEqual(tr.writes, [])

    def test_expect_notify_rejects_nonzero_seq(self):
        """expect_notify() must not accept an adjudicated (seq != 0)
        forward: it asserts seq == 0, per the interface contract."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:7,3,96,0")
        self.assertIsNone(CmdResponder(link).expect_notify(
            cluster=96, command=0, timeout=0.3))
        self.assertEqual(tr.writes, [])

    def test_unrelated_forward_stays_queued_after_a_miss(self):
        """Review finding (Important, task-2 review): _match() used to
        await the broad pattern +MTCMD: and filter by cluster/command in
        Python after await_urc_ts had already popped the entry, so a
        forward for an unrelated cluster was consumed and discarded by a
        call that was not asking for it, never to be seen again. This
        pins the fix: an expect() that times out on the wrong
        cluster/command must leave a queued, unrelated forward
        untouched, so a later expect() for it still finds it."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:3,1,257,0")   # door lock forward, cluster 257
        r = CmdResponder(link)
        # a miss on an unrelated cluster/command must not consume it
        self.assertIsNone(r.expect(cluster=129, command=0, verdict=1,
                                   timeout=0.2))
        # it is still there for the caller that actually wants it
        tr.expect_write("AT+MTCMDRESP=3,1\r\n", then_lines=["OK"])
        fwd = r.expect(cluster=257, command=0, verdict=1)
        self.assertEqual(fwd["seq"], 3)

    # -- RVC + Microwave batch: _RX/_match widened from one trailing
    # numeric field to up to four, per command (mt_cmd_forward_fields(),
    # AT_MT_SPEC.md 3.17). Tested both directions: parsing a wire tail
    # with interior empty positions into fields (this test group), and
    # the payload= matcher accepting a list/tuple to assert the full
    # field list, present-and-empty positions alike (the next group).

    def test_four_field_payload_with_interior_empty_positions_parsed(self):
        """The exact shape AT_MT_SPEC.md 3.17 uses to illustrate the
        empty-field convention: p1/p2 absent (rendered as empty, not
        omitted), p3=80, p4=1. fields must come back [None, None, 80, 1],
        not shifted down to [80, 1]."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:9,13,95,0,,,80,1")
        tr.expect_write("AT+MTCMDRESP=9,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=95, command=0, verdict=1)
        self.assertEqual(fwd["fields"], [None, None, 80, 1])
        self.assertIsNone(fwd["payload"])  # fields[0] is the empty p1

    def test_four_field_payload_all_present_parsed(self):
        """SetCookingParameters' real wire shape (Task 4's trace): every
        one of the four fields present, none empty."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:8,13,95,0,1,900,80,0")
        tr.expect_write("AT+MTCMDRESP=8,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=95, command=0, verdict=1)
        self.assertEqual(fwd["fields"], [1, 900, 80, 0])
        self.assertEqual(fwd["payload"], 1)  # fields[0], legacy alias

    def test_bare_four_field_form_still_parses_as_no_payload(self):
        """Backward compatibility: a forward with no trailing fields at
        all (the pre-existing bare form, e.g. the door lock/valve) must
        still come back fields=[], payload=None, exactly as before the
        widening."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:7,3,96,0")
        tr.expect_write("AT+MTCMDRESP=7,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=96, command=0, verdict=1)
        self.assertEqual(fwd["fields"], [])
        self.assertIsNone(fwd["payload"])

    def test_payload_list_matches_fields_including_none_positions(self):
        """The payload= filter accepts a list/tuple for the multi-field
        case and requires an exact match, empty positions (None) and
        all: this is how step_3_16_microwave asserts SetCookingParameters'
        four-field payload."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:9,13,95,0,,,80,1")
        tr.expect_write("AT+MTCMDRESP=9,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=95, command=0, verdict=1,
                                        payload=[None, None, 80, 1])
        self.assertIsNotNone(fwd)

    def test_payload_list_mismatch_returns_none(self):
        """A payload= list that does not match the parsed fields exactly
        (even by one position) must report the miss as None, the same
        exactness expect()'s single-int payload= already had."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:9,13,95,0,,,80,1")
        fwd = CmdResponder(link).expect(cluster=95, command=0, verdict=1,
                                        payload=[None, None, 80, 2],
                                        timeout=0.2)
        self.assertIsNone(fwd)
        self.assertEqual(tr.writes, [])  # never answered a mismatch

    def test_single_field_legacy_int_payload_still_works(self):
        """The pre-widening single-int payload= form (chime's chimeID,
        ModeBase's ChangeToMode newMode) must be unaffected by the
        widening: compares against fields[0] only."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:11,12,84,0,1")
        tr.expect_write("AT+MTCMDRESP=11,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=84, command=0, verdict=1,
                                        payload=1)
        self.assertEqual(fwd["fields"], [1])
        self.assertEqual(fwd["payload"], 1)

    # -- Energy round B: _RX widened from four trailing fields to five,
    # per Boost's documented arity (AT_MT_SPEC.md 3.17: duration, the
    # packed mask, then up to three numeric optionals). The canonical
    # vector is the design spec's own worked example.

    def test_boost_canonical_vector_matched_exactly(self):
        """The design spec's worked example verbatim: duration 3600,
        oneShot true, targetPercentage 80 -> mask bit0|bit3|bit8 = 265,
        tail "3600,265,80". step_3_22 asserts this with a full-list
        payload=, so a firmware that packed the mask wrong would leave
        the forward unanswered rather than being silently accepted."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:5,23,148,0,3600,265,80")
        tr.expect_write("AT+MTCMDRESP=5,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=148, command=0, verdict=1,
                                        payload=[3600, 265, 80])
        self.assertIsNotNone(fwd)
        self.assertEqual(fwd["fields"], [3600, 265, 80])

    def test_boost_wrong_mask_returns_none_and_never_answers(self):
        """A mask of 9 (presence bits only, bit 8 lost: the exact shape
        of a firmware that forgot the bool-VALUE bits) must not satisfy
        the canonical payload= vector, and a mismatch is never
        answered."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:5,23,148,0,3600,9,80")
        fwd = CmdResponder(link).expect(cluster=148, command=0, verdict=1,
                                        payload=[3600, 265, 80],
                                        timeout=0.2)
        self.assertIsNone(fwd)
        self.assertEqual(tr.writes, [])

    def test_five_field_boost_tail_parsed(self):
        """All five optionals present (mask 31 | bits 8-9 = 799):
        duration, mask, temporarySetpoint, targetPercentage,
        targetReheat. Four-capped _RX (the pre-round-B regex) would
        fail to match this line at all; the widening is what this test
        pins."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:6,23,148,0,3600,799,2100,80,50")
        tr.expect_write("AT+MTCMDRESP=6,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=148, command=0, verdict=1)
        self.assertEqual(fwd["fields"], [3600, 799, 2100, 80, 50])
        self.assertEqual(fwd["payload"], 3600)  # fields[0], legacy alias

    def test_cancel_boost_bare_form_parses_payloadless(self):
        """CancelBoost forwards payload-less (spec 3.17); the bare form
        must still come back fields=[] after the widening, the same
        backward-compatibility pin the four-field round carried."""
        tr = FakeTransport()
        link = ATLink(tr)
        tr.feed_line("+MTCMD:7,23,148,1")
        tr.expect_write("AT+MTCMDRESP=7,1\r\n", then_lines=["OK"])
        fwd = CmdResponder(link).expect(cluster=148, command=1, verdict=1)
        self.assertEqual(fwd["fields"], [])
        self.assertIsNone(fwd["payload"])


from mt_regression import (parse_int_attr, parse_status,
                           parse_accepted_command_list, parse_event_count,
                           parse_string_list, _strip_ansi)


class TestT5Parsers(unittest.TestCase):
    """T5 harness extension, Task 3: chip-tool output parsers for the
    post-August-1 device type families. Every fixture is a trimmed,
    verbatim excerpt of a real chip-tool capture from the C10 bench
    round (.superpowers/sdd/2026-08-07-seven-type-batch/task-C10-evidence/),
    grepped clean of SSID/PSK/pairing before being committed. Expected
    values are the ones the C10 report recorded for that excerpt."""

    def _fx(self, name):
        return fixture(os.path.join("t5", name))

    def test_parse_int_attr_reads_current_mode(self):
        # 035330-c14-modes-current-mode.log: CurrentMode: 2 after the
        # sketch's change-to-mode command landed.
        self.assertEqual(
            parse_int_attr(self._fx("modes-current-mode.txt")), 2)

    def test_parse_int_attr_no_match_is_none(self):
        self.assertIsNone(parse_int_attr("no such content"))

    def test_parse_status_reads_operationalstate_deny(self):
        # 022107-rerun-sketch-pause-deny.log: OperationalState's verdict
        # IS the wire response (spec 3.21) -- ErrorStateID 2 on deny.
        self.assertEqual(
            parse_status(self._fx("opstate-pause-deny.txt")), 0x02)

    def test_parse_status_no_match_is_none(self):
        self.assertIsNone(parse_status("no such content"))

    def test_parse_accepted_command_list_four_entries(self):
        # 021520-rerun-operationalstate-accepted-command-list.log:
        # Pause, Stop, Start, Resume.
        self.assertEqual(
            parse_accepted_command_list(
                self._fx("opstate-accepted-command-list.txt")),
            [0, 1, 2, 3])

    def test_parse_accepted_command_list_no_match_is_empty(self):
        self.assertEqual(parse_accepted_command_list("no such content"), [])

    def test_parse_event_count_two_occurrences(self):
        # 032413-c12-ev-after2.log: two SelfTestComplete events landed
        # in one read-event call.
        self.assertEqual(
            parse_event_count(self._fx("smoke-selftest-event-count.txt"),
                               "SelfTestComplete"),
            2)

    def test_parse_event_count_no_match_is_zero(self):
        self.assertEqual(parse_event_count("no such content", "Whatever"),
                          0)

    def test_parse_string_list_preserves_comma_in_label(self):
        # 035324-c14-modes-supported-modes.log: three SupportedModes
        # labels, one of them containing a comma.
        labels = parse_string_list(self._fx("modes-supported-modes.txt"))
        self.assertEqual(labels, ["Quiet", "Normal, standard", "Boost"])
        self.assertIn("Normal, standard", labels)

    def test_parse_string_list_no_match_is_empty(self):
        self.assertEqual(parse_string_list("no such content"), [])

    def test_parse_string_list_real_ansi_capture_needs_the_strip(self):
        """Task-7 fix F5 (fixture provenance): this fixture is a
        byte-exact excerpt of chiptool-capture-run5.txt's SupportedModes
        read (lines around the 3.7 check in task-7-evidence/), escapes
        included, unlike every other T5 fixture, which was hand-cleaned
        and so never exercised the ANSI defect (task-7-report.md section
        10, concern 1). RED first: parse_string_list on the raw text
        leaks the trailing '\\x1b[0m' into the last label, the exact H1
        shape. GREEN after _strip_ansi, the same function
        _subprocess_runner calls centrally (F1)."""
        raw = fixture(os.path.join("t5", "modes-supported-modes-raw.txt"))
        self.assertIn("\x1b[", raw)
        self.assertNotEqual(parse_string_list(raw), ["Quiet", "Eco, low"])
        self.assertEqual(parse_string_list(_strip_ansi(raw)),
                         ["Quiet", "Eco, low"])


class TestRvcMicrowaveParsers(unittest.TestCase):
    """RVC + Microwave batch, harness task 5: parse_mode_tag_values and
    parse_change_to_mode_status, the two INFERENCE-marked parsers this
    task adds. Unlike TestT5Parsers' fixtures (real chip-tool captures,
    trimmed), every fixture here is SYNTHETIC (header comment in the
    file): no local chip-tool build has an RvcRunMode/RvcCleanMode/
    MicrowaveOvenMode cluster to capture a real read against, so these
    were built to the pinned SDK's own print path instead of hand-cleaned
    from a real run, per the fixture-provenance rule (never hand-clean a
    real capture; when none exists, mark the substitute SYNTHETIC).
    Task 9's bench run is what confirms or corrects the shape."""

    def _fx(self, name):
        return fixture(os.path.join("rvc-microwave", name))

    def test_parse_mode_tag_values_explicit_then_defaulted(self):
        text = self._fx("rvcrunmode-supported-modes-synthetic.txt")
        self.assertEqual(parse_mode_tag_values(text), [0x4000, 0x4001])

    def test_parse_mode_tag_values_labels_still_agree(self):
        # The same fixture's labels must still parse the ModeSelect way
        # (parse_string_list, already proven real): the two parsers read
        # disjoint fields from the same struct list.
        text = self._fx("rvcrunmode-supported-modes-synthetic.txt")
        self.assertEqual(parse_string_list(text), ["Idle", "Cleaning"])

    def test_parse_mode_tag_values_no_match_is_empty(self):
        self.assertEqual(parse_mode_tag_values("no such content"), [])

    def test_parse_change_to_mode_status_allow(self):
        text = self._fx("changetomode-response-allow-synthetic.txt")
        self.assertEqual(parse_change_to_mode_status(text), 0)

    def test_parse_change_to_mode_status_deny(self):
        text = self._fx("changetomode-response-deny-synthetic.txt")
        self.assertEqual(parse_change_to_mode_status(text), 2)

    def test_parse_change_to_mode_status_ignores_status_text_line(self):
        # "statusText:" must never be mistaken for "status:" (no digits
        # follow it here, so a wrong match would raise, not just mislead).
        text = "[TOO]     statusText: some text\n[TOO]     status: 3"
        self.assertEqual(parse_change_to_mode_status(text), 3)

    def test_parse_change_to_mode_status_no_match_is_none(self):
        self.assertIsNone(parse_change_to_mode_status("no such content"))


class TestParseActivePowerReports(unittest.TestCase):
    """Energy round A, task 5: the meter subscription's report parser
    (INFERENCE shape, derivation in the parser docstring). Synthetic
    text modelled on the pinned DataModelLogger print path."""

    def test_values_in_order_including_beyond_32_bits(self):
        text = ("[TOO]   ActivePower: 99590\n"
                "[TOO]   ActivePower: 4294967297\n")
        self.assertEqual(parse_active_power_reports(text),
                         [99590, 4294967297])

    def test_negative_value(self):
        self.assertEqual(
            parse_active_power_reports("[TOO]   ActivePower: -500"),
            [-500])

    def test_null_report_is_invisible(self):
        # The Nullable template prints the literal "null": no digits,
        # so a null report never counts, the same invisibility
        # parse_onoff_reports gives heartbeat reports.
        self.assertEqual(
            parse_active_power_reports("[TOO]   ActivePower: null"), [])

    def test_sibling_power_attributes_never_match(self):
        text = ("[TOO]   ReactivePower: 7\n"
                "[TOO]   ApparentPower: 8\n")
        self.assertEqual(parse_active_power_reports(text), [])

    def test_trailing_ansi_escape_tolerated(self):
        # Subscriber output bypasses the central ANSI strip (F1), so
        # the parser must not be end-anchored.
        self.assertEqual(
            parse_active_power_reports(
                "[TOO]   ActivePower: 99590\x1b[0m\n"), [99590])


class TestParseEnergyValues(unittest.TestCase):
    """Energy round A, task 5: the EnergyMeasurementStruct parser
    (INFERENCE shape, derivation in the parser docstring), serving both
    the attribute read and the event's nested structs."""

    ATTR_READ = "\n".join([
        "[TOO]   CumulativeEnergyImported: {",
        "[TOO]     Energy: 1600000",
        "[TOO]     EndSystime: 123456",
        "[TOO]    }",
    ])
    EVENT_READ = "\n".join([
        "[TOO]   CumulativeEnergyMeasured: {",
        "[TOO]     EnergyImported: {",
        "[TOO]       Energy: 1500000",
        "[TOO]       EndSystime: 100000",
        "[TOO]      }",
        "[TOO]    }",
        "[TOO]   CumulativeEnergyMeasured: {",
        "[TOO]     EnergyImported: {",
        "[TOO]       Energy: 1600000",
        "[TOO]       EndSystime: 123456",
        "[TOO]      }",
        "[TOO]     EnergyExported: {",
        "[TOO]       Energy: 20000",
        "[TOO]       EndSystime: 123456",
        "[TOO]      }",
        "[TOO]    }",
    ])

    def test_attribute_read_struct(self):
        # EndSystime must NOT leak in: parse_int_attr's last-match rule
        # would have returned 123456 here, which is exactly why this
        # dedicated parser exists.
        self.assertEqual(parse_energy_values(self.ATTR_READ), [1600000])

    def test_event_read_both_events_in_order(self):
        self.assertEqual(parse_energy_values(self.EVENT_READ),
                         [1500000, 1600000, 20000])

    def test_event_count_shape_agrees(self):
        # The event-name block shape parse_event_count reads is the
        # CONFIRMED half of the pair (same `<EventName>: {` form as
        # SelfTestComplete); pin that the two parsers agree on one text.
        self.assertEqual(
            parse_event_count(self.EVENT_READ, "CumulativeEnergyMeasured"),
            2)

    def test_sibling_energy_labels_never_match(self):
        text = ("[TOO]   ApparentEnergy: 7\n"
                "[TOO]   ReactiveEnergy: 8\n"
                "[TOO]   EnergyImported: 9\n")
        self.assertEqual(parse_energy_values(text), [])


from mt_regression import (
    PHASE3_COMPOSITION, Phase3Context, stage_composition,
    restore_standard_state, step_3_1_compose, step_3_2_grammar,
    step_3_3_selftest_wedge, step_3_4_stores, run_phase3, PHASE3_STEPS,
    step_3_5_commission, step_3_6_valve, step_3_7_modes, step_3_8_chime,
    step_3_9_opstate, step_3_10_smoke, step_3_11_power,
    step_3_12_lock_switch_levels, step_3_13_restore,
    step_3_14_root_urc_sweep, step_3_15_rvc, step_3_16_microwave,
    step_3_17_composed_fridge, step_3_18_oven_cavity,
    step_3_19_cook_surface, step_3_20_electrical_sensor,
    step_3_21_electrical_meter, step_3_22_water_heater,
    step_3_23_heat_pump, t_meas_staged_wh_min, mtep_staging_negative,
    invoke_chip, parse_indexed_list, parse_mode_tag_values,
    parse_change_to_mode_status, parse_parts_list, parse_notify_active,
    parse_active_power_reports, parse_energy_values, parse_device_types,
    CmdResponder,
    step_3_24_solar_power, step_3_25_battery_storage, step_3_26_dem,
    t_staged_variant1_energy_c1, parse_power_adjust_entries,
    parse_cause_values, parse_esa_state_reports, _hold_clock_gap,
)


def fresh_phase3_ctx(link=None, chip=None):
    if link is None:
        link = FakeLink()
    opts = types.SimpleNamespace(node_id=0x4845, ssid="net", psk="secret",
                                 include_slow=False, include_manual=False,
                                 keyword=None)
    return Phase3Context(link=link, chip=chip, suite=Suite(), opts=opts)


class TestStageComposition(unittest.TestCase):
    """T5 task 4: extracted from 2.12's inline MTEPCLEAR/MTEP loop so
    step_3_1_compose (and, later, the full restore) reuse it instead of
    reimplementing the spec 3.9 grammar."""

    def test_clears_then_appends_each_devtype_in_order(self):
        link = FakeLink({"AT+MTEPCLEAR": (0, []), "AT+MTEP=0x0100": (0, []),
                         "AT+MTEP=0x0071,1": (0, [])})
        ok = stage_composition(link, ["0x0100", "0x0071,1"])
        self.assertTrue(ok)
        self.assertEqual(
            link.sent,
            ["AT+MTEPCLEAR", "AT+MTEP=0x0100", "AT+MTEP=0x0071,1"])

    def test_a_failed_append_makes_the_whole_call_false_but_keeps_going(self):
        link = FakeLink({"AT+MTEPCLEAR": (0, []), "AT+MTEP=0x0100": (0, []),
                         "AT+MTEP=0x9999": (6, [])})
        ok = stage_composition(link, ["0x0100", "0x9999"])
        self.assertFalse(ok)
        self.assertEqual(
            link.sent, ["AT+MTEPCLEAR", "AT+MTEP=0x0100", "AT+MTEP=0x9999"])


class TestPhase3Composition(unittest.TestCase):
    """T5 task 4: every id in PHASE3_COMPOSITION is quoted from
    AT_MT_SPEC.md's device table in the task-4 report; this test pins the
    shape (slot order, count, the one deliberate duplicate id) rather
    than the ids themselves, which a doc diff cannot catch. Slots 12-13
    (RVC + Microwave batch harness task) and 14-20 (composed appliance
    round, task 6) extend the same pin."""

    def test_twentyseven_slots_sequential(self):
        # The composition-size gate: energy round C1 grew the table to
        # 27 (round A had grown it to 22, round B to 24, which hit the
        # then-cap of 24 exactly). MT_COMP_MAX_ENDPOINTS is 28 now, so
        # this table leaves ONE slot of headroom, deliberately: the
        # exact-fit experiment ended with B247 and is not repeated
        # (design spec section 5). CONFIG_ESP_MATTER_MAX_DYNAMIC_
        # ENDPOINT_COUNT (the setting CLAUDE.md warns silently refuses
        # endpoints past its limit) is 29, not 28: the SDK counts the
        # root endpoint 0 against it (B247), so it stays
        # MT_COMP_MAX_ENDPOINTS + 1.
        slots = [slot for slot, _ in PHASE3_COMPOSITION]
        self.assertEqual(slots, list(range(1, 28)))
        self.assertLessEqual(len(slots), 28)

    def test_slot_ten_and_eleven_are_the_two_cabinet_variants(self):
        # Slot 11 is not in the T5 design spec's 10-row table; it exists
        # solely to make MTTEMPLEVELS's +MTERR:4 row testable (see the
        # comment on PHASE3_COMPOSITION), and is called out in the
        # task-4 report for review.
        by_slot = dict(PHASE3_COMPOSITION)
        self.assertEqual(by_slot[10], "0x0071,1")
        self.assertEqual(by_slot[11], "0x0071")

    def test_slot_twelve_and_thirteen_are_rvc_and_microwave(self):
        # Endpoints 12/13 deliberately match AT_MT_SPEC.md 3.20.1/3.21's
        # own worked examples verbatim, so step_3_15_rvc/step_3_16_
        # microwave read the same endpoint numbers the spec's prose does.
        by_slot = dict(PHASE3_COMPOSITION)
        self.assertEqual(by_slot[12], "0x0074")
        self.assertEqual(by_slot[13], "0x0079")

    def test_composed_trio_slots_and_parent_indexes(self):
        # Composed appliance round, task 6: the trio's entries carry
        # parent STAGING INDEXES (index = endpoint id - 1 in this table),
        # transcribed from the task brief; a wrong index here would
        # compose a cabinet under the wrong appliance and invalidate
        # every 3.17-3.19 assertion.
        by_slot = dict(PHASE3_COMPOSITION)
        self.assertEqual(by_slot[14], "0x0070")
        self.assertEqual(by_slot[15], "0x0071,0,13")
        self.assertEqual(by_slot[16], "0x0071,1,13")
        self.assertEqual(by_slot[17], "0x007B")
        self.assertEqual(by_slot[18], "0x0071,0,16")
        self.assertEqual(by_slot[19], "0x0078")
        self.assertEqual(by_slot[20], "0x0077,0,18")

    def test_modebase_pool_budget_holds(self):
        # MT_MB_MAX_LISTS is 12 (raised from 8 by energy round C1) and
        # this composition must consume exactly 10 ModeBase delegate
        # slots (RVC 2, microwave 1, fridge parent 1, two parented
        # cabinets 2, oven cavity 1, water heater 1, battery storage 1,
        # standalone DEM 1). The two remaining are DELIBERATE headroom
        # (design spec section 5), and a table edit that took the pool
        # past 12 without raising the firmware pool would abort the boot
        # rebuild on the bench. Consumers by construction: 0x0074 counts
        # twice (RvcRunMode + RvcCleanMode), 0x0079, 0x0070 and 0x050F
        # (WaterHeaterMode, either variant) once each, every PARENTED
        # 0x0071 cabinet once (Cooler or Heater conditional cluster;
        # standalone cabinets carry no ModeBase cluster), 0x050D once
        # (DeviceEnergyManagementMode rides BOTH variants: the XML makes
        # it otherwiseConform, so mt_add_dem_triple() always creates it),
        # and 0x0018 once but only on VARIANT 0, whose DEM triple is the
        # over-delivery variant 1 drops.
        consumed = 0
        for _slot, dt in PHASE3_COMPOSITION:
            parts = dt.split(",")
            base, parented = parts[0], len(parts) == 3
            variant = parts[1] if len(parts) > 1 else "0"
            if base == "0x0074":
                consumed += 2
            elif base in ("0x0079", "0x0070", "0x050F", "0x050D"):
                consumed += 1
            elif base == "0x0071" and parented:
                consumed += 1
            elif base == "0x0018" and variant == "0":
                consumed += 1
        self.assertEqual(consumed, 10)
        self.assertLessEqual(consumed, 12)

    def test_measurement_pool_budget_holds(self):
        # MT_MEAS_MAX is 8 (raised from 4 by energy round C1) and this
        # composition must consume exactly 6 EPM/PowerTopology pool
        # pairs: the sensor (21, variant 1 still draws its EPM slot),
        # the meter (22), the FULL water heater (23, the sensor graft),
        # the heat pump (24), solar power (25) and battery storage (26).
        # Round B's table hit the then-cap of 4 exactly; this round's
        # two spare slots are DELIBERATE headroom (design spec section
        # 5), and a seventh consumer is still fine while an ninth would
        # abort the boot rebuild, the ModeBase rule's sibling. A
        # variant-1 water heater would consume nothing, which is part of
        # why Phase 1 stages it instead of this table carrying it; the
        # variant-1 solar and battery endpoints Phase 1 stages DO draw a
        # pair each, but never at the same time as this composition.
        consumed = 0
        for _slot, dt in PHASE3_COMPOSITION:
            parts = dt.split(",")
            base = parts[0]
            variant = parts[1] if len(parts) > 1 else "0"
            if base in ("0x0510", "0x0514", "0x0309", "0x0017", "0x0018"):
                consumed += 1
            elif base == "0x050F" and variant == "0":
                consumed += 1
        self.assertEqual(consumed, 6)
        self.assertLessEqual(consumed, 8)

    def test_dem_pool_budget_holds(self):
        # MT_DEM_MAX is 4 (new in energy round C1) and this composition
        # must consume exactly 2 HearthDemDelegate slots: battery
        # storage variant 0 (26) and the standalone ESA (27). Headroom
        # is deliberate (design spec section 5): C2's EVSE pair takes
        # one more and the last stays spare. The delegate is pooled on
        # BOTH DEM variants (the five ESA attributes are Instance-served
        # either way, design spec 2.4), which is why the 0x050D arm has
        # no variant condition while the battery arm does.
        consumed = 0
        for _slot, dt in PHASE3_COMPOSITION:
            parts = dt.split(",")
            base = parts[0]
            variant = parts[1] if len(parts) > 1 else "0"
            if base == "0x050D":
                consumed += 1
            elif base == "0x0018" and variant == "0":
                consumed += 1
        self.assertEqual(consumed, 2)
        self.assertLessEqual(consumed, 4)

    def test_slot_twentyone_and_twentytwo_are_the_electrical_pair(self):
        # Energy round A, task 5: the variant assignment is deliberately
        # FLIPPED from the round's plan-time table (sensor full, meter
        # power-only). The 1.5.1 device library marks EEM MANDATORY on
        # the meter type, so a variant-1 meter would be a non-conformant
        # composition; the sensor's XML lists its measurement clusters
        # as a pick-at-least-one choice, so the variant-1 SENSOR is the
        # strictly conformant current-clamp declaration (spec 3.9's own
        # conformance note). A table edit that swapped these back would
        # silently turn slot 22 non-conformant AND invalidate every
        # 3.20/3.21 presence/absence assertion.
        by_slot = dict(PHASE3_COMPOSITION)
        self.assertEqual(by_slot[21], "0x0510,1")
        self.assertEqual(by_slot[22], "0x0514")

    def test_slot_twentythree_and_twentyfour_are_the_energy_b_pair(self):
        # Energy round B, task 4: the water heater is the FULL variant
        # (0x050F variant 0: WHM feature map EnergyManagement|TankPercent
        # plus the composed 0x0510 sensor graft the device type XML
        # mandates). A table edit that made it variant 1 would silently
        # invalidate every 3.22 tank-trio and graft assertion AND turn
        # Phase 1's staged-variant division (t_meas_staged_wh_min owns
        # the minimal variant) into double coverage of one variant with
        # zero coverage of the other.
        by_slot = dict(PHASE3_COMPOSITION)
        self.assertEqual(by_slot[23], "0x050F")
        self.assertEqual(by_slot[24], "0x0309")

    def test_slots_twentyfive_to_twentyseven_are_the_energy_c1_trio(self):
        # Energy round C1, task 4: all three are VARIANT 0, the full
        # surface. A table edit that made any of them variant 1 would
        # silently invalidate the assertions that depend on the dropped
        # part (solar's EEM read-back, battery's whole DEM triple, the
        # standalone ESA's PowerAdjust commands and capability), AND
        # turn Phase 1's staged-variant division
        # (t_staged_variant1_energy_c1 owns every variant-1 refusal row)
        # into double coverage of one variant with zero of the other.
        by_slot = dict(PHASE3_COMPOSITION)
        self.assertEqual(by_slot[25], "0x0017")
        self.assertEqual(by_slot[26], "0x0018")
        self.assertEqual(by_slot[27], "0x050D")

    def test_no_accidental_duplicate_devtype(self):
        devtypes = [dt for _slot, dt in PHASE3_COMPOSITION]
        bases = [dt.split(",")[0] for dt in devtypes]
        dupes = {b for b in bases if bases.count(b) > 1}
        self.assertEqual(dupes, {"0x0071"})


class TestPhase3Context(unittest.TestCase):
    def test_defaults(self):
        ctx = fresh_phase3_ctx()
        self.assertEqual(ctx.composition, PHASE3_COMPOSITION)
        self.assertIsNot(ctx.composition, PHASE3_COMPOSITION)  # a copy
        self.assertEqual(ctx.transport, "WIFI")
        self.assertIsNone(ctx.dataset)
        self.assertIsNone(ctx.chip2)
        self.assertEqual(ctx.node_id, 0x4845)


def phase3_reboot_commands():
    """The AT+MTFRESET/MTEPCLEAR/MTEP*/MTEPAPPLY/MTRESET commands
    step_3_1_compose sends for PHASE3_COMPOSITION, all answering OK."""
    cmds = {"AT+MTFRESET": (0, []), "AT+MTEPCLEAR": (0, []),
            "AT+MTEPAPPLY": (0, []), "AT+MTRESET": (0, [])}
    for _slot, dt in PHASE3_COMPOSITION:
        cmds["AT+MTEP=%s" % dt] = (0, [])
    return cmds


REBOOT_READY = {"AT+MTFRESET": ["+MTREADY"], "AT+MTEPAPPLY": ["+MTREADY"],
                "AT+MTRESET": ["+MTREADY"]}


class TestStep31Compose(unittest.TestCase):
    """T5 task 4: the three-trap boot-rebuild pin (design spec 4.2 step
    2)."""

    def test_happy_path_reads_back_exact(self):
        readback = ["+MTEP:%d,%d,%s" % (i, slot, dt)
                   for i, (slot, dt) in enumerate(PHASE3_COMPOSITION)]
        cmds = phase3_reboot_commands()
        cmds["AT+MTEP?"] = (0, readback)
        link = FakeLink(cmds, urcs_on_command=REBOOT_READY)
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_1_compose(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertIn("AT+MTEPAPPLY", link.sent)
        self.assertEqual(link.sent.count("AT+MTRESET"), 1)

    def test_no_ready_after_mtfresh_aborts(self):
        cmds = phase3_reboot_commands()
        link = FakeLink(cmds)  # no urcs_on_command: +MTREADY never arrives
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_3_1_compose(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_readback_mismatch_aborts(self):
        cmds = phase3_reboot_commands()
        cmds["AT+MTEP?"] = (0, ["+MTEP:0,1,0x0100"])  # short/stale readback
        link = FakeLink(cmds, urcs_on_command=REBOOT_READY)
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_3_1_compose(ctx)
        self.assertGreater(ctx.suite.failed, 0)


def phase3_grammar_commands():
    """Every AT command step_3_2_grammar sends, each answering the way
    the row it exercises expects (result code, no lines needed except
    the CurrentMode round trip)."""
    return {
        "AT+MTSWITCH=99": (2, []),
        "AT+MTSWITCH=0": (3, []),
        "AT+MTSWITCH=9": (0, []),
        "AT+MTLOCK=99,1": (2, []),
        "AT+MTLOCK=1,1": (3, []),
        "AT+MTLOCK=8,1": (0, []),
        "AT+MTVALVE=99,1": (2, []),
        "AT+MTVALVE=1,1": (3, []),
        "AT+MTVALVE=2,1": (0, []),
        "AT+MTVALVE=2,1,50": (0, []),
        'AT+MTMODES=99,0,"Quiet"': (2, []),
        'AT+MTMODES=1,0,"Quiet"': (3, []),
        'AT+MTMODES=3,0,"Quiet"': (0, []),
        'AT+MTMODES=3,0,"Eco, low"': (0, []),
        "AT+MTATTR=3,80,3": (0, ["+MTATTR:3,80,3,0"]),
        "AT+MTOPSTATE=99,1": (2, []),
        "AT+MTOPSTATE=1,1": (3, []),
        "AT+MTOPSTATE=7,1": (0, []),
        "AT+MTALARM=5,0,0": (1, []),
        "AT+MTALARM=5,1,3": (1, []),
        "AT+MTALARM=5,4,2": (1, []),
        "AT+MTALARM=5,5,2": (1, []),
        "AT+MTALARM=5,10,4": (1, []),
        "AT+MTALARM=99,1,1": (2, []),
        "AT+MTALARM=1,1,1": (3, []),
        "AT+MTALARM=5,1,1": (0, []),
        "AT+MTALARM=5,1,0": (0, []),
        "AT+MTALARM=5,5,0": (0, []),
        'AT+MTCHIMESOUNDS=99,1,"Doorbell"': (2, []),
        'AT+MTCHIMESOUNDS=1,1,"Doorbell"': (3, []),
        'AT+MTCHIMESOUNDS=4,1,"Doorbell"': (0, []),
        'AT+MTCHIMESOUNDS=4,1,"Doorbell",2,"Alert, urgent"': (0, []),
        "AT+MTCHIME=4,0,9": (1, []),
        "AT+MTCHIME=99,0,1": (2, []),
        "AT+MTCHIME=1,0,1": (3, []),
        "AT+MTCHIME=4,0,1": (0, []),
        "AT+MTCHIME=4,1,1": (0, []),
        'AT+MTTEMPLEVELS=99,"Low"': (2, []),
        'AT+MTTEMPLEVELS=1,"Low"': (3, []),
        'AT+MTTEMPLEVELS=11,"Low"': (4, []),
        'AT+MTTEMPLEVELS=10,"Low","Medium","High"': (0, []),
        'AT+MTTEMPLEVELS=10,"Wine, red","Wine, white"': (0, []),
    }


class TestStep32Grammar(unittest.TestCase):
    """T5 task 4: 40 of Task 1's 41 deferred rows (the 41st needs a real
    forward, Task 5's job), plus the composed appliance round's
    =<alarm ep>,0,0 migration row; see the step's own docstring."""

    def test_forty_one_rows_all_pass(self):
        link = FakeLink(phase3_grammar_commands())
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_2_grammar(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertEqual(len(ctx.suite.results), 41)

    def test_a_wrong_code_fails_only_that_row(self):
        cmds = phase3_grammar_commands()
        cmds["AT+MTVALVE=99,1"] = (1, [])  # wrong: should be +MTERR:2
        link = FakeLink(cmds)
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_2_grammar(ctx)
        self.assertEqual(ctx.suite.failed, 1)


class TestStep33SelftestWedge(unittest.TestCase):
    """T5 task 4: bug B165's host-only reproduction, C10 report C12-4b
    lifted verbatim (endpoint 1 -> 5), plus the SmokeState scenario."""

    def _commands(self, resurrects=False):
        after_reboot_expressed = (
            ["+MTATTR:5,92,0,4"] if resurrects else ["+MTATTR:5,92,0,0"])
        return {
            "AT+MTATTR=5,92,0": [
                (0, ["+MTATTR:5,92,0,0"]),        # clean start
                (0, ["+MTATTR:5,92,0,4"]),        # mid-test
                (0, ["+MTATTR:5,92,0,0"]),        # post-test
                (0, after_reboot_expressed),      # after reboot
                (0, ["+MTATTR:5,92,0,1"]),        # SmokeState Warning
                (0, ["+MTATTR:5,92,0,0"]),        # SmokeState cleared
            ],
            "AT+MTALARM=5,5,1": (0, ["+MTATTR:5,92,5,1", "+MTATTR:5,92,0,4"]),
            "AT+MTALARM=5,5,0": (0, ["+MTATTR:5,92,5,0", "+MTATTR:5,92,0,0"]),
            "AT+MTATTR=5,92,5": (0, ["+MTATTR:5,92,5,0"]),
            "AT+MTRESET": (0, []),
            "AT+MTALARM=5,1,1": (0, []),
            "AT+MTALARM=5,1,0": (0, []),
        }

    def test_c12_4b_sequence_plus_smokestate(self):
        link = FakeLink(self._commands(),
                        urcs_on_command={"AT+MTRESET": ["+MTREADY"]})
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_3_selftest_wedge(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_expressed_state_resurrecting_to_testing_fails(self):
        """The direct B165 regression pin: if ExpressedState were still 4
        (Testing) after the reboot, the old bug, this must FAIL, not
        silently pass."""
        link = FakeLink(self._commands(resurrects=True),
                        urcs_on_command={"AT+MTRESET": ["+MTREADY"]})
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_3_selftest_wedge(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep34Stores(unittest.TestCase):
    """T5 task 4: MTMODES/MTCHIMESOUNDS store edges re-pinned against
    their real endpoints (design spec 4.2 step 5)."""

    def test_seven_rows_all_pass(self):
        commands = {
            'AT+MTMODES=3,0,"Boost, high"': (0, []),
            'AT+MTMODES=3,0,"A",0,"B"': (1, []),
            "AT+MTMODES=3": (1, []),
            'AT+MTMODES=3,0,"A",1,"B",2,"C",3,"D",4,"E",5,"F",6,"G",7,"H",8,"I"':
                (1, []),
            'AT+MTCHIMESOUNDS=4,1,"Alarm, loud"': (0, []),
            'AT+MTCHIMESOUNDS=4,1,"Doorbell",1,"Chime"': (1, []),
            "AT+MTCHIMESOUNDS=4": (1, []),
        }
        link = FakeLink(commands)
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_4_stores(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertEqual(len(ctx.suite.results), 7)


class TestRestoreStandardState(unittest.TestCase):
    """T5 task 4: design spec 4.4's restore, shared by run_phase3's
    finally hook now and Task 5's full step_3_13_restore later."""

    def _commands(self, ep_readback=None):
        return {
            "AT+MTFRESET": (0, []), "AT+MTEPCLEAR": (0, []),
            "AT+MTEP=0x0100": (0, []), "AT+MTEPAPPLY": (0, []),
            "AT+MTEP?": (0, ep_readback or ["+MTEP:0,1,0x0100"]),
            "AT+MTFABRICS?": (0, ["+MTFABRICS:0"]),
        }

    def test_happy_path_scored(self):
        link = FakeLink(self._commands(), urcs_on_command={
            "AT+MTFRESET": ["+MTREADY"], "AT+MTEPAPPLY": ["+MTREADY"]})
        s = Suite()
        with contextlib.redirect_stdout(io.StringIO()):
            ok = restore_standard_state(link, s)
        self.assertTrue(ok)
        self.assertEqual(s.failed, 0)
        self.assertGreater(len(s.results), 0)

    def test_unscored_when_no_suite(self):
        link = FakeLink(self._commands(), urcs_on_command={
            "AT+MTFRESET": ["+MTREADY"], "AT+MTEPAPPLY": ["+MTREADY"]})
        ok = restore_standard_state(link, suite=None)
        self.assertTrue(ok)

    def test_failure_returns_false_and_is_scored(self):
        link = FakeLink(self._commands(ep_readback=["+MTEP:0,1,0x0104"]),
                        urcs_on_command={
                            "AT+MTFRESET": ["+MTREADY"],
                            "AT+MTEPAPPLY": ["+MTREADY"]})
        s = Suite()
        with contextlib.redirect_stdout(io.StringIO()):
            ok = restore_standard_state(link, s)
        self.assertFalse(ok)
        self.assertGreater(s.failed, 0)


class TestRunPhase3(unittest.TestCase):
    """T5 task 4: run_phase3's ordering/gate/requires machinery mirrors
    run_phase2's exactly, plus the always-on finally restore (spec
    4.4)."""

    def _restore_commands(self):
        return {
            "AT+MTFRESET": (0, []), "AT+MTEPCLEAR": (0, []),
            "AT+MTEP=0x0100": (0, []), "AT+MTEPAPPLY": (0, []),
            "AT+MTEP?": (0, ["+MTEP:0,1,0x0100"]),
            "AT+MTFABRICS?": (0, ["+MTFABRICS:0"]),
        }

    def test_ordering_and_requires(self):
        calls = []
        steps = [
            {"name": "one", "fn": lambda ctx: calls.append("one")},
            {"name": "needs-one", "fn": lambda ctx: calls.append("dep"),
             "requires": ["one"]},
            {"name": "needs-missing", "fn": lambda ctx: calls.append("no"),
             "requires": ["absent"]},
        ]
        link = FakeLink(self._restore_commands(), urcs_on_command={
            "AT+MTFRESET": ["+MTREADY"], "AT+MTEPAPPLY": ["+MTREADY"]})
        ctx = fresh_phase3_ctx(link)
        saved = list(PHASE3_STEPS)
        PHASE3_STEPS[:] = steps
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                run_phase3(ctx)
        finally:
            PHASE3_STEPS[:] = saved
        self.assertEqual(calls, ["one", "dep"])
        self.assertEqual([n for n, _ in ctx.suite.skipped], ["needs-missing"])

    def test_finally_restores_even_on_abort(self):
        def boom(ctx):
            ctx.suite.check("boom", False, tag="P3")
            raise StepAbort("dead")
        steps = [{"name": "boom", "fn": boom},
                 {"name": "after", "fn": lambda ctx: None}]
        cmds = self._restore_commands()
        cmds["AT+MTRESET"] = (0, [])  # recover_after_abort's own reset
        link = FakeLink(cmds, urcs_on_command={
            "AT+MTFRESET": ["+MTREADY"], "AT+MTEPAPPLY": ["+MTREADY"]})
        ctx = fresh_phase3_ctx(link)
        saved = list(PHASE3_STEPS)
        PHASE3_STEPS[:] = steps
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                run_phase3(ctx)  # must not raise
        finally:
            PHASE3_STEPS[:] = saved
        self.assertEqual([n for n, _ in ctx.suite.skipped], ["after"])
        # the finally restore ran too (a second AT+MTFRESET beyond
        # recover_after_abort's own AT+MTRESET)
        self.assertIn("AT+MTFRESET", link.sent)

    def test_registered_steps_are_the_full_1_through_26_chain_plus_sweep(self):
        names = [s["name"] for s in PHASE3_STEPS]
        self.assertEqual(names, [
            "3.1 compose + boot-rebuild pin",
            "3.2 endpoint-dependent grammar",
            "3.3 self-test wedge reproduction",
            "3.4 store grammar edges",
            "3.5 commission",
            "3.6 valve",
            "3.7 mode select",
            "3.8 chime",
            "3.9 operational state",
            "3.10 smoke/CO alarm",
            "3.11 power source",
            "3.12 lock, switch, temp levels",
            "3.15 robotic vacuum cleaner",
            "3.16 microwave oven",
            "3.17 composed refrigerator",
            "3.18 oven cavity",
            "3.19 cook surface",
            "3.20 electrical sensor",
            "3.21 electrical meter",
            "3.22 water heater",
            "3.23 heat pump",
            "3.24 solar power",
            "3.25 battery storage",
            "3.26 device energy management",
            "3.14 root-endpoint URC sweep",
        ])


def sync_chip_call(chip, argv, timeout=60):
    """Test double for ctx.chip_call (T5 task 5): calls chip.run()
    synchronously, on the calling thread, and returns a handle whose
    join() just returns the already-known result. Combined with a
    FakeChipRunner on_call hook that pushes a scripted +MTCMD forward
    onto the FakeLink BEFORE chip.run() returns (the same on_call timing
    TestStep23/TestStep27 already use for commissioning URCs), the
    forward is queued deterministically by the time the step calls
    CmdResponder.expect()/expect_notify() -- removing any dependency on
    real thread scheduling against FakeLink's single-pass, non-blocking
    await_urc_ts (see invoke_chip's docstring in mt_regression.py)."""
    rc, out = chip.run(argv, timeout)

    class _SyncHandle:
        def join(self, timeout=None):
            return rc, out

    return _SyncHandle()


class TestInvokeChip(unittest.TestCase):
    """T5 task 5: invoke_chip's two code paths -- the real background
    thread (production default) and the ctx.chip_call test seam."""

    def test_default_runs_on_a_real_background_thread(self):
        started = threading.Event()
        finished = threading.Event()

        def slow_runner(argv, timeout):
            started.set()
            time.sleep(0.2)
            finished.set()
            return 0, "done"

        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=slow_runner)
        ctx = fresh_phase3_ctx(FakeLink(), chip=chip)
        handle = invoke_chip(ctx, ["onoff", "read", "on-off", "0x1", "1"])
        self.assertTrue(started.wait(2.0))
        # invoke_chip must have returned already, before the slow call
        # finishes: that is the entire point of the background thread.
        self.assertFalse(finished.is_set())
        rc, out = handle.join(2.0)
        self.assertEqual((rc, out), (0, "done"))
        self.assertTrue(finished.is_set())

    def test_chip_call_seam_replaces_real_threading(self):
        calls = []

        def fake(chip, argv, timeout=60):
            calls.append(argv)

            class H:
                def join(self, timeout=None):
                    return 0, "fake"
            return H()

        ctx = fresh_phase3_ctx(FakeLink())
        ctx.chip_call = fake
        handle = invoke_chip(ctx, ["x", "y"], timeout=5)
        self.assertEqual(handle.join(), (0, "fake"))
        self.assertEqual(calls, [["x", "y"]])


class TestStep35Commission(unittest.TestCase):
    """T5 task 5: same shape as TestStep23 (Phase 2's commissioning
    proof), scoped to Phase 3's simpler ask (exit 0, +MTEVT:3, fabrics
    1)."""

    AT_OK = {
        "AT+MTCODES?": (0, list(CODES)),
        "AT+MTFABRICS?": (0, ["+MTFABRICS:1"]),
    }

    @staticmethod
    def _release_on_pairing(link, urcs):
        def on_call(argv):
            if "pairing" in argv:
                link.push_urcs(urcs)
        return on_call

    def _ctx(self, runner, urcs):
        link = FakeLink(dict(self.AT_OK))
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        runner.on_call = self._release_on_pairing(link, urcs)
        ctx = fresh_phase3_ctx(link, chip=chip)
        return ctx, runner

    def test_happy_path(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (0, "CHIP:TOO: Device commissioning completed with success"),
        ])
        ctx, runner = self._ctx(runner, ["+MTEVT:1", "+MTEVT:3"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_5_commission(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertEqual(ctx.qr, "MT:Y.K9042C00KA0648G00")
        self.assertEqual(ctx.passcode, 20202021)

    def test_no_codes_captured_aborts(self):
        link = FakeLink({"AT+MTCODES?": (0, [])})
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_3_5_commission(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_pairing_failure_aborts(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (1, "CHIP:TOO: Run command failure"),
        ])
        ctx, _ = self._ctx(runner, [])
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_3_5_commission(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_fabric_count_wrong_aborts(self):
        runner = FakeChipRunner([
            (0, fixture("chiptool_parse_setup_payload.txt")),
            (0, "CHIP:TOO: Device commissioning completed with success"),
        ])
        ctx, runner = self._ctx(runner, ["+MTEVT:1", "+MTEVT:3"])
        ctx.link.commands["AT+MTFABRICS?"] = (0, ["+MTFABRICS:2"])
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_3_5_commission(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep36Valve(unittest.TestCase):
    """T5 task 5: valve open allow, actuation, event, plus the MTCMDRESP
    stale-seq deferred row (C10 evidence's "3d" bullet)."""

    def _ctx(self, push_urc=True, both_cmdresp_ok=False):
        link = FakeLink({
            "AT+MTVALVE=2,0": (0, ["+MTATTR:2,129,4,0"]),
            "AT+MTVALVE=2,1": (0, []),
            "AT+MTCMDRESP=2,1": (0, []) if both_cmdresp_ok
                                else [(0, []), (1, [])],
        })
        script = [
            (0, "[TOO] Endpoint: 2 Cluster: 0x0000_0081 Command 0x0"),
            (0, "[TOO]   CurrentState: 1"),
            (0, "[TOO]   ValveStateChanged: {"),
        ]

        def on_call(argv):
            if push_urc and "open" in argv:
                link.push_urcs(["+MTCMD:2,2,129,0"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_6_valve(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_forward_never_answered_aborts(self):
        ctx = self._ctx(push_urc=False)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(StepAbort):
                step_3_6_valve(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_stale_seq_reanswer_not_rejected_fails_the_pin(self):
        # Regression shape: if the firmware ever answered OK to a second
        # AT+MTCMDRESP for the same seq, this must fail, not pass.
        ctx = self._ctx(both_cmdresp_ok=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_6_valve(ctx)
        self.assertEqual(ctx.suite.failed, 1)


class TestStep37Modes(unittest.TestCase):
    """T5 task 5: SupportedModes verbatim (comma label) plus the
    ChangeToMode/CurrentMode URC round trip (spec 3.20)."""

    def _ctx(self, urc=True):
        link = FakeLink({
            'AT+MTMODES=3,0,"Quiet",1,"Eco, low"': (0, []),
            "AT+MTATTR=3,80,3,0": (0, []),
            "AT+MTATTR=3,80,3": (0, ["+MTATTR:3,80,3,1"]),
        })
        script = [
            (0, "\n".join([
                "[TOO]   SupportedModes: 2 entries",
                "[TOO]     [1]: {  Label: Quiet",
                "[TOO]     [2]: {  Label: Eco, low",
            ])),
            (0, ""),
        ]

        def on_call(argv):
            if urc and "change-to-mode" in argv:
                link.push_urcs(["+MTATTR:3,80,3,1"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_7_modes(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_missing_current_mode_urc_fails(self):
        ctx = self._ctx(urc=False)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_7_modes(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep38Chime(unittest.TestCase):
    """T5 task 5: InstalledChimeSounds verbatim, PlayChimeSound allow AND
    deny (wire statuses differ), the Enabled=false short-circuit, the
    chimeID payload pin."""

    def _ctx(self, deny_status="0x01", disabled_pushes_urc=False):
        link = FakeLink({
            'AT+MTCHIMESOUNDS=4,1,"Doorbell",2,"Alert, urgent",7,'
            '"Westminster"': (0, []),
            "AT+MTCHIME=4,1,1": (0, []),
            "AT+MTCHIME=4,1,0": (0, []),
            "AT+MTCMDRESP=5,1": (0, []),
            "AT+MTCMDRESP=5,0": (0, []),
        })
        script = [
            (0, "\n".join([
                "[TOO]   InstalledChimeSounds: 3 entries",
                "[TOO]     [1]: {  Name: Doorbell",
                "[TOO]     [2]: {  Name: Alert, urgent",
                "[TOO]     [3]: {  Name: Westminster",
            ])),
            (0, "[TOO]     status = 0x00 (SUCCESS),"),
            # Task-7 fix F2: a denied PlayChimeSound is a bare StatusIB
            # Failure, and chip-tool exits non-zero on any non-success
            # StatusIB (task-7-report.md section 5, H2). rc=1 here
            # matches what run 5 actually captured.
            (1, "[TOO]     status = %s (FAILURE)," % deny_status),
            (0, "[TOO]     status = 0x00 (SUCCESS),"),
        ]
        calls = {"play": 0}

        def on_call(argv):
            if "play-chime-sound" not in argv:
                return
            calls["play"] += 1
            if calls["play"] <= 2 or disabled_pushes_urc:
                link.push_urcs(["+MTCMD:5,4,1366,0,7"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_8_chime(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_deny_status_not_failure_fails_the_check(self):
        ctx = self._ctx(deny_status="0x00")  # wrong: should be 0x01
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_8_chime(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_disabled_short_circuit_still_forwards_fails_the_check(self):
        # Regression shape: if Enabled=false ever failed to short-circuit
        # and a +MTCMD arrived anyway, assert_no_urc must catch it.
        ctx = self._ctx(disabled_pushes_urc=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_8_chime(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep39Opstate(unittest.TestCase):
    """T5 task 5: Pause/Start/Stop/Resume allow + deny (spec 3.21: the
    verdict IS the wire response here, unlike the valve), plus the
    in-state guard pin (a pause from Stopped never reaches +MTCMD)."""

    def _ctx(self, resume_ok=True, guard_leaks_forward=False):
        link = FakeLink({
            "AT+MTOPSTATE=7,0": (0, []),
            "AT+MTOPSTATE=7,1": (0, []),
            "AT+MTOPSTATE=7,2": (0, []),
            "AT+MTCMDRESP=1,1": (0, []),
            "AT+MTCMDRESP=2,1": (0, []),
            "AT+MTCMDRESP=3,1": (0, []),
            "AT+MTCMDRESP=4,1": (0, []),
            "AT+MTCMDRESP=5,0": (0, []),
        })
        resume_es = "0" if resume_ok else "1"
        script = [
            (0, "[TOO]       ErrorStateID: 0"),           # start
            (0, "[TOO]       ErrorStateID: 0"),           # pause allow
            (0, "[TOO]       ErrorStateID: %s" % resume_es),  # resume
            (0, "[TOO]       ErrorStateID: 0"),           # stop
            (0, "[TOO]       ErrorStateID: 2"),           # pause deny
            (0, "[TOO]   OperationalState: 1"),           # read after deny
            (0, "[TOO]       ErrorStateID: 3"),           # in-state guard
        ]
        counts = {"pause": 0}

        def on_call(argv):
            # argv here is chip.run's fully-built subprocess argv (the
            # binary prepended, --storage-directory appended), NOT the
            # ["operationalstate", verb, node, ep] list callers pass in
            # -- membership checks (matching every other step's on_call
            # in this file), not positional indexing.
            if "operationalstate" not in argv:
                return
            if "start" in argv:
                link.push_urcs(["+MTCMD:1,7,96,2"])
            elif "pause" in argv:
                counts["pause"] += 1
                if counts["pause"] == 1:
                    link.push_urcs(["+MTCMD:2,7,96,0"])
                elif counts["pause"] == 2:
                    link.push_urcs(["+MTCMD:5,7,96,0"])
                elif guard_leaks_forward:
                    link.push_urcs(["+MTCMD:6,7,96,0"])
            elif "resume" in argv:
                link.push_urcs(["+MTCMD:3,7,96,3"])
            elif "stop" in argv:
                link.push_urcs(["+MTCMD:4,7,96,1"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_9_opstate(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_wrong_error_state_id_on_allow_fails(self):
        ctx = self._ctx(resume_ok=False)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_9_opstate(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_guard_leaking_a_forward_fails(self):
        # If the in-state guard ever regressed and forwarded anyway,
        # assert_no_urc must catch it, not silently pass.
        ctx = self._ctx(guard_leaks_forward=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_9_opstate(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep310Smoke(unittest.TestCase):
    """T5 task 5: the self-test lifecycle twice in a row from a real
    controller (the B165/F-C10-2 regression pin), plus SmokeState
    Warning tracked both directions."""

    def _ctx(self, second_cycle_busy=False):
        link = FakeLink({
            "AT+MTATTR=5,92,0": (0, ["+MTATTR:5,92,0,0"]),
            "AT+MTALARM=5,5,0": (0, ["+MTATTR:5,92,5,0", "+MTATTR:5,92,0,0"]),
            "AT+MTALARM=5,1,1": (0, ["+MTATTR:5,92,1,1", "+MTATTR:5,92,0,1"]),
            "AT+MTALARM=5,1,0": (0, ["+MTATTR:5,92,1,0", "+MTATTR:5,92,0,0"]),
        })
        cycle2_text = ("[TOO]     status = 0x9c (BUSY)," if second_cycle_busy
                      else "")
        script = [
            (0, ""),                                              # baseline
            (0, ""),                                              # cycle1 request
            (0, "[TOO]   SelfTestComplete: {"),                   # cycle1 count
            (0, cycle2_text),                                     # cycle2 request
            (0, "\n".join(["[TOO]   SelfTestComplete: {"] * 2)),  # cycle2 count
            (0, "[TOO]   SmokeAlarm: {"),
            (0, "[TOO]   ExpressedState: 1"),
            (0, "[TOO]   ExpressedState: 0"),
        ]

        def on_call(argv):
            if "self-test-request" in argv:
                link.push_urcs(["+MTCMD:0,5,92,0"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_10_smoke(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_second_cycle_busy_fails_the_b165_pin(self):
        ctx = self._ctx(second_cycle_busy=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_10_smoke(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep311Power(unittest.TestCase):
    """T5 task 5: BatPercentRemaining written over AT+MTATTR in wire
    half-percent units, read back from the controller."""

    def _ctx(self, read_value="164"):
        link = FakeLink({
            "AT+MTATTR=6,47,12,164": (0, ["+MTATTR:6,47,12,164"]),
        })
        script = [(0, "[TOO]   BatPercentRemaining: %s" % read_value)]
        runner = FakeChipRunner(script)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_11_power(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_wrong_readback_fails(self):
        ctx = self._ctx(read_value="99")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_11_power(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep312LockSwitchLevels(unittest.TestCase):
    """T5 task 5: door lock state + forwarded lock command (both
    verdicts), the switch event, and the temp-levels verbatim
    read-back."""

    def _ctx(self, deny_rc=1, push_deny_urc=True):
        link = FakeLink({
            "AT+MTLOCK=8,1": (0, []),
            "AT+MTSWITCH=9": (0, []),
            'AT+MTTEMPLEVELS=10,"Low","Medium, high"': (0, []),
            "AT+MTCMDRESP=1,1": (0, []),
            "AT+MTCMDRESP=2,0": (0, []),
        })
        script = [
            (0, "[TOO]   LockState: 1"),
            (0, "[TOO]     status = 0x00 (SUCCESS),"),   # lock-door allow
            # Task-7 fix F4: pin WHICH failure, not just "some non-zero
            # exit" (task-7-report.md section 6, INFERENCE-WRONG). The
            # 0x1 body here is the harness self-test's fixture for the
            # derived expectation; task-7-fix-report.md marks the
            # production check itself awaiting bench re-confirmation.
            (deny_rc, "[TOO]     status = 0x01 (FAILURE),\n"
                      "[TOO] Run command failure"),      # lock-door deny
            (0, "[TOO]   InitialPress: {"),
            (0, "\n".join([
                "[TOO]   SupportedTemperatureLevels: 2 entries",
                "[TOO]     [1]: Low",
                "[TOO]     [2]: Medium, high",
            ])),
        ]
        calls = {"lock": 0}

        def on_call(argv):
            if "lock-door" not in argv:
                return
            calls["lock"] += 1
            if calls["lock"] == 1:
                link.push_urcs(["+MTCMD:1,8,257,0"])
            elif calls["lock"] == 2 and push_deny_urc:
                link.push_urcs(["+MTCMD:2,8,257,0"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_12_lock_switch_levels(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_deny_reported_as_success_fails(self):
        # Regression shape: if a denied LockDoor ever exited 0 (Success)
        # instead of failing, this must fail, not silently pass.
        ctx = self._ctx(deny_rc=0)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_12_lock_switch_levels(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_deny_forward_never_answered_fails(self):
        ctx = self._ctx(push_deny_urc=False)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_12_lock_switch_levels(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep315Rvc(unittest.TestCase):
    """RVC + Microwave batch, harness task 5: modes (explicit + tag-0
    default tags), ChangeToMode allow/deny, RvcOperationalState's
    Pause/Resume/GoHome each answered both ways, the 0x40-0x42 read-back
    trio, and the two GoHome no-forward server guards."""

    def _ctx(self, guard_leaks_forward=False, wrong_deny_fixture=False,
            ctm_modes=None):
        # ctm_modes, when given, collects every mode ChangeToMode was
        # actually invoked with, in call order: test_allow_then_deny_...
        # below uses this to pin the fix (allow 1, deny 0) rather than
        # just trusting that the step's checks all pass, since a
        # regression back to "deny asks for the same mode the allow just
        # installed" would still pass FakeChipRunner's scripted replies
        # (see that test's docstring for why).
        ctm_modes = [] if ctm_modes is None else ctm_modes
        link = FakeLink({
            "AT+MTOPSTATE=7,0x40": (1, []),
            "AT+MTOPSTATE=7,64": (1, []),
            'AT+MTMODES=12,84,0,16384,"Idle",1,0,"Cleaning"': (0, []),
            'AT+MTMODES=12,85,0,0,"Vacuum",1,0,"Mop"': (0, []),
            "AT+MTCMDRESP=1,1": (0, []),
            "AT+MTCMDRESP=2,0": (0, []),
            "AT+MTCMDRESP=3,1": (0, []),
            "AT+MTCMDRESP=4,1": (0, []),
            "AT+MTCMDRESP=5,1": (0, []),
            "AT+MTCMDRESP=6,0": (0, []),
            "AT+MTCMDRESP=7,0": (0, []),
            "AT+MTCMDRESP=8,0": (0, []),
            "AT+MTOPSTATE=12,1": (0, []),
            "AT+MTOPSTATE=12,2": (0, []),
            "AT+MTOPSTATE=12,64": (0, []),
            "AT+MTOPSTATE=12,65": (0, []),
            "AT+MTOPSTATE=12,66": (0, []),
            "AT+MTOPSTATE=12,0x42": (0, []),
            "AT+MTOPSTATE=12,0x40": (0, []),
            "AT+MTOPSTATE=12,0": (0, []),
        })
        deny_ctm_fixture = ("changetomode-response-allow-synthetic.txt"
                           if wrong_deny_fixture else
                           "changetomode-response-deny-synthetic.txt")
        script = [
            (0, fixture(os.path.join(
                "rvc-microwave", "rvcrunmode-supported-modes-synthetic.txt"))),
            (0, fixture(os.path.join(
                "rvc-microwave", "changetomode-response-allow-synthetic.txt"))),
            (0, fixture(os.path.join("rvc-microwave", deny_ctm_fixture))),
            (0, "[TOO]   CurrentMode: 1"),         # CurrentMode after deny
            (0, "[TOO]       ErrorStateID: 0"),   # pause allow
            (0, "[TOO]       ErrorStateID: 0"),   # resume allow
            (0, "[TOO]       ErrorStateID: 0"),   # go-home allow
            (0, "[TOO]       ErrorStateID: 2"),   # pause deny
            (0, "[TOO]       ErrorStateID: 2"),   # resume deny
            (0, "[TOO]       ErrorStateID: 2"),   # go-home deny
            (0, "[TOO]   OperationalState: 64"),  # 0x40 read-back
            (0, "[TOO]   OperationalState: 65"),  # 0x41 read-back
            (0, "[TOO]   OperationalState: 66"),  # 0x42 read-back
            (0, "[TOO]       ErrorStateID: 3"),   # go-home guard: Docked
            (0, "[TOO]       ErrorStateID: 0"),   # go-home guard: SeekingCharger
        ]
        counts = {"pause": 0, "resume": 0, "go-home": 0, "ctm": 0}

        def on_call(argv):
            if "rvcrunmode" in argv and "change-to-mode" in argv:
                # The mode requested is the positional argument right
                # after "change-to-mode" in chip.run's fully-built argv;
                # reading it back (rather than hardcoding "1" for every
                # call) is what makes this fixture actually exercise the
                # fix (allow mode 1, deny mode 0) instead of masking a
                # regression back to the same-mode short-circuit bug
                # (Task 9, mode-base-server.cpp:401-410) the same way the
                # old fixture did.
                mode = argv[argv.index("change-to-mode") + 1]
                ctm_modes.append(mode)
                counts["ctm"] += 1
                seq = 1 if counts["ctm"] == 1 else 2
                link.push_urcs(["+MTCMD:%d,12,84,0,%s" % (seq, mode)])
                return
            if "rvcoperationalstate" not in argv:
                return
            if "pause" in argv:
                counts["pause"] += 1
                if counts["pause"] == 1:
                    link.push_urcs(["+MTCMD:3,12,97,0"])
                elif counts["pause"] == 2:
                    link.push_urcs(["+MTCMD:6,12,97,0"])
            elif "resume" in argv:
                counts["resume"] += 1
                if counts["resume"] == 1:
                    link.push_urcs(["+MTCMD:4,12,97,3"])
                elif counts["resume"] == 2:
                    link.push_urcs(["+MTCMD:7,12,97,3"])
            elif "go-home" in argv:
                counts["go-home"] += 1
                if counts["go-home"] == 1:
                    link.push_urcs(["+MTCMD:5,12,97,128"])
                elif counts["go-home"] == 2:
                    link.push_urcs(["+MTCMD:8,12,97,128"])
                elif guard_leaks_forward:
                    # Regression shape: if a guard ever regressed and
                    # forwarded anyway, assert_no_urc must catch it.
                    link.push_urcs(["+MTCMD:9,12,97,128"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_15_rvc(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_guard_leaking_a_forward_fails(self):
        ctx = self._ctx(guard_leaks_forward=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_15_rvc(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_wrong_change_to_mode_status_fails(self):
        # A corrupted deny fixture (status 0/kSuccess instead of 2/
        # kGenericFailure) must fail the check, not silently pass -- pins
        # that parse_change_to_mode_status's value is actually asserted,
        # not just "some response arrived".
        ctx = self._ctx(wrong_deny_fixture=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_15_rvc(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_allow_then_deny_uses_a_different_mode(self):
        """Task 9 bench finding pin: the deny must ask for a mode
        different from the one the allow just installed as CurrentMode,
        or mode-base-server.cpp:401-410's same-mode short-circuit answers
        kSuccess before the delegate is ever consulted, raising no +MTCMD
        at all (task-9-report.md section 6, both failing checks traced to
        this exact line, reproduced live on the WiFi bench).

        FakeChipRunner has no model of CurrentMode at all: it answers
        whatever the scripted line says regardless of which mode was
        actually requested, so it could not have caught the original bug
        on its own (the deny's fixture was simply wrong for the sequence
        the step was really issuing, and nothing in this double would
        have noticed the mismatch). That is why this test inspects the
        actual argv the step sent, via ctm_modes, rather than only
        trusting that step_3_15_rvc's own checks passed: checking argv is
        the one thing standing between this test suite and silently
        re-accepting the same-mode regression the bench had to find.
        Building a real CurrentMode model into FakeChipRunner to catch
        this class of bug directly is deliberately out of scope this
        round (YAGNI): the bench is what actually validates command
        semantics against the live SDK, and the harness's job is the
        wire protocol and assertion machinery around it, not
        re-implementing ModeBase server logic."""
        modes = []
        ctx = self._ctx(ctm_modes=modes)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_15_rvc(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertEqual(modes, ["1", "0"])


class TestStep316Microwave(unittest.TestCase):
    """RVC + Microwave batch, harness task 5: mode list read-back,
    SetCookingParameters allow (four-field payload) and deny, cook-time/
    power-setting read-back, AddMoreTime's absolute finalCookTimeSec
    payload, and the washer-rule opstate spot check."""

    def _ctx(self, deny_rc=1):
        link = FakeLink({
            'AT+MTMODES=13,94,0,0,"Normal"': (0, []),
            "AT+MTCMDRESP=1,1": (0, []),
            "AT+MTCMDRESP=2,0": (0, []),
            "AT+MTCMDRESP=3,1": (0, []),
            "AT+MTOPSTATE=13,0": (0, []),
        })
        script = [
            (0, "\n".join([
                "[TOO]   SupportedModes: 1 entries",
                "[TOO]     [1]: {",
                "[TOO]       Label: Normal",
                "[TOO]       Mode: 0",
                "[TOO]       ModeTags: 1 entries",
                "[TOO]         [1]: {",
                "[TOO]           Value: 16384",
                "[TOO]          }",
                "[TOO]      }",
            ])),
            (0, "[TOO]     status = 0x00 (SUCCESS),"),   # set-cooking allow
            (0, "[TOO]   CookTime: 90"),                 # cook-time read
            (0, "[TOO]   PowerSetting: 80"),              # power-setting read
            (deny_rc, "[TOO]     status = 0x01 (FAILURE),\n"
                      "[TOO] Run command failure"),        # set-cooking deny
            (0, "[TOO]   CookTime: 90"),                  # cook-time re-read
            (0, "[TOO]     status = 0x00 (SUCCESS),"),    # add-more-time allow
            (0, "[TOO]   CookTime: 120"),                 # cook-time after add
            (0, "[TOO]       ErrorStateID: 3"),           # opstate spot check
        ]
        calls = {"scp": 0}

        def on_call(argv):
            if "set-cooking-parameters" in argv:
                calls["scp"] += 1
                if calls["scp"] == 1:
                    link.push_urcs(["+MTCMD:1,13,95,0,0,90,80,0"])
                else:
                    link.push_urcs(["+MTCMD:2,13,95,0,0,60,50,0"])
            elif "add-more-time" in argv:
                link.push_urcs(["+MTCMD:3,13,95,1,120"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_16_microwave(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_deny_reported_success_fails(self):
        # Regression shape: if a denied SetCookingParameters ever exited 0
        # (Success) instead of Failure, this must fail, not silently pass.
        ctx = self._ctx(deny_rc=0)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_16_microwave(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_add_more_time_payload_is_absolute_not_delta(self):
        # If the harness ever asserted the payload against the 30 s delta
        # chip-tool sent instead of the server-computed absolute
        # finalCookTimeSec (120), the forward would go unanswered and the
        # step must fail, not silently accept the wrong shape.
        link = FakeLink({
            'AT+MTMODES=13,94,0,0,"Normal"': (0, []),
            "AT+MTCMDRESP=1,1": (0, []), "AT+MTCMDRESP=2,0": (0, []),
            "AT+MTOPSTATE=13,0": (0, []),
        })
        script = [
            (0, "\n".join([
                "[TOO]   SupportedModes: 1 entries",
                "[TOO]     [1]: {  Label: Normal",
                "[TOO]       ModeTags: 1 entries",
                "[TOO]         [1]: {  Value: 16384",
            ])),
            (0, "[TOO]     status = 0x00 (SUCCESS),"),
            (0, "[TOO]   CookTime: 90"),
            (0, "[TOO]   PowerSetting: 80"),
            (1, "[TOO]     status = 0x01 (FAILURE),\n[TOO] Run command "
                "failure"),
            (0, "[TOO]   CookTime: 90"),
            (0, "[TOO]     status = 0x00 (SUCCESS),"),
            (0, "[TOO]   CookTime: 90"),            # unchanged: wrong payload
                                                     # meant the forward was
                                                     # never actually answered
            (0, "[TOO]       ErrorStateID: 3"),     # opstate spot check
        ]
        calls = {"scp": 0}

        def on_call(argv):
            if "set-cooking-parameters" in argv:
                calls["scp"] += 1
                if calls["scp"] == 1:
                    link.push_urcs(["+MTCMD:1,13,95,0,0,90,80,0"])
                else:
                    link.push_urcs(["+MTCMD:2,13,95,0,0,60,50,0"])
            elif "add-more-time" in argv:
                # wrong: pushes the 30 s delta instead of the absolute
                # finalCookTimeSec (120) -- must not satisfy payload=120
                link.push_urcs(["+MTCMD:3,13,95,1,30"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_16_microwave(ctx)
        self.assertGreater(ctx.suite.failed, 0)


def phase3_full_ep_readback():
    """The complete AT+MTEP? readback for PHASE3_COMPOSITION, the same
    expected-lines construction step_3_1_compose asserts against: a
    parented entry string ("0x0071,0,13") renders as the wire's own
    five-field line verbatim."""
    return ["+MTEP:%d,%d,%s" % (i, slot, dt)
            for i, (slot, dt) in enumerate(PHASE3_COMPOSITION)]


class TestStep317ComposedFridge(unittest.TestCase):
    """Composed appliance round, task 6: the composed refrigerator's
    five-field +MTEP? lines, PartsList/ServerList reads, the Cooler
    conditional cluster present-on-15/absent-on-11 pair, cluster-aware
    AT+MTMODES on 0x52, ChangeToMode allow/deny (B196 different-mode
    rule), and the AT+MTALARM fridge rows with the Notify event."""

    PARTS14 = "\n".join([
        "[TOO]   PartsList: 2 entries",
        "[TOO]     [1]: 15",
        "[TOO]     [2]: 16",
    ])
    SERVER15 = "\n".join([
        "[TOO]   ServerList: 4 entries",
        "[TOO]     [1]: 3 (Identify)",
        "[TOO]     [2]: 29 (Descriptor)",
        "[TOO]     [3]: 82 (RefrigeratorAndTemperatureControlledCabinetMode)",
        "[TOO]     [4]: 86 (TemperatureControl)",
    ])
    SERVER11 = "\n".join([
        "[TOO]   ServerList: 3 entries",
        "[TOO]     [1]: 3 (Identify)",
        "[TOO]     [2]: 29 (Descriptor)",
        "[TOO]     [3]: 86 (TemperatureControl)",
    ])
    MODES15 = "\n".join([
        "[TOO]   SupportedModes: 2 entries",
        "[TOO]     [1]: {",
        "[TOO]       Label: Auto",
        "[TOO]       Mode: 0",
        "[TOO]       ModeTags: 1 entries",
        "[TOO]         [1]: {",
        "[TOO]           Value: 0",
        "[TOO]          }",
        "[TOO]      }",
        "[TOO]     [2]: {",
        "[TOO]       Label: Rapid",
        "[TOO]       Mode: 1",
        "[TOO]       ModeTags: 1 entries",
        "[TOO]         [1]: {",
        "[TOO]           Value: 0",
        "[TOO]          }",
        "[TOO]      }",
    ])
    NOTIFY14 = "\n".join([
        "[TOO] Endpoint: 14 Cluster: 0x0000_0057 Event 0x0000_0000",
        "[TOO]   Notify: {",
        "[TOO]     Active: 1",
        "[TOO]     Inactive: 0",
        "[TOO]     State: 1",
        "[TOO]     Mask: 1",
        "[TOO]    }",
    ])

    def _ctx(self, server11=None, notify=None, ctm_modes=None):
        # ctm_modes: same argv-inspection seam TestStep315Rvc uses to pin
        # the B196 different-mode rule against what the step actually
        # sent, not just against scripted replies (FakeChipRunner has no
        # CurrentMode model, so only the argv can betray a regression).
        ctm_modes = [] if ctm_modes is None else ctm_modes
        link = FakeLink({
            "AT+MTEP?": (0, phase3_full_ep_readback()),
            'AT+MTMODES=15,0x52,0,0,"Auto"': (0, []),
            'AT+MTMODES=15,0x52,0,0,"Auto",1,0,"Rapid"': (0, []),
            "AT+MTCMDRESP=1,1": (0, []),
            "AT+MTCMDRESP=2,0": (0, []),
            "AT+MTALARM=14,0,1": (0, []),
            "AT+MTALARM=14,1,1": (1, []),
            "AT+MTALARM=14,0,2": (1, []),
            "AT+MTALARM=14,0,0": (0, []),
        })
        script = [
            (0, self.PARTS14),
            (0, self.SERVER15),
            (0, server11 if server11 is not None else self.SERVER11),
            (0, self.MODES15),
            (0, fixture(os.path.join(
                "rvc-microwave", "changetomode-response-allow-synthetic.txt"))),
            (0, fixture(os.path.join(
                "rvc-microwave", "changetomode-response-deny-synthetic.txt"))),
            (0, "[TOO]   CurrentMode: 1"),
            (0, "[TOO]   State: 1"),
            (0, notify if notify is not None else self.NOTIFY14),
        ]

        def on_call(argv):
            if ("refrigeratorandtemperaturecontrolledcabinetmode" in argv
                    and "change-to-mode" in argv):
                mode = argv[argv.index("change-to-mode") + 1]
                ctm_modes.append(mode)
                seq = len(ctm_modes)
                link.push_urcs(["+MTCMD:%d,15,82,0,%s" % (seq, mode)])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_17_composed_fridge(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_standalone_cabinet_carrying_cooler_cluster_fails(self):
        # The parent-conditional derivation pin: if the standalone
        # cabinet ever grew cluster 82, the absence check must fail.
        ctx = self._ctx(server11=self.SERVER15)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_17_composed_fridge(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_missing_notify_event_fails(self):
        ctx = self._ctx(notify="[TOO] No events")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_17_composed_fridge(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_allow_then_deny_uses_a_different_mode(self):
        # B196 (mode-base-server.cpp:401-410): a deny that repeats the
        # allow's mode never reaches the delegate at all, so the pair
        # must differ. Same argv-inspection reasoning as TestStep315Rvc's
        # sibling test: scripted replies alone cannot catch a regression
        # back to a same-mode pair.
        modes = []
        ctx = self._ctx(ctm_modes=modes)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_17_composed_fridge(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertEqual(modes, ["1", "0"])


class TestStep318OvenCavity(unittest.TestCase):
    """Composed appliance round, task 6: the oven cavity's conditional
    cluster pair, OvenMode staging/read-back and ChangeToMode both ways,
    the hand-rolled OvenCavityOperationalState Stop/Start entries
    adjudicated both ways, the disallowConform Pause probe (0x81, no
    forward), and the AT+MTOPSTATE membership rows."""

    def _ctx(self, pause_reply=None, pause_leaks_forward=False):
        link = FakeLink({
            'AT+MTMODES=18,0x49,0,0,"Bake",1,16386,"Grill"': (0, []),
            'AT+MTMODES=18,0x52,0,0,"Auto"': (3, []),
            "AT+MTCMDRESP=1,1": (0, []),
            "AT+MTCMDRESP=2,0": (0, []),
            "AT+MTCMDRESP=3,1": (0, []),
            "AT+MTCMDRESP=4,0": (0, []),
            "AT+MTCMDRESP=5,1": (0, []),
            "AT+MTCMDRESP=6,0": (0, []),
            "AT+MTOPSTATE=18,0": (0, []),
            "AT+MTOPSTATE=18,1": (0, []),
            "AT+MTOPSTATE=18,0x40": (1, []),
        })
        script = [
            (0, "[TOO]   PartsList: 1 entries\n[TOO]     [1]: 18"),
            (0, "\n".join([
                "[TOO]   ServerList: 4 entries",
                "[TOO]     [1]: 29 (Descriptor)",
                "[TOO]     [2]: 72 (OvenCavityOperationalState)",
                "[TOO]     [3]: 73 (OvenMode)",
                "[TOO]     [4]: 86 (TemperatureControl)",
            ])),
            (0, "\n".join([
                "[TOO]   SupportedModes: 2 entries",
                "[TOO]     [1]: {",
                "[TOO]       Label: Bake",
                "[TOO]       Mode: 0",
                "[TOO]       ModeTags: 1 entries",
                "[TOO]         [1]: {",
                "[TOO]           Value: 16384",
                "[TOO]          }",
                "[TOO]      }",
                "[TOO]     [2]: {",
                "[TOO]       Label: Grill",
                "[TOO]       Mode: 1",
                "[TOO]       ModeTags: 1 entries",
                "[TOO]         [1]: {",
                "[TOO]           Value: 16386",
                "[TOO]          }",
                "[TOO]      }",
            ])),
            (0, fixture(os.path.join(
                "rvc-microwave", "changetomode-response-allow-synthetic.txt"))),
            (0, fixture(os.path.join(
                "rvc-microwave", "changetomode-response-deny-synthetic.txt"))),
            (0, "[TOO]   CurrentMode: 1"),
            (0, "[TOO]       ErrorStateID: 0"),   # start allow
            (0, "[TOO]       ErrorStateID: 2"),   # stop deny
            (0, "[TOO]   OperationalState: 1"),   # unchanged by the deny
            (0, "[TOO]       ErrorStateID: 0"),   # stop allow
            (0, "[TOO]       ErrorStateID: 2"),   # start deny
            pause_reply if pause_reply is not None else
            (1, "[TOO]     status = 0x81 (UNSUPPORTED_COMMAND),\n"
                "[TOO] Run command failure"),
            (0, "[TOO]   OperationalState: 1"),   # membership row read-back
        ]
        counts = {"start": 0, "stop": 0, "ctm": 0}

        def on_call(argv):
            if "ovenmode" in argv and "change-to-mode" in argv:
                mode = argv[argv.index("change-to-mode") + 1]
                counts["ctm"] += 1
                link.push_urcs(["+MTCMD:%d,18,73,0,%s"
                                % (counts["ctm"], mode)])
                return
            if "ovencavityoperationalstate" not in argv or "read" in argv:
                return
            if "command-by-id" in argv:
                if pause_leaks_forward:
                    # Regression shape: a Pause entry sneaking into the
                    # hand-rolled shell would dispatch and forward;
                    # assert_no_urc must catch it.
                    link.push_urcs(["+MTCMD:9,18,72,0"])
                return
            if "start" in argv:
                counts["start"] += 1
                seq = 3 if counts["start"] == 1 else 6
                link.push_urcs(["+MTCMD:%d,18,72,2" % seq])
            elif "stop" in argv:
                counts["stop"] += 1
                seq = 4 if counts["stop"] == 1 else 5
                link.push_urcs(["+MTCMD:%d,18,72,1" % seq])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_18_oven_cavity(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_pause_answering_success_fails(self):
        # The wire-only observability net (the 8.6 disease): if a Pause
        # entry ever appeared in the shell's AcceptedCommandList, the
        # invoke would stop answering 0x81; both pause checks must fail.
        ctx = self._ctx(pause_reply=(0,
                        "[TOO]     status = 0x00 (SUCCESS),"))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_18_oven_cavity(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_pause_leaking_a_forward_fails(self):
        ctx = self._ctx(pause_leaks_forward=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_18_oven_cavity(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep319CookSurface(unittest.TestCase):
    """Composed appliance round, task 6: the cook surface's PartsList,
    the OnOff OffOnly pair (off accepted with the AT-side read, on
    rejected 0x81), and the temperature setpoint host-write/controller-
    read round trip. No CmdResponder anywhere: nothing on this endpoint
    forwards."""

    def _ctx(self, on_reply=None):
        link = FakeLink({
            "AT+MTATTR=20,6,0": [(0, ["+MTATTR:20,6,0,0"]),
                                 (0, ["+MTATTR:20,6,0,0"])],
            "AT+MTATTR=20,86,0,2500": (0, ["+MTATTR:20,86,0,2500"]),
        })
        script = [
            (0, "[TOO]   PartsList: 1 entries\n[TOO]     [1]: 20"),
            (0, "[TOO]     status = 0x00 (SUCCESS),"),
            on_reply if on_reply is not None else
            (1, "[TOO]     status = 0x81 (UNSUPPORTED_COMMAND),\n"
                "[TOO] Run command failure"),
            (0, "[TOO]   TemperatureSetpoint: 2500"),
        ]
        runner = FakeChipRunner(script)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_19_cook_surface(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_on_accepted_fails(self):
        # OffOnly pin: if On ever stopped answering 0x81 (say a device
        # type edit swapped the cluster create for the light's), both
        # rejection checks must fail, not silently pass.
        ctx = self._ctx(on_reply=(0, "[TOO]     status = 0x00 (SUCCESS),"))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_19_cook_surface(ctx)
        self.assertGreater(ctx.suite.failed, 0)


SERVER21_SENSOR = "\n".join([
    "[TOO]   ServerList: 4 entries",
    "[TOO]     [1]: 3 (Identify)",
    "[TOO]     [2]: 29 (Descriptor)",
    "[TOO]     [3]: 144 (ElectricalPowerMeasurement)",
    "[TOO]     [4]: 156 (PowerTopology)",
])
SERVER22_METER = "\n".join([
    "[TOO]   ServerList: 4 entries",
    "[TOO]     [1]: 3 (Identify)",
    "[TOO]     [2]: 29 (Descriptor)",
    "[TOO]     [3]: 144 (ElectricalPowerMeasurement)",
    "[TOO]     [4]: 145 (ElectricalEnergyMeasurement)",
])


class TestStep320ElectricalSensor(unittest.TestCase):
    """Energy round A, task 5: the variant-1 sensor's cluster-set
    presence/absence pair, the 6.2 endpoint-dependent MTMEAS rows, the
    power push with controller read-back, and the Breadcrumb u64 ember
    round trip (write echo-less, explicit read-back, UINT64_MAX, restore
    0)."""

    def _ctx(self, server21=None, breadcrumb_reads=None):
        link = FakeLink({
            "AT+MTMEAS=99,144,0,5": (2, []),
            "AT+MTMEAS=1,144,0,5": (3, []),
            "AT+MTMEAS=21,6,0,5": (3, []),
            "AT+MTMEAS=21,144,99,5": (1, []),
            "AT+MTMEAS=21,144,3,-50": (1, []),
            "AT+MTMEAS=21,145,0,1500000": (3, []),
            "AT+MTMEAS=21,144,0,230000,1,433,2,99590": (0, []),
            "AT+MTATTR=0,0x30,0,4294967296": (0, []),
            "AT+MTATTR=0,0x30,0,18446744073709551615": (0, []),
            "AT+MTATTR=0,0x30,0,0": (0, []),
            "AT+MTATTR=0,0x30,0": breadcrumb_reads if breadcrumb_reads
            is not None else [
                (0, ["+MTATTR:0,48,0,4294967296"]),
                (0, ["+MTATTR:0,48,0,18446744073709551615"]),
                (0, ["+MTATTR:0,48,0,0"]),
            ],
        })
        script = [
            (0, server21 if server21 is not None else SERVER21_SENSOR),
            (0, "[TOO]   Voltage: 230000"),
            (0, "[TOO]   ActiveCurrent: 433"),
            (0, "[TOO]   ActivePower: 99590"),
        ]
        runner = FakeChipRunner(script)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_20_electrical_sensor(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        # The no-command-traffic pin, transcript half: nothing this step
        # sent adjudicates a forward (the clusters carry no commands).
        self.assertFalse(
            any("MTCMDRESP" in c for c in ctx.link.sent))

    def test_eem_present_on_the_sensor_fails(self):
        # The variant pin: a variant-0 sensor sneaking into slot 21
        # would carry EEM and quietly absorb the energy-push +MTERR:3
        # row's meaning; the absence check must fail loudly instead.
        ctx = self._ctx(server21=SERVER22_METER)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_20_electrical_sensor(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_breadcrumb_truncated_readback_fails(self):
        # The 64-bit regression shape the row exists for: a pipeline
        # that silently truncated 2^32 to 32 bits reads back 0.
        ctx = self._ctx(breadcrumb_reads=[
            (0, ["+MTATTR:0,48,0,0"]),
            (0, ["+MTATTR:0,48,0,18446744073709551615"]),
            (0, ["+MTATTR:0,48,0,0"]),
        ])
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_20_electrical_sensor(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_breadcrumb_write_echo_line_fails(self):
        # Managed-internally attributes bypass the update callback, so
        # a write must answer a bare OK. An echo line appearing would
        # mean the ember/esp-matter storage split changed under us:
        # surface it.
        ctx = self._ctx()
        ctx.link.commands["AT+MTATTR=0,0x30,0,4294967296"] = (
            0, ["+MTATTR:0,48,0,4294967296"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_20_electrical_sensor(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep321ElectricalMeter(unittest.TestCase):
    """Energy round A, task 5: the variant-0 meter's full surface: the
    presence pair with the no-topology absence, the beyond-32-bit
    ActivePower push/read, the ActivePower subscription report, and the
    two energy pushes with attribute read-back and both events."""

    ENERGY_ATTR_1 = "\n".join([
        "[TOO]   CumulativeEnergyImported: {",
        "[TOO]     Energy: 1500000",
        "[TOO]     EndSystime: 100000",
        "[TOO]    }",
    ])
    ENERGY_ATTR_2 = "\n".join([
        "[TOO]   CumulativeEnergyImported: {",
        "[TOO]     Energy: 1600000",
        "[TOO]     EndSystime: 123456",
        "[TOO]    }",
    ])
    ENERGY_EVENTS = "\n".join([
        "[TOO]   CumulativeEnergyMeasured: {",
        "[TOO]     EnergyImported: {",
        "[TOO]       Energy: 1500000",
        "[TOO]       EndSystime: 100000",
        "[TOO]      }",
        "[TOO]    }",
        "[TOO]   CumulativeEnergyMeasured: {",
        "[TOO]     EnergyImported: {",
        "[TOO]       Energy: 1600000",
        "[TOO]       EndSystime: 123456",
        "[TOO]      }",
        "[TOO]     EnergyExported: {",
        "[TOO]       Energy: 20000",
        "[TOO]       EndSystime: 123456",
        "[TOO]      }",
        "[TOO]    }",
    ])

    def _ctx(self, sub=None, server22=None, active_power=None,
             events=None):
        link = FakeLink({
            "AT+MTMEAS=22,144,0,230000,1,433,2,4294967297": (0, []),
            "AT+MTMEAS=22,144,2,99590": (0, []),
            "AT+MTMEAS=22,145,0,1500000": (0, []),
            "AT+MTMEAS=22,145,0,1600000,1,20000": (0, []),
        })
        script = [
            (0, server22 if server22 is not None else SERVER22_METER),
            (0, "[TOO]   Voltage: 230000"),
            (0, "[TOO]   ActiveCurrent: 433"),
            (0, active_power if active_power is not None
             else "[TOO]   ActivePower: 4294967297"),
            (0, self.ENERGY_ATTR_1),
            (0, self.ENERGY_ATTR_2),
            (0, events if events is not None else self.ENERGY_EVENTS),
        ]
        runner = FakeChipRunner(script)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        sub = sub if sub is not None else FakeSubscriber(counts=[1, 2])
        ctx.subscriber_factory = lambda: sub
        ctx._sub = sub
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_21_electrical_meter(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertTrue(ctx._sub.stopped)
        # The no-command-traffic pin, transcript half.
        self.assertFalse(
            any("MTCMDRESP" in c for c in ctx.link.sent))

    def test_topology_present_on_the_meter_fails(self):
        # The other half of the flip pin: the meter has no topology
        # cluster at all (spec 3.9); a sensor build in slot 22 would
        # carry 156 and must fail the absence check.
        ctx = self._ctx(server22=SERVER21_SENSOR)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_21_electrical_meter(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_active_power_truncated_fails(self):
        # 4294967297 truncated to 32 bits is 1: the exact regression
        # shape the beyond-2^32 value exists to catch.
        ctx = self._ctx(active_power="[TOO]   ActivePower: 1")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_21_electrical_meter(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_missing_change_report_fails(self):
        # Subscription sees no new report after the push: the report
        # check must fail (and the subscriber still be stopped).
        ctx = self._ctx(sub=FakeSubscriber(counts=[1, 1]))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_21_electrical_meter(ctx)
        self.assertGreater(ctx.suite.failed, 0)
        self.assertTrue(ctx._sub.stopped)

    def test_subscriber_start_failure_scores_but_continues(self):
        # Unlike step_2_4 (whose whole later chain needs the fabric),
        # a dead subscription invalidates nothing after sub.stop():
        # the energy half must still run and pass, with the start and
        # report checks failed and the check COUNT unchanged (baseline
        # comparability).
        ctx = self._ctx(sub=FakeSubscriber(counts=[0], start_ok=False))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_21_electrical_meter(ctx)
        self.assertEqual(ctx.suite.failed, 2)
        self.assertTrue(ctx._sub.stopped)

    def test_single_event_fails(self):
        # NotifyCumulativeEnergyMeasured emits one event per push (spec
        # 3.25's "one call, both effects"); a second push whose event
        # vanished is exactly what the >= 2 pin is for.
        one_event = "\n".join([
            "[TOO]   CumulativeEnergyMeasured: {",
            "[TOO]     EnergyImported: {",
            "[TOO]       Energy: 1500000",
            "[TOO]      }",
            "[TOO]    }",
        ])
        ctx = self._ctx(events=one_event)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_21_electrical_meter(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestMeasurementStepsScriptNoCommandTraffic(unittest.TestCase):
    """Energy round A, task 5 (extended by round B's heat pump): the two
    measurement clusters and PowerTopology declare NO accepted commands
    (spec 3.25/3.9), and the heat pump endpoint carries nothing but them
    plus PowerSource (also command-less), so none of these steps may
    script any command-forward traffic. The transcript half of this pin
    lives in the happy-path tests (no AT+MTCMDRESP ever sent); this is
    the source half, so a future edit that reaches for the forward
    machinery on these endpoints trips a named test instead of quietly
    acquiring an adjudication path the device would never exercise.
    Energy round C1 extends the list with solar power (whose endpoint is
    the same sensor-plus-PowerSource shape) and battery storage: the
    battery DOES carry a DeviceEnergyManagement cluster, but this
    harness deliberately runs the whole PowerAdjust protocol once, on
    the standalone ESA at slot 27, so its step scripts no forwards
    either. The water heater and DEM steps are deliberately NOT here:
    their endpoints carry Boost/CancelBoost, ChangeToMode and the two
    PowerAdjust commands, real forwards."""

    def test_step_sources_use_no_forward_machinery(self):
        import inspect
        for fn in (step_3_20_electrical_sensor,
                   step_3_21_electrical_meter,
                   step_3_23_heat_pump,
                   step_3_24_solar_power,
                   step_3_25_battery_storage):
            src = inspect.getsource(fn)
            self.assertNotIn("CmdResponder", src)
            self.assertNotIn("invoke_chip", src)
            self.assertNotIn("MTCMDRESP", src)


SERVER23_WH = "\n".join([
    "[TOO]   ServerList: 7 entries",
    "[TOO]     [1]: 29 (Descriptor)",
    "[TOO]     [2]: 513 (Thermostat)",
    "[TOO]     [3]: 148 (WaterHeaterManagement)",
    "[TOO]     [4]: 158 (WaterHeaterMode)",
    "[TOO]     [5]: 156 (PowerTopology)",
    "[TOO]     [6]: 144 (ElectricalPowerMeasurement)",
    "[TOO]     [7]: 145 (ElectricalEnergyMeasurement)",
])
SERVER24_HP = "\n".join([
    "[TOO]   ServerList: 5 entries",
    "[TOO]     [1]: 29 (Descriptor)",
    "[TOO]     [2]: 47 (PowerSource)",
    "[TOO]     [3]: 156 (PowerTopology)",
    "[TOO]     [4]: 144 (ElectricalPowerMeasurement)",
    "[TOO]     [5]: 145 (ElectricalEnergyMeasurement)",
])
DEVTYPES24_HP = "\n".join([
    "[TOO]   DeviceTypeList: 3 entries",
    "[TOO]     [1]: {",
    "[TOO]       DeviceType: 777 (Heat Pump)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
    "[TOO]     [2]: {",
    "[TOO]       DeviceType: 17 (Power Source)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
    "[TOO]     [3]: {",
    "[TOO]       DeviceType: 1296 (Electrical Sensor)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
])
BOOST_STARTED_ONE = "\n".join([
    "[TOO]   BoostStarted: {",
    "[TOO]     BoostInfo: {",
    "[TOO]       Duration: 3600",
    "[TOO]       OneShot: TRUE",
    "[TOO]       TargetPercentage: 80",
    "[TOO]      }",
    "[TOO]    }",
])
BOOST_ENDED_ONE = "\n".join([
    "[TOO]   BoostEnded: {",
    "[TOO]    }",
])
WHM_MODES = "\n".join([
    "[TOO]   SupportedModes: 2 entries",
    "[TOO]     [1]: {",
    "[TOO]       Label: Manual",
    "[TOO]       Mode: 0",
    "[TOO]       ModeTags: 1 entries",
    "[TOO]         [1]: {",
    "[TOO]           Value: 16385",
    "[TOO]          }",
    "[TOO]      }",
    "[TOO]     [2]: {",
    "[TOO]       Label: Timed",
    "[TOO]       Mode: 1",
    "[TOO]       ModeTags: 1 entries",
    "[TOO]         [1]: {",
    "[TOO]           Value: 16386",
    "[TOO]          }",
    "[TOO]      }",
])
CTM_STATUS_0 = "[TOO]     status: 0\n[TOO]     statusText: "


class TestStep322WaterHeater(unittest.TestCase):
    """Energy round B, task 4: the FULL water heater's server list (SDK
    trio plus the sensor graft), the 6.2 full-variant 0x94 negatives,
    the pushes with controller read-back (EstimatedHeatRequired above
    2^32), the in-state guard (Success, no forward, no event), the
    canonical Boost vector adjudicated through CmdResponder, the
    derived-event chain (one BoostStarted, same-state silence, one
    BoostEnded), Thermostat both directions, and WaterHeaterMode with
    the B196 same-mode short-circuit."""

    def _ctx(self, boost_urc="+MTCMD:1,23,148,0,3600,265,80",
             guard_leaks_forward=False, shortcircuit_leaks_forward=False,
             thermostat_urc=True, second_boost_started=None,
             boost_started_first=None):
        link = FakeLink({
            "AT+MTMEAS=23,148,9,1": (1, []),
            "AT+MTMEAS=23,148,4,-5": (1, []),
            "AT+MTMEAS=23,148,0,4,1,4": (0, []),
            "AT+MTMEAS=23,148,3,300,4,4294967297,5,60": (0, []),
            "AT+MTMEAS=23,148,2,1": (0, []),
            "AT+MTMEAS=23,148,2,0": (0, []),
            "AT+MTCMDRESP=1,1": (0, []),
            "AT+MTCMDRESP=2,1": (0, []),
            "AT+MTCMDRESP=3,1": (0, []),
            "AT+MTATTR=23,513,18": (0, ["+MTATTR:23,513,18,2200"]),
            "AT+MTATTR=23,0x0201,0,2150": (0, ["+MTATTR:23,513,0,2150"]),
            'AT+MTMODES=23,158,0,0,"Manual",1,16386,"Timed"': (0, []),
        })
        first_started = (boost_started_first if boost_started_first
                         is not None else BOOST_STARTED_ONE)
        second_started = (second_boost_started if second_boost_started
                          is not None else first_started)
        script = [
            (0, SERVER23_WH),
            (0, "[TOO]   HeaterTypes: 4"),
            (0, "[TOO]   HeatDemand: 4"),
            (0, "[TOO]   TankVolume: 300"),
            (0, "[TOO]   EstimatedHeatRequired: 4294967297"),
            (0, "[TOO]   TankPercentage: 60"),
            (0, ""),                              # cancel-boost, guard
            (0, ""),                              # boost-ended read: none
            (0, ""),                              # boost invoke
            (0, "[TOO]   BoostState: 0"),
            (0, "[TOO]   BoostState: 1"),
            (0, first_started),                   # boost-started, first
            (0, second_started),                  # boost-started, repeat
            (0, ""),                              # cancel-boost, invoke
            (0, BOOST_ENDED_ONE),
            (0, ""),                              # thermostat write
            (0, "[TOO]   LocalTemperature: 2150"),
            (0, WHM_MODES),
            (0, CTM_STATUS_0),                    # change-to-mode allow
            (0, "[TOO]   CurrentMode: 1"),
            (0, CTM_STATUS_0),                    # same-mode short-circuit
        ]
        counts = {"cancel": 0, "ctm": 0}

        def on_call(argv):
            if "boost" in argv:
                link.push_urcs([boost_urc])
            elif "cancel-boost" in argv:
                counts["cancel"] += 1
                if counts["cancel"] == 2:
                    link.push_urcs(["+MTCMD:2,23,148,1"])
                elif guard_leaks_forward:
                    # Regression shape: an in-state guard that regressed
                    # into forwarding anyway must trip assert_no_urc.
                    link.push_urcs(["+MTCMD:9,23,148,1"])
            elif "occupied-heating-setpoint" in argv:
                if thermostat_urc:
                    link.push_urcs(["+MTATTR:23,513,18,2200"])
            elif "change-to-mode" in argv:
                counts["ctm"] += 1
                if counts["ctm"] == 1:
                    link.push_urcs(["+MTCMD:3,23,158,0,1"])
                elif shortcircuit_leaks_forward:
                    # Regression shape: a same-mode ChangeToMode reaching
                    # the delegate (B196 undone) must trip assert_no_urc.
                    link.push_urcs(["+MTCMD:9,23,158,0,1"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_22_water_heater(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_wrong_boost_mask_fails(self):
        # The canonical-vector pin: a firmware that lost the bool-VALUE
        # bits (mask 9 instead of 265) must fail the forward check, not
        # be silently answered.
        ctx = self._ctx(boost_urc="+MTCMD:1,23,148,0,3600,9,80")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_22_water_heater(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_guard_leaking_a_forward_fails(self):
        ctx = self._ctx(guard_leaks_forward=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_22_water_heater(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_same_state_push_emitting_an_event_fails(self):
        # The ledger's same-state rule: a repeat BoostState push emits
        # nothing. A second BoostStarted appearing in the re-read is
        # exactly the regression the "still exactly one" check is for.
        ctx = self._ctx(second_boost_started="\n".join(
            [BOOST_STARTED_ONE, BOOST_STARTED_ONE]))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_22_water_heater(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_boost_started_without_cached_params_fails(self):
        # Consume-on-emit gone wrong the other way: an event whose
        # boostInfo lost the accepted command's parameters (duration 0,
        # no optionals: the no-prior-accept shape) must fail the
        # parameter check even though the event COUNT is right.
        bare = "\n".join([
            "[TOO]   BoostStarted: {",
            "[TOO]     BoostInfo: {",
            "[TOO]       Duration: 0",
            "[TOO]      }",
            "[TOO]    }",
        ])
        ctx = self._ctx(boost_started_first=bare)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_22_water_heater(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_same_mode_shortcircuit_leaking_a_forward_fails(self):
        ctx = self._ctx(shortcircuit_leaks_forward=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_22_water_heater(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_missing_thermostat_urc_fails(self):
        # The ember-served contrast pin: a controller setpoint write
        # MUST raise +MTATTR (spec 3.5); its absence is the regression
        # the host library's setpoint callbacks would suffer from.
        ctx = self._ctx(thermostat_urc=False)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_22_water_heater(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep323HeatPump(unittest.TestCase):
    """Energy round B, task 4: the heat pump's triple device type
    identity, the server list with the WHM/Thermostat absences pinned,
    the 0x94 +MTERR:3 row, the negative-ActivePower push (the sign
    path's only wire coverage), and the energy push with struct and
    event read-back."""

    def _ctx(self, server24=None, active_power=None):
        link = FakeLink({
            "AT+MTMEAS=24,148,0,1": (3, []),
            "AT+MTMEAS=24,144,0,230000,1,433,2,-1500000": (0, []),
            "AT+MTMEAS=24,145,0,2500000": (0, []),
        })
        energy_attr = "\n".join([
            "[TOO]   CumulativeEnergyImported: {",
            "[TOO]     Energy: 2500000",
            "[TOO]     EndSystime: 100000",
            "[TOO]    }",
        ])
        energy_event = "\n".join([
            "[TOO]   CumulativeEnergyMeasured: {",
            "[TOO]     EnergyImported: {",
            "[TOO]       Energy: 2500000",
            "[TOO]       EndSystime: 100000",
            "[TOO]      }",
            "[TOO]    }",
        ])
        script = [
            (0, DEVTYPES24_HP),
            (0, server24 if server24 is not None else SERVER24_HP),
            (0, "[TOO]   Voltage: 230000"),
            (0, active_power if active_power is not None
             else "[TOO]   ActivePower: -1500000"),
            (0, energy_attr),
            (0, energy_event),
        ]
        runner = FakeChipRunner(script)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_23_heat_pump(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        # The no-command-traffic pin, transcript half.
        self.assertFalse(any("MTCMDRESP" in c for c in ctx.link.sent))

    def test_whm_on_the_heat_pump_fails(self):
        # The disclosed-gap pin: a thunk change that grew WHM (or
        # Thermostat) onto the heat pump endpoint must fail the absence
        # check loudly, not be silently absorbed.
        with_whm = SERVER24_HP + "\n[TOO]     [6]: 148 (WaterHeaterManagement)"
        ctx = self._ctx(server24=with_whm)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_23_heat_pump(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_sign_lost_on_active_power_fails(self):
        # A pipeline that dropped the sign reads 1500000: exactly the
        # regression the negative push exists to catch.
        ctx = self._ctx(active_power="[TOO]   ActivePower: 1500000")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_23_heat_pump(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class ApplyLink(FakeLink):
    """FakeLink whose AT+MTEPAPPLY releases +MTREADY every time it is
    sent, not only the first (urcs_on_command's one-shot semantics):
    t_meas_staged_wh_min applies twice, scratch then restore, and both
    reboots must produce the marker for the happy path to be honest."""

    def command(self, cmd, expect=None, timeout=None):
        v = super().command(cmd, expect, timeout)
        if cmd == "AT+MTEPAPPLY" and v[0] == 0:
            self.push_urcs(["+MTREADY"])
        return v


class TestStagedWhMin(unittest.TestCase):
    """Energy round B, task 4: the Phase 1 staged variant-1 water
    heater row (t_meas_staged_wh_min): scratch composition applied and
    VERIFIED (the known-start-state rule), the two +MTERR:3 gate rows
    (including gate-outranks-range on a negative field 4), the
    +MTERR:1 unknown-field control row, and the finally-block restore
    of the single-light standard state on every exit path."""

    RESTORE_TAIL = ["AT+MTEPCLEAR", "AT+MTEP=0x0100", "AT+MTEPAPPLY",
                    "AT+MTEP?"]

    def _link(self, overrides=None):
        cmds = {
            "AT+MTEPCLEAR": (0, []),
            "AT+MTEP=0x0100": (0, []),
            "AT+MTEP=0x050F,1": (0, []),
            "AT+MTEPAPPLY": (0, []),
            "AT+MTEP?": [(0, ["+MTEP:0,1,0x0100", "+MTEP:1,2,0x050F,1"]),
                         (0, ["+MTEP:0,1,0x0100"])],
            "AT+MTMEAS=2,148,3,200": (3, []),
            "AT+MTMEAS=2,148,4,-5": (3, []),
            "AT+MTMEAS=2,148,9,1": (1, []),
        }
        cmds.update(overrides or {})
        return ApplyLink(cmds)

    def test_happy_path(self):
        link = self._link()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(t_meas_staged_wh_min(link))
        # The restore actually ran, and ran LAST: the single-light
        # standard state is the bench contract every later Phase 1 row
        # depends on.
        self.assertEqual(link.sent[-4:], self.RESTORE_TAIL)

    def test_gate_answering_range_code_fails(self):
        # The precedence pin (ledger: gate outranks range): a firmware
        # that answered +MTERR:1 for a negative field 4 on the MINIMAL
        # variant would have run the range check before the feature
        # gate, and this row must fail on it.
        link = self._link({"AT+MTMEAS=2,148,4,-5": (1, [])})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_meas_staged_wh_min(link))

    def test_restore_runs_even_when_a_row_fails(self):
        link = self._link({"AT+MTMEAS=2,148,3,200": (1, [])})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_meas_staged_wh_min(link))
        self.assertEqual(link.sent[-4:], self.RESTORE_TAIL)

    def test_wrong_composition_readback_fails_but_still_restores(self):
        # The known-start-state rule made executable: if the scratch
        # composition did not actually take effect, the gate rows were
        # never observed against their variable and the row must fail,
        # while the restore still runs.
        link = self._link({"AT+MTEP?": [
            (0, ["+MTEP:0,1,0x0100"]),      # wh-min entry missing
            (0, ["+MTEP:0,1,0x0100"]),      # restore readback, correct
        ]})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_meas_staged_wh_min(link))
        self.assertEqual(link.sent[-4:], self.RESTORE_TAIL)

    def test_apply_failure_fails_and_attempts_restore(self):
        link = self._link({"AT+MTEPAPPLY": (-1, [])})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_meas_staged_wh_min(link))
        # Both the scratch apply and the restore apply were attempted.
        self.assertEqual(link.sent.count("AT+MTEPAPPLY"), 2)

    def test_restore_failure_alone_fails_the_row(self):
        # A row whose exercise half passed but whose restore did not
        # must NOT pass: a green report over a bench left in a
        # non-standard state would poison every later Phase 1 row.
        link = self._link({"AT+MTEP?": [
            (0, ["+MTEP:0,1,0x0100", "+MTEP:1,2,0x050F,1"]),
            (0, ["+MTEP:0,1,0x0100", "+MTEP:1,2,0x050F,1"]),  # restore
        ]})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_meas_staged_wh_min(link))


class TestParseDeviceTypes(unittest.TestCase):
    """Energy round B, task 4: the DeviceTypeList parser for
    step_3_23's triple-identity check (shape derived from the pinned
    generated DataModelLogger.cpp, INFERENCE until the bench)."""

    def test_parses_all_three_entries_in_order(self):
        self.assertEqual(parse_device_types(DEVTYPES24_HP),
                         [777, 17, 1296])

    def test_revision_lines_cannot_match(self):
        # Revision prints without a parenthesised name; the \( anchor
        # keeps it (and any other bare numeric) out.
        self.assertEqual(parse_device_types("[TOO]   Revision: 1"), [])

    def test_empty_on_no_match(self):
        self.assertEqual(parse_device_types(""), [])


SERVER25_SOLAR = "\n".join([
    "[TOO]   ServerList: 5 entries",
    "[TOO]     [1]: 29 (Descriptor)",
    "[TOO]     [2]: 47 (PowerSource)",
    "[TOO]     [3]: 156 (PowerTopology)",
    "[TOO]     [4]: 144 (ElectricalPowerMeasurement)",
    "[TOO]     [5]: 145 (ElectricalEnergyMeasurement)",
])
DEVTYPES25_SOLAR = "\n".join([
    "[TOO]   DeviceTypeList: 3 entries",
    "[TOO]     [1]: {",
    "[TOO]       DeviceType: 23 (Solar Power)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
    "[TOO]     [2]: {",
    "[TOO]       DeviceType: 17 (Power Source)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
    "[TOO]     [3]: {",
    "[TOO]       DeviceType: 1296 (Electrical Sensor)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
])
SERVER26_BATTERY = "\n".join([
    "[TOO]   ServerList: 7 entries",
    "[TOO]     [1]: 29 (Descriptor)",
    "[TOO]     [2]: 47 (PowerSource)",
    "[TOO]     [3]: 156 (PowerTopology)",
    "[TOO]     [4]: 144 (ElectricalPowerMeasurement)",
    "[TOO]     [5]: 145 (ElectricalEnergyMeasurement)",
    "[TOO]     [6]: 152 (DeviceEnergyManagement)",
    "[TOO]     [7]: 159 (DeviceEnergyManagementMode)",
])
DEVTYPES26_BATTERY = "\n".join([
    "[TOO]   DeviceTypeList: 4 entries",
    "[TOO]     [1]: {",
    "[TOO]       DeviceType: 24 (Battery Storage)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
    "[TOO]     [2]: {",
    "[TOO]       DeviceType: 17 (Power Source)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
    "[TOO]     [3]: {",
    "[TOO]       DeviceType: 1296 (Electrical Sensor)",
    "[TOO]       Revision: 1",
    "[TOO]      }",
    "[TOO]     [4]: {",
    "[TOO]       DeviceType: 1293 (Device Energy Management)",
    "[TOO]       Revision: 4",
    "[TOO]      }",
])
SERVER27_DEM = "\n".join([
    "[TOO]   ServerList: 3 entries",
    "[TOO]     [1]: 29 (Descriptor)",
    "[TOO]     [2]: 152 (DeviceEnergyManagement)",
    "[TOO]     [3]: 159 (DeviceEnergyManagementMode)",
])
DEVTYPES27_DEM = "\n".join([
    "[TOO]   DeviceTypeList: 1 entries",
    "[TOO]     [1]: {",
    "[TOO]       DeviceType: 1293 (Device Energy Management)",
    "[TOO]       Revision: 4",
    "[TOO]      }",
])
DEM_ACCEPTED_CMDS = "\n".join([
    "[TOO]   AcceptedCommandList: 2 entries",
    "[TOO]     [1]: 0 (PowerAdjustRequest)",
    "[TOO]     [2]: 1 (CancelPowerAdjustRequest)",
])
DEM_ATTR_LIST = "\n".join([
    "[TOO]   AttributeList: 13 entries",
    "[TOO]     [1]: 0 (ESAType)",
    "[TOO]     [2]: 1 (ESACanGenerate)",
    "[TOO]     [3]: 2 (ESAState)",
    "[TOO]     [4]: 3 (AbsMinPower)",
    "[TOO]     [5]: 4 (AbsMaxPower)",
    "[TOO]     [6]: 5 (PowerAdjustmentCapability)",
    "[TOO]     [7]: 7 (OptOutState)",
    "[TOO]     [8]: 65528 (GeneratedCommandList)",
    "[TOO]     [9]: 65529 (AcceptedCommandList)",
    "[TOO]     [10]: 65530 (EventList)",
    "[TOO]     [11]: 65531 (AttributeList)",
    "[TOO]     [12]: 65532 (FeatureMap)",
    "[TOO]     [13]: 65533 (ClusterRevision)",
])


def dem_capability(entries, cause):
    """Render a chip-tool PowerAdjustmentCapability read the way the
    pinned generated DataModelLogger.cpp does (INFERENCE, see
    parse_power_adjust_entries' docstring), so the fixtures below and
    the parser tests share one shape."""
    lines = ["[TOO]   PowerAdjustmentCapability: {",
             "[TOO]     PowerAdjustCapability: %d entries" % len(entries)]
    for i, (mnp, mxp, mnd, mxd) in enumerate(entries, 1):
        lines += ["[TOO]       [%d]: {" % i,
                  "[TOO]         MinPower: %d" % mnp,
                  "[TOO]         MaxPower: %d" % mxp,
                  "[TOO]         MinDuration: %d" % mnd,
                  "[TOO]         MaxDuration: %d" % mxd,
                  "[TOO]        }"]
    lines += ["[TOO]     Cause: %d" % cause, "[TOO]    }"]
    return "\n".join(lines)


CAP_ENTRIES_C1 = [(1000000000, 10000000000, 30, 3600), (500, 2000, 30, 600)]
CAP_NULL = "[TOO]   PowerAdjustmentCapability: null"
PA_START_ONE = "\n".join([
    "[TOO]   PowerAdjustStart: {",
    "[TOO]    }",
])
def pa_end_normal(duration=20, energy=120000):
    """A NormalCompletion PowerAdjustEnd block. `duration` is a real knob,
    not decoration: step_3_26 bounds it against its own clock (the
    TC_DEM_2_2 step 14 rule that a re-adjust does not re-arm the clock),
    and the default is chosen to clear the floor FakeClock's default
    scripted span implies (20 s span, 2 s slack, so 18 s floor)."""
    return "\n".join([
        "[TOO]   PowerAdjustEnd: {",
        "[TOO]     Cause: 0",
        "[TOO]     Duration: %d" % duration,
        "[TOO]     EnergyUse: %d" % energy,
        "[TOO]    }",
    ])


PA_END_NORMAL = pa_end_normal()
PA_END_CANCELLED = "\n".join([
    "[TOO]   PowerAdjustEnd: {",
    "[TOO]     Cause: 4",
    "[TOO]     Duration: 3",
    "[TOO]     EnergyUse: 0",
    "[TOO]    }",
])
INVALID_IN_STATE = "[TOO]     status = 0xCB (INVALID_IN_STATE),"
STATUS_FAILURE = "[TOO]     status = 0x01 (FAILURE),"


class FakeClock:
    """Deterministic `ctx.clock`: successive calls return the scripted
    values in order, the last repeating forever so a wait loop always
    terminates. step_3_26 takes exactly three readings (the allow
    timestamp, one pass of `_hold_clock_gap`, the end timestamp), so the
    default `[1000.0, 1010.0, 1020.0]` gives a 20 s allow-to-end span and
    therefore an 18 s duration floor, with the gap hold satisfied on its
    first look. Without this seam the self-test's span would be ~0 and
    the duration bound would accept any value at all, which is precisely
    the hole the bound was added to close."""

    def __init__(self, values):
        self.values = list(values)

    def __call__(self):
        if len(self.values) > 1:
            return self.values.pop(0)
        return self.values[0]


def dem_modes(labels, tags):
    lines = ["[TOO]   SupportedModes: %d entries" % len(labels)]
    for i, (label, tag) in enumerate(zip(labels, tags)):
        lines += ["[TOO]     [%d]: {" % (i + 1),
                  "[TOO]       Label: %s" % label,
                  "[TOO]       Mode: %d" % i,
                  "[TOO]       ModeTags: 1 entries",
                  "[TOO]         [1]: {",
                  "[TOO]           Value: %d" % tag,
                  "[TOO]          }",
                  "[TOO]      }"]
    return "\n".join(lines)


class TestStep324SolarPower(unittest.TestCase):
    """Energy round C1, task 4: solar's three-device-type identity, the
    DEM-absence pair (a 0x0098 push and an AT+MTDEMCAP both answering
    +MTERR:3), the fifth measurement pool pair's power push, and the
    EXPORTED energy counter read back at full 64-bit width."""

    def _ctx(self, server25=None, exported=None):
        link = FakeLink({
            "AT+MTMEAS=25,152,0,1": (3, []),
            "AT+MTDEMCAP=25,1,0": (3, []),
            "AT+MTMEAS=25,144,0,230000,1,433,2,99590": (0, []),
            "AT+MTMEAS=25,145,1,4294967297": (0, []),
        })
        energy_attr = "\n".join([
            "[TOO]   CumulativeEnergyExported: {",
            "[TOO]     Energy: %d" % (exported if exported is not None
                                      else 4294967297),
            "[TOO]     EndSystime: 100000",
            "[TOO]    }",
        ])
        energy_event = "\n".join([
            "[TOO]   CumulativeEnergyMeasured: {",
            "[TOO]     EnergyExported: {",
            "[TOO]       Energy: %d" % (exported if exported is not None
                                        else 4294967297),
            "[TOO]       EndSystime: 100000",
            "[TOO]      }",
            "[TOO]    }",
        ])
        script = [
            (0, DEVTYPES25_SOLAR),
            (0, server25 if server25 is not None else SERVER25_SOLAR),
            (0, "[TOO]   Voltage: 230000"),
            (0, "[TOO]   ActivePower: 99590"),
            (0, energy_attr),
            (0, energy_event),
        ]
        runner = FakeChipRunner(script)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_24_solar_power(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertFalse(any("MTCMDRESP" in c for c in ctx.link.sent))

    def test_dem_on_solar_fails(self):
        # The absence pin: a thunk change that grew a DEM cluster onto
        # solar (the battery's over-delivery shape) must fail loudly.
        with_dem = SERVER25_SOLAR + \
            "\n[TOO]     [6]: 152 (DeviceEnergyManagement)"
        ctx = self._ctx(server25=with_dem)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_24_solar_power(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_exported_energy_truncated_fails(self):
        # 4294967297 truncated to 32 bits is 1: the regression shape the
        # beyond-2^32 exported value exists to catch, on the side no
        # other step reads back.
        ctx = self._ctx(exported=1)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_24_solar_power(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep325BatteryStorage(unittest.TestCase):
    """Energy round C1, task 4: the battery's four-device-type identity,
    the ember-served battery attributes (BatChargeState above all: the
    wire evidence for the SDK RECHG conformance fix), the sixth
    measurement pool pair, the second DEM pool slot proven by a
    capability round trip, and DEMMode's tag-0 default."""

    def _ctx(self, server26=None, capability=None, modes=None,
             charge_state=None, wide_capacity=None):
        link = FakeLink({
            "AT+MTATTR=26,47,11,12600": (0, ["+MTATTR:26,47,11,12600"]),
            "AT+MTATTR=26,0x2F,0x0C,180": (0, ["+MTATTR:26,47,12,180"]),
            "AT+MTATTR=26,47,26,1": (0, ["+MTATTR:26,47,26,1"]),
            "AT+MTATTR=26,47,28": (0, ["+MTATTR:26,47,28,0"]),
            "AT+MTATTR=26,47,24,5000": (0, ["+MTATTR:26,47,24,5000"]),
            "AT+MTATTR=26,47,24,13500000":
                (0, ["+MTATTR:26,47,24,13500000"]),
            "AT+MTMEAS=26,144,0,230000,2,-2200000": (0, []),
            "AT+MTMEAS=26,152,0,5,1,1": (0, []),
            "AT+MTDEMCAP=26,2,1,1000000,5000000,60,1800": (0, []),
            'AT+MTMODES=26,159,0,0,"NoOptimization",1,16385,"DeviceOpt"':
                (0, []),
        })
        script = [
            (0, DEVTYPES26_BATTERY),
            (0, server26 if server26 is not None else SERVER26_BATTERY),
            (0, "[TOO]   BatVoltage: 12600"),
            (0, "[TOO]   BatPercentRemaining: 180"),
            (0, charge_state if charge_state is not None
             else "[TOO]   BatChargeState: 1"),
            (0, "[TOO]   BatCapacity: 5000"),
            (0, wide_capacity if wide_capacity is not None
             else "[TOO]   BatCapacity: 13500000"),
            (0, "[TOO]   ActivePower: -2200000"),
            (0, "[TOO]   ESAType: 5"),
            (0, "[TOO]   ESACanGenerate: TRUE"),
            (0, capability if capability is not None
             else dem_capability([(1000000, 5000000, 60, 1800)], 2)),
            (0, modes if modes is not None
             else dem_modes(["NoOptimization", "DeviceOpt"],
                            [0x4000, 0x4001])),
        ]
        runner = FakeChipRunner(script)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        return fresh_phase3_ctx(link, chip=chip)

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_25_battery_storage(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertFalse(any("MTCMDRESP" in c for c in ctx.link.sent))

    def test_variant_one_battery_without_the_dem_triple_fails(self):
        # A slot-26 edit to variant 1 (or a thunk that dropped the DEM
        # over-delivery) leaves no 152/159 on the endpoint; the presence
        # check must fail rather than the capability rows quietly
        # answering +MTERR:3 later.
        without = "\n".join(SERVER26_BATTERY.split("\n")[:6])
        ctx = self._ctx(server26=without)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_25_battery_storage(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_wrong_capability_readback_fails(self):
        # The struct list must come back verbatim; a dropped or reordered
        # field would be invisible to a count-only check.
        ctx = self._ctx(capability=dem_capability(
            [(1000000, 5000000, 60, 1801)], 2))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_25_battery_storage(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_undefaulted_mode_tag_fails(self):
        # The tag-0 default must resolve to kNoOptimization (0x4000);
        # a firmware that stored the literal 0 reads back 0 here.
        ctx = self._ctx(modes=dem_modes(["NoOptimization", "DeviceOpt"],
                                        [0, 0x4001]))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_25_battery_storage(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_missing_bat_charge_state_fails(self):
        # The RECHG conformance fix's observability net: without
        # feature::rechargeable::add() the attribute does not exist and
        # the controller read answers nothing this parser can find.
        ctx = self._ctx(charge_state="[TOO]   Error: 0x86 "
                                     "(UNSUPPORTED_ATTRIBUTE)")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_25_battery_storage(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_bat_capacity_regressed_to_old_cap_fails(self):
        # B263 pin's failure shape. The real pre-fix path REJECTS the
        # write rather than clamping it: with the SDK example's
        # 0x00..0xFFFF bound restored, ember refuses 13500000 outright
        # and the AT write answers a bare ERROR (B264: no +MTERR code
        # on an ember min/max refusal), so BatCapacity keeps whatever
        # the earlier 5000-scale row left and the controller reads that
        # back, not 13500000. Hardware-confirmed both ways, round C1
        # tasks 7 and 8. The 65535 fixture below stands in for "any
        # stale value that is not 13500000", which is the whole class
        # this check has to catch; the earlier 5000-scale row alone
        # cannot tell a correct bound from a too-narrow one.
        ctx = self._ctx(wide_capacity="[TOO]   BatCapacity: 65535")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_25_battery_storage(ctx)
        self.assertGreater(ctx.suite.failed, 0)


class TestStep326Dem(unittest.TestCase):
    """Energy round C1, task 4: the standalone ESA's identity and the
    two derived lists (the iron rule's net), the AT+MTDEMCAP round trip
    including the null distinction, the server's own InvalidInState
    guard with the no-forward assertion, and the whole PowerAdjust
    chain: deny, allow, re-adjust with Start SUPPRESSED, the host's
    normal end with the consumed field-6 energy use, the Cancelled path,
    and the on-change-only ESAState report contract."""

    def _ctx(self, guard_reply=None, guard_leaks_forward=False,
             second_start=None, end_after_readjust=None,
             cap_after_end=None, pa_payloads=None, sub=None,
             accepted_cmds=None, attr_list=None, cap_null=None,
             start_after_allow=None, esastate_after_allow=None,
             end_pair=None, clock=None):
        link = FakeLink({
            "AT+MTMEAS=27,144,0,230000": (3, []),
            "AT+MTDEMCAP=99,1,0": (2, []),
            "AT+MTDEMCAP=1,1,0": (3, []),
            "AT+MTDEMCAP=27,3,0": (1, []),
            "AT+MTDEMCAP=27,1,1,5000,1000,60,3600": (1, []),
            "AT+MTDEMCAP=27,1,1,1000,5000,3600,60": (1, []),
            "AT+MTMEAS=27,152,7,1": (1, []),
            "AT+MTMEAS=27,152,0,14": (1, []),
            "AT+MTMEAS=27,152,2,9": (1, []),
            "AT+MTMEAS=27,152,5,4": (1, []),
            "AT+MTMEAS=27,152,0,255,3,-5000000000,4,5000000000": (0, []),
            "AT+MTDEMCAP=27,1,2,1000000000,10000000000,30,3600,500,2000,"
            "30,600": (0, []),
            "AT+MTDEMCAP=27,1,0": (0, []),
            "AT+MTMEAS=27,152,6,120000": (0, []),
            "AT+MTMEAS=27,152,2,1": (0, []),
            "AT+MTMEAS=27,152,2,2": (0, []),
            "AT+MTCMDRESP=1,0": (0, []),
            "AT+MTCMDRESP=2,1": (0, []),
            "AT+MTCMDRESP=3,1": (0, []),
            "AT+MTCMDRESP=4,1": (0, []),
            "AT+MTCMDRESP=5,1": (0, []),
            'AT+MTMODES=27,159,0,0,"NoOptimization",1,16387,"GridOpt"':
                (0, []),
        })
        cap_c1 = dem_capability(CAP_ENTRIES_C1, 1)
        script = [
            (0, DEVTYPES27_DEM),
            (0, SERVER27_DEM),
            (0, accepted_cmds if accepted_cmds is not None
             else DEM_ACCEPTED_CMDS),
            (0, attr_list if attr_list is not None else DEM_ATTR_LIST),
            (0, "[TOO]   ESAType: 255"),
            (0, "[TOO]   AbsMinPower: -5000000000"),
            (0, "[TOO]   AbsMaxPower: 5000000000"),
            (guard_reply if guard_reply is not None
             else (1, INVALID_IN_STATE)),          # the in-state guard
            (0, cap_c1),                           # capability read back
            (0, cap_null if cap_null is not None
             else CAP_NULL),                       # n=0 reads null
            (1, STATUS_FAILURE),                   # deny invoke
            (0, ""),                               # no PowerAdjustStart
            (0, "[TOO]   ESAState: 1"),
            (0, ""),                               # allow invoke
            (0, esastate_after_allow if esastate_after_allow is not None
             else "[TOO]   ESAState: 3"),
            (0, start_after_allow if start_after_allow is not None
             else PA_START_ONE),
            (0, cap_c1),                           # cause stamped 1
            (0, ""),                               # re-adjust invoke
            (0, second_start if second_start is not None
             else PA_START_ONE),                   # STILL one Start
            (0, dem_capability(CAP_ENTRIES_C1, 2)),  # cause restamped 2
            (0, end_after_readjust if end_after_readjust is not None
             else PA_END_NORMAL),
            (0, cap_after_end if cap_after_end is not None else cap_c1),
            (0, ""),                               # second adjust invoke
            (0, ""),                               # cancel invoke
            (0, end_pair if end_pair is not None
             else "\n".join([PA_END_NORMAL, PA_END_CANCELLED])),
            (0, "[TOO]   ESAState: 1"),
            (0, dem_modes(["NoOptimization", "GridOpt"],
                          [0x4000, 0x4003])),
        ]
        tails = pa_payloads if pa_payloads is not None else [
            "5000000000,60,0", "5000000000,60,0", "5000000000,60,1",
            "5000000000,60,0"]
        counts = {"pa": 0, "cancel": 0}

        def on_call(argv):
            if "power-adjust-request" in argv:
                counts["pa"] += 1
                i = counts["pa"]
                link.push_urcs(["+MTCMD:%d,27,152,0,%s"
                                % (i, tails[i - 1])])
            elif "cancel-power-adjust-request" in argv:
                counts["cancel"] += 1
                if counts["cancel"] == 2:
                    link.push_urcs(["+MTCMD:5,27,152,1"])
                elif guard_leaks_forward:
                    # Regression shape: a firmware that forwarded a
                    # cancel the server should have refused outright.
                    link.push_urcs(["+MTCMD:9,27,152,1"])

        runner = FakeChipRunner(script, on_call=on_call)
        d = tempfile.mkdtemp()
        chip = ChipTool("/bin/chip-tool", d, runner=runner)
        ctx = fresh_phase3_ctx(link, chip=chip)
        ctx.chip_call = sync_chip_call
        sub = sub if sub is not None else FakeSubscriber(counts=[1, 1, 2])
        ctx.subscriber_factory = lambda: sub
        ctx._sub = sub
        ctx.clock = clock if clock is not None else FakeClock(
            [1000.0, 1010.0, 1020.0])
        ctx.sleeper = lambda _s: None
        return ctx

    def test_happy_path(self):
        ctx = self._ctx()
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])
        self.assertTrue(ctx._sub.stopped)

    def test_guard_answering_success_fails(self):
        # The guard is the CHIP SERVER's, and its status is specific: a
        # Success (or any other code) means the ESAState precondition
        # stopped being checked where the spec says it is.
        ctx = self._ctx(guard_reply=(0, "[TOO]     status = 0x00 "
                                        "(SUCCESS),"))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_guard_leaking_a_forward_fails(self):
        ctx = self._ctx(guard_leaks_forward=True)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_second_power_adjust_start_fails(self):
        # The re-adjust rule (TC_DEM_2_2 step 14) is a SUPPRESSION pin: a
        # firmware that emitted a second PowerAdjustStart on the second
        # accept must fail the "still exactly one" check.
        ctx = self._ctx(second_start="\n".join([PA_START_ONE,
                                                PA_START_ONE]))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_wrong_power_adjust_payload_fails(self):
        # The canonical vector pin: a forward that lost the 64-bit power
        # (5000000000 truncated to 705032704) must not be answered.
        ctx = self._ctx(pa_payloads=["705032704,60,0", "5000000000,60,0",
                                     "5000000000,60,1", "5000000000,60,0"])
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_energy_use_not_carried_into_the_end_event_fails(self):
        # Field 6 is an event carrier and the End is where it surfaces;
        # a 0 there means the cache never reached the emission. The
        # duration stays at the passing value so this isolates the
        # EnergyUse mutation.
        ctx = self._ctx(end_after_readjust=pa_end_normal(energy=0))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_rearmed_duration_clock_fails(self):
        # TC_DEM_2_2 step 14's other half, and the review's F1: a
        # firmware that re-armed m_pa_start_ms on the re-adjust reports
        # the interval since the RE-ADJUST, not since the first accept.
        # With the default clock the allow-to-end span is 20 s and the
        # floor 18 s, so a 3 s duration (the shape a re-arm produces) is
        # unambiguously short. Every other check still passes, so this
        # isolates the clock rule.
        ctx = self._ctx(end_after_readjust=pa_end_normal(duration=3))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        failed = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertEqual(
            failed,
            ["3.26 PowerAdjustEnd duration measures from the FIRST "
             "accept, not the re-adjust (the clock is not re-armed)"])

    def test_duration_bound_tolerates_truncation_and_jitter(self):
        # The other side of the bound: the firmware truncates to whole
        # seconds and the harness's timestamps bracket the firmware's,
        # so a duration one second under the measured span must still
        # pass. A bound tight enough to fail here would be flaky on the
        # bench, which is the failure mode this test exists to prevent.
        ctx = self._ctx(end_after_readjust=pa_end_normal(duration=19))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertEqual(ctx.suite.failed, 0,
                         msg=[n for n, ok, _ in ctx.suite.results if not ok])

    def test_hold_clock_gap_waits_until_the_gap_has_passed(self):
        # The gap hold is what keeps the duration bound discriminating
        # on a rig whose chip-tool round trips are fast: without it the
        # two accepts could be close enough together that a re-armed
        # clock still cleared the floor.
        slept = []
        ctx = fresh_phase3_ctx()
        ctx.clock = FakeClock([100.0, 101.0, 103.0, 110.0])
        ctx.sleeper = slept.append
        got = _hold_clock_gap(ctx, 100.0)
        self.assertEqual(got, 110.0)
        # 100.0, 101.0 and 103.0 are all short of the 106.0 deadline.
        self.assertEqual(len(slept), 3)

    def test_accepted_command_list_with_an_extra_entry_fails(self):
        # The IRON RULE's observability net: the ember command entries
        # and the Instance's FeatureMap-derived list agree only because
        # feature::power_adjustment::add() created both. A third entry
        # means a feature bit was set some other way.
        extra = DEM_ACCEPTED_CMDS + \
            "\n[TOO]     [3]: 2 (StartTimeAdjustRequest)"
        ctx = self._ctx(accepted_cmds=extra)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_attribute_list_with_a_ninth_id_fails(self):
        # The field-6 net: AdjustmentEnergyUse is an event carrier and
        # must never acquire an attribute. Any extra non-global id here
        # (this fixture uses Forecast, 6) has to fail loudly.
        extra = DEM_ATTR_LIST + "\n[TOO]     [14]: 6 (Forecast)"
        ctx = self._ctx(attr_list=extra)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_n_zero_reading_an_empty_list_instead_of_null_fails(self):
        # The distinction the CHIP server's ConstraintError rests on: a
        # capability that reads as an EMPTY LIST is not null, and a
        # firmware that produced one would leave power-adjust-request
        # answering something other than ConstraintError.
        empty = "\n".join([
            "[TOO]   PowerAdjustmentCapability: {",
            "[TOO]     PowerAdjustCapability: 0 entries",
            "[TOO]     Cause: 1",
            "[TOO]    }",
        ])
        ctx = self._ctx(cap_null=empty)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_missing_power_adjust_start_after_allow_fails(self):
        # The accept must emit exactly one Start. Zero means the
        # firmware stopped owning the transition's event.
        ctx = self._ctx(start_after_allow="")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_esastate_not_active_after_allow_fails(self):
        # The firmware owns the entry transition (the contrast with
        # round B's Boost, where the host pushed the state). A device
        # still reading Online after an accepted request has lost that.
        ctx = self._ctx(esastate_after_allow="[TOO]   ESAState: 1")
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_cancel_end_with_the_wrong_cause_or_stale_energy_fails(self):
        # The Cancelled path carries cause 4, and EnergyUse 0 because
        # the FIRST End consumed the field-6 cache. This fixture is a
        # second NormalCompletion still carrying 120000: both halves of
        # the check are wrong, and both must be caught.
        ctx = self._ctx(end_pair="\n".join([PA_END_NORMAL,
                                            PA_END_NORMAL]))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_capability_cause_not_restored_fails(self):
        # Every PowerAdjustEnd restores the AT+MTDEMCAP baseline (task
        # review F2); a cause still reading the stamped 2 is the
        # regression.
        ctx = self._ctx(cap_after_end=dem_capability(CAP_ENTRIES_C1, 2))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)

    def test_same_state_push_reporting_fails(self):
        # The on-change-only contract: a report arriving after a
        # same-value ESAState push is exactly the round B behaviour this
        # round deliberately diverges from.
        ctx = self._ctx(sub=FakeSubscriber(counts=[1, 2, 3]))
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_26_dem(ctx)
        self.assertGreater(ctx.suite.failed, 0)
        self.assertTrue(ctx._sub.stopped)


class TestEnergyC1Parsers(unittest.TestCase):
    """Energy round C1, task 4: the three parsers the round adds, all
    INFERENCE-derived from the pinned generated DataModelLogger.cpp (see
    their docstrings) and awaiting bench confirmation."""

    def test_power_adjust_entries_in_order(self):
        self.assertEqual(
            parse_power_adjust_entries(dem_capability(CAP_ENTRIES_C1, 1)),
            [(1000000000, 10000000000, 30, 3600), (500, 2000, 30, 600)])

    def test_power_adjust_entries_empty_on_null(self):
        self.assertEqual(parse_power_adjust_entries(CAP_NULL), [])

    def test_power_adjust_entries_empty_on_a_ragged_capture(self):
        # A truncated struct (the "Struct truncated due to invalid value"
        # path) leaves the four label counts disagreeing; an empty list
        # is the honest answer, not a mis-zipped one.
        ragged = "\n".join(["[TOO]         MinPower: 1",
                            "[TOO]         MaxPower: 2",
                            "[TOO]         MinDuration: 3"])
        self.assertEqual(parse_power_adjust_entries(ragged), [])

    def test_power_adjust_entries_ignores_abs_min_max_power(self):
        # Review F3: without word boundaries `AbsMinPower:`/`AbsMaxPower:`
        # satisfy the two power patterns (no boundary sits between `Abs`
        # and `Min`). This fixture is the shape that turns that into a
        # PHANTOM ENTRY rather than a harmless miss: the DEM power
        # envelope supplies the two powers and a duration-bearing struct
        # supplies the other two, so all four counts agree at 1 and the
        # unanchored parser happily zips them into
        # (-5000000000, 5000000000, 30, 3600), an entry no host ever
        # pushed. Anchored, the powers do not match at all, the counts
        # disagree, and the honest empty list comes back.
        phantom = "\n".join(["[TOO]   AbsMinPower: -5000000000",
                             "[TOO]   AbsMaxPower: 5000000000",
                             "[TOO]     MinDuration: 30",
                             "[TOO]     MaxDuration: 3600"])
        self.assertEqual(parse_power_adjust_entries(phantom), [])

    def test_power_adjust_entries_keeps_negative_powers(self):
        self.assertEqual(
            parse_power_adjust_entries(
                dem_capability([(-5000000000, 5000000000, 0, 60)], 0)),
            [(-5000000000, 5000000000, 0, 60)])

    def test_cause_values_from_a_capability_read(self):
        self.assertEqual(parse_cause_values(dem_capability(
            CAP_ENTRIES_C1, 2)), [2])

    def test_cause_values_from_both_end_events_in_order(self):
        self.assertEqual(
            parse_cause_values("\n".join([PA_END_NORMAL,
                                          PA_END_CANCELLED])), [0, 4])

    def test_cause_values_empty_on_no_match(self):
        self.assertEqual(parse_cause_values(PA_START_ONE), [])

    def test_esa_state_reports_in_order(self):
        text = "\n".join(["[TOO]   ESAState: 1", "[TOO]   ESAState: 3",
                          "[TOO]   ESAState: 1"])
        self.assertEqual(parse_esa_state_reports(text), [1, 3, 1])

    def test_esa_state_reports_survive_a_trailing_escape(self):
        # Subscriber output bypasses the central ANSI strip, so the
        # parser must not be end-anchored (reports()'s F1 note).
        self.assertEqual(
            parse_esa_state_reports("[TOO]   ESAState: 3\x1b[0m"), [3])

    def test_esa_state_reports_empty_on_no_match(self):
        self.assertEqual(parse_esa_state_reports(""), [])


class TestStagedVariant1EnergyC1(unittest.TestCase):
    """Energy round C1, task 4: the Phase 1 staged variant-1 row
    (t_staged_variant1_energy_c1): one scratch composition carrying all
    three variant-1 types, VERIFIED after the apply (the
    known-start-state rule), the cluster-missing rows with their OK
    controls, the DEM v1 +MTERR:4 attribute-missing row, and the
    finally-block restore of the single-light standard state on every
    exit path."""

    RESTORE_TAIL = ["AT+MTEPCLEAR", "AT+MTEP=0x0100", "AT+MTEPAPPLY",
                    "AT+MTEP?"]
    STAGED_READBACK = ["+MTEP:0,1,0x0100", "+MTEP:1,2,0x0017,1",
                       "+MTEP:2,3,0x0018,1", "+MTEP:3,4,0x050D,1"]

    def _link(self, overrides=None):
        cmds = {
            "AT+MTEPCLEAR": (0, []),
            "AT+MTEP=0x0100": (0, []),
            "AT+MTEP=0x0017,1": (0, []),
            "AT+MTEP=0x0018,1": (0, []),
            "AT+MTEP=0x050D,1": (0, []),
            "AT+MTEPAPPLY": (0, []),
            "AT+MTEP?": [(0, list(self.STAGED_READBACK)),
                         (0, ["+MTEP:0,1,0x0100"])],
            "AT+MTMEAS=2,145,0,1500000": (3, []),
            "AT+MTMEAS=2,144,0,230000": (0, []),
            "AT+MTMEAS=3,152,0,5": (3, []),
            "AT+MTDEMCAP=3,1,0": (3, []),
            "AT+MTDEMCAP=4,1,0": (4, []),
            "AT+MTMEAS=4,152,0,5": (0, []),
        }
        cmds.update(overrides or {})
        return ApplyLink(cmds)

    def test_happy_path(self):
        link = self._link()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(t_staged_variant1_energy_c1(link))
        self.assertEqual(link.sent[-4:], self.RESTORE_TAIL)

    def test_dem_v1_answering_cluster_missing_fails(self):
        # The whole point of the +MTERR:4 row: on DEM variant 1 the
        # cluster IS there and only the PowerAdjustment feature is
        # absent, so a firmware answering 3 has lost the distinction
        # between "no cluster" and "no attribute".
        link = self._link({"AT+MTDEMCAP=4,1,0": (3, [])})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_staged_variant1_energy_c1(link))
        self.assertEqual(link.sent[-4:], self.RESTORE_TAIL)

    def test_solar_v1_losing_the_whole_graft_fails(self):
        # The control row: variant 1 drops EEM only. A thunk that
        # dropped the sensor graft entirely would still pass the
        # +MTERR:3 energy row and must fail on the power row.
        link = self._link({"AT+MTMEAS=2,144,0,230000": (3, [])})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_staged_variant1_energy_c1(link))

    def test_wrong_composition_readback_fails_but_still_restores(self):
        link = self._link({"AT+MTEP?": [
            (0, ["+MTEP:0,1,0x0100"]),      # the three entries missing
            (0, ["+MTEP:0,1,0x0100"]),      # restore readback, correct
        ]})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_staged_variant1_energy_c1(link))
        self.assertEqual(link.sent[-4:], self.RESTORE_TAIL)

    def test_apply_failure_fails_and_attempts_restore(self):
        link = self._link({"AT+MTEPAPPLY": (-1, [])})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_staged_variant1_energy_c1(link))
        self.assertEqual(link.sent.count("AT+MTEPAPPLY"), 2)

    def test_restore_failure_alone_fails_the_row(self):
        link = self._link({"AT+MTEP?": [
            (0, list(self.STAGED_READBACK)),
            (0, list(self.STAGED_READBACK)),   # restore did not take
        ]})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(t_staged_variant1_energy_c1(link))


class TestStep313Restore(unittest.TestCase):
    """T5 task 5: step_3_13_restore is the named wrapper the brief and
    Task 4's report anticipated, delegating to the one shared restore
    path; deliberately NOT registered in PHASE3_STEPS (see its
    docstring) so run_phase3's finally stays the single place restore
    actually runs."""

    def test_delegates_to_restore_standard_state(self):
        commands = {
            "AT+MTFRESET": (0, []), "AT+MTEPCLEAR": (0, []),
            "AT+MTEP=0x0100": (0, []), "AT+MTEPAPPLY": (0, []),
            "AT+MTEP?": (0, ["+MTEP:0,1,0x0100"]),
            "AT+MTFABRICS?": (0, ["+MTFABRICS:0"]),
        }
        link = FakeLink(commands, urcs_on_command={
            "AT+MTFRESET": ["+MTREADY"], "AT+MTEPAPPLY": ["+MTREADY"]})
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            ok = step_3_13_restore(ctx)
        self.assertTrue(ok)
        self.assertGreater(len(ctx.suite.results), 0)

    def test_not_registered_in_phase3_steps(self):
        fn_objs = [s["fn"] for s in PHASE3_STEPS]
        self.assertNotIn(step_3_13_restore, fn_objs)


class TestStep314RootUrcSweep(unittest.TestCase):
    """T5 task 5 fix round (Important finding): the identical
    root-endpoint URC sweep Phase 2 needed (TestStep26/
    step_2_6_root_urc_sweep), mirrored for Phase 3, scored last across
    the whole run's urc_history."""

    def test_clean_history_passes(self):
        link = FakeLink()
        link.urc_history = [(0.0, "+MTEVT:0"), (1.0, "+MTATTR:2,129,4,0"),
                            (2.0, "+MTREADY")]
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_14_root_urc_sweep(ctx)
        self.assertEqual(ctx.suite.failed, 0)

    def test_root_endpoint_attr_urc_fails(self):
        link = FakeLink()
        link.urc_history = [(0.0, "+MTEVT:0"), (1.0, "+MTATTR:0,40,2,1"),
                            (2.0, "+MTREADY")]
        ctx = fresh_phase3_ctx(link)
        with contextlib.redirect_stdout(io.StringIO()):
            step_3_14_root_urc_sweep(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("3.14 no root-endpoint +MTATTR URC in the whole run",
                      failed_names)


class TestParseIndexedList(unittest.TestCase):
    """T5 task 5: added beyond Task 3's original five parsers to cover a
    plain list[string] read (SupportedTemperatureLevels), which has no
    Label:/Name: struct field for parse_string_list to match. See
    parse_indexed_list's docstring for the DataModelLogger.h
    verification (no bench evidence exists for this attribute)."""

    def test_reads_plain_string_list_entries(self):
        text = "\n".join([
            "[TOO]   SupportedTemperatureLevels: 2 entries",
            "[TOO]     [1]: Low",
            "[TOO]     [2]: Medium, high",
        ])
        self.assertEqual(parse_indexed_list(text), ["Low", "Medium, high"])

    def test_no_match_is_empty(self):
        self.assertEqual(parse_indexed_list("no such content"), [])

    def test_real_ansi_capture_needs_the_strip(self):
        """Task-7 fix F5: byte-exact excerpt of chiptool-capture-run5.txt's
        SupportedTemperatureLevels read (the 3.12 check, task-7-evidence/),
        escapes included -- this attribute now HAS bench evidence (the
        report's HELD verdict, section 6), unlike when this parser was
        first written against the SDK source alone. RED on the raw text
        (H1's trailing-escape leak), GREEN after _strip_ansi."""
        raw = fixture(os.path.join("t5", "templevels-raw.txt"))
        self.assertIn("\x1b[", raw)
        self.assertNotEqual(parse_indexed_list(raw), ["Low", "Medium, high"])
        self.assertEqual(parse_indexed_list(_strip_ansi(raw)),
                         ["Low", "Medium, high"])


class TestComposedTrioParsers(unittest.TestCase):
    """Composed appliance round, task 6: the two parsers added for the
    trio's reads, both INFERENCE (derived from DataModelLogger source,
    see their docstrings) until Tasks 11/12 confirm them on the wire.
    Synthetic texts below carry the full real-output line prefix
    ([ts] [pid:tid] [TOO]) on at least one case each, so the regexes are
    proven immune to the bracketed prefixes a live capture carries."""

    def test_parse_parts_list_reads_plain_int_entries(self):
        text = "\n".join([
            "[1786300000.100] [200000:200001] [TOO]   PartsList: 2 entries",
            "[1786300000.100] [200000:200001] [TOO]     [1]: 15",
            "[1786300000.100] [200000:200001] [TOO]     [2]: 16",
        ])
        self.assertEqual(parse_parts_list(text), [15, 16])

    def test_parse_parts_list_ignores_struct_and_named_entries(self):
        # A struct-open entry ("[1]: {", the SupportedModes shape) and a
        # named entry ("[1]: 3 (Identify)", the ServerList shape) are NOT
        # plain-int entries and must not match: ServerList reads go
        # through parse_accepted_command_list instead.
        text = "\n".join([
            "[TOO]     [1]: {",
            "[TOO]     [1]: 3 (Identify)",
        ])
        self.assertEqual(parse_parts_list(text), [])

    def test_parse_parts_list_no_match_is_empty(self):
        self.assertEqual(parse_parts_list("no such content"), [])

    def test_server_list_shape_parses_via_accepted_command_list(self):
        # The reuse pin: ServerList prints through LogClusterId
        # ("<id> (<Name>)"), the exact shape parse_accepted_command_list
        # was built for, so the steps reuse it rather than growing a
        # third indexed-list parser.
        text = "\n".join([
            "[1786300001.200] [200000:200001] [TOO]   ServerList: 2 entries",
            "[1786300001.200] [200000:200001] [TOO]     [1]: 29 (Descriptor)",
            "[1786300001.200] [200000:200001] [TOO]     [2]: 82 "
            "(RefrigeratorAndTemperatureControlledCabinetMode)",
        ])
        self.assertEqual(parse_accepted_command_list(text), [29, 82])

    def test_parse_notify_active_skips_inactive(self):
        text = "\n".join([
            "[1786300002.300] [200000:200001] [TOO]   Notify: {",
            "[1786300002.300] [200000:200001] [TOO]     Active: 1",
            "[1786300002.300] [200000:200001] [TOO]     Inactive: 0",
            "[1786300002.300] [200000:200001] [TOO]     State: 1",
            "[1786300002.300] [200000:200001] [TOO]     Mask: 1",
            "[1786300002.300] [200000:200001] [TOO]    }",
        ])
        self.assertEqual(parse_notify_active(text), [1])

    def test_parse_notify_active_orders_multiple_events(self):
        text = ("[TOO]   Notify: {\n[TOO]     Active: 1\n"
                "[TOO]     Inactive: 0\n[TOO]    }\n"
                "[TOO]   Notify: {\n[TOO]     Active: 0\n"
                "[TOO]     Inactive: 1\n[TOO]    }\n")
        self.assertEqual(parse_notify_active(text), [1, 0])

    def test_parse_notify_active_no_match_is_empty(self):
        self.assertEqual(parse_notify_active("no such content"), [])


class TestPhase1T7Negative(unittest.TestCase):
    """Composed appliance round, task 6: the Phase 1 registration for
    the AT+MTEP parenting negatives and the AT+MTALARM field-0
    migration. The staging-sequence helper is exercised against a real
    ATLink over FakeTransport (expect_write scripting), so the +MTERR
    parsing and the trailing-clear behaviour are the engine's own, not a
    FakeLink shortcut."""

    def _link(self):
        ft = FakeTransport()
        return ATLink(ft, default_timeout=0.2), ft

    def test_wrong_parent_type_row_happy_path(self):
        link, ft = self._link()
        ft.expect_write(b"AT+MTEPCLEAR\r\n", ["OK"])
        ft.expect_write(b"AT+MTEP=0x0100\r\n", ["OK"])
        ft.expect_write(b"AT+MTEP=0x0071,0,0\r\n", ["+MTERR:1", "ERROR"])
        ft.expect_write(b"AT+MTEPCLEAR\r\n", ["OK"])
        fn = mtep_staging_negative(["AT+MTEPCLEAR", "AT+MTEP=0x0100"],
                                   "AT+MTEP=0x0071,0,0")
        self.assertTrue(fn(link))
        # The trailing clear ran, so no staged entry leaks onward.
        self.assertEqual(ft.writes[-1], b"AT+MTEPCLEAR\r\n")

    def test_wrong_error_code_fails_the_row(self):
        link, ft = self._link()
        ft.expect_write(b"AT+MTEPCLEAR\r\n", ["OK"])
        ft.expect_write(b"AT+MTEP=0x0077\r\n", ["+MTERR:6", "ERROR"])
        ft.expect_write(b"AT+MTEPCLEAR\r\n", ["OK"])
        fn = mtep_staging_negative(["AT+MTEPCLEAR"], "AT+MTEP=0x0077")
        self.assertFalse(fn(link))

    def test_failed_setup_is_reported_not_masked(self):
        # A prelude command failing (session never opened) must fail the
        # row rather than run the negative against unknown state.
        link, ft = self._link()
        ft.expect_write(b"AT+MTEPCLEAR\r\n", ["ERROR"])
        fn = mtep_staging_negative(["AT+MTEPCLEAR"], "AT+MTEP=0x0077")
        self.assertFalse(fn(link))
        self.assertNotIn(b"AT+MTEP=0x0077\r\n", ft.writes)

    def test_t7_rows_registered_and_t5_row_migrated(self):
        phase1 = [n for p, n, _, _, _ in TESTS if p == 1]
        for prefix in (
                "MTEPCLEAR then MTEP=0x0100,0,0 -> +MTERR:1",
                "MTEP=0x0100,0,zz -> +MTERR:1",
                "MTEP=0x0071,0,0 under a light -> +MTERR:1",
                "MTEP=0x0077 unparented -> +MTERR:1",
                "MTEP=0x0077,0,0 under a light -> +MTERR:1",
                "MTALARM=1,0,0 -> +MTERR:3",
        ):
            self.assertTrue(any(n.startswith(prefix) for n in phase1),
                            msg="missing t7 row: %s" % prefix)
        # The pre-migration t5 row is gone: field 0 is a legal
        # RefrigeratorAlarm bit now, so its +MTERR:1 expectation would
        # fail on current firmware.
        self.assertFalse(any(n.startswith("MTALARM=1,0,0 -> +MTERR:1")
                             for n in phase1))


class TestMainPhase3Wiring(unittest.TestCase):
    """T5 task 4: --phase 3 must be a valid argparse choice, and (the
    same structural guarantee --phase 2 already relies on: args.phase
    defaults to None) must never run without it being passed explicitly."""

    def test_phase_three_is_a_valid_choice(self):
        with contextlib.redirect_stdout(io.StringIO()):
            rc = main(["--port", "/dev/definitely-not-a-real-port-xyz",
                      "--phase", "3"])
        self.assertEqual(rc, 2)  # reaches the port-open failure, so
                                  # argparse accepted the choice

    def test_phase_four_is_rejected_by_argparse(self):
        with contextlib.redirect_stdout(io.StringIO()), \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                main(["--phase", "4"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
