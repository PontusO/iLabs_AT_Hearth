#!/usr/bin/env python3
"""Self-test for the catalogue proof script: the table is legal, the row
names match the baseline, and the prove functions record the right rows."""
import os, re, sys, threading, time, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mt_catalogue_proof as P
import mt_regression as H

BATCH1 = ["0x0101", "0x0015", "0x0044", "0x0041", "0x0043", "0x0107", "0x0307",
          "0x0305", "0x0106", "0x0306", "0x010A", "0x010B"]

class TestBatchTable(unittest.TestCase):
    def test_batch1_lists_every_type_once_after_the_anchor(self):
        comp = P.composition_for("mg24-batch1")
        self.assertEqual(comp[0], (1, "0x0100"))
        self.assertEqual([d for _, d in comp[1:]], BATCH1)
        self.assertEqual([ep for ep, _ in comp], list(range(1, 14)))

    def test_every_row_names_a_legal_cluster_and_revision(self):
        for t in P.BATCHES["mg24-batch1"]:
            self.assertRegex(t["devtype"], r"^0x[0-9A-F]{4}$")
            self.assertIn(t["revision"], range(1, 10))
            self.assertIn(t["chip_cluster"], P.CHIP_CLUSTERS)
            self.assertIsInstance(t["checks"][0]["cluster"], int)
            self.assertIsInstance(t["checks"][0]["attr"], int)

    def test_revisions_match_the_registry(self):
        want = {"0x0101": 3, "0x0015": 2, "0x0044": 1, "0x0041": 1, "0x0043": 1,
                "0x0107": 4, "0x0307": 2, "0x0305": 2, "0x0106": 3, "0x0306": 2,
                "0x010A": 4, "0x010B": 5}
        got = {t["devtype"]: t["revision"] for t in P.BATCHES["mg24-batch1"]}
        self.assertEqual(got, want)

    def test_a_controller_command_moves_the_value_away_from_the_at_write(self):
        """A command asking for the value the AT write of the previous
        step already put there is a no-op: the cluster server changes
        nothing, MatterPostAttributeChangeCallback never runs, no
        +MTATTR URC is raised, and the row waits for something that can
        never arrive. Found on the bench 2026-09-22 on 0x010A, whose
        controller command was `on` after an AT write of 1."""
        for t in P.BATCHES["mg24-batch1"]:
            c = t["checks"][0]
            if c["controller"] is None:
                continue
            self.assertNotEqual(c["controller"][2], c["at_value"],
                                "%s: the controller value equals the AT write"
                                % t["devtype"])

    def test_boolean_attributes_are_read_with_the_boolean_parser(self):
        """chip-tool prints a BOOLEAN attribute as `OnOff: TRUE`, which
        carries no integer for parse_int_attr to find. Found on the
        bench 2026-09-22 on 0x010A's on-off row."""
        for t in P.BATCHES["mg24-batch1"]:
            if t["chip_cluster"] in ("booleanstate", "onoff"):
                self.assertEqual(t["checks"][0]["parse"], "bool", t["devtype"])

    def test_levelcontrol_commands_are_the_with_on_off_variants(self):
        """LevelControl's Move, MoveToLevel, Stop and Step are refused
        with SUCCESS on an endpoint whose OnOff attribute is FALSE and
        whose Options ExecuteIfOff bit is clear (level-control.cpp's
        shouldExecuteIfOff), so they change nothing, raise no URC and
        still exit 0. A freshly composed dimmable light is off."""
        for t in P.BATCHES["mg24-batch1"]:
            c = t["checks"][0]
            if t["chip_cluster"] == "levelcontrol" and c["controller"] is not None:
                self.assertTrue(c["controller"][1][0].endswith("-with-on-off"),
                                "%s: %s is gated by Options processing"
                                % (t["devtype"], c["controller"][1][0]))

class TestParsers(unittest.TestCase):
    def test_parse_bool_attr(self):
        # Real chip-tool line shape (mirrors modes-current-mode.txt):
        # "[ts] [pid:tid] [TOO]   Label: VALUE", anchored on `]` + 2+ spaces.
        self.assertIs(P.parse_bool_attr(
            "[1740000000.123] [1234:1234] [TOO]   StateValue: TRUE\n"), True)
        self.assertIs(P.parse_bool_attr(
            "[1740000000.123] [1234:1234] [TOO]   StateValue: FALSE\n"), False)
        self.assertIsNone(P.parse_bool_attr("nothing here"))

    def test_parse_bool_attr_last_match_wins_over_an_earlier_true(self):
        # An earlier TRUE on a different label (e.g. a preceding attribute
        # in the same read) must not win: only the last labelled value
        # counts, the same discipline parse_int_attr enforces.
        out = ("[1740000000.100] [1234:1234] [TOO]   SomeOtherFlag: TRUE\n"
               "[1740000000.123] [1234:1234] [TOO]   StateValue: FALSE\n")
        self.assertIs(P.parse_bool_attr(out), False)

    def test_parse_device_types(self):
        # Real chip-tool shape (DataModelLogger): the id is followed by
        # a parenthesised name on the same line, and the revision is a
        # separate line with no id on it.
        out = ("CHIP:TOO:   DeviceTypeList: 2 entries\n"
               "CHIP:TOO:     [1]: {\n"
               "CHIP:TOO:       DeviceType: 257 (On/Off Light)\n"
               "CHIP:TOO:       Revision: 3\n"
               "CHIP:TOO:     }\n"
               "CHIP:TOO:     [2]: {\n"
               "CHIP:TOO:       DeviceType: 17 (Power Source)\n"
               "CHIP:TOO:       Revision: 1\n"
               "CHIP:TOO:     }\n")
        self.assertEqual(P.parse_device_types(out), [(257, 3), (17, 1)])

class TestArgs(unittest.TestCase):
    def test_batch_is_required_and_known(self):
        with self.assertRaises(SystemExit):
            P.build_parser().parse_args(["--port", "/dev/serial/by-id/x"])
        with self.assertRaises(SystemExit):
            P.build_parser().parse_args(["--port", "/dev/serial/by-id/x", "--batch", "nope"])

    def test_dry_run_prints_the_rows_and_opens_no_port(self):
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = P.main(["--batch", "mg24-batch1", "--dry-run"])
        self.assertEqual(rc, 0)
        self.assertIn("0x010B", buf.getvalue())
        self.assertIn("13 endpoints", buf.getvalue())

BATCH2 = ["0x010C", "0x010D", "0x0301", "0x002B", "0x0202", "0x002C"]

# chip-tool cluster name -> the table's checks that carry a controller
# action on that cluster. _FakeChip uses it to queue the URC a
# controller run would raise, mirroring the table rather than the
# command text (the fake chip-tool cannot parse the table's arguments).
_CONTROLLER_BY_CLUSTER_ARG = {}
for _t in P.BATCHES["mg24-batch1"] + P.BATCHES["mg24-batch2"]:
    _cc = _t["chip_cluster"]
    for _c in _t["checks"]:
        if _c["controller"] is not None:
            _CONTROLLER_BY_CLUSTER_ARG.setdefault(
                _c["chip_cluster"] or _t["chip_cluster"], []).append(_c)

class TestBatch2Table(unittest.TestCase):
    def test_batch2_lists_every_type_once_after_the_anchor(self):
        comp = P.composition_for("mg24-batch2")
        self.assertEqual(comp[0], (1, "0x0100"))
        self.assertEqual([d for _, d in comp[1:]], BATCH2)

    def test_revisions_match_the_registry(self):
        want = {"0x010C": 4, "0x010D": 4, "0x0301": 4, "0x002B": 4, "0x0202": 5, "0x002C": 1}
        got = {t["devtype"]: t["revision"] for t in P.BATCHES["mg24-batch2"]}
        self.assertEqual(got, want)

    def test_every_check_is_legal(self):
        for batch in ("mg24-batch1", "mg24-batch2"):
            for t in P.BATCHES[batch]:
                self.assertTrue(t["checks"], t["devtype"])
                for c in t["checks"]:
                    self.assertIn(c["chip_cluster"] or t["chip_cluster"], P.CHIP_CLUSTERS)
                    self.assertIsInstance(c["cluster"], int)
                    self.assertIsInstance(c["attr"], int)
                    if c["controller"] is not None:
                        kind, args, urc = c["controller"]
                        self.assertIn(kind, ("command", "write"))
                        self.assertEqual(len(urc), 3)

    def test_controller_moves_the_value_away_from_the_at_write(self):
        """TESTING.md section 8's first rule: a command that asks for the
        value already held raises no +MTATTR, so the row could never pass."""
        for batch in ("mg24-batch1", "mg24-batch2"):
            for t in P.BATCHES[batch]:
                for c in t["checks"]:
                    if c["controller"] is None:
                        continue
                    ucl, uat, uval = c["controller"][2]
                    if (ucl, uat) == (c["cluster"], c["attr"]):
                        self.assertNotEqual(uval, c["at_value"], t["devtype"])

    def test_colour_commands_carry_execute_if_off(self):
        """A freshly composed colour light is off; without OptionsMask and
        OptionsOverride bit 0 the command answers Success and moves nothing."""
        for t in P.BATCHES["mg24-batch2"]:
            for c in t["checks"]:
                if c["controller"] and (c["chip_cluster"] or t["chip_cluster"]) == "colorcontrol":
                    self.assertEqual(c["controller"][1][-2:], ["1", "1"], t["devtype"])

    def test_window_covering_urc_lands_on_target_not_current(self):
        wc = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x0202"][0]
        c = wc["checks"][0]
        self.assertEqual((c["cluster"], c["attr"]), (0x0102, 0x000E))
        self.assertEqual(c["controller"][2][:2], (0x0102, 0x000B))
        self.assertEqual(c["urc_chip_attr"], "target-position-lift-percent100ths")

    def test_air_quality_asserts_the_echo_and_has_no_controller(self):
        aq = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x002C"][0]
        self.assertTrue(aq["checks"][0]["echo"])
        self.assertIsNone(aq["checks"][0]["controller"])

    def test_thermostat_reads_null_before_the_signed_write(self):
        th = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x0301"][0]
        lt = th["checks"][0]
        self.assertTrue(lt["null_read"])
        self.assertLess(lt["at_value"], 0)

class TestBatch1Shape(unittest.TestCase):
    def test_type_helper_builds_one_check(self):
        t = P._type("0x0101", "dimmable light", 3, "levelcontrol", 8, 0, "current-level", 100)
        self.assertEqual(len(t["checks"]), 1)
        self.assertFalse(t["checks"][0]["echo"])
        self.assertFalse(t["checks"][0]["null_read"])

    def test_batch1_controller_urc_is_its_own_attribute(self):
        for t in P.BATCHES["mg24-batch1"]:
            c = t["checks"][0]
            if c["controller"]:
                self.assertEqual(c["controller"][2][:2], (c["cluster"], c["attr"]))

class _FakeLink:
    """Answers every command from a dict the fake updates on writes and
    URCs, so the row names come from the table, not from the answers.
    The read shape is what ATLink._collect returns: the +MTERR line is
    consumed and comes back as the result code, not in the lines."""

    def __init__(self, state=None, null_read=None):
        self.state = dict(state or {})
        self.null_read = null_read
        self.urc_queue = []

    def command(self, cmd, *a, **k):
        m = re.fullmatch(r"AT\+MTATTR=(\d+),(\d+),(\d+)(?:,(\S+))?", cmd)
        if m:
            if m.group(4) is not None:
                self.state[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = m.group(4)
                return (0, [])
            if self.null_read and (int(m.group(2)), int(m.group(3))) == self.null_read:
                return (5, [])
            v = self.state.get((int(m.group(1)), int(m.group(2)), int(m.group(3))), "0")
            return (0, ["+MTATTR:%s,%s,%s,%s" % (m.group(1), m.group(2), m.group(3), v)])
        return (0, [])

    def drain(self, *a, **k):
        pass

    def await_urc(self, pattern, *a, **k):
        """Like ATLink.await_urc, this serves only lines that actually
        landed in the URC queue. The queue is filled by the fake's
        controller action: a chip-tool command or write runs as the
        table's controller tuple and queues the +MTATTR line the row
        waits for. An AT write's own echo is NOT queued (it belongs in
        the write's returned lines), so await_urc after an AT write
        finds nothing unless the table also names a controller."""
        m = re.search(r"MTATTR:(\d+),(\d+),(\d+),(-?\d+)", pattern)
        if not m:
            return None
        for (ucl, uat, uval) in self.urc_queue:
            if (int(m.group(2)), int(m.group(3)),
                    int(m.group(4))) == (ucl, uat, uval):
                return "+MTATTR:%d,%d,%d,%d" % (int(m.group(1)), ucl, uat, uval)
        return None

class _EchoLink(_FakeLink):
    """_FakeLink plus echo control. A write with echo_in_lines=true
    returns (0, ["+MTATTR:ep,cl,at,val"]), the shape ATLink._collect
    yields when the +MTATTR echo arrives before the write's OK (AT_MT_SPEC
    3.8: _derive_expect gives an AT+MTATTR write the +MTATTR: expect
    prefix, so the line lands in the command's own lines). With
    echo_late=false the write raises no echo at all, and await_urc stays
    empty too, so the late branch has nothing to find. The default
    mirrors _FakeLink: no echo in the write's lines, await_urc answers
    from urc_queue."""

    def __init__(self, echo_in_lines=False, echo_late=True):
        super().__init__()
        self.echo_in_lines = echo_in_lines
        self.echo_late = echo_late

    def command(self, cmd, *a, **k):
        m = re.fullmatch(r"AT\+MTATTR=(\d+),(\d+),(\d+)(?:,(\S+))?", cmd)
        if m and m.group(4) is not None:
            self.state[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = m.group(4)
            if self.echo_in_lines:
                return (0, ["+MTATTR:%s,%s,%s,%s" % (m.group(1), m.group(2),
                                                      m.group(3), m.group(4))])
            return (0, [])
        return super().command(cmd, *a, **k)

    def await_urc(self, pattern, *a, **k):
        if not self.echo_late:
            return None
        return super().await_urc(pattern, *a, **k)

class _FakeChip:
    """Every run exits 0 and prints the attribute under the chip-tool
    line shape parse_int_attr and parse_bool_attr parse: a `]` (the
    [TOO] tag closing bracket), 2+ spaces, then `Label: VALUE` at the
    line end. The brief's sketch said "CHIP:TOO:   X: <value>"; that
    plain shape has no `]`, so parse_int_attr answers None on it and
    every read row would fail. The bracket shape is the one the
    harness's own tests and fixtures use (test_parse_bool_attr,
    fixtures/t5/modes-current-mode.txt).

    A non-read run is a controller command or write; it queues on its
    link the +MTATTR line the table's controller tuple says will come,
    so the link's URC queue mirrors ATLink's: a controller action
    queues its URC, an AT write's own echo does not (it belongs in the
    write's returned lines)."""

    def __init__(self, link=None):
        self.link = link

    def run(self, args, *a, **k):
        if len(args) >= 4 and args[1] in ("read", "read-event"):
            return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: 0\n" % args[2])
        if self.link is not None:
            for c in _CONTROLLER_BY_CLUSTER_ARG.get(args[0], ()):
                ucl, uat, uval = c["controller"][2]
                self.link.urc_queue.append((ucl, uat, uval))
        return (0, "CHIP:TOO:   Accepted: 0\n")

class _FakeSuite:
    def __init__(self):
        self.results = []
        self.na = []

    def check(self, name, ok, *a, **k):
        self.results.append((name, bool(ok)))
        return bool(ok)

    def not_applicable(self, name, *a, **k):
        self.na.append((name, ""))

def _names_for(batch, null_read=None):
    """Every row name prove_endpoint records (checks plus N/A) over the
    batch's types, with the bench's endpoints, against the fakes."""
    link = _FakeLink(null_read=null_read)
    chip = _FakeChip(link)
    comp = P.composition_for(batch)
    names = []
    for (ep, _), t in zip(comp[1:], P.rows_for(batch)):
        s = _FakeSuite()
        P.prove_endpoint(link, chip, s, "0x4845", ep, t)
        names += [n for n, _ in s.results]
        names += [n for n, _ in s.na]
    return names

def _baseline_endpoint_names(path=None):
    """A proven result file's endpoint row names: the results keys minus
    the compose:, commission: and restore: rows main() owns. The N/A
    names are among the keys (write_baseline merges suite.na into the
    same dict), so they count as names in the pin. Without a path,
    core-batch1.json, as in the original batch 1 pin."""
    import json
    path = path or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "platform", "silabs", "core-batch1.json")
    keys = json.load(open(path))["results"]
    return {k for k in keys
            if not k.startswith(("compose:", "commission:", "restore:"))}

class TestBatch1RowNames(unittest.TestCase):
    def test_fake_run_yields_exactly_the_baseline_row_names(self):
        self.assertEqual(set(_names_for("mg24-batch1")), _baseline_endpoint_names())

class TestBatch2RowNames(unittest.TestCase):
    def test_fake_run_yields_no_duplicate_name(self):
        names = _names_for("mg24-batch2")
        self.assertEqual(len(names), len(set(names)))

    def test_fake_run_yields_exactly_the_baseline_row_names(self):
        # Batch 2's proven result file is the pin, as batch 1's is: the
        # fake run must yield exactly the recorded endpoint row names,
        # N/A names among them.
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "platform", "silabs", "core-batch2.json")
        self.assertEqual(set(_names_for("mg24-batch2")), _baseline_endpoint_names(path))

class TestNullRead(unittest.TestCase):
    """ATLink._collect consumes the +MTERR:<n> line and returns n as the
    result code, the error line NOT in the lines, so the null-read row
    is res == 5, not a scan of lines for +MTERR:5."""

    def test_null_read_returns_five_and_the_row_passes(self):
        link = _FakeLink(null_read=(0x0201, 0x0000))
        chip = _FakeChip(link)
        th = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x0301"][0]
        s = _FakeSuite()
        P.prove_endpoint(link, chip, s, "0x4845", 2, th)
        got = {n: ok for n, ok in s.results}
        null_rows = [n for n in got if "null" in n.lower()]
        self.assertEqual(len(null_rows), 1)
        self.assertTrue(got[null_rows[0]])

    def test_null_read_answering_a_value_fails_the_row(self):
        link = _FakeLink(null_read=(0, ["+MTATTR:2,513,0,-500"]))
        chip = _FakeChip(link)
        th = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x0301"][0]
        s = _FakeSuite()
        P.prove_endpoint(link, chip, s, "0x4845", 2, th)
        got = {n: ok for n, ok in s.results}
        null_rows = [n for n in got if "null" in n.lower()]
        self.assertEqual(len(null_rows), 1)
        self.assertFalse(got[null_rows[0]])

class TestEchoRow(unittest.TestCase):
    """AT_MT_SPEC 3.8: a write that changes the value echoes the +MTATTR
    URC BEFORE the OK. _derive_expect maps an AT+MTATTR write to the
    +MTATTR: prefix, so _collect puts that line in the write's own
    returned lines and the URC queue never sees it; the echo row must
    accept it there (keeping the await_urc wait for the late case) or it
    fails a firmware that echoes correctly."""

    def test_echo_in_the_write_lines_passes(self):
        link = _EchoLink(echo_in_lines=True)
        chip = _FakeChip(link)
        s = _FakeSuite()
        aq = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x002C"][0]
        P.prove_endpoint(link, chip=chip, s=s, node="0x4845", ep=2, t=aq)
        got = {n: ok for n, ok in s.results}
        echo_rows = [n for n in got if "echoes" in n]
        self.assertEqual(len(echo_rows), 1)
        self.assertTrue(got[echo_rows[0]])

    def test_echo_nowhere_fails(self):
        link = _EchoLink(echo_in_lines=False, echo_late=False)
        chip = _FakeChip(link)
        s = _FakeSuite()
        aq = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x002C"][0]
        P.prove_endpoint(link, chip=chip, s=s, node="0x4845", ep=2, t=aq)
        got = {n: ok for n, ok in s.results}
        echo_rows = [n for n in got if "echoes" in n]
        self.assertEqual(len(echo_rows), 1)
        self.assertFalse(got[echo_rows[0]])

# --------------------------------------------------------------------------
# Batch 3 (the door lock and the water valve) and the verdict kind.
#
# The verdict controller kind is the one the batch 1 and batch 2 fakes never
# reach: their _FakeLink answers only the MTATTR read/write shape and their
# _FakeChip queues only +MTATTR URCs. A verdict check instead forwards a
# +MTCMD to the device, the device waits for AT+MTCMDRESP=<seq>,<verdict>,
# and the host reports the actuation (rc and the wire status) rather than the
# controller. So the fakes here model the harness: a link that records every
# AT line and pops URCs out of a queue, and a chip_call double that enqueues
# the +MTCMD forward the firmware would raise and returns the (rc, out) the
# row's rc/status rows read. prove_check builds the ctx itself (chip_call is
# always None), so the double is installed by patching the module-level
# H._threaded_chip_call that invoke_chip falls back to; the real production
# call keeps running chip.run on a background thread, but the double runs it
# synchronously on the caller's thread, so the forward is enqueued before
# CmdResponder.expect starts polling and no thread race is in play.

BATCH3 = ["0x000A", "0x0042"]


class _VerdictLink(_FakeLink):
    """_FakeLink widened to the verdict shape: command() records every line
    it is sent (so a test can assert exactly what went over the wire) and
    still answers the AT+MTATTR read/write shape _FakeLink answers; the
    at_cmd commands (AT+MTLOCK / AT+MTVALVE) answer OK and leave the
    attribute unchanged, mirroring a firmware that answers the interaction
    but whose actuation the host, not the link, reports. await_urc /
    await_urc_ts pop from self.urcs, the queue the fake chip_call fills,
    the way ATLink.await_urc_ts pops ATLink.urcs."""

    def __init__(self, state=None, null_read=None):
        super().__init__(state=state, null_read=null_read)
        self.sent = []
        self.urcs = []

    def command(self, cmd, *a, **k):
        self.sent.append(cmd)
        m = re.fullmatch(r"AT\+MTLOCK=(\d+),(\S+)", cmd)
        if m:
            self.state[(int(m.group(1)), 0x0101, 0x0000)] = m.group(2)
            return (0, [])
        m = re.fullmatch(r"AT\+MTVALVE=(\d+),(\S+)", cmd)
        if m:
            self.state[(int(m.group(1)), 0x0081, 0x0004)] = m.group(2)
            return (0, [])
        return super().command(cmd, *a, **k)

    def await_urc_ts(self, pattern, timeout=5.0):
        # Serves from self.urcs, the queue the chip_call double fills
        # synchronously before CmdResponder starts polling. The one pass is
        # enough: unlike _FakeLink, whose single-pass await_urc serves a
        # queue that is already full when the row waits, this queue is full
        # by the time the first pass runs, and the poll loop of
        # ATLink.await_urc_ts exists only to wait on a serial port.
        rx = re.compile(pattern)
        for i, u in enumerate(self.urcs):
            if rx.search(u):
                return (0.0, self.urcs.pop(i))
        return None

    def await_urc(self, pattern, timeout=5.0):
        got = self.await_urc_ts(pattern, timeout)
        return got[1] if got is not None else None

    @property
    def urc_queue(self):
        # _FakeLink's instance attribute name for the URC queue; here it is
        # self.urcs, the same list (so an assignment in _FakeLink.__init__
        # or an append by the base await_urc reaches the same queue).
        # Not dead: _FakeLink.__init__ assigns self.urc_queue = [], which
        # goes through the setter below, aliasing the tuple queue onto the
        # string queue, so every fake's queue IS this list.
        return self.urcs

    @urc_queue.setter
    def urc_queue(self, value):
        self.urcs = list(value)


class _VerdictChip:
    """The chip side of a verdict run, standing in for chip-tool.

    The verdict's chip-tool run goes through H.invoke_chip, which, when
    ctx.chip_call is unset (prove_check sets it to None), falls back to the
    module-level H._threaded_chip_call. The test's chip_call double patches
    that name, so invoke_chip still runs its real logic (ctx.chip_call or
    the fallback) while the double stands in for the threaded call: it
    runs chip.run(argv, timeout) synchronously on the caller's thread and
    wraps the (rc, out) in the same handle shape join() reads, so the
    forward the firmware would raise is enqueued on the link before
    CmdResponder.expect starts polling. run() enqueues
    "+MTCMD:<seq>,<ep>,<cluster>,<cmd>" (+MTCMDTO:<seq> in the unanswered
    case) and returns the (rc, out) the rc/status rows read; out carries
    "status = 0xNN" (or a success line) in the shape H.parse_status reads.

    The read path (descriptor / read) prints the attribute under the
    chip-tool line shape parse_int_attr parses (a ] bracket, 2+ spaces,
    Label: VALUE at the line end, Label with no hyphen), so the
    "controller reads" row can pass against the fakes. The descriptor read
    prints the DeviceType in the shape parse_device_types parses."""

    def __init__(self, link=None, ep=2, cluster=257, command=0, seq=7,
                 timeout=False, rc=0, status="0x00",
                 devtype="0x000A", revision=3, value=None):
        self.link = link
        self.ep = ep
        self.cluster = cluster
        self.command = command
        self.seq = seq
        self.timeout = timeout
        self.rc = rc
        self.status = status
        self.devtype = devtype
        self.revision = revision
        self.value = value

    def run(self, args, timeout=None):
        if args and args[0] == "descriptor":
            # The endpoint's own devtype/revision, set per endpoint by the
            # row-name test (the fake models one endpoint at a time).
            d, r = self.devtype, self.revision
            return (0, "CHIP:TOO:   DeviceTypeList: 1 entries\n"
                       "CHIP:TOO:     [1]: {\n"
                       "CHIP:TOO:       DeviceType: %d (%s)\n"
                       "CHIP:TOO:       Revision: %d\n"
                       "CHIP:TOO:     }\n" % (int(d, 16), d, r))
        if len(args) >= 4 and args[1] in ("read", "read-event"):
            if args[1] == "read-event":
                name = args[2].replace("-", " ").title().replace(" ", "")
                return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: 1\n" % name)
            # The chime's enabled check is parse="bool": the line must carry
            # the table's value in the TRUE/FALSE shape parse_bool_attr reads
            # (chip-tool prints booleans that way). The int shape stays the
            # default for every other read.
            if self.value is None:
                body = "1"
            elif isinstance(self.value, bool):
                body = "TRUE" if self.value else "FALSE"
            else:
                body = str(self.value)
            return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: %s\n"
                       % (args[2].replace("-", ""), body))
        if self.link is not None:
            self.link.urcs.append("+MTCMD:%d,%d,%d,%d" % (self.seq, self.ep, self.cluster, self.command))
            if self.timeout:
                self.link.urcs.append("+MTCMDTO:%d" % self.seq)
        # status carries the full hex value with its 0x prefix, matching the
        # wire shape parse_status's first form expects ("status = 0x01").
        out = "CHIP:TOO:   status = %s\n" % self.status if self.status else ""
        return (self.rc, out)

    def chip_call(self, chip, argv, timeout=60):
        # The synchronous double the tests install for H._threaded_chip_call:
        # it runs chip.run on the caller's thread (the real threaded call
        # runs it in a background thread) and returns the same handle shape
        # join() reads, so the forward is already queued by the time
        # CmdResponder.expect polls and the ordering the production call
        # needs (start chip-tool, then adjudicate the forward) is exact.
        result = {"rc": None, "out": None}
        result["rc"], result["out"] = chip.run(argv, timeout)
        t = threading.Thread(target=lambda: None, daemon=True)
        t.start()
        return H._ChipCallHandle(t, result)


class TestBatch3Table(unittest.TestCase):
    def test_batch3_lists_every_type_once_after_the_anchor(self):
        comp = P.composition_for("mg24-batch3")
        self.assertEqual(comp[0], (1, "0x0100"))
        self.assertEqual([d for _, d in comp[1:]], BATCH3)

    def test_revisions_match_the_registry(self):
        want = {"0x000A": 3, "0x0042": 1}
        got = {t["devtype"]: t["revision"] for t in P.BATCHES["mg24-batch3"]}
        self.assertEqual(got, want)

    def test_door_lock_verdicts_carry_the_timed_interaction_timeout(self):
        dl = [t for t in P.BATCHES["mg24-batch3"] if t["devtype"] == "0x000A"][0]
        for c in dl["checks"]:
            if c["controller"] is None or c["controller"][0] != "verdict":
                continue
            args = c["controller"][1]
            self.assertIn("--timedInteractionTimeoutMs", args,
                          "every door lock verdict carries --timedInteractionTimeoutMs")
            self.assertEqual(args[args.index("--timedInteractionTimeoutMs") + 1], "5000")

    def test_lock_answers_are_exactly_allow_deny_unanswered(self):
        dl = [t for t in P.BATCHES["mg24-batch3"] if t["devtype"] == "0x000A"][0]
        answers = [c["controller"][3] for c in dl["checks"]
                   if c["controller"] is not None and c["controller"][0] == "verdict"]
        # allow (1), deny (0) and unanswered (None), one of each: sorting
        # with None last gives the fixed order [0, 1, None].
        self.assertEqual(sorted(answers, key=lambda a: (a is None, a)), [0, 1, None])
        self.assertEqual(len(answers), 3)

    def test_valve_verdict_rows_expect_rc0(self):
        v = [t for t in P.BATCHES["mg24-batch3"] if t["devtype"] == "0x0042"][0]
        for c in v["checks"]:
            if c["controller"] is None or c["controller"][0] != "verdict":
                continue
            self.assertTrue(c["controller"][4]["rc0"],
                            "every valve verdict row expects rc 0")

    def test_every_check_is_legal(self):
        for t in P.BATCHES["mg24-batch3"]:
            self.assertTrue(t["checks"], t["devtype"])
            for c in t["checks"]:
                self.assertIn(c["chip_cluster"] or t["chip_cluster"], P.CHIP_CLUSTERS)
                self.assertIsInstance(c["cluster"], int)
                self.assertIsInstance(c["attr"], int)
                if c["controller"] is not None:
                    kind = c["controller"][0]
                    if kind == "verdict":
                        self.assertEqual(len(c["controller"]), 5)
                        self.assertEqual(len(c["controller"][2]), 2)
                    else:
                        self.assertIn(kind, ("command", "write"))
                        self.assertEqual(len(c["controller"][2]), 3)


class _OrderingLink(_VerdictLink):
    """A _VerdictLink whose attribute moves AFTER the chip-tool command
    runs: a read before the command sees the at_value (so the second AT
    read, which the ordering rule runs before the action, agrees), and a
    read after it sees the moved value. Models an allowed command that
    moves the attribute (a locked door's LockState to Locked)."""

    def __init__(self, key, moved):
        super().__init__()
        self._key = key
        self._moved = moved
        self._acted = False

    def command(self, cmd, *a, **k):
        m = re.fullmatch(r"AT\+MTATTR=(\d+),(\d+),(\d+)(?:,(\S+))?", cmd)
        if m and m.group(4) is None:
            key = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            if key == self._key and self._acted:
                return (0, ["+MTATTR:%s,%s,%s,%s" % (m.group(1), m.group(2), m.group(3), self._moved)])
        return super().command(cmd, *a, **k)

    def await_urc_ts(self, pattern, timeout=5.0):
        # The +MTCMD forward is enqueued synchronously by the chip_call
        # double; when it is answered the chip-tool command has run, which
        # is when the attribute moves.
        got = super().await_urc_ts(pattern, timeout)
        if got is not None:
            self._acted = True
        return got


class TestVerdictKind(unittest.TestCase):
    """The verdict controller kind against fakes that model the harness:
    a link whose command() records every line and whose await_urc pops
    from a queue, and a chip_call double (installed over the module-level
    H._threaded_chip_call that invoke_chip falls back to, since prove_check
    always sets ctx.chip_call to None) that enqueues the +MTCMD forward and
    returns the (rc, out) the rc/status rows read."""

    def _patch_chip_call(self, chip):
        self._saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        self.addCleanup(self._unpatch_chip_call)

    def _unpatch_chip_call(self):
        if getattr(self, "_saved", None) is not None:
            H._threaded_chip_call = self._saved
        self._saved = None

    def test_allow_sends_one_mtcmdresp_and_every_row_passes(self):
        # The door lock's allow check: lock-door, forward (257,0), answer 1,
        # want rc0 + status 0x0.
        link = _VerdictLink()
        chip = _VerdictChip(link=link, ep=2, cluster=257, command=0, seq=7, rc=0, status="0x00")
        t = [x for x in P.BATCHES["mg24-batch3"] if x["devtype"] == "0x000A"][0]
        c = [c for c in t["checks"] if c["controller"] and c["controller"][0] == "verdict"
             and c["controller"][1][0] == "lock-door" and c["controller"][3] == 1][0]
        s = _FakeSuite()
        self._patch_chip_call(chip)
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c, "door lock ep2 lock-state lock-door allow")
        finally:
            self._unpatch_chip_call()
        # Exactly one AT+MTCMDRESP sent, and it is the allow for seq 7.
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         ["AT+MTCMDRESP=7,1"])
        got = {n: ok for n, ok in s.results}
        self.assertTrue(all(ok for ok in got.values()),
                        "every row passes: %s" % [n for n, ok in got.items() if not ok])
        self.assertIn("door lock ep2 lock-state lock-door allow forward 257/0 answered allow", got)
        self.assertIn("door lock ep2 lock-state lock-door allow chip-tool exits 0", got)
        self.assertTrue(got["door lock ep2 lock-state lock-door allow wire status 0x0"])

    def test_deny_with_status_0x1_passes_and_0x0_fails(self):
        # The door lock's deny check: unlock-door, forward (257,1), answer 0,
        # want rc fail + status 0x1.
        t = [x for x in P.BATCHES["mg24-batch3"] if x["devtype"] == "0x000A"][0]
        c = [c for c in t["checks"] if c["controller"] and c["controller"][0] == "verdict"
             and c["controller"][1][0] == "unlock-door"][0]
        # status 0x01 (FAILURE; UNSUPPORTED_COMMAND is 0x81) with a
        # non-zero rc: the row passes.
        link = _VerdictLink(null_read=(0x0101, 0x0000))
        chip = _VerdictChip(link=link, ep=2, cluster=257, command=1, seq=7, rc=1, status="0x01")
        s = _FakeSuite()
        self._patch_chip_call(chip)
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c, "door lock ep2 lock-state unlock-door deny")
        finally:
            self._unpatch_chip_call()
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         ["AT+MTCMDRESP=7,0"])
        self.assertTrue(got["door lock ep2 lock-state unlock-door deny chip-tool fails"])
        self.assertTrue(got["door lock ep2 lock-state unlock-door deny wire status 0x1"])
        # The same row with status 0x0 (Success) instead of 0x1 fails.
        # A fresh link: the first run's write already moved the lock-state
        # away from the at_value, so the second run's "controller reads
        # after the AT write" row would fail on a shared link.
        link2 = _VerdictLink(null_read=(0x0101, 0x0000))
        chip2 = _VerdictChip(link=link2, ep=2, cluster=257, command=1, seq=7, rc=1, status="0x00")
        s2 = _FakeSuite()
        self._patch_chip_call(chip2)
        try:
            P.prove_check(link2, chip2, s2, "0x4845", 2, t, c, "door lock ep2 lock-state unlock-door deny")
        finally:
            self._unpatch_chip_call()
        got2 = {n: ok for n, ok in s2.results}
        self.assertTrue(got2["door lock ep2 lock-state unlock-door deny chip-tool fails"])
        self.assertFalse(got2["door lock ep2 lock-state unlock-door deny wire status 0x1"])

    def test_unanswered_sends_no_mtcmdresp_and_passes_only_with_mtcmdto(self):
        # The door lock's unanswered check: lock-door, forward (257,0), answer
        # None, want rc fail + status 0x1. No AT+MTCMDRESP is sent; the row
        # passes only when +MTCMDTO:7 arrives, and fails without it.
        t = [x for x in P.BATCHES["mg24-batch3"] if x["devtype"] == "0x000A"][0]
        c = [c for c in t["checks"] if c["controller"] and c["controller"][0] == "verdict"
             and c["controller"][3] is None][0]
        # +MTCMDTO:7 arrives: passes.
        link = _VerdictLink()
        chip = _VerdictChip(link=link, ep=2, cluster=257, command=0, seq=7,
                            timeout=True, rc=1, status="0x01")
        s = _FakeSuite()
        self._patch_chip_call(chip)
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c, "door lock ep2 lock-state lock-door unanswered")
        finally:
            self._unpatch_chip_call()
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")], [])
        self.assertTrue(got["door lock ep2 lock-state lock-door unanswered forward 257/0 seen"])
        self.assertTrue(got["door lock ep2 lock-state lock-door unanswered unanswered -> +MTCMDTO"])
        self.assertTrue(got["door lock ep2 lock-state lock-door unanswered chip-tool fails"])
        self.assertTrue(got["door lock ep2 lock-state lock-door unanswered wire status 0x1"])
        # No +MTCMDTO: the forward is seen but the row fails.
        link2 = _VerdictLink()
        chip2 = _VerdictChip(link=link2, ep=2, cluster=257, command=0, seq=7,
                             timeout=False, rc=1, status="0x01")
        s2 = _FakeSuite()
        self._patch_chip_call(chip2)
        try:
            P.prove_check(link2, chip2, s2, "0x4845", 2, t, c, "door lock ep2 lock-state lock-door unanswered")
        finally:
            self._unpatch_chip_call()
        got2 = {n: ok for n, ok in s2.results}
        self.assertEqual([x for x in link2.sent if x.startswith("AT+MTCMDRESP=")], [])
        self.assertTrue(got2["door lock ep2 lock-state lock-door unanswered forward 257/0 seen"])
        self.assertFalse(got2["door lock ep2 lock-state lock-door unanswered unanswered -> +MTCMDTO"])

    def test_a_forward_for_another_cluster_is_not_taken(self):
        # The fake chip enqueues a forward for a DIFFERENT cluster than the
        # check expects; the responder must not take it (it is left queued),
        # the "forward seen" row fails, and nothing is answered.
        t = [x for x in P.BATCHES["mg24-batch3"] if x["devtype"] == "0x000A"][0]
        c = [c for c in t["checks"] if c["controller"] and c["controller"][0] == "verdict"
             and c["controller"][1][0] == "lock-door" and c["controller"][3] == 1][0]
        # The check expects forward (257,0); the chip enqueues (999,0).
        link = _VerdictLink()
        chip = _VerdictChip(link=link, ep=2, cluster=999, command=0, seq=7, rc=0, status="0x00")
        s = _FakeSuite()
        self._patch_chip_call(chip)
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c, "door lock ep2 lock-state lock-door allow")
        finally:
            self._unpatch_chip_call()
        got = {n: ok for n, ok in s.results}
        # No AT+MTCMDRESP for a forward that was never taken.
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")], [])
        self.assertFalse(got["door lock ep2 lock-state lock-door allow forward 257/0 answered allow"])
        # The stray forward is left queued for its own recipient.
        self.assertIn("+MTCMD:7,2,999,0", link.urcs)

    def test_attribute_change_after_the_verdict_does_not_fail(self):
        # Ordering rule: an allowed command may move the attribute. The
        # "second AT read agrees" row runs BEFORE the chip-tool command is
        # launched, and nothing about the attribute is asserted after the
        # action, so a fake whose attribute changes after the verdict does
        # not fail the check.
        t = [x for x in P.BATCHES["mg24-batch3"] if x["devtype"] == "0x000A"][0]
        c = [c for c in t["checks"] if c["controller"] and c["controller"][0] == "verdict"
             and c["controller"][1][0] == "lock-door" and c["controller"][3] == 1][0]
        # The attribute moves to 2 (Locked) after the verdict; the second AT
        # read happens before the chip-tool command, so it still agrees with
        # the at_value of 1, and nothing after the action re-reads it.
        link = _OrderingLink((2, 0x0101, 0x0000), "2")
        chip = _VerdictChip(link=link, ep=2, cluster=257, command=0, seq=7, rc=0, status="0x00")
        s = _FakeSuite()
        self._patch_chip_call(chip)
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c, "door lock ep2 lock-state lock-door allow")
        finally:
            self._unpatch_chip_call()
        got = {n: ok for n, ok in s.results}
        self.assertTrue(all(ok for ok in got.values()),
                        "no row fails when the attribute moves after the verdict: %s"
                        % [n for n, ok in got.items() if not ok])
        self.assertTrue(got["door lock ep2 lock-state lock-door allow second AT read agrees (1)"])


class _DrainingVerdictLink(_VerdictLink):
    """_VerdictLink whose drain behaves like ATLink.drain: it discards
    every queued URC and returns them, so one check's stray URC cannot
    satisfy the next check's expectation. The base _FakeLink.drain is a
    no-op, so only this fake can model the drain prove_check does before
    the write (AT_MT_SPEC 3.8: the echo row must prove THIS write raised
    its own +MTATTR, and a stale +MTATTR for the same endpoint, cluster
    and attribute would otherwise let a silent write pass)."""

    def drain(self, quiet=0.2):
        drained = list(self.urcs)
        del self.urcs[:]
        return drained


class _AtCmdEchoLink(_DrainingVerdictLink):
    """_DrainingVerdictLink plus the AT layer's echo of the at_cmd write.
    AT+MTVALVE (and AT+MTLOCK) make the AT layer raise a +MTATTR URC for
    the attribute the command drives, and the echo row's late branch
    (link.await_urc) finds it there. at_cmd_echo=false models a firmware
    that answers the command but raises no echo: the echo row must then
    fail, the way it would on the wire for a silent write."""

    def __init__(self, at_cmd_echo=True, **kw):
        super().__init__(**kw)
        self.at_cmd_echo = at_cmd_echo

    def command(self, cmd, *a, **k):
        res, lines = super().command(cmd, *a, **k)
        if self.at_cmd_echo:
            m = re.fullmatch(r"AT\+MTVALVE=(\d+),(\S+)", cmd)
            if m:
                self.urcs.append("+MTATTR:%s,129,4,%s" % (m.group(1), m.group(2)))
        return res, lines


def _valve_open_check():
    # The water valve's open check: the at_cmd form of the write step
    # (AT+MTVALVE=<ep>,1) with the echo row on.
    t = [x for x in P.BATCHES["mg24-batch3"] if x["devtype"] == "0x0042"][0]
    c = [c for c in t["checks"] if c["controller"] and c["controller"][0] == "verdict"
         and c["controller"][1][0] == "open"][0]
    return t, c


class TestVerdictKindStaleUrc(unittest.TestCase):
    """I2: the drain before the write step in prove_check. A stale
    +MTATTR URC for the check's own endpoint, cluster and attribute must
    be discarded before the write goes out, or the echo row would be
    satisfied by the stale line and a silent write would pass."""

    def _run_open_check(self, link, stale=()):
        t, c = _valve_open_check()
        chip = _VerdictChip(link=link, ep=3, cluster=129, command=0, seq=9,
                            rc=0, status="0x00")
        for u in stale:
            link.urcs.append(u)
        s = _FakeSuite()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", 3, t, c,
                          "water valve ep3 current-state open allow")
        finally:
            H._threaded_chip_call = saved
        return s

    def test_stale_mtattr_urc_is_drained_and_the_echo_row_fails(self):
        # The stale URC is the echo line itself, so without the pre-write
        # drain the await_urc late branch would find it and the row would
        # pass a write that raised no echo of its own.
        link = _DrainingVerdictLink()
        s = self._run_open_check(link, stale=["+MTATTR:3,129,4,1"])
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["water valve ep3 current-state open allow AT write echoes +MTATTR:3,129,4,1"])
        # The drain ran: the stale URC is gone, not left in the queue to
        # satisfy a later row.
        self.assertNotIn("+MTATTR:3,129,4,1", link.urcs)

    def test_write_with_no_stale_and_no_echo_fails(self):
        # The control case: no stale URC queued, the write raises no echo
        # of its own, so the echo row fails exactly as it should. The
        # drain is a no-op on the empty queue and changes nothing.
        link = _DrainingVerdictLink()
        s = self._run_open_check(link)
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["water valve ep3 current-state open allow AT write echoes +MTATTR:3,129,4,1"])
        self.assertEqual(link.urcs, [])


class TestVerdictKindAtCmdEcho(unittest.TestCase):
    """M9: the at_cmd echo path. AT+MTVALVE makes the AT layer raise a
    +MTATTR URC for the attribute it drives, and the echo row's late
    branch (link.await_urc) finds it: one test where the fake queues the
    echo (the row passes) and one where it does not (the row fails)."""

    def _run_open_check(self, at_cmd_echo):
        link = _AtCmdEchoLink(at_cmd_echo=at_cmd_echo)
        t, c = _valve_open_check()
        chip = _VerdictChip(link=link, ep=3, cluster=129, command=0, seq=9,
                            rc=0, status="0x00")
        s = _FakeSuite()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", 3, t, c,
                          "water valve ep3 current-state open allow")
        finally:
            H._threaded_chip_call = saved
        return link, s

    def test_at_cmd_write_with_echo_passes(self):
        link, s = self._run_open_check(at_cmd_echo=True)
        got = {n: ok for n, ok in s.results}
        # The null-read row fails against a bare fake (no null_read set on
        # the link), as in the other verdict-kind tests; every other row,
        # and the echo row, passes: the AT+MTVALVE write queued its own
        # +MTATTR echo, and the late branch found it.
        self.assertFalse(got["water valve ep3 current-state open allow AT read of the null seed -> +MTERR:5"])
        for name, ok in got.items():
            if "null seed" not in name:
                self.assertTrue(ok, "row fails: %s" % name)
        self.assertTrue(got["water valve ep3 current-state open allow AT write echoes +MTATTR:3,129,4,1"])

    def test_at_cmd_write_without_echo_fails(self):
        link, s = self._run_open_check(at_cmd_echo=False)
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["water valve ep3 current-state open allow AT write echoes +MTATTR:3,129,4,1"])


class TestBatch3RowNames(unittest.TestCase):
    def _fake_run_names(self):
        # Running prove_endpoint over both batch 3 types with the fakes:
        # the AT rows are plain, the descriptor read reads the devtype and
        # revision out of the chip tool, the controller rows enqueue their
        # +MTCMD URC. Returns the row names, results first, N/A after, as
        # the batch 1 and 2 helpers do.
        link = _VerdictLink()
        chip = _VerdictChip(link=link, ep=2, cluster=257, command=0, seq=7)
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            comp = P.composition_for("mg24-batch3")
            names = []
            for (ep, dev), t in zip(comp[1:], P.rows_for("mg24-batch3")):
                chip.ep = ep
                chip.devtype = t["devtype"]
                chip.revision = t["revision"]
                s = _FakeSuite()
                P.prove_endpoint(link, chip, s, "0x4845", ep, t)
                names += [n for n, _ in s.results]
                names += [n for n, _ in s.na]
        finally:
            H._threaded_chip_call = saved
        return names

    def test_fake_run_yields_no_duplicate_name(self):
        # The door lock's four checks all prove LockState, and the
        # " <command> <answer>" suffix the verdict kind adds disambiguates
        # them, so the names must not collide.
        names = self._fake_run_names()
        self.assertEqual(len(names), len(set(names)))

    def test_fake_run_yields_exactly_the_baseline_row_names(self):
        # Batch 3's proven result file is the pin, as batch 1's and 2's
        # are: the fake run must yield exactly the recorded endpoint row
        # names, N/A names among them.
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "platform", "silabs", "core-batch3.json")
        self.assertEqual(set(self._fake_run_names()), _baseline_endpoint_names(path))


# ---------------------------------------------------------------- batch 4

# Batch 4's devtypes in table order: power source, smoke/CO alarm, the
# trio (washer, dishwasher, dryer), mode select, chime.
BATCH4 = ["0x0011", "0x0076", "0x0073", "0x0075", "0x007C", "0x0027", "0x0146"]


class _Batch4Chip(_VerdictChip):
    """_VerdictChip widened to the batch 4 shapes: the forwarded payload
    the verdict rows filter on (the chime's PlayChimeSound 7; the trio's
    verdict forwards carry no payload), the ErrorStateID wire shape
    H.parse_status reads after a `status = 0xNN` line, and a no-forward
    switch for the no_forward rows (the SDK short-circuits before the
    delegate, so the firmware raises nothing). The +MTCMD line gains its
    payload fields positionally, the shape the _RX regex's {0,5} tail
    parses (the chime's 1-field payload is [7]). The link is the one
    passed to prove_endpoint or
    prove_check (the _VerdictLink's queue is the single wire both sides
    see): the chip_call double enqueues the +MTCMD forward the firmware
    would raise on that same link, so the queue the rows poll is the queue
    the rows read."""

    def __init__(self, payload=None, error_state=None,
                 forward=True, **kw):
        super().__init__(**kw)
        self.payload = payload
        self.error_state = error_state
        self.forward = forward

    def _response(self):
        # The response the rc/status rows read. The error_state and status
        # shapes are mutually exclusive: H.parse_status tries the
        # `status = 0xNN` shape FIRST, last match wins, so an output that
        # carried both would answer the hex status and the ErrorStateID
        # rows would read the wrong value.
        if self.error_state is not None:
            return (self.rc, "CHIP:TOO:   ErrorStateID: %d\n" % self.error_state)
        if self.status:
            return (self.rc, "CHIP:TOO:   status = %s\n" % self.status)
        return (self.rc, "")

    def run(self, args, timeout=None):
        if self.forward:
            if len(args) >= 4 and args[1] in ("read", "read-event"):
                return super().run(args, timeout=timeout)
            if self.link is not None:
                tail = ",".join(str(p) for p in self.payload) if self.payload else ""
                line = "+MTCMD:%d,%d,%d,%d" % (self.seq, self.ep,
                                               self.cluster, self.command)
                if tail:
                    line += "," + tail
                self.link.urcs.append(line)
                if self.timeout:
                    self.link.urcs.append("+MTCMDTO:%d" % self.seq)
            return self._response()
        # No forward at all (the no_forward rows): nothing is enqueued and
        # the response carries only what the rc/status rows read.
        return self._response()


class _RefusedLink(_VerdictLink):
    """A _VerdictLink that answers the refused write with a +MTERR code:
    the trio's Instance-served OperationalState row (AT+MTATTR=<ep>,96,4,2,
    AT_MT_SPEC 3.21's DE270). The at_refused kind sends that one line and
    nothing else -- no normal write before it, no read after it -- so the
    link only needs to refuse it; every other command answers as
    _VerdictLink does (and the check sends no other)."""

    def __init__(self, cmd, code):
        super().__init__()
        self._refuse = cmd
        self._code = code

    def command(self, cmd, *a, **k):
        if cmd == self._refuse:
            # ATLink._collect consumes the +MTERR:<n> line into the result
            # code (it never lands in lines), so (code, []) is the shape
            # prove_check's res == err check reads.
            return (self._code, [])
        return super().command(cmd, *a, **k)


class _SetupChip(_VerdictChip):
    """A _VerdictChip whose attribute reads carry a configurable body, so
    the setup read rows (the substrings prove_endpoint greps the chip-tool
    read for) pass when the list carries every named entry and fail when
    one is missing (a firmware that answers the list read but whose
    supported-modes / installed-chime-sounds list drops an entry)."""

    def __init__(self, link=None, read_body="", **kw):
        super().__init__(link=link, **kw)
        self.read_body = read_body

    def run(self, args, timeout=None):
        if len(args) >= 4 and args[1] in ("read", "read-event"):
            return (0, self.read_body)
        return super().run(args, timeout=timeout)


class TestBatch4Table(unittest.TestCase):
    def test_batch4_lists_every_type_once_after_the_anchor(self):
        comp = P.composition_for("mg24-batch4")
        self.assertEqual(comp[0], (1, "0x0100"))
        self.assertEqual([d for _, d in comp[1:]], BATCH4)

    def test_revisions_match_the_registry(self):
        want = {"0x0011": 1, "0x0076": 1, "0x0073": 2, "0x0075": 2,
                "0x007C": 2, "0x0027": 1, "0x0146": 1}
        got = {t["devtype"]: t["revision"] for t in P.BATCHES["mg24-batch4"]}
        self.assertEqual(got, want)

    def test_every_verdict_trio_carries_the_error_state(self):
        for dt in ("0x0073", "0x0075", "0x007C"):
            t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == dt][0]
            for c in t["checks"]:
                if c["controller"] is None or c["controller"][0] != "verdict":
                    continue
                want = c["controller"][4]
                self.assertIn("error_state", want, dt)
                self.assertEqual(want["rc0"], True, dt)

    def test_chime_play_chime_sound_carries_payload_7_without_a_chime_id(self):
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == "0x0146"][0]
        c = [c for c in t["checks"]
             if c["controller"] and c["controller"][0] == "verdict"
             and c["controller"][1][0] == "play-chime-sound"][0]
        self.assertEqual(c["controller"][5], 7)
        self.assertNotIn("--ChimeID", c["controller"][1])

    def test_chip_clusters_gains_the_batch4_names(self):
        for name in ("powersource", "smokecoalarm", "operationalstate",
                     "modeselect", "chime"):
            self.assertIn(name, P.CHIP_CLUSTERS)

    def test_trio_checks_use_operational_state_attribute_4(self):
        # OperationalState's OperationalState attribute is id 4 (the
        # refusal row's own AT line says AT+MTATTR=<ep>,96,4,2); every trio
        # check must prove that attribute, not id 0.
        for dt in ("0x0073", "0x0075", "0x007C"):
            t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == dt][0]
            for c in t["checks"]:
                self.assertEqual((c["cluster"], c["attr"]), (96, 4), dt)

    def test_chime_checks_use_selected_chime_1_and_enabled_2(self):
        # Chime attributes: InstalledChimeSounds 0, SelectedChime 1,
        # Enabled 2 (the AT+MTCHIME <what> field is a different numbering).
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == "0x0146"][0]
        for c in t["checks"]:
            self.assertEqual(c["cluster"], 1366)
            if c["chip_attr"] == "selected-chime":
                self.assertEqual(c["attr"], 1)
            elif c["chip_attr"] == "enabled":
                self.assertEqual(c["attr"], 2)


class TestNotifyKind(unittest.TestCase):
    """The notify kind against the fakes: the smoke/CO self-test-request
    forward is seq 0 (notify-only), so expect_notify must see it and NEVER
    send AT+MTCMDRESP; the host's own completion line (AT+MTALARM) and the
    read-event are the answer the firmware expects (AT_MT_SPEC 3.22)."""

    def _self_test_check(self):
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == "0x0076"][0]
        c = [c for c in t["checks"]
             if c["controller"] and c["controller"][0] == "notify"][0]
        return t, c

    def _run(self, ep=2, link=None, **chipkw):
        link = link if link is not None else _VerdictLink()
        chip = _Batch4Chip(link=link, ep=ep, **chipkw)
        s = _FakeSuite()
        t, c = self._self_test_check()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", ep, t, c,
                          "smoke/CO alarm ep2 smoke-state self-test-request")
        finally:
            H._threaded_chip_call = saved
        return link, s

    def test_seq0_forward_is_seen_and_never_answered(self):
        # The firmware raises +MTCMD:0,... (notify-only): the forward row
        # passes, the follow-up AT line and the event pass, and no
        # AT+MTCMDRESP is ever sent.
        link, s = self._run(ep=2, cluster=92, command=0, seq=0, rc=0)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         [])
        self.assertTrue(got["smoke/CO alarm ep2 smoke-state self-test-request"
                          " forward 92/0 is seq 0, not answered"])
        self.assertTrue(got["smoke/CO alarm ep2 smoke-state self-test-request"
                          " AT+MTALARM=2,5,0 -> OK"])
        self.assertTrue(got["smoke/CO alarm ep2 smoke-state self-test-request"
                          " self-test-complete event present"])
        self.assertTrue(got["smoke/CO alarm ep2 smoke-state self-test-request"
                          " chip-tool self-test-request exits 0"])

    def test_no_forward_fails_the_row_and_stays_unanswered(self):
        # The firmware raises no forward at all: the forward row fails and
        # still nothing is answered.
        link, s = self._run(ep=2, cluster=92, command=0, seq=0,
                            forward=False, rc=0)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         [])
        self.assertFalse(got["smoke/CO alarm ep2 smoke-state self-test-request"
                          " forward 92/0 is seq 0, not answered"])

    def test_nonseq0_forward_fails_and_is_not_answered(self):
        # An adjudicated forward where a notify is required: the +MTCMD
        # carries seq 7, not 0: expect_notify answers None, the forward
        # row fails, and expect_notify NEVER sends AT+MTCMDRESP -- the
        # seq-7 line is consumed by the await and is not answered.
        link, s = self._run(ep=2, cluster=92, command=0, seq=7, rc=0)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         [])
        self.assertEqual(link.urcs, [])
        self.assertFalse(got["smoke/CO alarm ep2 smoke-state self-test-request"
                             " forward 92/0 is seq 0, not answered"])

    def test_missing_selftestcomplete_event_fails_the_event_row(self):
        # chip-tool's read-event carries no SelfTestComplete: the event row
        # fails. The forward and the AT+MTALARM completion row still pass
        # (the firmware raised its seq-0 notify and the host's completion
        # line answered OK; the link answers every command the check sends).
        # The chip's read-event arm is patched to print nothing (the shape
        # _SetupChip uses for its list reads), every other run shape is
        # the _Batch4Chip default.
        link = _VerdictLink()
        chip = _Batch4Chip(link=link, ep=2, cluster=92, command=0, seq=0, rc=0)
        saved_run = chip.run
        def _no_event(args, timeout=None):
            if len(args) >= 4 and args[1] == "read-event":
                return (0, "")
            return saved_run(args, timeout=timeout)
        chip.run = _no_event
        s = _FakeSuite()
        t, c = self._self_test_check()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c,
                          "smoke/CO alarm ep2 smoke-state self-test-request")
        finally:
            H._threaded_chip_call = saved
            chip.run = saved_run
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         [])
        self.assertFalse(got["smoke/CO alarm ep2 smoke-state self-test-request"
                             " self-test-complete event present"])
        self.assertTrue(got["smoke/CO alarm ep2 smoke-state self-test-request"
                            " forward 92/0 is seq 0, not answered"])
        self.assertTrue(got["smoke/CO alarm ep2 smoke-state self-test-request"
                            " AT+MTALARM=2,5,0 -> OK"])


class TestVerdictPayloadKind(unittest.TestCase):
    """The verdict kind's 6th tuple element (batch 4): the payload the
    forward must carry. A matching forward is answered, a mismatching one
    is not taken (expect returns None, the row fails, no AT+MTCMDRESP)."""

    def _chime_check(self):
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == "0x0146"][0]
        c = [c for c in t["checks"] if c["controller"]
             and c["controller"][0] == "verdict"
             and c["controller"][1][0] == "play-chime-sound"][0]
        return t, c

    def _run(self, ep=7, **chipkw):
        link = _VerdictLink()
        chip = _Batch4Chip(link=link, ep=ep, **chipkw)
        s = _FakeSuite()
        t, c = self._chime_check()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", ep, t, c,
                          "0x0146 chime ep7 enabled play-chime-sound allow")
        finally:
            H._threaded_chip_call = saved
        return link, s

    def test_forward_with_payload_7_is_answered_and_passes(self):
        # The chime's PlayChimeSound forward carries payload 7 (the
        # SelectedChime the chime forwards): the row answers the seq-7
        # forward with the allow and every row passes.
        link, s = self._run(ep=7, cluster=1366, command=0, seq=7, payload=[7],
                            rc=0, value=1)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         ["AT+MTCMDRESP=7,1"])
        self.assertTrue(got["0x0146 chime ep7 enabled play-chime-sound allow"
                          " forward 1366/0 answered allow"])
        self.assertTrue(got["0x0146 chime ep7 enabled play-chime-sound allow"
                          " chip-tool exits 0"])
        # The verdict kind's own rows pass; the write step's read rows read
        # the fake's fixed value (no value double in this class) and are
        # not this test's subject.
        for n, ok in s.results:
            if " forward " in n or " chip-tool " in n:
                self.assertTrue(ok, n)

    def test_forward_with_payload_9_is_not_taken(self):
        # The forward carries 9, not the expected 7: _match's payload
        # filter answers None (it is a genuine failure to report, not a
        # foreign forward to leave alone -- the line was popped from the
        # queue by the await and is NOT left there), the "answered" row
        # fails, and the forward is not answered (no AT+MTCMDRESP).
        link, s = self._run(ep=7, cluster=1366, command=0, seq=7, payload=[9],
                            rc=0, value=1)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         [])
        self.assertFalse(got["0x0146 chime ep7 enabled play-chime-sound allow"
                          " forward 1366/0 answered allow"])
        self.assertNotIn("+MTCMD:7,7,1366,0,9", link.urcs)


class TestVerdictErrorStateKind(unittest.TestCase):
    """The want["error_state"] row: H.parse_status answers the
    `ErrorStateID: N` wire shape (a Success at the StatusIB level can still
    carry a non-zero ErrorStateID, so the row reads ErrorStateID, not the
    hex status). The trio's pause deny wants ErrorStateID 2; a response
    with ErrorStateID 0 fails the row."""

    def _pause_check(self):
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == "0x0073"][0]
        c = [c for c in t["checks"] if c["controller"]
             and c["controller"][0] == "verdict"
             and c["controller"][1][0] == "pause"][0]
        return t, c

    def _run(self, ep=2, **chipkw):
        link = _VerdictLink()
        chip = _Batch4Chip(link=link, ep=ep, **chipkw)
        s = _FakeSuite()
        t, c = self._pause_check()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", ep, t, c,
                          "laundry washer ep2 operational-state pause deny")
        finally:
            H._threaded_chip_call = saved
        return link, s

    def test_error_state_2_answers_the_deny_row(self):
        # ErrorStateID: 2 (the pause forward carries no payload): the row
        # passes and the allow/deny answer went out.
        link, s = self._run(ep=2, cluster=96, command=0, seq=7,
                            rc=0, error_state=2)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         ["AT+MTCMDRESP=7,0"])
        self.assertTrue(got["laundry washer ep2 operational-state pause deny"
                          " ErrorStateID 2"])
        self.assertTrue(got["laundry washer ep2 operational-state pause deny"
                          " forward 96/0 answered deny"])

    def test_error_state_0_fails_the_deny_row(self):
        # ErrorStateID: 0 instead of 2: the row fails.
        link, s = self._run(ep=2, cluster=96, command=0, seq=7,
                            rc=0, error_state=0)
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["laundry washer ep2 operational-state pause deny"
                          " ErrorStateID 2"])


class TestAtRefusedKind(unittest.TestCase):
    """The at_refused kind: the AT write for the Instance-served
    OperationalState attribute (AT+MTATTR=<ep>,96,4,2) is refused with the
    +MTERR code the table names (11) -- no forward, no read, no second
    read (AT_MT_SPEC 3.21's DE270 rule)."""

    def _refused_check(self):
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == "0x0073"][0]
        c = [c for c in t["checks"]
             if c["controller"] and c["controller"][0] == "at_refused"][0]
        return t, c

    def _run(self, link, ep=2):
        s = _FakeSuite()
        t, c = self._refused_check()
        # The at_refused kind sends the refused write and nothing else --
        # no normal write before it, no read after it -- so the check
        # produces exactly one row, named with the AT line and the +MTERR
        # code the table names.
        chip = _Batch4Chip(link=link, ep=ep, rc=0, value=0)
        P.prove_check(link, chip, s, "0x4845", ep, t, c,
                      "laundry washer ep2 operational-state AT write refused")
        return s

    def test_refused_with_code_11_passes(self):
        link = _RefusedLink("AT+MTATTR=2,96,4,2", 11)
        s = self._run(link)
        self.assertEqual([n for n, _ in s.results],
                         ["laundry washer ep2 operational-state AT write refused"
                          " AT+MTATTR=2,96,4,2 -> +MTERR:11"])
        got = {n: ok for n, ok in s.results}
        self.assertTrue(got["laundry washer ep2 operational-state AT write refused"
                          " AT+MTATTR=2,96,4,2 -> +MTERR:11"])
        self.assertTrue(all(ok for ok in got.values()),
                        "every row passes: %s" % [n for n, ok in got.items() if not ok])

    def test_refused_with_code_0_fails(self):
        # The write is accepted (res 0) instead of refused with 11: the row
        # fails.
        link = _RefusedLink("AT+MTATTR=2,96,4,2", 0)
        s = self._run(link)
        self.assertEqual([n for n, _ in s.results],
                         ["laundry washer ep2 operational-state AT write refused"
                          " AT+MTATTR=2,96,4,2 -> +MTERR:11"])
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["laundry washer ep2 operational-state AT write refused"
                          " AT+MTATTR=2,96,4,2 -> +MTERR:11"])


class TestSetupKind(unittest.TestCase):
    """The setup kind (mode select, chime): every AT line of the setup
    goes out once, in table order, and the chip-tool list read must carry
    every named entry (a missing supported-mode or installed chime sound
    fails the row)."""

    def _run(self, link, chip, ep, dt):
        s = _FakeSuite()
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == dt][0]
        # The write step's own read rows read the attribute back through
        # chip-tool: the fake must print the table's value, in the parse
        # shape the check names (the chime's enabled is bool), so those
        # rows pass on the value, not on the fake's fixed 1.
        def _value_for(chip=chip, t=t, saved_run=None):
            def _run(args, timeout=None):
                for c in t["checks"]:
                    if args and args[1] == "read" and args[2] == c["chip_attr"]:
                        chip.value = c["at_value"] if c["parse"] == "int" \
                            else (c["at_value"] != 0)
                # The setup list read (supported-modes, installed-chime-sounds)
                # is not one of the check's own attributes: it carries the
                # named entries the carries row filters on, served whole.
                if t.get("setup") and args and args[1] == "read" \
                        and args[2] == t["setup"][2][1]:
                    return (0, chip.read_body)
                return saved_run(args, timeout=timeout)
            return _run
        saved_run = chip.run
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        chip.run = _value_for(saved_run=saved_run)
        try:
            P.prove_endpoint(link, chip, s, "0x4845", ep, t)
        finally:
            H._threaded_chip_call = saved
            chip.run = saved_run
        return t, s

    def test_modeselect_setup_lines_sent_once_in_order(self):
        link = _VerdictLink()
        chip = _SetupChip(link=link, ep=6, devtype="0x0027", revision=1,
                          read_body="CHIP:TOO:   SupportedModes: 3\n"
                                    "CHIP:TOO:     Quiet\n"
                                    "CHIP:TOO:     Normal\n"
                                    "CHIP:TOO:     Boost\n")
        t, s = self._run(link, chip, 6, "0x0027")
        sent = [x for x in link.sent if x.startswith("AT+MTMODES=")]
        self.assertEqual(sent, ['AT+MTMODES=6,0,"Quiet",1,"Normal",2,"Boost"'])
        # The setup AT line is on the wire before the setup read (chip.run
        # for supported-modes), which is the ordering the row pair proves.
        got = {n: ok for n, ok in s.results}
        self.assertTrue(got["0x0027 mode select ep6 setup"
                          ' AT+MTMODES=6,0,"Quiet",1,"Normal",2,"Boost" -> OK'])
        self.assertTrue(got["0x0027 mode select ep6 setup supported-modes"
                          " read carries Quiet and Normal and Boost"])

    def test_modeselect_read_missing_a_named_entry_fails(self):
        # The list read drops Boost: the carries row fails, the AT write row
        # is unaffected.
        link = _VerdictLink()
        chip = _SetupChip(link=link, ep=6, devtype="0x0027", revision=1,
                          read_body="CHIP:TOO:   SupportedModes: 2\n"
                                    "CHIP:TOO:     Quiet\n"
                                    "CHIP:TOO:     Normal\n")
        t, s = self._run(link, chip, 6, "0x0027")
        got = {n: ok for n, ok in s.results}
        self.assertTrue(got["0x0027 mode select ep6 setup"
                          ' AT+MTMODES=6,0,"Quiet",1,"Normal",2,"Boost" -> OK'])
        self.assertFalse(got["0x0027 mode select ep6 setup supported-modes"
                             " read carries Quiet and Normal and Boost"])

    def test_chime_setup_lines_sent_once_in_order(self):
        link = _VerdictLink()
        chip = _SetupChip(link=link, ep=7, devtype="0x0146", revision=1,
                          read_body='CHIP:TOO:   InstalledChimeSounds: "Ding"\n')
        t, s = self._run(link, chip, 7, "0x0146")
        sent = [x for x in link.sent if x.startswith("AT+MTCHIMESOUNDS=")]
        self.assertEqual(sent, ['AT+MTCHIMESOUNDS=7,7,"Ding"'])
        got = {n: ok for n, ok in s.results}
        self.assertTrue(got["0x0146 chime ep7 setup AT+MTCHIMESOUNDS=7,7,\"Ding\" -> OK"])
        self.assertTrue(got["0x0146 chime ep7 setup installed-chime-sounds"
                          " read carries Ding"])

    def test_chime_read_missing_a_named_entry_fails(self):
        # The list read drops the Ding entry: the carries row fails.
        link = _VerdictLink()
        chip = _SetupChip(link=link, ep=7, devtype="0x0146", revision=1,
                          read_body='CHIP:TOO:   InstalledChimeSounds: 0\n')
        t, s = self._run(link, chip, 7, "0x0146")
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["0x0146 chime ep7 setup installed-chime-sounds"
                             " read carries Ding"])

    def test_setup_runs_before_the_checks(self):
        # The mode list must exist before change-to-mode 1: record the
        # order of link.command calls and the AT+MTMODES setup line must
        # precede the first AT+MTATTR write on that endpoint. The link
        # answers as the plain _FakeLink (its command answers an
        # AT+MTATTR write from its state) but records every command in
        # `order`, not in a sent/urcs list: the change-to-mode forward
        # arrives only as the chip-tool response this run patches in,
        # so the link never needs the _VerdictLink's urcs queue.
        link = _FakeLink()
        order = []
        inner = link.command
        def command(cmd, *a, **k):
            order.append(cmd)
            return inner(cmd, *a, **k)
        link.command = command
        chip = _SetupChip(link=link, ep=6, devtype="0x0027", revision=1,
                          read_body="CHIP:TOO:   SupportedModes: 3\n"
                                    "CHIP:TOO:     Quiet\n"
                                    "CHIP:TOO:     Normal\n"
                                    "CHIP:TOO:     Boost\n")
        def _status_only(args, timeout=None):
            return (0, "status = 0x00\n")
        chip.run = _status_only
        t, s = self._run(link, chip, 6, "0x0027")
        atlines = [c for c in order
                   if c.startswith("AT+MTMODES=") or c.startswith("AT+MTATTR=")]
        self.assertIn('AT+MTMODES=6,0,"Quiet",1,"Normal",2,"Boost"', atlines)
        self.assertLess(
            atlines.index('AT+MTMODES=6,0,"Quiet",1,"Normal",2,"Boost"'),
            atlines.index("AT+MTATTR=6,80,3,0"))


class TestNoForwardKind(unittest.TestCase):
    """The no_forward kind: one 3 s await for any +MTCMD on the
    endpoint and cluster (the pattern names neither command nor seq, so
    a forward on ANY command of the cluster fails the row -- including
    one the row itself does not send), None only when the window closes
    empty. No AT+MTCMDRESP is ever sent; the want dict carries rc0 and
    the optional ErrorStateID."""

    def _pause_no_forward_check(self):
        t = [x for x in P.BATCHES["mg24-batch4"] if x["devtype"] == "0x0073"][0]
        c = [c for c in t["checks"]
             if c["controller"] and c["controller"][0] == "no_forward"][0]
        return t, c

    def _run(self, ep=2, link=None, **chipkw):
        link = link if link is not None else _VerdictLink()
        chip = _Batch4Chip(link=link, ep=ep, **chipkw)
        s = _FakeSuite()
        t, c = self._pause_no_forward_check()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", ep, t, c,
                          "laundry washer ep2 operational-state pause no-forward")
        finally:
            H._threaded_chip_call = saved
        return link, s

    def test_nothing_forwarded_in_the_window_passes(self):
        # The SDK short-circuits (Pause from Stopped): no +MTCMD at all,
        # ErrorStateID: 3, rc 0: the row passes and nothing is answered.
        # The chip's cluster is 96 (the row's own cluster) so the fail
        # variant's forward is in-cluster and the pass variant's absence
        # proves nothing is enqueued at all.
        link, s = self._run(ep=2, cluster=96, command=0, seq=7, forward=False,
                            rc=0, error_state=3)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         [])
        self.assertEqual(link.urcs, [])
        self.assertTrue(got["laundry washer ep2 operational-state pause no-forward"
                          " no +MTCMD within 3s"])
        self.assertTrue(got["laundry washer ep2 operational-state pause no-forward"
                          " chip-tool exits 0"])
        self.assertTrue(got["laundry washer ep2 operational-state pause no-forward"
                          " ErrorStateID 3"])

    def test_a_forward_for_the_cluster_fails(self):
        # A +MTCMD for the cluster's own command 0 (Pause) arrives within
        # the window: the no-forward row fails, and no AT+MTCMDRESP is sent
        # (the kind never answers).
        link, s = self._run(ep=2, cluster=96, command=0, seq=7, forward=True,
                            rc=0, error_state=3)
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
                         [])
        self.assertFalse(got["laundry washer ep2 operational-state pause no-forward"
                          " no +MTCMD within 3s"])

    def test_a_late_forward_within_the_3s_window_fails(self):
        # The forward arrives late: it lands in the serial buffer while the
        # row's 3 s window is still open, and the row's await hands the
        # link that full window, so a +MTCMD for the cluster on ANY command
        # that lands within it fails the row. The fake models the real
        # link: its await_urc polls until its deadline (the base class's
        # single pass is the first poll of that loop), and the chip
        # enqueues the line while the window is open -- after the first
        # poll has already read the empty queue -- so the poll that the
        # real await_urc_ts runs when the line lands returns it, exactly
        # as the row's check would read it.
        class _LateChip(_Batch4Chip):
            def run(self, args, timeout=None):
                rc, out = super().run(args, timeout=timeout)
                if self.link is not None:
                    self.link.urcs.append("+MTCMD:7,2,96,9")
                return (rc, out)

        class _LateLink(_VerdictLink):
            def await_urc(self, pattern, timeout=5.0):
                self.last_timeout = timeout
                deadline = time.monotonic() + timeout
                rx = re.compile(pattern)
                while True:
                    for i, u in enumerate(self.urcs):
                        if rx.search(u):
                            return self.urcs.pop(i)
                    if time.monotonic() >= deadline:
                        return None
                    time.sleep(0.05)

        link = _LateLink()
        chip = _LateChip(link=link, ep=2, cluster=96, command=0, seq=7,
                         rc=0, error_state=3)
        s = _FakeSuite()
        t, c = self._pause_no_forward_check()
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c,
                          "laundry washer ep2 operational-state pause no-forward")
        finally:
            H._threaded_chip_call = saved
        got = {n: ok for n, ok in s.results}
        self.assertEqual(link.last_timeout, 3.0)
        self.assertEqual([x for x in link.sent
                          if x.startswith("AT+MTCMDRESP=")], [])
        self.assertFalse(got["laundry washer ep2 operational-state pause no-forward"
                             " no +MTCMD within 3s"])


class TestBatch4RowNames(unittest.TestCase):
    def _fake_run_names(self):
        # Running prove_endpoint over all seven batch 4 types with the
        # fakes: the chime's play-chime-sound row enqueues its
        # payload-carrying +MTCMD forward, the trio's verdict rows their
        # payload-less forwards, the notify row its seq-0 forward, the
        # no_forward and at_refused rows nothing. One link per endpoint (as
        # the batch 3 name test uses one link per endpoint's run shape)
        # keeps each endpoint's queue clean, and the chip's forward follows
        # the endpoint. Returns the row names, results first, N/A after.
        comp = P.composition_for("mg24-batch4")
        names = []
        for (ep, dev), t in zip(comp[1:], P.rows_for("mg24-batch4")):
            link = _VerdictLink()
            chip = _Batch4Chip(link=link, ep=ep, devtype=t["devtype"],
                               revision=t["revision"])
            chip.payload = [7] if t["devtype"] == "0x0146" else None
            # The write step's own read rows read the attribute back: print
            # the table's value per read (the chime's enabled in its bool
            # shape) so those rows pass on the value, not on a fixed 1.
            def _value_for(chip=chip, t=t):
                def _run(args, timeout=None):
                    for c in t["checks"]:
                        if args and args[1] == "read" and args[2] == c["chip_attr"]:
                            chip.value = c["at_value"] if c["parse"] == "int" \
                                else (c["at_value"] != 0)
                    return super(_Batch4Chip, chip).run(args, timeout=timeout)
                return _run
            saved_run = chip.run
            saved = H._threaded_chip_call
            H._threaded_chip_call = chip.chip_call
            chip.run = _value_for()
            try:
                s = _FakeSuite()
                P.prove_endpoint(link, chip, s, "0x4845", ep, t)
            finally:
                H._threaded_chip_call = saved
                chip.run = saved_run
            names += [n for n, _ in s.results]
            names += [n for n, _ in s.na]
        return names

    def test_fake_run_yields_no_duplicate_name(self):
        # The trio's three types share one check table (same attribute,
        # same commands) and the chime's two enabled checks share one
        # attribute: the per-type tag and the kind's suffixes must
        # disambiguate every row.
        names = self._fake_run_names()
        self.assertGreater(len(names), 0)
        self.assertEqual(len(names), len(set(names)))


class TestBatch5aTable(unittest.TestCase):
    def test_batch5a_lists_every_type_once_after_the_anchor(self):
        comp = P.composition_for("mg24-batch5a")
        self.assertEqual(comp, [(1, "0x0100"), (2, "0x002D"), (3, "0x010F"),
                                (4, "0x0110"), (5, "0x000F"), (6, "0x0303"),
                                (7, "0x0072")])

    def test_revisions_match_the_registry(self):
        self.assertEqual([t["revision"] for t in P.BATCHES["mg24-batch5a"]],
                         [2, 2, 2, 3, 3, 3])

    def test_generic_switch_checks(self):
        t = [x for x in P.BATCHES["mg24-batch5a"] if x["devtype"] == "0x000F"][0]
        self.assertEqual([(c["cluster"], c["attr"]) for c in t["checks"]],
                         [(59, 1), (59, 0), (59, 1)])
        self.assertIsNone(t["checks"][0]["controller"])
        self.assertEqual(t["checks"][1]["at_cmd"], "AT+MTSWITCH=%(ep)d")
        self.assertIsNone(t["checks"][1]["controller"])
        self.assertEqual(t["checks"][2]["controller"],
                         ("at_refused", ["AT+MTSWITCH=%(ep)d,1"], 1))

    def test_generic_switch_extra_reads(self):
        t = [x for x in P.BATCHES["mg24-batch5a"] if x["devtype"] == "0x000F"][0]
        self.assertEqual(t["extra_reads"],
                         [("read-event", "initial-press"),
                          ("read", "feature-map", 2)])

    def test_pump_checks_and_extra_reads(self):
        t = [x for x in P.BATCHES["mg24-batch5a"] if x["devtype"] == "0x0303"][0]
        self.assertEqual([(c["cluster"], c["attr"]) for c in t["checks"]],
                         [(6, 0), (512, 32)])
        self.assertEqual(t["checks"][1]["chip_cluster"],
                         "pumpconfigurationandcontrol")
        self.assertEqual(t["checks"][1]["controller"][0], "write")
        self.assertEqual(t["checks"][1]["controller"][2], (512, 32, 1))
        self.assertEqual(t["extra_reads"], [("read", "feature-map", 0)])

    def test_room_ac_checks_and_extra_reads(self):
        t = [x for x in P.BATCHES["mg24-batch5a"]
             if x["devtype"] == "0x0072"][0]
        self.assertEqual([(c["cluster"], c["attr"]) for c in t["checks"]],
                         [(6, 0), (513, 18)])
        self.assertEqual(t["checks"][1]["chip_cluster"], "thermostat")
        self.assertEqual(t["checks"][1]["controller"][2], (513, 18, 2100))
        self.assertEqual(t["extra_reads"], [("read", "feature-map", 2)])

    def test_chip_clusters_gains_the_batch5a_names(self):
        self.assertIn("switch", P.CHIP_CLUSTERS)
        self.assertIn("pumpconfigurationandcontrol", P.CHIP_CLUSTERS)


class _Batch5aChip(_Batch4Chip):
    """_Batch4Chip widened to the batch 5a read shapes: the extra_reads
    rows (feature-map, the switch's initial-press read-event) pass on the
    table's own value, and every write step's controller read (percent-
    setting, on-off, current-level, current-position, number-of-positions,
    operation-mode, occupied-heating-setpoint) passes on the controller's
    URC value -- the value the device holds after the controller action,
    the way the row's read-back row asserts it."""

    def _read_value(self, t, attr):
        for c in t["checks"]:
            if c["chip_attr"] == attr:
                if c["controller"] is not None:
                    want = c["controller"][2][2]
                else:
                    want = c["at_value"]
                return (want != 0) if c["parse"] == "bool" else want
        return None

    def run(self, args, timeout=None):
        if len(args) >= 4 and args[1] in ("read", "read-event"):
            if args[1] == "read-event":
                name = args[2].replace("-", " ").title().replace(" ", "")
                return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: 1\n"
                           % name)
            body = "0"
            for (ep, dt), t in zip(P.composition_for("mg24-batch5a")[1:],
                                   P.rows_for("mg24-batch5a")):
                if int(ep) == int(args[-1]) and t["devtype"] == dt:
                    body = str(self._read_value(t, args[2]))
                    break
            return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: %s\n"
                       % (args[2].replace("-", ""), body))
        return super().run(args, timeout=timeout)


class _Batch5aLink(_RefusedLink):
    """A name for _RefusedLink in the batch 5a run; it adds no behaviour.
    The constructor arguments make it refuse the generic switch's
    Instance-served write AT+MTSWITCH=<ep>,1 with +MTERR:1 (the table's
    at_refused code); _RefusedLink's command() answers the refused write,
    every other line as _VerdictLink does."""


class TestBatch5aRowNames(unittest.TestCase):
    def _fake_run_names(self):
        # Running prove_endpoint over all six batch 5a types with the
        # fakes (as TestBatch4RowNames does for batch 4): one link per
        # endpoint keeps each endpoint's queue clean, the chip's forward
        # follows the endpoint, and the chip's read rows pass on the
        # table's values so the names come from the table, not from the
        # answers. Returns the row names, results first, N/A after.
        comp = P.composition_for("mg24-batch5a")
        names = []
        for (ep, dev), t in zip(comp[1:], P.rows_for("mg24-batch5a")):
            link = _Batch5aLink("AT+MTSWITCH=%d,1" % ep, 1)
            chip = _Batch5aChip(link=link, ep=ep, devtype=t["devtype"],
                                revision=t["revision"])
            saved = H._threaded_chip_call
            H._threaded_chip_call = chip.chip_call
            try:
                s = _FakeSuite()
                P.prove_endpoint(link, chip, s, "0x4845", ep, t)
            finally:
                H._threaded_chip_call = saved
            names += [n for n, _ in s.results]
            names += [n for n, _ in s.na]
        return names

    def test_fake_run_yields_no_duplicate_name(self):
        # The generic switch's two current-position checks share one
        # attribute (the second carries the at_refused suffix) and the
        # pump and the room air conditioner both prove the same OnOff
        # on-off attribute: the per-type tag and the kind's suffixes must
        # disambiguate every row.
        names = self._fake_run_names()
        self.assertGreater(len(names), 0)
        self.assertEqual(len(names), len(set(names)))

    def test_the_bench_rows_the_brief_names_occur(self):
        names = set(self._fake_run_names())
        for name in ("0x000F generic switch ep5 current-position AT write"
                     " refused AT+MTSWITCH=5,1 -> +MTERR:1",
                     "0x000F generic switch ep5 initial-press event present",
                     "0x0303 pump ep6 feature-map = 0",
                     "0x0072 room air conditioner ep7 feature-map = 2"):
            self.assertIn(name, names)


class _ModeChip(_VerdictChip):
    """_VerdictChip widened to the ModeBase ChangeToMode shapes.

    The kind under test has no write step, so _VerdictChip's forward
    enqueuing is exactly what a change-to-mode run raises (nothing when
    the table answers "none"), and its out carries the chip-tool line
    the two status rows read. The ChangeToMode rows read the ModeBase
    status from the `status: <int>` line parse_change_to_mode_status
    parses (the interaction-model `status = 0x..` branch parse_status
    uses is not here: a denied ChangeToMode is still a StatusIB
    Success), and the current-mode row reads CurrentMode in the
    parse_int_attr shape, so out must carry both: a bare "status: N"
    line would also fail the current-mode row (and a `status = 0x..`
    line would also fail the ChangeToMode row)."""

    def __init__(self, *a, change_status=0, current_mode=1, forward=True,
                 **kw):
        kw.setdefault("status", "0x00")
        super().__init__(*a, **kw)
        self.change_status = change_status
        self.current_mode = current_mode
        self.forward = forward

    def run(self, args, timeout=None):
        if len(args) >= 4 and args[1] in ("read", "read-event"):
            if args[2].replace("-", "").lower() == "currentmode":
                body = str(self.current_mode)
            else:
                body = "1"
            return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: %s\n"
                       % (args[2].replace("-", ""), body))
        if not self.forward and args[1] == "change-to-mode":
            # The unlisted mode: the SDK answers the ChangeToMode itself
            # and nothing reaches the link, so the forward is not
            # enqueued (a no-forward row must pass on an empty queue).
            return (self.rc, "CHIP:TOO:   status: %d\n"
                             % self.change_status)
        # The ChangeToMode forward is the five-field wire form: the
        # command carries the mode as a payload field (spec 3.17's
        # ChangeToMode newMode), so +MTCMD:<seq>,<ep>,<cluster>,<cmd>,
        # <mode>. Enqueuing the bare four-field shape would leave the
        # tail empty and the responder's payload check (the check's
        # answer, 1 or 0) could never match: expect() would return None
        # and the AT+MTCMDRESP the row asserts would never be sent.
        if self.link is not None:
            self.link.urcs.append("+MTCMD:%d,%d,%d,%d,%d"
                                  % (self.seq, self.ep, self.cluster,
                                     self.command,
                                     int(args[2]) if args[2].isdigit()
                                     else 1))
        out = "CHIP:TOO:   status: %d\n" % self.change_status
        out += ("[1786148467.112] [3186308:3186310] [TOO]   "
                "CurrentMode: %d\n" % self.current_mode)
        return (0, out)


class TestModeVerdictKind(unittest.TestCase):
    """The mode_verdict kind against the same fakes the verdict tests
    use: a _VerdictLink (recorded commands, urcs queue) and the
    _ModeChip chip_call double, which enqueues the ChangeToMode
    forward and returns the (rc, out) the status rows read. The kind
    has no write step: the chip-tool call is the whole interaction,
    and its argv is [cluster, change-to-mode, <mode>, node, ep]."""

    def _patch_chip_call(self, chip):
        self._saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        self.addCleanup(self._unpatch_chip_call)

    def _unpatch_chip_call(self):
        if getattr(self, "_saved", None) is not None:
            H._threaded_chip_call = self._saved
        self._saved = None

    def _check_for_mode(self, mode, fcl=84):
        # The check the fake drives, by the mode it requests and the
        # cluster its ChangeToMode forward lands on: the table asks for
        # mode "1" twice (run-mode allow, forward 84; clean-mode allow,
        # forward 85), and for "0" (run-mode deny) and "5" (run-mode
        # unlisted) once each.
        t = [x for x in P.BATCHES["mg24-batch5b"] if x["devtype"] == "0x0074"][0]
        return [c for c in t["checks"]
                if c["controller"] and c["controller"][0] == "mode_verdict"
                and c["controller"][1][1] == mode
                and c["controller"][2][0] == fcl]

    def _run(self, link, chip, mode, prefix, fcl=84, answer=None):
        # (mode, fcl) picks the check the fake is to drive; fcl also
        # overrides the chip's forward cluster, so the forward the chip
        # enqueues is the check's own (the clean-mode check's lands on
        # cluster 85, not the run-mode 84 the chip defaults to). answer
        # overrides the table's adjudication word (the unlisted-fails
        # test drives an allow check through the none branch).
        checks = self._check_for_mode(mode, fcl)
        self.assertEqual(len(checks), 1, (mode, fcl))
        c = dict(checks[0])
        if answer is not None:
            ctrl = list(c["controller"])
            ctrl[3] = answer
            c["controller"] = tuple(ctrl)
        if fcl != chip.cluster:
            chip.cluster = fcl
        t = [x for x in P.BATCHES["mg24-batch5b"]
             if x["devtype"] == "0x0074"][0]
        s = _FakeSuite()
        self._patch_chip_call(chip)
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c, prefix)
        finally:
            self._unpatch_chip_call()
        return t, s

    def test_allow_passes_and_answers_the_forward(self):
        link = _VerdictLink()
        chip = _ModeChip(link=link, ep=2, cluster=84, command=0, seq=7,
                         rc=0, change_status=0, current_mode=1)
        t, s = self._run(link, chip, "1",
                         "rvcrunmode ep2 change-to-mode 1 allow")
        got = {n: ok for n, ok in s.results}
        self.assertEqual(
            [x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
            ["AT+MTCMDRESP=7,1"])
        self.assertEqual(
            [n for n, ok in s.results if not ok], [],
            "every row passes: %s"
            % [n for n, ok in s.results if not ok])
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 1 allow forward 84/0 answered"
            " allow", got)
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 1 allow ChangeToMode status 0",
            got)
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 1 allow current-mode = 1 after",
            got)

    def test_change_to_mode_argv_is_positional_before_the_destination(self):
        # chip-tool's ChangeToMode takes the mode as a positional command
        # argument: [cluster, change-to-mode, <mode>, node, ep]. A
        # regression to a flag shape (a --mode or --value word) must fail
        # this test: the mode is argv[2], and the destination (node, ep)
        # is the tail.
        t = [x for x in P.BATCHES["mg24-batch5b"] if x["devtype"] == "0x0074"][0]
        c = [c for c in t["checks"]
             if c["controller"] and c["controller"][0] == "mode_verdict"
             and c["controller"][1][1] == "1"][0]
        argv = []
        def _record(chip, args, timeout=60):
            argv.append(list(args))
            return super(_ModeChip, chip).chip_call(chip, args, timeout)
        link = _VerdictLink()
        chip = _ModeChip(link=link, ep=2, cluster=84, command=0, seq=7,
                         rc=0, change_status=0, current_mode=1)
        s = _FakeSuite()
        chip.chip_call = _record
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            P.prove_check(link, chip, s, "0x4845", 2, t, c,
                          "rvcrunmode ep2 change-to-mode 1 allow")
        finally:
            H._threaded_chip_call = saved
        self.assertEqual(len(argv), 1)
        self.assertEqual(argv[0], ["rvcrunmode", "change-to-mode", "1",
                                   "0x4845", "2"])

    def test_deny_passes_and_answers_zero(self):
        link = _VerdictLink()
        chip = _ModeChip(link=link, ep=2, cluster=84, command=0, seq=7,
                         rc=0, change_status=2, current_mode=1)
        t, s = self._run(link, chip, "0",
                         "rvcrunmode ep2 change-to-mode 1 deny")
        got = {n: ok for n, ok in s.results}
        self.assertEqual(
            [x for x in link.sent if x.startswith("AT+MTCMDRESP=")],
            ["AT+MTCMDRESP=7,0"])
        self.assertEqual([n for n, ok in s.results if not ok], [],
                         "every row passes: %s"
                         % [n for n, ok in s.results if not ok])
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 1 deny forward 84/0 answered"
            " deny", got)
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 1 deny ChangeToMode status 2",
            got)
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 1 deny current-mode = 1 after",
            got)

    def test_unexpected_status_fails_only_the_status_row(self):
        # The allow is answered (the firmware accepts the change) but the
        # response's status says 2 (kGenericFailure): the status row is
        # the one that fails; the forward and the current-mode rows still
        # pass.
        link = _VerdictLink()
        chip = _ModeChip(link=link, ep=2, cluster=84, command=0, seq=7,
                         rc=0, change_status=2, current_mode=1)
        t, s = self._run(link, chip, "1",
                         "rvcrunmode ep2 change-to-mode 1 allow")
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["rvcrunmode ep2 change-to-mode 1 allow"
                             " ChangeToMode status 0"])
        self.assertTrue(got["rvcrunmode ep2 change-to-mode 1 allow"
                            " forward 84/0 answered allow"])
        self.assertTrue(got["rvcrunmode ep2 change-to-mode 1 allow"
                            " current-mode = 1 after"])
        self.assertEqual(
            [n for n, ok in s.results if not ok],
            ["rvcrunmode ep2 change-to-mode 1 allow ChangeToMode status 0"])

    def test_unlisted_passes_and_answers_nothing(self):
        # No +MTCMD arrives in the window and the response says
        # kUnsupportedMode: the no-forward row passes and the link is
        # never answered.
        link = _VerdictLink()
        chip = _ModeChip(link=link, ep=2, cluster=84, command=0, seq=7,
                         rc=0, change_status=1, current_mode=1,
                         forward=False)
        t, s = self._run(link, chip, "5",
                         "rvcrunmode ep2 change-to-mode 5 unlisted")
        got = {n: ok for n, ok in s.results}
        self.assertEqual([x for x in link.sent
                          if x.startswith("AT+MTCMDRESP=")], [])
        self.assertEqual(link.urcs, [])
        self.assertEqual([n for n, ok in s.results if not ok], [],
                         "every row passes: %s"
                         % [n for n, ok in s.results if not ok])
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 5 unlisted"
            " no +MTCMD for the command", got)
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 5 unlisted ChangeToMode status 1",
            got)
        self.assertIn(
            "rvcrunmode ep2 change-to-mode 5 unlisted current-mode = 1 after",
            got)

    def test_unlisted_fails_when_a_forward_does_arrive(self):
        # A ChangeToMode forward lands on the link after the response
        # has joined: the no-forward row awaits the check's own forward
        # cluster (84, run-mode) for the command's short window, so a
        # forward on that cluster is the arrival that fails the row,
        # and the kind still answers nothing (the adjudication for an
        # unlisted mode is the no-forward row, not a response). The chip
        # stays forward-less, so the window sees only the arrival.
        link = _VerdictLink()
        chip = _ModeChip(link=link, ep=2, cluster=84, command=0, seq=7,
                         rc=0, change_status=1, current_mode=1,
                         forward=False)
        # A run-mode ChangeToMode forward (cluster 84) already on the
        # link when the join returns: the await checks the queue before
        # the wire, so the no-forward row sees exactly this arrival.
        link.urcs.append("+MTCMD:7,2,84,0,1")
        # The unlisted check (mode "5", forward cluster 84, want status
        # 1): its no-forward row sees the arrival and fails; the status
        # and current-mode rows still pass on the response.
        t, s = self._run(link, chip, "5",
                         "rvcrunmode ep2 change-to-mode 5 unlisted")
        got = {n: ok for n, ok in s.results}
        self.assertFalse(got["rvcrunmode ep2 change-to-mode 5 unlisted"
                             " no +MTCMD for the command"])
        self.assertEqual([x for x in link.sent
                          if x.startswith("AT+MTCMDRESP=")], [])
        # The arrival is consumed by the await (ATLink's pop semantics):
        # the row saw it, so it is off the queue; the status and
        # current-mode rows still pass on the response.
        self.assertEqual(link.urcs, [])
        self.assertTrue(got["rvcrunmode ep2 change-to-mode 5 unlisted"
                              " ChangeToMode status 1"])
        self.assertTrue(got["rvcrunmode ep2 change-to-mode 5 unlisted"
                              " current-mode = 1 after"])


class TestSetupListForm(unittest.TestCase):
    """The setup's list form (batch 5b): a setup whose third element is a
    LIST of (cluster, attr, names) reads sends every AT line once, reads
    every named cluster, and names each read row with its chip cluster
    (both of the RVC's reads are supported-modes, so the cluster word is
    the only thing that tells the rows apart). Batch 4's single-tuple
    form still names its row without the cluster word, byte for byte."""

    def test_rvc_setup_sends_each_at_line_once_and_reads_both_clusters(self):
        link = _VerdictLink()
        chip = _SetupChip(link=link, ep=2, devtype="0x0074", revision=4,
                          read_body="CHIP:TOO:   SupportedModes: 4\n"
                                    "CHIP:TOO:     Idle\n"
                                    "CHIP:TOO:     Cleaning\n"
                                    "CHIP:TOO:     Vacuum\n"
                                    "CHIP:TOO:     Mop\n")
        t = [x for x in P.BATCHES["mg24-batch5b"]
             if x["devtype"] == "0x0074"][0]
        self.assertTrue(isinstance(t["setup"][2], list))
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            s = _FakeSuite()
            P.prove_endpoint(link, chip, s, "0x4845", 2, t)
        finally:
            H._threaded_chip_call = saved
        self.assertEqual(
            [x for x in link.sent if x.startswith("AT+MTMODES=")],
            ["AT+MTMODES=2,84,0,16384,\"Idle\",1,0,\"Cleaning\"",
             "AT+MTMODES=2,85,0,0,\"Vacuum\",1,0,\"Mop\""])
        got = {n: ok for n, ok in s.results}
        self.assertIn(
            '0x0074 robotic vacuum cleaner ep2 setup AT+MTMODES=2,84,0,'
            '16384,"Idle",1,0,"Cleaning" -> OK', got)
        self.assertIn(
            '0x0074 robotic vacuum cleaner ep2 setup AT+MTMODES=2,85,0,0,'
            '"Vacuum",1,0,"Mop" -> OK', got)
        self.assertIn(
            "0x0074 robotic vacuum cleaner ep2 setup rvcrunmode"
            " supported-modes read carries Idle and Cleaning", got)
        self.assertIn(
            "0x0074 robotic vacuum cleaner ep2 setup rvccleanmode"
            " supported-modes read carries Vacuum and Mop", got)
        # Both rows pass: the fake's read carries all four entries, and
        # the rows filter on their own pair.
        self.assertTrue(
            got["0x0074 robotic vacuum cleaner ep2 setup rvcrunmode"
                 " supported-modes read carries Idle and Cleaning"])
        self.assertTrue(
            got["0x0074 robotic vacuum cleaner ep2 setup rvccleanmode"
                 " supported-modes read carries Vacuum and Mop"])

    def test_rvc_setup_read_missing_a_name_fails_its_row_only(self):
        # The list read drops Cleaning: the rvcrunmode carries row
        # fails, the rvccleanmode row (Vacuum and Mop still there)
        # passes, and the AT write rows are unaffected.
        link = _VerdictLink()
        chip = _SetupChip(link=link, ep=2, devtype="0x0074", revision=4,
                          read_body="CHIP:TOO:   SupportedModes: 3\n"
                                    "CHIP:TOO:     Idle\n"
                                    "CHIP:TOO:     Vacuum\n"
                                    "CHIP:TOO:     Mop\n")
        t = [x for x in P.BATCHES["mg24-batch5b"]
             if x["devtype"] == "0x0074"][0]
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            s = _FakeSuite()
            P.prove_endpoint(link, chip, s, "0x4845", 2, t)
        finally:
            H._threaded_chip_call = saved
        got = {n: ok for n, ok in s.results}
        self.assertFalse(
            got["0x0074 robotic vacuum cleaner ep2 setup rvcrunmode"
                 " supported-modes read carries Idle and Cleaning"])
        self.assertTrue(
            got["0x0074 robotic vacuum cleaner ep2 setup rvccleanmode"
                 " supported-modes read carries Vacuum and Mop"])
        self.assertTrue(
            got['0x0074 robotic vacuum cleaner ep2 setup AT+MTMODES=2,84,0,'
                '16384,"Idle",1,0,"Cleaning" -> OK'])
        self.assertTrue(
            got['0x0074 robotic vacuum cleaner ep2 setup AT+MTMODES=2,85,0,0,'
                '"Vacuum",1,0,"Mop" -> OK'])

    def test_batch4_single_tuple_setup_keeps_its_old_row_name(self):
        # The mode select's setup third element is one (cluster, attr,
        # names) tuple, not a list: its row keeps batch 4's name, with
        # no cluster word in it.
        link = _VerdictLink()
        chip = _SetupChip(link=link, ep=6, devtype="0x0027", revision=1,
                          read_body="CHIP:TOO:   SupportedModes: 3\n"
                                    "CHIP:TOO:     Quiet\n"
                                    "CHIP:TOO:     Normal\n"
                                    "CHIP:TOO:     Boost\n")
        t = [x for x in P.BATCHES["mg24-batch4"]
             if x["devtype"] == "0x0027"][0]
        self.assertFalse(isinstance(t["setup"][2], list))
        saved = H._threaded_chip_call
        H._threaded_chip_call = chip.chip_call
        try:
            s = _FakeSuite()
            P.prove_endpoint(link, chip, s, "0x4845", 6, t)
        finally:
            H._threaded_chip_call = saved
        names = [n for n, _ in s.results]
        self.assertIn(
            "0x0027 mode select ep6 setup supported-modes read carries"
            " Quiet and Normal and Boost", names)
        # No list-form row name for the same read: the cluster word
        # (modebase) must NOT be in the row.
        self.assertNotIn(
            "0x0027 mode select ep6 setup modebase supported-modes read"
            " carries Quiet and Normal and Boost", names)


class TestProductIdGuard(unittest.TestCase):
    """main's pairing guard: the paired output must name the expected
    product id, or the guard row fails and the endpoint proofs are
    skipped (a proof on the wrong board proves nothing). The guard's
    test is the module-level paired_board_ok, which main calls on the
    pairing output: the tests exercise that function, and the source
    sniff below pins that main calls it with the row name, the expected
    id, and the abort print that follows a failed pairing."""

    def _pairing_row(self, out):
        # The guard as main calls it: the row name, the check on
        # paired_board_ok's result, and the skip decision the rest of
        # main makes on it.
        s = _FakeSuite()
        paired = s.check("commission: paired the board under test (%s)"
                         % P.EXPECTED_PRODUCT_ID, P.paired_board_ok(out))
        return s, paired

    def test_expected_product_id_passes_the_guard_row(self):
        out = ("PairingComplete: Success\n"
               "Discriminator: 0xF00\n"
               "productId=0x8010\n")
        s, paired = self._pairing_row(out)
        self.assertTrue(paired)
        self.assertEqual(
            [n for n, ok in s.results],
            ["commission: paired the board under test (productId=0x8010)"])
        self.assertTrue(s.results[0][1])

    def test_a_wrong_product_id_fails_the_guard_and_skips_the_proofs(self):
        # The other Hearth board shares the discriminator; its product
        # id is 0x8000: the guard row fails and main skips the endpoint
        # proofs (and prints the pairing tail, as main does on a failed
        # pairing).
        out = ("PairingComplete: Success\n"
               "Discriminator: 0xF00\n"
               "productId=0x8000\n")
        s, paired = self._pairing_row(out)
        self.assertFalse(paired)
        self.assertEqual(
            [n for n, ok in s.results],
            ["commission: paired the board under test (productId=0x8010)"])
        self.assertFalse(s.results[0][1])
        # The guard's row name is the one main records for this check:
        # the skip is the decision main makes on the failed row.
        self.assertIn("commission:", s.results[0][0])
        # main prints the pairing tail (scrubbed of the psk) when the
        # pairing output fails the guard.
        tail = H._pairing_tail(out)
        self.assertIn("productId=0x8000", tail)

    def test_main_carries_the_guard_row_name(self):
        # The guard's row name is main's: the check name, the expected
        # id, and the skip print that follows a failed pairing must all
        # be in main, or the pinned bench rows and the abort message
        # would change. (main's guard spans two lines, so the check is
        # on the two parts, not the joined string.)
        src = open(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "mt_catalogue_proof.py")).read()
        self.assertIn('s.check("commission: paired the board under test (%s)"', src)
        self.assertIn("% EXPECTED_PRODUCT_ID,", src)
        self.assertIn("paired_board_ok(out)", src)
        self.assertIn("def paired_board_ok(out):", src)
        self.assertIn('EXPECTED_PRODUCT_ID = "productId=0x8010"', src)
        self.assertIn("ABORT: chip-tool paired another board (no %s in its output)", src)
        self.assertIn("ABORT: pairing failed, skipping the endpoint proofs", src)


class TestBatch5bTable(unittest.TestCase):
    def test_batch5b_lists_the_type_once_after_the_anchor(self):
        comp = P.composition_for("mg24-batch5b")
        self.assertEqual(comp, [(1, "0x0100"), (2, "0x0074")])

    def test_revisions_match_the_registry(self):
        self.assertEqual([t["revision"] for t in P.BATCHES["mg24-batch5b"]],
                         [4])

    def test_checks_carry_the_expected_cluster_and_attr_pairs(self):
        t = [x for x in P.BATCHES["mg24-batch5b"]
             if x["devtype"] == "0x0074"][0]
        self.assertEqual([(c["cluster"], c["attr"]) for c in t["checks"]],
                         [(84, 1), (84, 1), (84, 1), (85, 1),
                          (97, 4), (97, 4), (97, 4), (97, 4)])

    def test_mode_verdict_tuples(self):
        t = [x for x in P.BATCHES["mg24-batch5b"]
             if x["devtype"] == "0x0074"][0]
        mv = [c for c in t["checks"]
              if c["controller"] and c["controller"][0] == "mode_verdict"]
        self.assertEqual(len(mv), 4)
        # The answer, the response status, and the current mode each mode
        # change proves: allow/deny/unlisted/allow, 0/2/1/0, 1/1/1/1.
        self.assertEqual([c["controller"][3] for c in mv],
                         [1, 0, "none", 1])
        self.assertEqual([c["controller"][4]["mode_status"] for c in mv],
                         [0, 2, 1, 0])
        self.assertEqual([c["controller"][4]["current_mode"] for c in mv],
                         [1, 1, 1, 1])
        # The mode requested, in order: 1 (Idle), 0, 5 (not in the list),
        # 1 (Vacuum on the clean-mode cluster).
        self.assertEqual([c["controller"][1][1] for c in mv],
                         ["1", "0", "5", "1"])
        self.assertEqual([c["controller"][2] for c in mv],
                         [(84, 0), (84, 0), (84, 0), (85, 0)])
        self.assertEqual([c["controller"][5] for c in mv],
                         [1, 0, None, 1])

    def test_setup_carrying_the_two_at_lines_and_two_reads(self):
        t = [x for x in P.BATCHES["mg24-batch5b"]
             if x["devtype"] == "0x0074"][0]
        self.assertEqual(t["setup"][0], "setup")
        self.assertEqual(
            t["setup"][1],
            ["AT+MTMODES=%(ep)d,84,0,16384,\"Idle\",1,0,\"Cleaning\"",
             "AT+MTMODES=%(ep)d,85,0,0,\"Vacuum\",1,0,\"Mop\""])
        self.assertEqual(
            t["setup"][2],
            [("rvcrunmode", "supported-modes", ["Idle", "Cleaning"]),
             ("rvccleanmode", "supported-modes", ["Vacuum", "Mop"])])

    def test_chip_clusters_gain_the_batch5b_names(self):
        self.assertIn("rvcrunmode", P.CHIP_CLUSTERS)
        self.assertIn("rvccleanmode", P.CHIP_CLUSTERS)
        self.assertIn("rvcoperationalstate", P.CHIP_CLUSTERS)


class _Batch5bChip(_ModeChip):
    """_ModeChip widened to the batch 5b row-name run: the reads pass on
    the table's values (the current-mode reads print 1, the supported-
    modes reads carry every named entry, the operational-state read-
    backs print the check's at value) and the response shapes pass on
    the check the chip-tool call is for, so every row of the fake run
    passes and the row names come from the table, not from the answers
    (the pinned-name test needs every pinned name among the fake
    run's, pass or fail). The ChangeToMode responses carry the check's
    own mode_status (0, 2, 1, 0) and the operational-state responses
    carry their own ErrorStateID (0, 2, 3); the mode's own ChangeToMode
    forward is never enqueued, because the unlisted check's no-forward
    row must pass on the fake run."""

    def run(self, args, timeout=None):
        if len(args) >= 4 and args[1] in ("read", "read-event"):
            attr = args[2]
            if attr.replace("-", "").lower() == "currentmode":
                body = "1"
            elif attr == "supported-modes":
                body = ("CHIP:TOO:   SupportedModes: 4\n"
                        "CHIP:TOO:     Idle\n"
                        "CHIP:TOO:     Cleaning\n"
                        "CHIP:TOO:     Vacuum\n"
                        "CHIP:TOO:     Mop\n")
            else:
                body = str(self._at_value_for(attr))
            return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: %s\n"
                       % (attr.replace("-", ""), body))
        t = P.BATCHES["mg24-batch5b"][0]
        if len(args) >= 2 and args[1] == "change-to-mode":
            for c in t["checks"]:
                if (c["controller"] is not None
                        and c["controller"][0] == "mode_verdict"
                        and c["controller"][1][1] == args[2]):
                    # The mode is the discriminator for the three
                    # run-mode checks but the clean-mode allow shares
                    # it, so the row is matched by its cluster too.
                    if c["cluster"] != self.cluster:
                        continue
                    return (0, "CHIP:TOO:   status: %d\n"
                               % c["controller"][4]["mode_status"])
        if len(args) >= 2 and args[1] in ("pause", "resume", "go-home"):
            for c in t["checks"]:
                if c["controller"] is not None \
                        and c["controller"][1][0] == args[1]:
                    # The ErrorStateID the response carries is the want
                    # field of the check: index 4 in the verdict tuple
                    # (kind, args, (fcl, fcmd), answer, want), index 2
                    # in the no_forward tuple (kind, args, want).
                    want = c["controller"][4] \
                        if c["controller"][0] == "verdict" \
                        else c["controller"][2]
                    return (0, "CHIP:TOO:   ErrorStateID: %d\n"
                               % want["error_state"])
        return super().run(args, timeout=timeout)

    def _at_value_for(self, attr):
        # The check that owns the read: the one whose chip_attr matches.
        t = P.BATCHES["mg24-batch5b"][0]
        for c in t["checks"]:
            if c["chip_attr"] == attr:
                return c["at_value"]
        return 1


class TestBatch5bRowNames(unittest.TestCase):
    def _fake_run_names(self):
        # Running prove_endpoint over the batch 5b type with the fakes:
        # the mode_verdict rows answer their ChangeToMode forwards (the
        # unlisted one raises nothing), the verdict rows their forwards,
        # the no_forward row nothing. The chip's forward follows the
        # endpoint's cluster. Returns the row names, results first, N/A
        # after.
        comp = P.composition_for("mg24-batch5b")
        names = []
        for (ep, dev), t in zip(comp[1:], P.rows_for("mg24-batch5b")):
            link = _VerdictLink()
            chip = _Batch5bChip(link=link, ep=ep, devtype=t["devtype"],
                                revision=t["revision"])
            saved = H._threaded_chip_call
            H._threaded_chip_call = chip.chip_call
            try:
                s = _FakeSuite()
                P.prove_endpoint(link, chip, s, "0x4845", ep, t)
            finally:
                H._threaded_chip_call = saved
            names += [n for n, _ in s.results]
            names += [n for n, _ in s.na]
        return names

    def test_fake_run_yields_no_duplicate_name(self):
        # The three run-mode checks share one attribute (current-mode on
        # cluster 84) and the four operational-state checks share theirs
        # (cluster 97, attr 4): the mode_verdict suffix (cluster, command,
        # mode, adjudication) and the verdict suffix (command,
        # adjudication) must disambiguate every row.
        names = self._fake_run_names()
        self.assertGreater(len(names), 0)
        self.assertEqual(len(names), len(set(names)))

    def test_the_pinned_bench_names_occur(self):
        # Every row name the bench recorded (b5b-pinned-names.txt), the
        # commission: row excepted (main records it, not prove_endpoint),
        # must occur among the fake run's names. The list is a copy of
        # that file (the docs workspace is not part of this repo): the
        # unlisted change-to-mode row carries the name the kind gives it
        # now (no +MTCMD for the command), and the go-home no-forward
        # row keeps its own kind's name (no +MTCMD within 3s).
        pinned = [
            "0x0074 robotic vacuum cleaner ep2 current-mode rvcrunmode"
            " change-to-mode 5 unlisted ChangeToMode status 1",
            "0x0074 robotic vacuum cleaner ep2 current-mode rvcrunmode"
            " change-to-mode 5 unlisted current-mode = 1 after",
            "0x0074 robotic vacuum cleaner ep2 current-mode rvcrunmode"
            " change-to-mode 5 unlisted no +MTCMD for the command",
            "0x0074 robotic vacuum cleaner ep2 operational-state go-home"
            " no-forward AT+MTOPSTATE=2,0x42 -> OK",
            "0x0074 robotic vacuum cleaner ep2 operational-state go-home"
            " no-forward ErrorStateID 3",
            "0x0074 robotic vacuum cleaner ep2 operational-state go-home"
            " no-forward chip-tool exits 0",
            "0x0074 robotic vacuum cleaner ep2 operational-state go-home"
            " no-forward controller reads operational-state = 66 after the AT write",
            "0x0074 robotic vacuum cleaner ep2 operational-state go-home"
            " no-forward no +MTCMD within 3s",
            '0x0074 robotic vacuum cleaner ep2 setup AT+MTMODES=2,84,0,'
            '16384,"Idle",1,0,"Cleaning" -> OK',
            '0x0074 robotic vacuum cleaner ep2 setup AT+MTMODES=2,85,0,0,'
            '"Vacuum",1,0,"Mop" -> OK',
            "0x0074 robotic vacuum cleaner ep2 setup rvccleanmode"
            " supported-modes read carries Vacuum and Mop",
            "0x0074 robotic vacuum cleaner ep2 setup rvcrunmode"
            " supported-modes read carries Idle and Cleaning",
        ]
        names = set(self._fake_run_names())
        for name in pinned:
            self.assertIn(name, names, "missing pinned bench row: %s" % name)


# Batch 7a-1 (catalogue batch 7a-1 design spec section 4): variant rows,
# extra device types and the push kind (AT+MTMEAS, AT_MT_SPEC 3.25).

class _MeasLink(_VerdictLink):
    """_VerdictLink widened to AT+MTMEAS: a push answers OK with no lines
    and stores its value where the AT read serves it (cluster 144's field
    0 is Voltage, attribute 4, and field 2 ActivePower, attribute 8; cluster 145's field 0 is the CumulativeEnergyImported
    struct, attribute 1). refuse maps an exact command to its +MTERR code
    (ATLink._collect returns the code, never the line); null holds the
    (ep, cluster, attr) triples that read +MTERR:5 until pushed; stray,
    when set, is a +MTATTR line every push queues, the URC a push must
    never raise. at_read, when set, is the value every non-null live AT
    read answers."""

    def __init__(self, refuse=None, null=(), stray=None, at_read=None):
        super().__init__()
        self._refuse = dict(refuse or {})
        self._null = set(null)
        self._stray = stray
        self._at_read = at_read

    def command(self, cmd, *a, **k):
        if cmd in self._refuse:
            self.sent.append(cmd)
            return (self._refuse[cmd], [])
        m = re.fullmatch(r"AT\+MTMEAS=(\d+),(\d+),(\d+),(-?\d+)", cmd)
        if m:
            self.sent.append(cmd)
            ep, cl, field, value = (int(x) for x in m.groups())
            attr = 1 if cl == 145 else {0: 4, 2: 8}[field]
            self.state[(ep, cl, attr)] = str(value)
            self._null.discard((ep, cl, attr))
            if self._stray is not None:
                self.urcs.append(self._stray)
            return (0, [])
        m = re.fullmatch(r"AT\+MTATTR=(\d+),(\d+),(\d+)", cmd)
        if m and tuple(int(x) for x in m.groups()) in self._null:
            self.sent.append(cmd)
            return (5, [])
        if m and self._at_read is not None:
            # A live AT read that disagrees with what was pushed.
            self.sent.append(cmd)
            return (0, ["+MTATTR:%s,%s,%s,%s" % (m.group(1), m.group(2),
                                                 m.group(3), self._at_read)])
        return super().command(cmd, *a, **k)

    def assert_no_urc(self, pattern, window):
        return self.await_urc(pattern, window) is None


class _MeasChip:
    """chip-tool for the batch 7a-1 rows. A device-type-list read prints
    every (id, revision) in types; an attribute read prints what the link
    holds for (ep, cluster, attr), or override when set: an integer in the
    parse_int_attr shape, the energy attribute as an
    EnergyMeasurementStruct whose Energy line parse_energy_values reads."""

    ATTRS = {"voltage": (144, 4), "active-power": (144, 8),
             "cumulative-energy-imported": (145, 1)}

    def __init__(self, link, types=(), override=None):
        self.link = link
        self.types = list(types)
        self.override = override

    def run(self, args, timeout=None):
        if args[:3] == ["descriptor", "read", "device-type-list"]:
            out = "CHIP:TOO:   DeviceTypeList: %d entries\n" % len(self.types)
            for i, (d, r) in enumerate(self.types, 1):
                out += ("CHIP:TOO:     [%d]: {\n"
                        "CHIP:TOO:       DeviceType: %d (0x%04X)\n"
                        "CHIP:TOO:       Revision: %d\n"
                        "CHIP:TOO:     }\n" % (i, d, d, r))
            return (0, out)
        cl, attr = self.ATTRS[args[2]]
        value = self.link.state.get((int(args[4]), cl, attr), "0")
        if self.override is not None:
            value = self.override
        if cl == 145:
            return (0, "CHIP:TOO:   CumulativeEnergyImported: {\n"
                       "CHIP:TOO:     Energy: %s\n"
                       "CHIP:TOO:   }\n" % value)
        return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: %s\n"
                   % (args[2].replace("-", ""), value))


def _b7_row(devtype, variant=None):
    return [t for t in P.BATCHES["mg24-batch7a1"]
            if t["devtype"] == devtype and t.get("variant") == variant][0]


def _b7_types(t):
    return [(int(t["devtype"], 16), t["revision"])] + \
        [(int(d, 16), r) for d, r in t["extra_types"]]


class TestBatch7a1Table(unittest.TestCase):
    def test_composition_carries_the_variant_suffixes(self):
        self.assertEqual(P.composition_for("mg24-batch7a1"),
                         [(1, "0x0100"), (2, "0x0510"), (3, "0x0510,1"),
                          (4, "0x0514"), (5, "0x0514,1"), (6, "0x0309"),
                          (7, "0x0017"), (8, "0x0017,1")])

    def test_earlier_batches_stage_plain_ids(self):
        for b in P.BATCHES:
            if b == "mg24-batch7a1":
                continue
            for _ep, d in P.composition_for(b):
                self.assertNotIn(",", d, b)

    def test_seven_rows_all_revision_1(self):
        rows = P.BATCHES["mg24-batch7a1"]
        self.assertEqual(len(rows), 7)
        self.assertEqual([t["revision"] for t in rows], [1] * 7)
        for t in rows:
            self.assertIn(t["chip_cluster"], P.CHIP_CLUSTERS)

    def test_extra_types_name_power_source_and_electrical_sensor(self):
        for t in P.BATCHES["mg24-batch7a1"]:
            want = [("0x0011", 1), ("0x0510", 1)] \
                if t["devtype"] in ("0x0309", "0x0017") else []
            self.assertEqual(t["extra_types"], want, t["name"])

    def test_push_and_refusal_counts(self):
        kinds = [c["controller"][0] for t in P.BATCHES["mg24-batch7a1"]
                 for c in t["checks"]]
        self.assertEqual(kinds.count("push"), 10)
        self.assertEqual(kinds.count("at_refused"), 3)
        self.assertEqual(len(kinds), 13)

    def test_chip_clusters_gain_the_batch7a1_names(self):
        for name in ("electricalpowermeasurement",
                     "electricalenergymeasurement", "powertopology"):
            self.assertIn(name, P.CHIP_CLUSTERS)


class TestPushKind(unittest.TestCase):
    V = "0x0510 electrical sensor ep2 voltage pushed"
    E = "0x0510 electrical sensor ep2 cumulative-energy-imported pushed"

    def _run(self, c, link, chip, prefix):
        s = _FakeSuite()
        P.prove_check(link, chip, s, "0x4845", 2, _b7_row("0x0510"), c, prefix)
        return s

    def _voltage(self):
        return _b7_row("0x0510")["checks"][0]

    def _energy(self):
        return _b7_row("0x0510")["checks"][1]

    def test_int_push_passes_every_row(self):
        link = _MeasLink(null={(2, 144, 4)})
        s = self._run(self._voltage(), link, _MeasChip(link), self.V)
        self.assertEqual(s.results, [
            (self.V + " AT read before the push -> +MTERR:5", True),
            (self.V + " AT+MTMEAS=2,144,0,230000 -> OK", True),
            (self.V + " AT+MTMEAS=2,144,0,230000 raises no +MTATTR", True),
            (self.V + " chip-tool electricalpowermeasurement read voltage = 230000", True),
            (self.V + " AT read = 230000", True)])
        self.assertEqual(link.sent, ["AT+MTATTR=2,144,4",
                                     "AT+MTMEAS=2,144,0,230000",
                                     "AT+MTATTR=2,144,4"])

    def test_a_seeded_value_fails_only_the_null_row(self):
        link = _MeasLink()
        s = self._run(self._voltage(), link, _MeasChip(link), self.V)
        self.assertEqual([n for n, ok in s.results if not ok],
                         [self.V + " AT read before the push -> +MTERR:5"])

    def test_a_urc_fails_only_the_no_urc_row(self):
        link = _MeasLink(null={(2, 144, 4)}, stray="+MTATTR:2,144,4,230000")
        s = self._run(self._voltage(), link, _MeasChip(link), self.V)
        self.assertEqual([n for n, ok in s.results if not ok],
                         [self.V + " AT+MTMEAS=2,144,0,230000 raises no +MTATTR"])

    def test_a_wrong_controller_value_fails_only_the_read_row(self):
        link = _MeasLink(null={(2, 144, 4)})
        s = self._run(self._voltage(), link, _MeasChip(link, override="1"), self.V)
        self.assertEqual([n for n, ok in s.results if not ok],
                         [self.V + " chip-tool electricalpowermeasurement read voltage = 230000"])

    def test_a_wrong_at_readback_fails_only_the_at_read_row(self):
        link = _MeasLink(null={(2, 144, 4)}, at_read="1")
        s = self._run(self._voltage(), link, _MeasChip(link), self.V)
        self.assertEqual([n for n, ok in s.results if not ok],
                         [self.V + " AT read = 230000"])

    def test_a_refused_push_fails_its_ok_row(self):
        link = _MeasLink(refuse={"AT+MTMEAS=2,144,0,230000": 3},
                         null={(2, 144, 4)})
        s = self._run(self._voltage(), link, _MeasChip(link), self.V)
        failed = [n for n, ok in s.results if not ok]
        self.assertIn(self.V + " AT+MTMEAS=2,144,0,230000 -> OK", failed)
        self.assertNotIn(self.V + " AT read before the push -> +MTERR:5", failed)

    def test_energy_push_reads_the_struct_and_skips_the_at_read(self):
        link = _MeasLink()
        s = self._run(self._energy(), link, _MeasChip(link), self.E)
        self.assertEqual(s.results, [
            (self.E + " AT+MTMEAS=2,145,0,1500000 -> OK", True),
            (self.E + " AT+MTMEAS=2,145,0,1500000 raises no +MTATTR", True),
            (self.E + " chip-tool electricalenergymeasurement read"
                      " cumulative-energy-imported = 1500000", True)])
        self.assertEqual(link.sent, ["AT+MTMEAS=2,145,0,1500000"])

    def test_an_energy_struct_without_the_value_fails_the_read_row(self):
        link = _MeasLink()
        s = self._run(self._energy(), link, _MeasChip(link, override="7"), self.E)
        self.assertEqual([n for n, ok in s.results if not ok],
                         [self.E + " chip-tool electricalenergymeasurement read"
                                   " cumulative-energy-imported = 1500000"])


class TestExtraTypes(unittest.TestCase):
    H6 = "0x0309 heat pump ep6 device-type-list carries "

    def _dt_rows(self, types):
        link = _MeasLink()
        s = _FakeSuite()
        P.prove_endpoint(link, _MeasChip(link, types=types), s, "0x4845", 6,
                         _b7_row("0x0309"))
        return [(n, ok) for n, ok in s.results if "device-type-list" in n]

    def test_all_three_pairs_pass(self):
        self.assertEqual(self._dt_rows([(0x0309, 1), (0x0011, 1), (0x0510, 1)]),
                         [(self.H6 + "(777, 1)", True),
                          (self.H6 + "(17, 1)", True),
                          (self.H6 + "(1296, 1)", True)])

    def test_a_missing_pair_fails_only_its_row(self):
        self.assertEqual(self._dt_rows([(0x0309, 1), (0x0510, 1)]),
                         [(self.H6 + "(777, 1)", True),
                          (self.H6 + "(17, 1)", False),
                          (self.H6 + "(1296, 1)", True)])


class TestBatch7a1RowNames(unittest.TestCase):
    REFUSE = {"AT+MTATTR=2,144,4,5": 11, "AT+MTMEAS=3,145,0,1500000": 3,
              "AT+MTMEAS=8,145,0,1500000": 3}

    def _fake_run(self):
        # Every endpoint of the table against the fakes: its own link (the
        # voltage on ep2 null until pushed, the three refusals answered
        # with their codes) and a chip whose device-type list carries the
        # row's own type and its extra types.
        comp = P.composition_for("mg24-batch7a1")
        results = []
        for (ep, _), t in zip(comp[1:], P.rows_for("mg24-batch7a1")):
            link = _MeasLink(refuse=self.REFUSE, null={(2, 144, 4), (3, 144, 8)})
            s = _FakeSuite()
            P.prove_endpoint(link, _MeasChip(link, types=_b7_types(t)), s,
                             "0x4845", ep, t)
            results += s.results
        return results

    def test_every_row_passes_on_the_fakes(self):
        results = self._fake_run()
        self.assertEqual(len(results), 55)
        self.assertEqual([n for n, ok in results if not ok], [])

    def test_no_duplicate_name(self):
        names = [n for n, _ in self._fake_run()]
        self.assertEqual(len(names), len(set(names)))

    def test_the_pinned_names_occur(self):
        pinned = [
            "0x0510 electrical sensor ep2 voltage pushed AT read before the push -> +MTERR:5",
            "0x0510 electrical sensor power-only ep3 active-power pushed AT read before the push -> +MTERR:5",
            "0x0510 electrical sensor ep2 voltage AT write refused"
            " AT+MTATTR=2,144,4,5 -> +MTERR:11",
            "0x0510 electrical sensor power-only ep3 cumulative-energy-imported"
            " AT write refused AT+MTMEAS=3,145,0,1500000 -> +MTERR:3",
            "0x0514 electrical meter power-only ep5 pushed AT read = 50000",
            "0x0309 heat pump ep6 device-type-list carries (17, 1)",
            "0x0309 heat pump ep6 cumulative-energy-imported pushed chip-tool"
            " electricalenergymeasurement read cumulative-energy-imported = 2500000",
            "0x0017 solar power ep7 pushed chip-tool electricalpowermeasurement"
            " read active-power = -3000000",
            "0x0017 solar power power-only ep8 device-type-list carries (1296, 1)",
            "0x0017 solar power power-only ep8 active-power pushed"
            " AT+MTMEAS=8,144,2,-1500000 raises no +MTATTR",
        ]
        names = set(n for n, _ in self._fake_run())
        for name in pinned:
            self.assertIn(name, names, "missing pinned row: %s" % name)


if __name__ == "__main__":
    unittest.main()
