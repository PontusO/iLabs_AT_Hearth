#!/usr/bin/env python3
"""Hardware-free self-test for the T1 regression harness.

Exercises ATLink against a scripted fake transport, so the result
mapping and URC handling are pinned without a board on the desk.
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import types
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
        self.assertEqual(rc, 0)


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


if __name__ == "__main__":
    unittest.main(verbosity=2)
