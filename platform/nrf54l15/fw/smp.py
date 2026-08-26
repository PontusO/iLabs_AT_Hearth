#!/usr/bin/env python3
"""Minimal MCUboot boot-serial (SMP/newtmgr) framing, stdlib only.

Everything below was derived from the in-tree MCUboot source at
~/ncs/v3.0.2/bootloader/mcuboot/boot/boot_serial/src/ (NCS v3.0.2, the SDK
this project builds against), plus the vendored zcbor copy it links
(~/ncs/v3.0.2/bootloader/mcuboot/boot/zcbor/), plus the ACTUAL generated
build config for this project's mcuboot image at
platform/nrf54l15/build/mcuboot/zephyr/.config. Every constant is cited
file:line against those trees. Where the task brief's "expected shape"
disagreed with the tree, the tree wins; both are noted below.

--------------------------------------------------------------------------
1. nmgr_hdr: the 8-byte SMP header (boot_serial_priv.h:62-77)
--------------------------------------------------------------------------

    struct nmgr_hdr {
        uint8_t  nh_op;      // 3-bit bitfield, LE layout: op is the low
                              // 3 bits, version 2 bits above it, 3
                              // reserved bits on top. Version and
                              // reserved are always 0 in this protocol,
                              // so the whole byte's numeric value equals
                              // nh_op's value; treated as a plain byte
                              // here.
        uint8_t  nh_flags;   // always 0 on the wire (boot_serial.c:1377,
                              // "bs_hdr->nh_flags = 0")
        uint16_t nh_len;     // payload length, big-endian on the wire
                              // (boot_serial.c:1378 nh_len = htons(len);
                              // boot_serial.c:1301 ntohs(hdr->nh_len) on
                              // decode)
        uint16_t nh_group;   // big-endian on the wire
                              // (boot_serial.c:1379 htons on encode;
                              // boot_serial.c:1305 ntohs on decode)
        uint8_t  nh_seq;     // sequence number, echoed back unchanged
        uint8_t  nh_id;      // message ID within nh_group
    } __packed;

Wire layout confirmed (not just the struct): boot_serial_output()
(boot_serial.c:1361-1434) memcpy's totlen(2) + bs_hdr(8, raw struct bytes,
after the two htons() above) + payload + crc(2) into one buffer before
base64-encoding it, so on the wire nh_len and nh_group are big-endian,
matching the brief's expected shape exactly: `op(1) flags(1) len(2,BE)
group(2,BE) seq(1) id(1)`.

Op / group / id codes (boot_serial_priv.h:39-56, 82-84):

    NMGR_OP_READ            = 0   (priv.h:47)
    NMGR_OP_WRITE           = 2   (priv.h:48)   (write-response = op+1 = 3,
                                                  boot_serial.c:1376)
    MGMT_GROUP_ID_DEFAULT   = 0   (priv.h:50)
    MGMT_GROUP_ID_IMAGE     = 1   (priv.h:51)
    NMGR_ID_ECHO            = 0   (priv.h:54)   (group 0)
    NMGR_ID_CONS_ECHO_CTRL  = 1   (priv.h:55)   (group 0)
    NMGR_ID_RESET           = 5   (priv.h:56)   (group 0)
    IMGMGR_NMGR_ID_STATE    = 0   (priv.h:82)   (group 1)
    IMGMGR_NMGR_ID_UPLOAD   = 1   (priv.h:83)   (group 1)
    IMGMGR_NMGR_ID_SLOT_INFO= 6   (priv.h:84)   (group 1)

    MGMT_ERR_OK      = 0  (priv.h:39)
    MGMT_ERR_EUNKNOWN= 1  (priv.h:40)
    MGMT_ERR_ENOMEM  = 2  (priv.h:41)
    MGMT_ERR_EINVAL  = 3  (priv.h:42)
    MGMT_ERR_ENOENT  = 5  (priv.h:43)
    MGMT_ERR_ENOTSUP = 8  (priv.h:44)
    MGMT_ERR_EBUSY   = 10 (priv.h:45)

This matches the brief's expected shape exactly: group 1/id 1 = image
upload, group 0/id 0 = echo, group 0/id 5 = reset.

DISAGREEMENT WITH THE BRIEF (build config, not source per se): this
project's actual mcuboot build has `# CONFIG_BOOT_MGMT_ECHO is not set`
(platform/nrf54l15/build/mcuboot/zephyr/.config:74). bs_echo() is compiled
in only `#ifdef MCUBOOT_BOOT_MGMT_ECHO` (boot_serial.c:1204,1334-1338); with
it off, NMGR_ID_ECHO falls through to the `default:` case and gets
MGMT_ERR_ENOTSUP (rc=8) back, not an echoed payload. flash.py's handshake
accounts for this: it still sends the echo command (matching the brief's
API), but treats receipt of ANY well-formed decoded SMP frame, regardless
of rc, as proof the bootloader is alive and framing/CRC are correct,
which is all the handshake needs before an upload.

--------------------------------------------------------------------------
2. Serial line framing (boot_serial_priv.h:30-34, boot_serial.c:125,
   1361-1434, 1439-1493, 1550-1557)
--------------------------------------------------------------------------

    SHELL_NLIP_PKT_START1  = 6   (priv.h:30)  -> 0x06 \
    SHELL_NLIP_PKT_START2  = 9   (priv.h:31)  -> 0x09 / first-line marker
    SHELL_NLIP_DATA_START1 = 4   (priv.h:33)  -> 0x04 \
    SHELL_NLIP_DATA_START2 = 20  (priv.h:34)  -> 0x14 / continuation marker
                                                  (20 decimal == 0x14)

    BOOT_SERIAL_FRAME_MTU = 124  (boot_serial.c:125, comment: "127 - pkt
                                   start (2 bytes) and stop (1 byte)")
                                   -> up to 124 base64 bytes per line; with
                                   the 2-byte marker and the trailing '\n'
                                   (boot_serial.c:1430) a full line is at
                                   most 127 bytes, matching the brief.

A "packet" on the wire is:

    base64( totlen(2,BE) ++ nmgr_hdr(8) ++ payload ++ crc16(2,BE) )

split into lines of <=124 base64 bytes, first line prefixed 0x06 0x09,
continuation lines prefixed 0x04 0x14, each line terminated with '\n'
(boot_serial.c:1417-1431 encode; boot_serial.c:1550-1557 decode dispatch;
boot_serial.c:1439-1493 boot_serial_in_dec, the per-line accumulate/verify
state machine this module's SerialDecoder mirrors).

`totlen` = len(payload) + sizeof(nmgr_hdr) + sizeof(crc) = len(frame) + 2,
where "frame" in this module's API (see encode_frame below) means
header(8) + payload, i.e. everything the CRC covers
(boot_serial.c:1394-1404). totlen itself is NOT covered by the CRC.

--------------------------------------------------------------------------
3. CRC16 (boot_serial.c:135-136, 1382-1391, 1479-1489;
   ~/ncs/v3.0.2/zephyr/lib/crc/crc16_sw.c:66-78;
   ~/ncs/v3.0.2/zephyr/include/zephyr/sys/crc.h:176-210)
--------------------------------------------------------------------------

boot_serial.c uses `crc16_itu_t(CRC16_INITIAL_CRC, ...)` with
CRC16_INITIAL_CRC = 0 (boot_serial.c:135) and poly comment
CRC_CITT_POLYMINAL 0x1021 (boot_serial.c:136). Zephyr's crc16_itu_t()
(crc16_sw.c:66-78) is a direct (non-reflected) 0x1021-poly CRC with no
output XOR; crc.h's docstring for it (crc.h:176-210) explicitly names
"CRC-16/XMODEM, CRC-16/ACORN, CRC-16/LTE" as the checksums it computes
when seeded 0: despite the "itu_t" function name, seeded with
CRC16_INITIAL_CRC=0 this IS the standard CRC-16/XMODEM variant, matching
the brief's "crc16_xmodem" naming even though the C symbol says
"crc16_itu_t". Known-vector cross-check: CRC-16/XMODEM(poly=0x1021,
init=0x0000) of ASCII "123456789" = 0x31C3 (the standard CRC catalog check
value), confirmed against this module's crc16_xmodem() below.

Coverage: crc = crc16_xmodem(header(8) ++ payload) (boot_serial.c:1382-
1383); the 2-byte crc is appended big-endian (boot_serial.c:1392-1403,
via htons then memcpy). Decode verifies by recomputing the CRC over
header+payload+the-transmitted-crc-bytes together and requiring a
residue of exactly 0 (boot_serial.c:1479-1489, `if (crc || len <=
sizeof(crc)) return 0;`), the standard self-checking property of a
non-reflected CRC when the check value is appended in the same byte
order the arithmetic runs in.

--------------------------------------------------------------------------
4. CBOR: zcbor, and whether it emits indefinite-length maps
   (~/ncs/v3.0.2/bootloader/mcuboot/boot/zcbor/{src,include}/zcbor_*.c/.h;
   platform/nrf54l15/build/mcuboot/zephyr/.config:1061)
--------------------------------------------------------------------------

zcbor's major types are standard CBOR (zcbor_common.h:174-182): PINT=0,
NINT=1, BSTR=2, TSTR=3, LIST=4, MAP=5, TAG=6, SIMPLE=7. Header-byte
encoding is the standard `(major<<5)|additional`
(zcbor_encode.c:48-55), booleans are simple values 20/21
(zcbor_encode.c:463-497, ZCBOR_BOOL_TO_SIMPLE base 20): plain RFC
8949 CBOR, so this module's own encoder (used for OUTGOING requests) just
implements standard CBOR directly.

The important derived fact, because it changes what the DECODER must
accept: `zcbor_map_start_encode()` / `zcbor_map_end_encode()`
(zcbor_encode.c:345-366, 381-433) branch on `ZCBOR_CANONICAL`. Defined:
encodes a definite-length header with the element count. NOT defined:
writes an indefinite-length header (additional-info 31,
ZCBOR_VALUE_IS_INDEFINITE_LENGTH = 31, zcbor_common.h:241) and closes
with a 0xFF break byte instead of a length. This project's actual mcuboot
build has `# CONFIG_ZCBOR_CANONICAL is not set`
(build/mcuboot/zephyr/.config:1061), CONFIRMING the brief's suspicion:
every bs_rc_rsp / bs_upload / bs_echo response map (boot_serial.c:664-672,
1175-1182, 1237-1249) is encoded as an INDEFINITE-length CBOR map: byte
0xBF, then key/value pairs, then byte 0xFF. cbor_decode() below handles
both definite- and indefinite-length maps (and bstr/tstr) for this
reason; this module's own encoder emits definite-length maps for
requests (simpler, and standard zcbor decoders accept definite-length
input just as well: nothing in boot_serial.c requires indefinite-length
on the way IN).

--------------------------------------------------------------------------
5. Image upload CBOR keys (boot_serial.c:869-933, 907-912)
--------------------------------------------------------------------------

    "image" -> uint, image number (single-slot build here: always 0;
               MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD is not set,
               CONFIG_SINGLE_APPLICATION_SLOT=y,
               build/mcuboot/zephyr/.config:23)
    "data"  -> bstr, chunk bytes
    "len"   -> uint, TOTAL image length (first frame only, off==0)
    "off"   -> uint, offset of this chunk within the image (every frame)
    ("sha" is accepted by newer mcumgr image_upload_decode variants for
     resume-by-hash, but this vendored boot_serial.c's
     image_upload_decode table (boot_serial.c:907-912) only decodes
     "image"/"data"/"len"/"off", no "sha" key. flash.py still SENDS "sha"
     on the first frame for host-side bookkeeping/logging; the bootloader
     silently ignores unrecognized keys (zcbor_map_decode_bulk skips
     unmatched keys) so this is harmless, but it is NOT consulted by this
     build for resume: resume here is purely offset-based, per
     boot_serial.c:934-951.)

Response to an upload frame: `{"rc": <int>}` always, plus `{"off": <uint>}`
appended only when rc==0 (boot_serial.c:1175-1182). Response to a
mismatched offset: rc=0, off=curr_off (the offset the bootloader still
expects); this is the resend/resync signal (boot_serial.c:1041-1047).

--------------------------------------------------------------------------
"""

import base64
import struct
from collections import namedtuple

# ---------------------------------------------------------------------- #
# Op / group / id / error constants (boot_serial_priv.h, cited above).
# ---------------------------------------------------------------------- #
NMGR_OP_READ = 0
NMGR_OP_WRITE = 2

MGMT_GROUP_ID_DEFAULT = 0
MGMT_GROUP_ID_IMAGE = 1
MGMT_GROUP_ID_PERUSER = 64

NMGR_ID_ECHO = 0
NMGR_ID_CONS_ECHO_CTRL = 1
NMGR_ID_RESET = 5

IMGMGR_NMGR_ID_STATE = 0
IMGMGR_NMGR_ID_UPLOAD = 1
IMGMGR_NMGR_ID_SLOT_INFO = 6

MGMT_ERR_OK = 0
MGMT_ERR_EUNKNOWN = 1
MGMT_ERR_ENOMEM = 2
MGMT_ERR_EINVAL = 3
MGMT_ERR_ENOENT = 5
MGMT_ERR_ENOTSUP = 8
MGMT_ERR_EBUSY = 10

# ---------------------------------------------------------------------- #
# Serial framing constants (boot_serial_priv.h:30-34, boot_serial.c:125).
# ---------------------------------------------------------------------- #
PKT_START = bytes((0x06, 0x09))
PKT_CONT = bytes((0x04, 0x14))
FRAME_MTU = 124  # BOOT_SERIAL_FRAME_MTU, boot_serial.c:125

HEADER_LEN = 8  # sizeof(struct nmgr_hdr)

Header = namedtuple("Header", "op flags length group seq id")


# ---------------------------------------------------------------------- #
# CRC16 (boot_serial.c:135-136, 1382-1391; see module docstring section 3)
# ---------------------------------------------------------------------- #
def crc16_xmodem(data, crc=0):
    """CRC-16/XMODEM: poly 0x1021, no reflection, no output XOR.

    check(b"123456789") == 0x31C3, the standard CRC-16/XMODEM catalog
    value; independently confirmed to match the direct 0x1021-poly,
    seed-0 arithmetic boot_serial.c actually runs (see module docstring).
    """
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


# ---------------------------------------------------------------------- #
# Minimal CBOR encoder (standard RFC 8949; see module docstring section 4).
# Emits definite-length maps/arrays for outgoing requests.
# ---------------------------------------------------------------------- #
def _encode_head(major, value):
    major_bits = major << 5
    if value < 24:
        return bytes((major_bits | value,))
    if value < 256:
        return bytes((major_bits | 24, value))
    if value < 0x10000:
        return bytes((major_bits | 25,)) + value.to_bytes(2, "big")
    if value < 0x100000000:
        return bytes((major_bits | 26,)) + value.to_bytes(4, "big")
    return bytes((major_bits | 27,)) + value.to_bytes(8, "big")


def cbor_encode(obj):
    """Encode obj (int, bytes, str, bool, None, dict, list/tuple) as CBOR."""
    if isinstance(obj, bool):
        return bytes((0xE0 | (21 if obj else 20),))
    if isinstance(obj, int):
        if obj >= 0:
            return _encode_head(0, obj)
        return _encode_head(1, -1 - obj)
    if isinstance(obj, (bytes, bytearray)):
        return _encode_head(2, len(obj)) + bytes(obj)
    if isinstance(obj, str):
        raw = obj.encode("utf-8")
        return _encode_head(3, len(raw)) + raw
    if obj is None:
        return bytes((0xF6,))
    if isinstance(obj, dict):
        out = _encode_head(5, len(obj))
        for key, val in obj.items():
            out += cbor_encode(key)
            out += cbor_encode(val)
        return out
    if isinstance(obj, (list, tuple)):
        out = _encode_head(4, len(obj))
        for item in obj:
            out += cbor_encode(item)
        return out
    raise TypeError("cbor_encode: unsupported type %r" % type(obj))


# ---------------------------------------------------------------------- #
# Tolerant CBOR decoder: uint, negative int, bstr, tstr, bool, null, map,
# list; definite- AND indefinite-length bstr/tstr/list/map (mcuboot's
# responses are indefinite-length maps, see module docstring section 4).
# ---------------------------------------------------------------------- #
_BREAK = object()  # sentinel: saw a 0xFF break byte while scanning an item


def _decode_item(buf, pos):
    head = buf[pos]
    major = head >> 5
    info = head & 0x1F
    pos += 1

    if major == 7:
        if info == 31:
            return _BREAK, pos
        if info == 20:
            return False, pos
        if info == 21:
            return True, pos
        if info in (22, 23):
            return None, pos
        raise ValueError("cbor_decode: unsupported simple value %d" % info)

    if info < 24:
        value = info
    elif info == 24:
        value = buf[pos]
        pos += 1
    elif info == 25:
        value = int.from_bytes(buf[pos:pos + 2], "big")
        pos += 2
    elif info == 26:
        value = int.from_bytes(buf[pos:pos + 4], "big")
        pos += 4
    elif info == 27:
        value = int.from_bytes(buf[pos:pos + 8], "big")
        pos += 8
    elif info == 31:
        value = None  # indefinite length marker
    else:
        raise ValueError("cbor_decode: reserved additional info %d" % info)

    if major == 0:
        return value, pos
    if major == 1:
        return -1 - value, pos
    if major == 2:  # bstr
        if info == 31:
            data = b""
            while True:
                item, pos = _decode_item(buf, pos)
                if item is _BREAK:
                    break
                data += item
            return data, pos
        data = bytes(buf[pos:pos + value])
        return data, pos + value
    if major == 3:  # tstr
        if info == 31:
            text = ""
            while True:
                item, pos = _decode_item(buf, pos)
                if item is _BREAK:
                    break
                text += item
            return text, pos
        text = bytes(buf[pos:pos + value]).decode("utf-8")
        return text, pos + value
    if major == 4:  # list
        result = []
        if info == 31:
            while True:
                item, pos = _decode_item(buf, pos)
                if item is _BREAK:
                    break
                result.append(item)
        else:
            for _ in range(value):
                item, pos = _decode_item(buf, pos)
                result.append(item)
        return result, pos
    if major == 5:  # map
        result = {}
        if info == 31:
            while True:
                key, pos = _decode_item(buf, pos)
                if key is _BREAK:
                    break
                val, pos = _decode_item(buf, pos)
                result[key] = val
        else:
            for _ in range(value):
                key, pos = _decode_item(buf, pos)
                val, pos = _decode_item(buf, pos)
                result[key] = val
        return result, pos
    if major == 6:  # tag: transparently unwrap
        return _decode_item(buf, pos)
    raise ValueError("cbor_decode: unsupported major type %d" % major)


def cbor_decode(buf):
    """Decode a single top-level CBOR item from buf (bytes-like)."""
    value, _ = _decode_item(bytes(buf), 0)
    return value


# ---------------------------------------------------------------------- #
# SMP frame encode/decode. "frame" = header(8) + cbor payload, i.e.
# everything the CRC covers (see module docstring section 2).
# ---------------------------------------------------------------------- #
def encode_frame(op, group, cmd_id, payload_map, seq):
    payload = cbor_encode(payload_map)
    header = struct.pack(">BBHHBB", op & 0xFF, 0, len(payload), group & 0xFFFF,
                          seq & 0xFF, cmd_id & 0xFF)
    return header + payload


def decode_frames(buf):
    """Split a buffer of one or more concatenated header+payload frames.

    Returns a list of (Header, payload_map). This is the counterpart of
    encode_frame, and the piece a future RP2350 host client ports as-is:
    it needs no serial/base64/CRC state, just header parsing + CBOR.
    """
    frames = []
    pos = 0
    n = len(buf)
    while pos + HEADER_LEN <= n:
        op, flags, length, group, seq, cmd_id = struct.unpack_from(">BBHHBB", buf, pos)
        payload_start = pos + HEADER_LEN
        payload_end = payload_start + length
        if payload_end > n:
            break
        payload_bytes = buf[payload_start:payload_end]
        payload_map = cbor_decode(payload_bytes) if payload_bytes else {}
        frames.append((Header(op, flags, length, group, seq, cmd_id), payload_map))
        pos = payload_end
    return frames


def serial_encode(frame):
    """Wrap a header+payload frame in the base64 line framing (section 2)."""
    crc = crc16_xmodem(frame)
    totlen = len(frame) + 2
    raw = totlen.to_bytes(2, "big") + frame + crc.to_bytes(2, "big")
    b64 = base64.b64encode(raw)

    out = bytearray()
    off = 0
    n = len(b64)
    while off < n:
        marker = PKT_START if off == 0 else PKT_CONT
        chunk = b64[off:off + FRAME_MTU]
        out += marker
        out += chunk
        out += b"\n"
        off += len(chunk)
    return bytes(out)


class SerialDecoder:
    """Streaming counterpart of boot_serial_in_dec (boot_serial.c:1439-1493).

    Feed it raw bytes as they arrive from the serial port (any chunking);
    it reassembles lines, base64-decodes each line's chunk (matching the
    C side's per-line base64_decode + accumulate), and once a full
    totlen-delimited packet with a valid CRC residue has arrived, yields
    the decoded frame (header + payload, CRC and totlen stripped).
    """

    def __init__(self):
        self._linebuf = bytearray()
        self._acc = None  # None: not mid-packet; else accumulated raw bytes

    def feed(self, data):
        frames = []
        self._linebuf += data
        while True:
            idx = self._linebuf.find(b"\n")
            if idx < 0:
                break
            line = bytes(self._linebuf[:idx])
            del self._linebuf[:idx + 1]
            frame = self._feed_line(line)
            if frame is not None:
                frames.append(frame)
        return frames

    def _feed_line(self, line):
        if len(line) < 2:
            return None
        marker = line[:2]
        b64_part = line[2:]

        if marker == PKT_START:
            self._acc = bytearray()
        elif marker == PKT_CONT:
            if self._acc is None:
                return None  # continuation with no start seen; drop it
        else:
            return None  # not an NLIP line; ignore

        try:
            decoded = base64.b64decode(bytes(b64_part))
        except Exception:
            self._acc = None
            return None

        self._acc += decoded

        if len(self._acc) <= 2:
            return None  # still waiting on more continuation lines

        totlen = int.from_bytes(bytes(self._acc[:2]), "big")
        if len(self._acc) - 2 != totlen:
            return None  # not complete yet, or a length mismatch: keep waiting

        body = bytes(self._acc[2:2 + totlen])  # header + payload + crc
        self._acc = None

        if len(body) <= 2 or crc16_xmodem(body) != 0:
            return None  # bad CRC residue or too short: drop the packet

        return body[:-2]  # strip the trailing crc16; header+payload remains
