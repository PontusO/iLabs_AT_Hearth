#!/usr/bin/env python3
"""Host golden tests for smp.py (and flash.py's argparse/hash logic).

Plain asserts, no framework, matching the repo's host-test style
(test/host in this repo, run with `make -C test/host run`). Run with
plain python3 from a normal shell (never the NCS environment -- its
PYTHONHOME breaks system python):

    python3 test_smp.py

Golden byte vectors are derived BY HAND from the constants smp.py's
header comment cites against the MCUboot source, with a comment showing
the derivation next to each one. Where a test only checks a round-trip
property (encode then decode gives the original back) that is noted too
-- it is weaker than a byte-for-byte golden vector but still exercises
real wire-format code, and is used where hand-deriving the full byte
string would be unwieldy (the multi-line continuation case).
"""

import hashlib
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import smp  # noqa: E402
import flash  # noqa: E402

_passed = 0
_failed = 0


def check(name, cond):
    global _passed, _failed
    if cond:
        _passed += 1
    else:
        _failed += 1
        print(f"FAIL: {name}")


# ------------------------------------------------------------------ #
# Brief's own example tests (Step 2), verbatim in spirit.
# ------------------------------------------------------------------ #
def test_echo_frame_header():
    f = smp.encode_frame(op=2, group=0, cmd_id=0, payload_map={"d": "hi"}, seq=7)
    # header: 02 00 <len,len> 00 00 07 00 followed by CBOR {"d":"hi"}
    # op=NMGR_OP_WRITE=2 (boot_serial_priv.h:48), flags always 0
    # (boot_serial.c:1377), group=MGMT_GROUP_ID_DEFAULT=0 (priv.h:50) BE,
    # seq=7, id=NMGR_ID_ECHO=0 (priv.h:54)
    check("echo header op", f[0] == 0x02)
    check("echo header group BE", f[4:6] == b"\x00\x00")
    check("echo header seq", f[6] == 7)
    check("echo header id", f[7] == 0x00)
    check("echo header len field", int.from_bytes(f[2:4], "big") == len(f) - 8)
    check("echo payload decodes", smp.cbor_decode(f[8:]) == {"d": "hi"})


def test_serial_roundtrip():
    frame = smp.encode_frame(2, 1, 1, {"off": 0}, 0)
    wire = smp.serial_encode(frame)
    check("serial wire starts with pkt-start marker", wire[:2] == b"\x06\x09")
    check("serial wire ends with newline", wire.endswith(b"\n"))
    dec = smp.SerialDecoder()
    out = dec.feed(wire)
    check("serial roundtrip yields original frame", out == [frame])


# ------------------------------------------------------------------ #
# CBOR encoder: one hand-derived byte vector per type the flasher emits.
# All are standard RFC 8949 CBOR (zcbor's major types match, see smp.py's
# header comment section 4): major<<5 | additional-info header byte.
# ------------------------------------------------------------------ #
def test_cbor_uint_small():
    # uint 7: major 0 (PINT), info 7 (<24, value in the header byte itself)
    # header byte = (0<<5)|7 = 0x07
    check("cbor uint<24", smp.cbor_encode(7) == b"\x07")


def test_cbor_uint_2byte():
    # uint 500: 256 <= 500 < 65536 -> info 25 (2-byte BE follows)
    # header byte = (0<<5)|25 = 0x19; 500 = 0x01F4
    check("cbor uint 2-byte", smp.cbor_encode(500) == b"\x19\x01\xf4")


def test_cbor_bstr():
    # byte string {0xAA,0xBB,0xCC}: major 2 (BSTR), len 3 (<24)
    # header byte = (2<<5)|3 = 0x43
    check("cbor bstr", smp.cbor_encode(b"\xaa\xbb\xcc") == b"\x43\xaa\xbb\xcc")


def test_cbor_tstr():
    # text "hi": major 3 (TSTR), len 2 -> header byte = (3<<5)|2 = 0x62
    check("cbor tstr", smp.cbor_encode("hi") == b"\x62hi")


def test_cbor_bool():
    # bool: major 7 (SIMPLE), value 20=false/21=true
    # (zcbor_encode.c ZCBOR_BOOL_TO_SIMPLE base 20)
    # header byte false = (7<<5)|20 = 0xF4, true = (7<<5)|21 = 0xF5
    check("cbor bool false", smp.cbor_encode(False) == b"\xf4")
    check("cbor bool true", smp.cbor_encode(True) == b"\xf5")


def test_cbor_map():
    # {"rc": 0}: major 5 (MAP), definite length 1 -> header (5<<5)|1 = 0xA1
    # key "rc": tstr len2 -> 0x62 'r' 'c'; value uint 0 -> 0x00
    expect = bytes((0xA1, 0x62, ord("r"), ord("c"), 0x00))
    check("cbor map", smp.cbor_encode({"rc": 0}) == expect)


def test_cbor_negative_int():
    # -1: major 1 (NINT), n = -1-(-1) = 0 -> header (1<<5)|0 = 0x20
    check("cbor negative int", smp.cbor_encode(-1) == b"\x20")


# ------------------------------------------------------------------ #
# CRC16: known catalog vector for CRC-16/XMODEM (poly 0x1021, init
# 0x0000), which is what boot_serial.c actually computes when seeded
# with CRC16_INITIAL_CRC=0 (boot_serial.c:135) -- see smp.py's header
# comment section 3 for the full derivation chain.
# ------------------------------------------------------------------ #
def test_crc16_known_vector():
    check("crc16 XMODEM check value", smp.crc16_xmodem(b"123456789") == 0x31C3)


def test_crc16_residue_property():
    # Appending the big-endian CRC of a message to that message and
    # recomputing the CRC over the whole thing must give residue 0 --
    # this is exactly what boot_serial.c:1479-1489 checks on decode.
    msg = b"hello mcuboot"
    crc = smp.crc16_xmodem(msg)
    check("crc16 residue property",
          smp.crc16_xmodem(msg + crc.to_bytes(2, "big")) == 0)


# ------------------------------------------------------------------ #
# Indefinite-length CBOR map decode: this build's mcuboot emits these
# for every response (CONFIG_ZCBOR_CANONICAL not set, see smp.py's
# header comment section 4). Hand-derived bytes:
#   0xBF                 map, indefinite length
#   62 72 63             tstr "rc" (major3 len2)
#   00                   uint 0
#   63 6F 66 66          tstr "off" (major3 len3)
#   19 02 00             uint 512 (major0 info25, 2-byte BE = 0x0200)
#   FF                   break
# ------------------------------------------------------------------ #
def test_cbor_indefinite_map_decode():
    raw = bytes((0xBF, 0x62, 0x72, 0x63, 0x00,
                 0x63, 0x6F, 0x66, 0x66, 0x19, 0x02, 0x00,
                 0xFF))
    check("indefinite map decode", smp.cbor_decode(raw) == {"rc": 0, "off": 512})


def test_cbor_indefinite_map_rc_only():
    # {"rc": 8} (MGMT_ERR_ENOTSUP) as mcuboot actually sends it for the
    # echo command in this build (CONFIG_BOOT_MGMT_ECHO not set, see
    # smp.py header comment section 1's disagreement note).
    raw = bytes((0xBF, 0x62, 0x72, 0x63, 0x08, 0xFF))
    check("indefinite map rc-only decode", smp.cbor_decode(raw) == {"rc": 8})


# ------------------------------------------------------------------ #
# Multi-line (continuation-marker) round-trip: a frame whose base64
# encoding exceeds one 124-byte line (FRAME_MTU, boot_serial.c:125),
# forcing 0x04 0x14 continuation lines. Round-trip property test (byte-
# for-byte hand derivation of a >124-byte base64 stream is unwieldy);
# the assertions below still pin the exact framing behaviour that
# matters: more than one line, correct markers, correct reassembly.
# ------------------------------------------------------------------ #
def test_serial_multiline_roundtrip():
    payload = {"data": bytes(range(256)) * 1, "off": 0}  # 256-byte bstr payload
    frame = smp.encode_frame(2, 1, 1, payload, 0)
    wire = smp.serial_encode(frame)

    lines = wire.split(b"\n")[:-1]  # trailing '\n' after the last line
    check("multiline: more than one line", len(lines) > 1)
    check("multiline: first line starts with pkt-start", lines[0][:2] == b"\x06\x09")
    check("multiline: continuation lines marked",
          all(ln[:2] == b"\x04\x14" for ln in lines[1:]))
    check("multiline: each line <= 126 bytes (2 marker + <=124 payload)",
          all(len(ln) <= 126 for ln in lines))

    dec = smp.SerialDecoder()
    out = dec.feed(wire)
    check("multiline roundtrip yields original frame", out == [frame])


def test_serial_decoder_fed_byte_at_a_time():
    # Streaming property: feeding one byte at a time must give the same
    # result as feeding it all at once -- exercises the partial-line
    # buffering path.
    payload = {"data": bytes(range(200)), "off": 0}
    frame = smp.encode_frame(2, 1, 1, payload, 3)
    wire = smp.serial_encode(frame)

    dec = smp.SerialDecoder()
    out = []
    for i in range(len(wire)):
        out += dec.feed(wire[i:i + 1])
    check("byte-at-a-time roundtrip", out == [frame])


# ------------------------------------------------------------------ #
# decode_frames: the inverse of encode_frame, over a raw concatenated
# buffer (what a future RP2350 host client would hand it after its own
# line/CRC layer strips totlen+crc).
# ------------------------------------------------------------------ #
def test_decode_frames_roundtrip():
    frame = smp.encode_frame(op=3, group=1, cmd_id=1, payload_map={"rc": 0, "off": 128}, seq=5)
    parsed = smp.decode_frames(frame)
    check("decode_frames returns one frame", len(parsed) == 1)
    header, payload = parsed[0]
    check("decode_frames header op", header.op == 3)
    check("decode_frames header group", header.group == 1)
    check("decode_frames header id", header.id == 1)
    check("decode_frames header seq", header.seq == 5)
    check("decode_frames payload", payload == {"rc": 0, "off": 128})


def test_decode_frames_multiple_concatenated():
    f1 = smp.encode_frame(2, 0, 0, {"d": "a"}, 1)
    f2 = smp.encode_frame(2, 1, 1, {"off": 0}, 2)
    parsed = smp.decode_frames(f1 + f2)
    check("decode_frames: two frames found", len(parsed) == 2)
    check("decode_frames: first payload", parsed[0][1] == {"d": "a"})
    check("decode_frames: second payload", parsed[1][1] == {"off": 0})


# ------------------------------------------------------------------ #
# flash.py: argument parsing and image-hash printing against a temp
# file. No serial-port access (global constraint for this task) -- the
# port-touching paths are exercised on hardware in Task 7.
# ------------------------------------------------------------------ #
def test_flash_argparse_no_default_port_value():
    # argparse itself has no required=True on --port: "no default port" is
    # deliberately enforced by hand in main() (a guessing run is worse than
    # a refusing one), not by argparse, so a bare parse succeeds with
    # args.port left None for main() to reject.
    parser = flash.build_arg_parser()
    args = parser.parse_args(["--image", "foo.bin"])
    check("argparse: port defaults to None (no guessed default)", args.port is None)


def test_flash_argparse_no_default_port():
    parser = flash.build_arg_parser()
    args = parser.parse_args(["--image", "x.bin", "--port", "/dev/serial/by-id/usb-foo"])
    check("argparse: port carried through", args.port == "/dev/serial/by-id/usb-foo")
    check("argparse: default baud is 115200", args.baud == 115200)
    check("argparse: enter_only default False", args.enter_only is False)
    check("argparse: no_wait default False", args.no_wait is False)


def test_flash_argparse_enter_only_no_wait_flags():
    parser = flash.build_arg_parser()
    args = parser.parse_args(["--port", "/dev/serial/by-id/usb-foo", "--enter-only", "--no-wait"])
    check("argparse: --enter-only sets flag", args.enter_only is True)
    check("argparse: --no-wait sets flag", args.no_wait is True)


def test_flash_argparse_port_help_mentions_by_id():
    parser = flash.build_arg_parser()
    port_action = next(a for a in parser._actions if "--port" in a.option_strings)
    check("argparse: --port help mentions by-id", "by-id" in port_action.help)


def _make_signed_like_image(tmpdir, magic=0x96F3B83D, major=1, minor=2, rev=3, build=0):
    # IMAGE_HEADER_SIZE=32 (bootutil image.h:57); layout (all LE):
    # ih_magic(4) ih_load_addr(4) ih_hdr_size(2) ih_protect_tlv_size(2)
    # ih_img_size(4) ih_flags(4) ih_ver{major(1) minor(1) rev(2) build(4)}
    # _pad1(4)
    header = struct.pack("<IIHHII", magic, 0, 32, 0, 100, 0)
    header += struct.pack("<BBHI", major, minor, rev, build)
    header += b"\x00" * 4
    body = bytes(range(256)) * 4  # 1024 bytes of deterministic "app" data
    path = os.path.join(tmpdir, "zephyr.signed.bin")
    with open(path, "wb") as f:
        f.write(header + body)
    return path, header + body


def test_flash_sha256_and_header_against_temp_file():
    with tempfile.TemporaryDirectory() as tmp:
        path, data = _make_signed_like_image(tmp)
        digest = flash.sha256_of(path)
        check("sha256_of matches hashlib", digest == hashlib.sha256(data).digest())

        info = flash.parse_image_header(data)
        check("image header magic recognized", info["magic_ok"] is True)
        check("image header version string", info["version"] == "1.2.3")


def test_flash_refuses_bad_magic():
    with tempfile.TemporaryDirectory() as tmp:
        path, data = _make_signed_like_image(tmp, magic=0xDEADBEEF)
        info = flash.parse_image_header(data)
        check("image header magic mismatch detected", info["magic_ok"] is False)


# ------------------------------------------------------------------ #
# Run everything.
# ------------------------------------------------------------------ #
def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
    print(f"{_passed} passed, {_failed} failed")
    return 0 if _failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
