#!/usr/bin/env python3
"""flash.py - flash a Hearth .gbl onto the MGM240P through the iLabs UART
XMODEM Gecko Bootloader (GPIO activation on PC00), over the carrier's CDC
port.

CDC contract, shared with the nRF flasher: DTR asserted holds the module in
reset, RTS asserted pulls the recovery strap (PC00) low, both released =
running. Entering the bootloader: assert RTS, pulse DTR, release RTS after
the menu appears. --no-strap skips the strap sequence for a module that is
already sitting in the bootloader menu (a blank application slot, or a
bench session that entered it by hand, though item 3 below is what that
second case runs into on this carrier); AT+MTBOOTLOADER is NOT part of the
wire contract and there is no application-side entry.

Only /dev/serial/by-id paths are accepted (TESTING.md section 2).

Three things here are bench facts rather than protocol generalities, and
each one is a place where a reasonable-looking change breaks the tool:

1. **128-byte blocks, not 1K.** The Gecko Bootloader's parser is
   XMODEM_DATA_SIZE 128 (the SDK's btl_xmodem.h), measured in Task 5. It
   never accepts an STX frame, so BLOCK_SIZE below is 128 and xmodem.py's
   1024-byte default is not used on this platform.

2. **The menu echoes before it handshakes.** After "1" the bootloader
   sends b"\\r\\nbegin upload\\r\\n\\x00" and only then its 'C'. xmodem.send()
   looks at one byte at a time for a bounded number of tries, so that
   preamble would exhaust the handshake budget. This file drains the
   preamble itself and pushes the 'C' it finds back into the reader, so
   the framer's handshake still sees the byte that started it.

3. **--no-strap drives nothing, and opening the port still resets the
   module.** Measured 2026-09-17: opening this CDC pulses the module's
   reset even with DTR and RTS cleared before the open, and a running
   image answers with +MTREADY about 200 ms later (the same open-time
   reset test/mt_regression.py's --bridge cpico waits out). So --no-strap
   is usable only where that reset lands back in the menu, which means a
   blank application slot; a module parked in the menu with a valid
   application in flash cannot be reached this way by any host-side
   choice, because the open resets it before the first byte is written.
   What --no-strap does guarantee is that this tool adds no reset and no
   strap of its own. It still waits for the menu before writing anything,
   and for the same reason the strap path does: after that open-time reset
   the bootloader needs its ~100 ms to come up, and a "1" written into
   those milliseconds is written to a USART that is not listening yet.

Exit status is 0 only when +MTREADY was read back from the new image; every
failure path exits 1 with a one-line reason on stderr.
"""

import argparse
import sys
import time

import serial

import xmodem

BAUD = 115200          # the bootloader's USART0, 115200 8N1 (README, Bootloader)
BLOCK_SIZE = 128       # XMODEM_DATA_SIZE in btl_xmodem.h; see the header, item 1

STRAP_SETTLE_S = 0.100  # RTS (strap low) asserted this long before the reset
RESET_PULSE_S = 0.100   # width of the DTR (RESETn low) pulse
MENU_TIMEOUT_S = 3.0    # the banner lands ~100 ms after reset; this is margin
HANDSHAKE_TIMEOUT_S = 5.0   # "1" to the bootloader's first 'C'
READY_TIMEOUT_S = 20.0

MENU_MARKERS = (b"Gecko Bootloader", b"1. upload gbl", b"BL >")
UPLOAD_DONE = b"Serial upload complete"


class PortReader:
    """read(n, timeout) over a pyserial port, with a pushback buffer.

    xmodem.send() takes read(n, timeout); pyserial's own read() takes the
    timeout from the port object, so the adaptation has to live somewhere.
    The pushback is what lets this file consume the menu's echo hunting for
    the XMODEM 'C' and then hand that same byte to the framer's handshake,
    rather than relying on the bootloader sending a second 'C' later.
    """

    def __init__(self, port):
        self.port = port
        self._pending = bytearray()

    def pushback(self, data):
        self._pending = bytearray(data) + self._pending

    def reset(self):
        """Drop everything buffered, here and in the port.

        Both halves, always: clearing only the port's buffer would leave a
        pushed-back byte to be read as if it had just arrived, which is
        precisely the stale-reply confusion the pushback exists to avoid.
        """
        self._pending = bytearray()
        self.port.reset_input_buffer()

    def read(self, n, timeout=None):
        if self._pending:
            take = bytes(self._pending[:n])
            del self._pending[:n]
            return take
        if timeout is not None and self.port.timeout != timeout:
            self.port.timeout = timeout
        return self.port.read(n)

    def read_until(self, markers, timeout):
        """Read until any of markers appears; return (found, everything read)."""
        deadline = time.time() + timeout
        seen = bytearray()
        while time.time() < deadline:
            chunk = self.read(256, timeout=0.1)
            if chunk:
                seen += chunk
                if any(m in seen for m in markers):
                    return True, bytes(seen)
        return False, bytes(seen)


def die(msg):
    sys.exit("flash.py: " + msg)


def open_port(path):
    """Open the carrier's CDC with both lines released BEFORE the open.

    pyserial applies pre-open DTR/RTS at open time. Setting them only after
    the open lets Linux's default (both asserted) reach the module first,
    which is a reset pulse and a strap assertion nobody asked for. This is
    the same order test/mt_regression.py's open_at_port() uses for
    --bridge cpico, and it is deliberately the one behaviour in this file
    that is NOT allowed to differ from the harness.
    """
    if not path.startswith("/dev/serial/by-id/"):
        die("refusing %r: use the /dev/serial/by-id path (ttyACM<n> is not an "
            "identity, and on this bench ttyACM0 is the Thread RCP)" % path)
    s = serial.Serial()
    s.port = path
    s.baudrate = BAUD
    s.timeout = 1.0
    s.dtr = False
    s.rts = False
    try:
        s.open()
    except (serial.SerialException, OSError) as exc:
        die("cannot open %s: %s" % (path, exc))
    s.dtr = False
    s.rts = False
    return s


def enter_bootloader(s, reader, strap):
    """Reset the module with the strap held and wait for the menu.

    With strap=False nothing is driven, but the wait is the same: see the
    header, item 3.
    """
    if not strap:
        # Wait for the menu exactly as the strap path does, and drive
        # nothing to get it. The open has already reset the module (header,
        # item 3), so on a blank slot the bootloader is still coming up:
        # writing "1" straight away would put the byte on a USART that is
        # not listening yet, and the run would die at the handshake timeout
        # looking like a bootloader fault. The banner may equally have
        # landed before this function was reached, which is why nothing is
        # discarded before the wait.
        print("--no-strap: driving neither reset nor strap; waiting for the "
              "menu the port's own open-time reset produces")
        found, seen = reader.read_until(MENU_MARKERS, MENU_TIMEOUT_S)
        _require_menu(found, seen,
                      "--no-strap needs a module that reaches the menu on "
                      "its own, which means a blank application slot; a "
                      "module with a valid application is started by the "
                      "open's own reset and has to be caught with the strap "
                      "(drop --no-strap).")
        return
    s.rts = True                # strap low (PC00)
    time.sleep(STRAP_SETTLE_S)
    s.dtr = True                # RESETn low
    time.sleep(RESET_PULSE_S)
    s.dtr = False               # released into the bootloader
    found, seen = reader.read_until(MENU_MARKERS, MENU_TIMEOUT_S)
    s.rts = False               # the strap is sampled at boot; release it now
    _require_menu(found, seen,
                  "Check the strap pin (README board table) and that the "
                  "module is on PA05/PA06.")


def _require_menu(found, seen, hint):
    """Announce the menu, or die saying what was read instead.

    Both entry paths end here, so the failure always names the same window
    and quotes the same tail; only the hint differs. The strap path
    releases RTS before calling this, because the line has to be let go
    whether the menu appeared or not.
    """
    if not found:
        die("no bootloader menu within %.0f s; got %r. %s"
            % (MENU_TIMEOUT_S, seen[-120:], hint))
    print("bootloader menu: %s" % _menu_line(seen))


def _menu_line(seen):
    """The bootloader's version banner, for the transcript."""
    for line in seen.replace(b"\x00", b"").splitlines():
        if b"Gecko Bootloader" in line:
            return line.decode("ascii", "replace").strip()
    return seen[-60:].decode("ascii", "replace").strip()


def upload(s, reader, image):
    """Menu "1", then the XMODEM transfer. Returns the block count."""
    reader.reset()
    s.write(b"1")
    found, seen = reader.read_until([bytes([xmodem.CRC_C])], HANDSHAKE_TIMEOUT_S)
    if not found:
        die("the bootloader did not start an XMODEM transfer after the menu's "
            "\"1\" (no 'C' within %.0f s); got %r"
            % (HANDSHAKE_TIMEOUT_S, seen[-120:]))
    # Hand exactly one 'C' back to the framer's own handshake. The
    # bootloader repeats 'C' about once a second until the first block
    # arrives, so more may already be queued in the port or land while
    # block 1 is in flight; xmodem.send()'s reply wait skips bytes it did
    # not ask for, which is what absorbs them.
    reader.pushback(bytes([xmodem.CRC_C]))

    total = (len(image) + BLOCK_SIZE - 1) // BLOCK_SIZE
    state = {"frames": 0}

    def write(data):
        if data and data[0] in (xmodem.SOH, xmodem.STX):
            state["frames"] += 1
            if state["frames"] % 100 == 0:
                print("  %d/%d blocks" % (state["frames"], total))
        s.write(data)

    started = time.time()
    try:
        blocks = xmodem.send(reader.read, write, image, block_size=BLOCK_SIZE)
    except xmodem.XmodemError as exc:
        die("XMODEM upload failed after %d frame(s): %s" % (state["frames"], exc))
    took = time.time() - started
    retransmits = state["frames"] - blocks
    print("  %d blocks, %d frame(s) sent (%d retransmit(s)), %.1f s"
          % (blocks, state["frames"], retransmits, took))
    found, seen = reader.read_until([UPLOAD_DONE], 5.0)
    if not found:
        die("the bootloader ACKed the EOT but did not report %r; got %r"
            % (UPLOAD_DONE.decode(), seen[-120:]))
    print("bootloader: %s" % UPLOAD_DONE.decode())
    return blocks


def run_application(s, reader):
    """Menu "2": leave the bootloader and start the application."""
    time.sleep(0.2)
    reader.reset()
    s.write(b"2")


def wait_ready(reader, timeout):
    found, seen = reader.read_until([b"+MTREADY"], timeout)
    return found, seen


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True,
                    help="/dev/serial/by-id path of the carrier's CDC port")
    ap.add_argument("--image", required=True, help="the .gbl to upload")
    ap.add_argument("--no-strap", action="store_true",
                    help="drive neither reset nor strap; the module must "
                         "already be at the bootloader menu")
    ap.add_argument("--ready-timeout", type=float, default=READY_TIMEOUT_S,
                    help="seconds to wait for +MTREADY after the application "
                         "is started (default %(default)s; the skeleton takes "
                         "about 0.1 s, so raise it only for an image that "
                         "does real work before the marker)")
    args = ap.parse_args()

    try:
        with open(args.image, "rb") as f:
            image = f.read()
    except OSError as exc:
        die("cannot read the image: %s" % exc)
    if not image:
        die("%s is empty" % args.image)
    print("image: %s, %d bytes, %d block(s) of %d"
          % (args.image, len(image),
             (len(image) + BLOCK_SIZE - 1) // BLOCK_SIZE, BLOCK_SIZE))

    s = open_port(args.port)
    reader = PortReader(s)
    try:
        enter_bootloader(s, reader, strap=not args.no_strap)
        blocks = upload(s, reader, image)
        print("uploaded %d block(s), %d bytes" % (blocks, len(image)))
        run_application(s, reader)
        found, seen = wait_ready(reader, args.ready_timeout)
        if not found:
            die("no +MTREADY within %.0f s after the upload; the AT port read "
                "%r" % (args.ready_timeout, seen[-120:]))
        print("+MTREADY seen: the module is running the new image")
    finally:
        try:
            s.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
