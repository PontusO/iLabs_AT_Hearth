#!/usr/bin/env python3
"""Flash a signed Hearth image into the Ophelia-IV over the CPico bridge.

Usage:
  python3 flash.py --image build/nrf54l15/zephyr/zephyr.signed.bin \
                   --port /dev/serial/by-id/usb-...

No default port, deliberately (bench rule: a run that guesses is worse
than a run that refuses). Always pass a /dev/serial/by-id/... path, never
/dev/ttyACM<n> -- the number moves across re-enumeration, and on the bench
rig a stray write to the wrong ttyACM can hit the Thread RCP and kill
otbr-agent.

Sequence (spec section 4, over pyserial), matching Task 4's bridge
contract (DTR asserted = module held in reset, RTS asserted = recovery
strap low, both released = module runs, the bridge follows host baud):

  1. read the image, print size + sha256 + the signed image's own header
     (magic + version), refuse to continue if the magic doesn't match
     MCUboot's IMAGE_MAGIC
  2. open the port at 115200 (MCUboot's serial-recovery baud; the
     CPico bridge follows whatever baud the host opens at)
  3. enter recovery: RTS True, DTR True, 50 ms, DTR False, 250 ms --
     holds the recovery strap through the reset pulse and for longer
     than CONFIG_BOOT_SERIAL_DETECT_DELAY=50 ms
     (build/mcuboot/zephyr/.config:81)
  4. SMP echo handshake, 3 attempts (see smp.py's header comment for why
     this build's echo replies ENOTSUP rather than echoing -- any
     well-formed reply still proves the link works)
  5. upload loop: <=512-byte payload chunks, resend on timeout from the
     last acked offset, progress line like the C6 flasher
     (platform/esp32c6/fw/flash.py)
  6. exit recovery: RTS False, DTR True, 50 ms, DTR False
  7. wait up to 20 s for a line containing +MTREADY; print it and exit 0,
     or exit 1 with the tail of what was read

--enter-only and --no-wait are for Task 7's demo 2 and for debugging: the
former drives the strap sequence and stops before touching SMP at all,
the latter skips the post-flash +MTREADY wait.
"""

import argparse
import hashlib
import os
import struct
import sys
import time

import smp

DEFAULT_BAUD = 115200  # MCUboot serial-recovery baud (CONFIG_BOOT_SERIAL_UART)

# Recovery strap sequence timing (spec section 4 / task context): the
# bridge follows DTR=reset, RTS=strap (Task 4 report). Detect window is
# CONFIG_BOOT_SERIAL_DETECT_DELAY=50ms (build/mcuboot/zephyr/.config:81);
# the hold below is comfortably longer.
ENTER_STRAP_S = 0.050
ENTER_HOLD_S = 0.250
EXIT_HOLD_S = 0.050

CHUNK = 512  # payload bytes/frame; well under
             # CONFIG_BOOT_SERIAL_MAX_RECEIVE_SIZE=1024
             # (build/mcuboot/zephyr/.config:73)
ECHO_ATTEMPTS = 3
ECHO_TIMEOUT_S = 2.0
UPLOAD_FRAME_TIMEOUT_S = 5.0
UPLOAD_MAX_CONSECUTIVE_TIMEOUTS = 5
READY_TIMEOUT_S = 20.0
READY_MARKER = "+MTREADY"

IMAGE_MAGIC = 0x96F3B83D  # bootutil/image.h:51
IMAGE_HEADER_SIZE = 32    # bootutil/image.h:57


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


# ------------------------------------------------------------------ #
# Image inspection: sha256 + the signed image's own MCUboot header.
# No serial access here -- exercised by test_smp.py against a temp file.
# ------------------------------------------------------------------ #
def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.digest()


def parse_image_header(data):
    """Parse the leading MCUboot image_header (bootutil/image.h:164-173).

    All fields little-endian:
      ih_magic(4) ih_load_addr(4) ih_hdr_size(2) ih_protect_tlv_size(2)
      ih_img_size(4) ih_flags(4) ih_ver{major(1) minor(1) rev(2) build(4)}
      _pad1(4)   -- total 32 bytes (IMAGE_HEADER_SIZE, image.h:57)

    Returns a dict; magic_ok False means "refuse to flash this" (mirrors
    the C6 flasher's check_plan_is_this_project refusal-on-mismatch).
    """
    if len(data) < IMAGE_HEADER_SIZE:
        return {"magic_ok": False, "version": None, "img_size": None}
    magic, load_addr, hdr_size, tlv_size, img_size, flags = struct.unpack_from(
        "<IIHHII", data, 0)
    major, minor, rev, build = struct.unpack_from("<BBHI", data, 20)
    version = f"{major}.{minor}.{rev}" + (f".{build}" if build else "")
    return {
        "magic_ok": magic == IMAGE_MAGIC,
        "version": version,
        "hdr_size": hdr_size,
        "img_size": img_size,
        "flags": flags,
    }


# ------------------------------------------------------------------ #
# Recovery strap sequence (Task 4 bridge contract: DTR=reset, RTS=strap).
# ------------------------------------------------------------------ #
def enter_recovery(ser):
    ser.rts = True
    ser.dtr = True
    time.sleep(ENTER_STRAP_S)
    ser.dtr = False
    time.sleep(ENTER_HOLD_S)


def exit_recovery(ser):
    ser.rts = False
    ser.dtr = True
    time.sleep(EXIT_HOLD_S)
    ser.dtr = False


# ------------------------------------------------------------------ #
# SMP request/response plumbing.
# ------------------------------------------------------------------ #
def send_frame(ser, op, group, cmd_id, payload, seq):
    frame = smp.encode_frame(op, group, cmd_id, payload, seq)
    ser.write(smp.serial_encode(frame))


def read_response(ser, decoder, timeout):
    """Read from ser until one decoded SMP frame arrives or timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        waiting = getattr(ser, "in_waiting", 0) or 1
        chunk = ser.read(waiting)
        if chunk:
            for raw in decoder.feed(chunk):
                parsed = smp.decode_frames(raw)
                if parsed:
                    return parsed[0]
        else:
            time.sleep(0.02)
    return None


def handshake(ser, decoder):
    for attempt in range(1, ECHO_ATTEMPTS + 1):
        send_frame(ser, smp.NMGR_OP_WRITE, smp.MGMT_GROUP_ID_DEFAULT,
                   smp.NMGR_ID_ECHO, {"d": "hearth"}, attempt & 0xFF)
        resp = read_response(ser, decoder, ECHO_TIMEOUT_S)
        if resp is not None:
            _, payload = resp
            # CONFIG_BOOT_MGMT_ECHO is not set in this build (see smp.py's
            # header comment, section 1): echo replies MGMT_ERR_ENOTSUP
            # rather than echoing "d". Any decodable frame at all already
            # proves the bootloader is alive and framing/CRC are correct.
            print(f"  handshake attempt {attempt}/{ECHO_ATTEMPTS}: "
                  f"reply rc={payload.get('rc')} (link OK)")
            return True
        print(f"  handshake attempt {attempt}/{ECHO_ATTEMPTS}: no response")
    return False


def upload(ser, decoder, data, digest):
    total = len(data)
    off = 0
    seq = 0
    consecutive_timeouts = 0
    while off < total:
        chunk = data[off:off + CHUNK]
        payload = {"image": 0, "data": chunk, "off": off}
        if off == 0:
            payload["len"] = total
            payload["sha"] = digest
        send_frame(ser, smp.NMGR_OP_WRITE, smp.MGMT_GROUP_ID_IMAGE,
                   smp.IMGMGR_NMGR_ID_UPLOAD, payload, seq & 0xFF)
        resp = read_response(ser, decoder, UPLOAD_FRAME_TIMEOUT_S)
        if resp is None:
            consecutive_timeouts += 1
            if consecutive_timeouts > UPLOAD_MAX_CONSECUTIVE_TIMEOUTS:
                die(f"upload stalled: {consecutive_timeouts} consecutive "
                    f"timeouts at off={off}/{total}")
            print(f"  timeout at off={off}, resending from last acked offset")
            continue
        consecutive_timeouts = 0
        _, rpayload = resp
        rc = rpayload.get("rc", smp.MGMT_ERR_EUNKNOWN)
        if rc != smp.MGMT_ERR_OK:
            die(f"upload failed at off={off}: rc={rc}")
        new_off = rpayload.get("off")
        if new_off is None:
            die(f"upload response missing 'off' at off={off}")
        off = new_off  # bootloader's own idea of the next offset: this is
                        # what makes a resend-on-timeout safe (it always
                        # tells us where it actually got to, including
                        # alignment padding and offset mismatches)
        seq += 1
        pct = 100 * off // total if total else 100
        print(f"  {off}/{total} bytes ({pct}%)")


# ------------------------------------------------------------------ #
# Argument parsing.
# ------------------------------------------------------------------ #
def build_arg_parser():
    ap = argparse.ArgumentParser(
        description="Flash a signed Hearth image into the Ophelia-IV over "
                     "the CPico bridge (MCUboot serial recovery).")
    ap.add_argument("--image",
                     help="signed image, e.g. "
                          "build/nrf54l15/zephyr/zephyr.signed.bin "
                          "(required unless --enter-only)")
    ap.add_argument("--port",
                     help="serial port of the CPico bridge, by-id "
                          "(e.g. /dev/serial/by-id/usb-...); never "
                          "/dev/ttyACM<n> -- the id moves across "
                          "re-enumeration. No default: a guessed port is "
                          "worse than a refusal.")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                     help=f"serial baud (default {DEFAULT_BAUD})")
    ap.add_argument("--enter-only", action="store_true",
                     help="drive the recovery strap sequence and exit; "
                          "no SMP handshake, no upload (Task 7 demo 2 / "
                          "debugging)")
    ap.add_argument("--no-wait", action="store_true",
                     help="skip waiting for +MTREADY after exiting "
                          "recovery; exit 0 once the reset sequence is "
                          "issued")
    return ap


# ------------------------------------------------------------------ #
# Main.
# ------------------------------------------------------------------ #
def main(argv=None):
    args = build_arg_parser().parse_args(argv)

    if not args.port:
        die("--port is required (no default port -- pass "
            "/dev/serial/by-id/..., never /dev/ttyACM<n>)")

    data = None
    digest = None
    if not args.enter_only:
        if not args.image:
            die("--image is required unless --enter-only")
        if not os.path.isfile(args.image):
            die(f"image not found: {args.image}")
        with open(args.image, "rb") as f:
            data = f.read()
        digest = hashlib.sha256(data).digest()
        info = parse_image_header(data)
        print(f"image: {args.image}")
        print(f"  size: {len(data)} bytes")
        print(f"  sha256: {digest.hex()}")
        if not info["magic_ok"]:
            die(f"{args.image} does not look like a signed MCUboot image "
                f"(bad header magic); refusing to flash it.")
        print(f"  version: {info['version']}  img_size: {info['img_size']}")

    import serial
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0
    ser.open()

    try:
        print("entering recovery (RTS strap + DTR reset pulse)...")
        enter_recovery(ser)

        if args.enter_only:
            print("--enter-only: recovery strap driven; not touching the "
                  "port further.")
            return 0

        decoder = smp.SerialDecoder()
        print("SMP handshake...")
        if not handshake(ser, decoder):
            die(f"no response from the bootloader after {ECHO_ATTEMPTS} "
                f"attempts; check the port and that the strap held "
                f"through CONFIG_BOOT_SERIAL_DETECT_DELAY")

        print("uploading...")
        upload(ser, decoder, data, digest)
        print("upload complete.")

        print("exiting recovery (release strap, reset pulse)...")
        exit_recovery(ser)

        if args.no_wait:
            print("--no-wait: not waiting for +MTREADY.")
            return 0

        print(f"waiting up to {READY_TIMEOUT_S:.0f}s for {READY_MARKER}...")
        deadline = time.time() + READY_TIMEOUT_S
        buf = bytearray()
        while time.time() < deadline:
            waiting = getattr(ser, "in_waiting", 0) or 1
            chunk = ser.read(waiting)
            if chunk:
                buf += chunk
                text = buf.decode("utf-8", errors="replace")
                if READY_MARKER in text:
                    for line in text.splitlines():
                        if READY_MARKER in line:
                            print(line.strip())
                            break
                    return 0
            else:
                time.sleep(0.05)
        tail = bytes(buf[-200:]).decode("utf-8", errors="replace")
        die(f"{READY_MARKER} not seen within {READY_TIMEOUT_S:.0f}s. "
            f"Tail of what was read:\n{tail}")
        return 1
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
