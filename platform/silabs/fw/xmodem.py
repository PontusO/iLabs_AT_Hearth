"""xmodem.py - XMODEM-1K with CRC-16, the sender side, stdlib only.

The Gecko UART XMODEM bootloader is the receiver: after its menu's "1" it
emits 'C' until the first block arrives, ACKs each good block, NAKs a bad
one, and ACKs the final EOT. Sender behaviour follows the classic protocol:
STX for 1024-byte blocks (SOH for 128), a one-byte sequence starting at 1
with its complement, 0x1A padding, CRC-16/XMODEM big-endian.

read(n, timeout) and write(data) are injected so the framer is testable
without a serial port (test_xmodem.py) and flash.py supplies pyserial's.

Block size on this platform: the Gecko Bootloader's parser is 128-byte
blocks only (XMODEM_DATA_SIZE 128 in the SDK's btl_xmodem.h, measured in
Task 5 and recorded in platform/silabs/README.md), so flash.py calls
send() with block_size=128 and the 1024-byte default is never used against
the bench bootloader. The default is kept because it is the signature the
task brief specifies and because a receiver that does take 1K blocks needs
no other change here.
"""

from typing import Callable

SOH, STX, EOT, ACK, NAK, CAN, CRC_C = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, ord("C")


class XmodemError(Exception):
    pass


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_block(seq: int, payload: bytes, block_size: int = 1024) -> bytes:
    if block_size not in (128, 1024):
        raise ValueError("block_size must be 128 or 1024")
    body = payload[:block_size].ljust(block_size, b"\x1a")
    head = bytes([STX if block_size == 1024 else SOH, seq & 0xFF, (~seq) & 0xFF])
    return head + body + crc16_xmodem(body).to_bytes(2, "big")


def _wait_for(read: Callable[[int, float], bytes], wanted: bytes, timeout: float, tries: int) -> int:
    for _ in range(tries):
        got = read(1, timeout)
        if got and got[0] in wanted:
            return got[0]
    raise XmodemError("no %r from receiver" % (wanted,))


def send(read: Callable[[int, float], bytes], write: Callable[[bytes], None],
         image: bytes, block_size: int = 1024, retries: int = 10) -> int:
    _wait_for(read, bytes([CRC_C, NAK]), timeout=1.0, tries=retries)   # handshake
    blocks = 0
    seq = 1
    for off in range(0, len(image), block_size):
        frame = build_block(seq, image[off:off + block_size], block_size)
        for attempt in range(retries + 1):
            write(frame)
            reply = _wait_for(read, bytes([ACK, NAK, CAN]), timeout=3.0, tries=3)
            if reply == ACK:
                break
            if reply == CAN:
                raise XmodemError("receiver cancelled at block %d" % seq)
            if attempt == retries:
                raise XmodemError("block %d not acknowledged after %d retries" % (seq, retries))
        seq = (seq + 1) & 0xFF
        blocks += 1
    write(bytes([EOT]))
    _wait_for(read, bytes([ACK]), timeout=3.0, tries=retries)
    return blocks
