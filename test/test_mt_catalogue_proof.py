#!/usr/bin/env python3
"""Self-test for the catalogue proof script: the table is legal, the row
names match the baseline, and the prove functions record the right rows."""
import os, re, sys, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mt_catalogue_proof as P

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
        m = re.search(r"MTATTR:(\d+),(\d+),(\d+),(-?\d+)", pattern)
        if m:
            v = self.state.get((int(m.group(1)), int(m.group(2)), int(m.group(3))), "0")
            return "+MTATTR:%s,%s,%s,%s" % (m.group(1), m.group(2), m.group(3), v)
        return None

class _FakeChip:
    """Every run exits 0 and prints the attribute under the chip-tool
    line shape parse_int_attr and parse_bool_attr parse: a `]` (the
    [TOO] tag closing bracket), 2+ spaces, then `Label: VALUE` at the
    line end. The brief's sketch said "CHIP:TOO:   X: <value>"; that
    plain shape has no `]`, so parse_int_attr answers None on it and
    every read row would fail. The bracket shape is the one the
    harness's own tests and fixtures use (test_parse_bool_attr,
    fixtures/t5/modes-current-mode.txt)."""

    def run(self, args, *a, **k):
        if len(args) >= 4 and args[1] in ("read", "read-event"):
            return (0, "[1786148467.112] [3186308:3186310] [TOO]   %s: 0\n" % args[2])
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
    chip = _FakeChip()
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
        chip = _FakeChip()
        th = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x0301"][0]
        s = _FakeSuite()
        P.prove_endpoint(link, chip, s, "0x4845", 2, th)
        got = {n: ok for n, ok in s.results}
        null_rows = [n for n in got if "null" in n.lower()]
        self.assertEqual(len(null_rows), 1)
        self.assertTrue(got[null_rows[0]])

    def test_null_read_answering_a_value_fails_the_row(self):
        link = _FakeLink(null_read=(0, ["+MTATTR:2,513,0,-500"]))
        chip = _FakeChip()
        th = [t for t in P.BATCHES["mg24-batch2"] if t["devtype"] == "0x0301"][0]
        s = _FakeSuite()
        P.prove_endpoint(link, chip, s, "0x4845", 2, th)
        got = {n: ok for n, ok in s.results}
        null_rows = [n for n in got if "null" in n.lower()]
        self.assertEqual(len(null_rows), 1)
        self.assertFalse(got[null_rows[0]])

if __name__ == "__main__":
    unittest.main()
