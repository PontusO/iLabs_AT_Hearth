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
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mt_regression import ATLink, cmd_retry


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


from mt_regression import Suite, capture_header, write_baseline, phase0


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


from mt_regression import add_test, TESTS, main
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
        self.assertIn("timed out", out)

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

    def test_missing_creds_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem = phase2_gate(chip, self._args(ssid=None))
        self.assertIn("MT_SSID", problem)

    def test_missing_binary_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool(os.path.join(d, "nope"), d)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                problem = phase2_gate(chip, self._args())
        self.assertIn("chip-tool", problem)

    def test_unparseable_reference_payload_abort(self):
        runner = FakeChipRunner([(0, "garbage")])
        with tempfile.TemporaryDirectory() as d:
            binary = os.path.join(d, "chip-tool")
            open(binary, "w").close()
            os.chmod(binary, 0o755)
            chip = ChipTool(binary, d, runner=runner)
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
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
            with mock.patch("mt_regression.shutil.which",
                            return_value="/usr/local/bin/openocd"):
                self.assertIsNone(phase2_gate(chip, self._args()))
        self.assertEqual(runner.calls[0][0][1:3],
                         ["payload", "parse-setup-payload"])

    def test_missing_openocd_abort(self):
        with tempfile.TemporaryDirectory() as d:
            chip = ChipTool("/bin/true", d)
            with mock.patch("mt_regression.shutil.which",
                            return_value=None):
                problem = phase2_gate(chip, self._args())
        self.assertIn("openocd", problem)


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
                           step_2_5_controller_to_host)


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
            # Pre-reboot read echoes the write; the post-reboot read is
            # 0: the firmware does not persist OnOff (bug B63), and the
            # step pins that measured behavior.
            "AT+MTATTR=1,6,0": [(0, ["+MTATTR:1,6,0,1"]),
                                (0, ["+MTATTR:1,6,0,0"])],
            "AT+MTFABRICS?": (0, ["+MTFABRICS:1"]),
            "AT+MTSTATE?": (0, ["+MTSTATE:2,1"]),
        }

    def test_happy_path(self):
        link = FakeLink(self._commands())
        relink_called = []

        def relink(action):
            relink_called.append(True)
            link.push_urcs(["+MTREADY"])
            return True, ""

        ctx = fresh_ctx(link)
        ctx.relink = relink
        with contextlib.redirect_stdout(io.StringIO()):
            step_2_8_warm_reboot(ctx)
        self.assertEqual(ctx.suite.failed, 0)
        self.assertTrue(relink_called)

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
    only a cold boot with a pending StartUpOnOff change reaches the URC
    path that fires during esp_matter::start(), before the AT UART
    exists; 2.8's warm reset preserves state and cannot exercise it.
    ctx.relink here wraps ctx.power_cycler (not swd_reset, unlike 2.8),
    so the fake relink calls the injected action and propagates its
    (ok, detail) the way make_relink's real relink does, instead of
    assuming success outright: that is what lets the cycle-not-observed
    case reach StepAbort through the same code path as a real refused
    power cycle."""

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
        link = FakeLink(self._commands(post_boot_attr=0))
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

    def test_inconclusive_path_fails_and_prints(self):
        link = FakeLink(self._commands(post_boot_attr=1))
        ctx = fresh_ctx(link)
        ctx.relink = self._relink(link)
        ctx.power_cycler = lambda: (True, "")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            step_2_9_cold_boot(ctx)
        failed_names = [n for n, ok, _ in ctx.suite.results if not ok]
        self.assertIn("2.9 StartUpOnOff toggled at init", failed_names)
        self.assertIn("inconclusive", buf.getvalue())

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
