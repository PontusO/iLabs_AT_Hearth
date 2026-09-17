import unittest
import xmodem as x


class ScriptedReceiver:
    """A receiver that answers like the Gecko bootloader: 'C' to start
    (CRC mode), ACK each good block, NAK a block whose seq it dislikes."""

    def __init__(self, nak_blocks=()):
        self.sent = []
        self.nak_blocks = set(nak_blocks)
        self.replies = [bytes([x.CRC_C])]
        self.naks_left = dict.fromkeys(self.nak_blocks, 1)

    def read(self, n, timeout):
        return self.replies.pop(0) if self.replies else b""

    def write(self, data):
        self.sent.append(bytes(data))
        if data == bytes([x.EOT]):
            self.replies.append(bytes([x.ACK]))
            return
        seq = data[1]
        if seq in self.naks_left and self.naks_left[seq] > 0:
            self.naks_left[seq] -= 1
            self.replies.append(bytes([x.NAK]))
        else:
            self.replies.append(bytes([x.ACK]))


class TestCrc(unittest.TestCase):
    def test_known_vector(self):
        # CRC-16/XMODEM of "123456789" is 0x31C3 (standard check value).
        self.assertEqual(x.crc16_xmodem(b"123456789"), 0x31C3)


class TestBlock(unittest.TestCase):
    def test_1k_block_layout(self):
        b = x.build_block(1, b"\x00" * 1024)
        self.assertEqual(b[0], x.STX)
        self.assertEqual(b[1], 1)
        self.assertEqual(b[2], 0xFE)
        self.assertEqual(len(b), 3 + 1024 + 2)

    def test_short_payload_is_padded_with_0x1a(self):
        b = x.build_block(2, b"AB")
        self.assertEqual(b[3:5], b"AB")
        self.assertEqual(b[5], 0x1A)
        self.assertEqual(b[-2:], x.crc16_xmodem(b[3:-2]).to_bytes(2, "big"))

    def test_seq_wraps_at_256(self):
        self.assertEqual(x.build_block(256, b"")[1], 0)

    def test_128_block_layout_is_soh(self):
        # The size flash.py actually sends: the Gecko Bootloader's parser is
        # 128-byte blocks only (XMODEM_DATA_SIZE in btl_xmodem.h), so the
        # bench path is SOH, never STX. Covered here because the brief's own
        # cases only exercise the 1024-byte default, which this bootloader
        # does not accept.
        b = x.build_block(1, b"\x00" * 128, block_size=128)
        self.assertEqual(b[0], x.SOH)
        self.assertEqual(len(b), 3 + 128 + 2)

    def test_rejects_an_unsupported_block_size(self):
        with self.assertRaises(ValueError):
            x.build_block(1, b"", block_size=512)


class TestSend(unittest.TestCase):
    def test_sends_all_blocks_then_eot(self):
        rx = ScriptedReceiver()
        n = x.send(rx.read, rx.write, b"\x55" * 2500)
        self.assertEqual(n, 3)                      # 1024 + 1024 + 452 (padded)
        self.assertEqual(rx.sent[-1], bytes([x.EOT]))
        self.assertEqual([f[1] for f in rx.sent[:-1]], [1, 2, 3])

    def test_retransmits_a_naked_block(self):
        rx = ScriptedReceiver(nak_blocks=(2,))
        x.send(rx.read, rx.write, b"\x55" * 2500)
        seqs = [f[1] for f in rx.sent if f[0] in (x.STX, x.SOH)]
        self.assertEqual(seqs, [1, 2, 2, 3])

    def test_gives_up_after_retries(self):
        class AlwaysNak(ScriptedReceiver):
            def write(self, data):
                self.sent.append(bytes(data)); self.replies.append(bytes([x.NAK]))
        rx = AlwaysNak()
        with self.assertRaises(x.XmodemError):
            x.send(rx.read, rx.write, b"\x55" * 10, retries=3)

    def test_sends_128_byte_blocks_when_asked(self):
        # The bench path end to end: every frame is an SOH frame, the
        # sequence runs 1..N and the transfer still ends with EOT.
        rx = ScriptedReceiver()
        n = x.send(rx.read, rx.write, b"\x55" * 300, block_size=128)
        self.assertEqual(n, 3)                      # 128 + 128 + 44 (padded)
        self.assertEqual([f[0] for f in rx.sent[:-1]], [x.SOH] * 3)
        self.assertEqual(rx.sent[-1], bytes([x.EOT]))

    def test_no_handshake_raises(self):
        class Silent(ScriptedReceiver):
            def __init__(self): super().__init__(); self.replies = []
        with self.assertRaises(x.XmodemError):
            x.send(Silent().read, lambda d: None, b"\x55" * 10, retries=2)


if __name__ == "__main__":
    unittest.main()
