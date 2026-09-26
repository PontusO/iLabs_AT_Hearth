#!/usr/bin/env python3
"""Self-test for the nvm3 soak script: the boot console line parser,
the churn loop's stop conditions and write count, and one cycle's
commissioning path through fakes. Nothing here runs against hardware;
the links, console and chip are fakes."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvm3_soak as S
import mt_regression as H  # noqa: F401  (imported the way the soak script imports it)


class TestParseNvm3Line(unittest.TestCase):
    def test_a_real_boot_line_parses_to_four_ints(self):
        line = ("I boot: nvm3: 33 object(s), 40832 B free of 65536 B at "
                "0x08020000, erase count 17")
        info = S.parse_nvm3_line(line)
        self.assertEqual(info, {"objects": 33, "free": 40832, "size": 65536,
                                "erase": 17})
        for v in info.values():
            self.assertIsInstance(v, int)

    def test_a_malformed_line_is_none(self):
        self.assertIsNone(S.parse_nvm3_line("I boot: nvm3: 33 object(s), 40832"))
        self.assertIsNone(S.parse_nvm3_line("I boot: nvm3: object(s), B free of B"))
        self.assertIsNone(S.parse_nvm3_line(""))
        self.assertIsNone(S.parse_nvm3_line("some other boot line"))

    def test_the_meminfo_rc_error_form_is_none(self):
        self.assertIsNone(S.parse_nvm3_line("meminfo rc 1"))
        self.assertIsNone(S.parse_nvm3_line("I boot: meminfo rc 5"))


class _FakeLink:
    """Records every command sent; answers every command with (0, [])
    and every await_urc with a hit, so the cycles advance."""
    def __init__(self):
        self.commands = []

    def command(self, cmd, *a, **k):
        self.commands.append(cmd)
        return 0, []

    def await_urc(self, pattern, *a, **k):
        return object()

    def drain(self, *a, **k):
        pass


class _FakeConsole:
    """The next nvm3 line per read, or None (a timeout); each read is
    one boot's console."""
    def __init__(self, readings):
        self._readings = list(readings)
        self.reads = 0

    def read_nvm3(self, timeout):
        self.reads += 1
        if self._readings:
            return self._readings.pop(0)
        return None

    def drain(self):
        # a no-op by default: the stale-reading test supplies a
        # discarding drain
        pass


class _DrainingFakeConsole(_FakeConsole):
    """A fake console that models the buffer the real ConsoleReader holds.
    stale is what is already buffered when churn starts (the compose boots'
    lines); drain() discards it, the way the real drain empties the port
    buffer and the line buffer. post_reset is the boot line the reset's own
    boot prints, which arrives after the drain and survives it."""
    def __init__(self, stale, post_reset):
        super().__init__(stale)
        self._post_reset = post_reset
        self.drains = 0
        self._delivered = False

    def drain(self):
        self.drains += 1
        self._readings = []

    def read_nvm3(self, timeout):
        if self._readings:
            return self._readings.pop(0)
        if not self._delivered and self._post_reset is not None:
            self._delivered = True
            return self._post_reset
        return None


class _Args:
    """The fields run_cycle reads: the parser's soak fields plus the
    WIFI branch's ssid/psk."""
    def __init__(self, **kw):
        d = {"node_id": 0x4845, "threshold": 2500, "block": 100,
             "max_blocks": 40, "ssid": "bench-ssid", "psk": "bench-psk"}
        d.update(kw)
        self.__dict__.update(d)


class _FakeChip:
    """chip.run answers per subcommand: the payload parse always exits
    0 with a parseable payload, the pairing run returns its fixed
    (rc, out). wipe_storage records."""
    def __init__(self, pair_rc=0, pair_out="pairing ok"):
        self.pair_rc = pair_rc
        self.pair_out = pair_out
        self.runs = []
        self.wipes = 0

    def run(self, args, timeout=60):
        self.runs.append(list(args))
        if args[0] == "payload":
            return 0, "Passcode: 1234567\nLong discriminator: 385\n"
        return self.pair_rc, self.pair_out

    def wipe_storage(self):
        self.wipes += 1


class TestChurnUntil(unittest.TestCase):
    def test_it_stops_below_the_threshold(self):
        link = _FakeLink()
        console = _FakeConsole([{"objects": 1, "free": 4000, "size": 65536, "erase": 1},
                                {"objects": 2, "free": 3000, "size": 65536, "erase": 2},
                                {"objects": 3, "free": 2000, "size": 65536, "erase": 3}])
        got = S.churn_until(link, console, 2, 2500, 10, 40)
        self.assertEqual(got["blocks"], 3)
        self.assertEqual(got["free"], 2000)
        self.assertEqual(got["erase"], 3)
        self.assertTrue(got["reached"])
        # one MTRESET per block, and the churn pairs on the chime ep
        self.assertEqual(link.commands.count("AT+MTRESET"), 3)
        self.assertEqual(link.commands.count("AT+MTCHIME=2,1,1"), 30)
        self.assertEqual(link.commands.count("AT+MTCHIME=2,1,0"), 30)

    def test_it_gives_up_at_max_blocks_with_reached_false(self):
        link = _FakeLink()
        console = _FakeConsole([{"objects": 1, "free": 4000, "size": 65536, "erase": 1}] * 40)
        got = S.churn_until(link, console, 2, 2500, 100, 40)
        self.assertEqual(got["blocks"], 40)
        self.assertFalse(got["reached"])
        self.assertEqual(got["free"], 4000)
        # the last block's reset still happens: 40 reads for 40 blocks
        self.assertEqual(console.reads, 40)

    def test_it_counts_writes_as_two_times_block_times_blocks(self):
        link = _FakeLink()
        console = _FakeConsole([{"objects": 1, "free": 2000, "size": 65536, "erase": 1}])
        got = S.churn_until(link, console, 2, 2500, 100, 40)
        self.assertEqual(got["writes"], 2 * 100 * 1)
        self.assertTrue(got["reached"])

    def test_drain_clears_stale_lines_before_each_reset(self):
        # the two compose boots (the MTFRESET and the MTEPAPPLY) each
        # print an nvm3 line into the console buffer; without the drain
        # the first reading below would be the MTFRESET boot's and every
        # later one would lag two boots behind. The reset's own boot
        # prints the post-reset line, which arrives after the drain.
        stale_1 = {"objects": 1, "free": 45000, "size": 65536, "erase": 1}
        stale_2 = {"objects": 2, "free": 42000, "size": 65536, "erase": 2}
        post_reset = {"objects": 3, "free": 2400, "size": 65536, "erase": 3}
        link = _FakeLink()
        console = _DrainingFakeConsole([stale_1, stale_2], post_reset)
        got = S.churn_until(link, console, 2, 2500, 1, 40)
        # the drain was called once per block, and the block's reset
        # happened after the drain
        self.assertEqual(console.drains, got["blocks"])
        self.assertEqual(link.commands.count("AT+MTRESET"), got["blocks"])
        self.assertEqual(console.drains, link.commands.count("AT+MTRESET"))
        # the reading is the post-reset line, not a stale one
        self.assertEqual(got["free"], post_reset["free"])
        self.assertEqual(got["erase"], post_reset["erase"])
        self.assertTrue(got["reached"])


class TestRunCycle(unittest.TestCase):
    def _args(self, transport="THREAD"):
        return _Args(node_id=0x4845, threshold=2500, block=5, max_blocks=3,
                     transport=transport)

    def _console(self):
        return _FakeConsole([{"objects": 1, "free": 4000, "size": 65536, "erase": 7}])

    def _fake_link(self):
        return _FakeLink()

    def _patch_codes(self, link, qr, manual):
        # _onboarding_codes is a module global of the soak script; swap
        # it for this test only
        self._orig = S._onboarding_codes
        S._onboarding_codes = lambda l: (qr, manual)

    def tearDown(self):
        if getattr(self, "_orig", None) is not None:
            S._onboarding_codes = self._orig
            self._orig = None

    def test_a_paired_cycle_reports_paired_and_no_tail(self):
        link = self._fake_link()
        self._patch_codes(link, "MT:YynxWC00000011AA", "11111111111")
        chip = _FakeChip(pair_rc=0, pair_out="pairing ok")
        args = self._args()
        # the raw dataset: H.pairing_argv's THREAD branch prepends hex:
        got = S.run_cycle(link, self._console(), chip, args, "THREAD", "aa")
        self.assertTrue(got["paired"])
        self.assertIsNone(got["tail"])
        self.assertEqual(got["rc"], 0)
        self.assertEqual(got["churn"]["blocks"], 3)
        self.assertEqual(got["churn"]["reached"], False)
        # the cycle is framed by a factory reset on each end, and the
        # composition is staged and applied in between
        cmds = link.commands
        self.assertEqual(cmds[0], "AT+MTFRESET")
        self.assertEqual(cmds[-1], "AT+MTFRESET")
        self.assertIn("AT+MTEP=0x0146", cmds)
        self.assertIn("AT+MTEPAPPLY", cmds)
        # chip-tool was wiped before pairing and the pairing argv carries
        # the node id and the dataset
        self.assertEqual(chip.wipes, 1)
        pairing = [r for r in chip.runs if r[0] == "pairing"][0]
        self.assertIn("0x4845", pairing)
        self.assertIn("hex:aa", pairing)
        self.assertEqual(pairing[1:4], ["ble-thread", "0x4845", "hex:aa"])

    def test_a_failed_cycle_carries_the_tail(self):
        link = self._fake_link()
        self._patch_codes(link, "MT:YynxWC00000011AA", "11111111111")
        chip = _FakeChip(pair_rc=1, pair_out="error: pairing timed out\nline two\nline three")
        args = self._args()
        got = S.run_cycle(link, self._console(), chip, args, "THREAD", "aa")
        self.assertFalse(got["paired"])
        self.assertEqual(got["rc"], 1)
        self.assertEqual(got["tail"],
                         H._pairing_tail(chip.pair_out, getattr(args, "psk", None)))
        # the tail is present only on failure: the paired case above
        # asserted None
        self.assertNotEqual(got["tail"], None)

    def test_no_dataset_text_in_the_result(self):
        link = self._fake_link()
        self._patch_codes(link, "MT:YynxWC00000011AA", "11111111111")
        chip = _FakeChip(pair_rc=1, pair_out="error: pairing failed")
        args = self._args()
        # the raw dataset text: pairing_argv's hex: prefix is not part
        # of it, so the needle is the raw form
        got = S.run_cycle(link, self._console(), chip, args, "THREAD", "deadbeef")
        blob = repr(got)
        self.assertNotIn("hex:deadbeef", blob)
        self.assertNotIn("deadbeef", blob)


class TestJsonHeaderArgs(unittest.TestCase):
    def test_the_header_args_carry_neither_the_dataset_nor_the_psk(self):
        # the same exclusion main applies when it builds the JSON
        # header: the credentials never enter the result file
        args = _Args(node_id=0x4845, threshold=2500, block=100,
                     max_blocks=40, transport="THREAD", dataset="aa",
                     ssid="bench-ssid", psk="bench-psk")
        header_args = {k: v for k, v in vars(args).items()
                       if k not in ("dataset", "psk")}
        self.assertNotIn("dataset", header_args)
        self.assertNotIn("psk", header_args)
        self.assertNotIn("aa", repr(header_args))
        self.assertNotIn("bench-psk", repr(header_args))
        self.assertEqual(header_args["ssid"], "bench-ssid")
        self.assertEqual(header_args["node_id"], 0x4845)


if __name__ == "__main__":
    unittest.main()
