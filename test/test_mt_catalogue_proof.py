#!/usr/bin/env python3
"""Hardware-free self-test for the catalogue proof script."""
import os, sys, unittest
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
            self.assertIsInstance(t["primary"]["cluster"], int)
            self.assertIsInstance(t["primary"]["attr"], int)

    def test_revisions_match_the_registry(self):
        want = {"0x0101": 3, "0x0015": 2, "0x0044": 1, "0x0041": 1, "0x0043": 1,
                "0x0107": 4, "0x0307": 2, "0x0305": 2, "0x0106": 3, "0x0306": 2,
                "0x010A": 4, "0x010B": 5}
        got = {t["devtype"]: t["revision"] for t in P.BATCHES["mg24-batch1"]}
        self.assertEqual(got, want)

class TestParsers(unittest.TestCase):
    def test_parse_bool_attr(self):
        self.assertIs(P.parse_bool_attr("CHIP:TOO:   StateValue: TRUE\n"), True)
        self.assertIs(P.parse_bool_attr("CHIP:TOO:   StateValue: FALSE\n"), False)
        self.assertIsNone(P.parse_bool_attr("nothing here"))

    def test_parse_device_types(self):
        out = ("CHIP:TOO:   DeviceTypeList: 1 entries\n"
               "CHIP:TOO:     [1]: {\n"
               "CHIP:TOO:       DeviceType: 257\n"
               "CHIP:TOO:       Revision: 3\n"
               "CHIP:TOO:     }\n")
        self.assertEqual(P.parse_device_types(out), [(257, 3)])

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

if __name__ == "__main__":
    unittest.main()
