#!/usr/bin/env python3
"""Self-test for the catalogue proof script: the table is legal, the row
names match the baseline, and the prove functions record the right rows."""
import os, re, sys, threading, unittest
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

def _baseline_endpoint_names():
    """core-batch1.json's endpoint row names: the results keys minus
    the compose:, commission: and restore: rows main() owns. The N/A
    names are among the keys (write_baseline merges suite.na into the
    same dict)."""
    import json
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
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
                 devtype="0x000A", revision=3):
        self.link = link
        self.ep = ep
        self.cluster = cluster
        self.command = command
        self.seq = seq
        self.timeout = timeout
        self.rc = rc
        self.status = status

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
            return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: 1\n"
                       % args[2].replace("-", ""))
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
        # status 0x01 (UNSUPPORTED_COMMAND) with a non-zero rc: the row passes.
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


class TestBatch3RowNames(unittest.TestCase):
    def test_fake_run_yields_no_duplicate_name(self):
        # Running prove_endpoint over both batch 3 types with the fakes yields
        # no duplicate row name: the door lock's four checks all prove
        # LockState, and the " <command> <answer>" suffix the verdict kind
        # adds disambiguates them.
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
        self.assertEqual(len(names), len(set(names)))


if __name__ == "__main__":
    unittest.main()
