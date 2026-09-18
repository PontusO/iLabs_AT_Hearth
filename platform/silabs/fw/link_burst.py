#!/usr/bin/env python3
"""Push the AT link past the parser's line rate, deliberately, and account
for every byte.

Why this exists. The RX ring (port/hearth_ring.c, 1024 bytes against
MT_AT_LINE_MAX 512) raises "W link: rx ring overflow, dropped N byte(s)" on
the console when it has to discard input, and through round 1 and most of
round 2 that warning had never fired: the regression harness is synchronous,
so it never has more than one command line in flight and the ring never holds
more than one line. An untested diagnostic is not a diagnostic. This script is
the load the harness deliberately does not apply: command lines written back
to back with no wait for their terminal responses, optionally while a Matter
controller drives the device over Thread so controller-originated +MTATTR URCs
interleave with the responses.

It is a measurement tool, not a test: it has no pass criterion and exits 0
whatever it observes. Read the numbers next to the console's own
"W link: rx ring overflow" lines, which are the device's side of the same
event (`hearth_link_read()` reports the delta of a monotonic per-byte counter,
so the reported drops sum to the true total).

This carrier wires no RTS/CTS (see the README), so the OK/ERROR handshake is
the only thing throttling a host. A host that honours it never sees any of
this; that is the point of running it here instead.

    export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
    python3 platform/silabs/fw/link_burst.py --port "$MT_PORT" --lines 300
    python3 platform/silabs/fw/link_burst.py --port "$MT_PORT" --lines 300 \
            --toggle-node 0x4902

--port must be a /dev/serial/by-id path, never /dev/ttyACM<n>: ttyACM
numbering moves when USB devices come and go, and on this bench ttyACM0 is the
Thread RCP, where a stray write kills otbr-agent.
"""

import argparse
import os
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("link_burst.py: pyserial is required (pip install pyserial)")

DEFAULT_COMMAND = "AT+MTATTR=1,6,0"
DEFAULT_CHIPTOOL = os.path.expanduser(
    "~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool")


def open_port(path, baud):
    """The CPico bridge contract, applied BEFORE the open: both lines
    cleared, because on this carrier DTR holds RESETn low and RTS pulls the
    PC00 recovery strap low, and Linux asserts both by default at open time.
    Same order as fw/flash.py's open_port() and the harness's
    open_at_port(); setting them after the open lets the default reach the
    module first."""
    port = serial.Serial()
    port.port = path
    port.baudrate = baud
    port.timeout = 0.05
    port.dtr = False
    port.rts = False
    port.open()
    port.dtr = False
    port.rts = False
    return port


def wait_ready(port, timeout):
    """Opening this CDC pulses the module's reset even with both lines
    cleared, so a +MTREADY follows every open. Eat it, or the burst races
    the boot."""
    deadline = time.monotonic() + timeout
    seen = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(256)
        if chunk:
            seen += chunk
            if b"+MTREADY" in seen:
                return True
    return False


class Toggler(threading.Thread):
    """Drive OnOff on endpoint 1 from a commissioned controller in a loop,
    so the device raises +MTATTR URCs into the same stream the burst's
    responses are coming back on. Each completed toggle is timestamped, so
    the report can count only the ones inside the drain window."""

    def __init__(self, chip_tool, node, storage_dir):
        super().__init__(daemon=True)
        self.chip_tool = chip_tool
        self.node = node
        self.storage_dir = storage_dir
        self.stop = threading.Event()
        self.done = []          # monotonic timestamps of completed toggles

    def run(self):
        argv = [self.chip_tool, "onoff", "toggle", self.node, "1",
                "--storage-directory", self.storage_dir]
        while not self.stop.is_set():
            try:
                subprocess.run(argv, capture_output=True, text=True, timeout=60)
            except subprocess.TimeoutExpired:
                pass
            self.done.append(time.monotonic())


def classify(raw):
    """Split the returned stream into lines and count what came back.

    'malformed' is the interesting bucket: a dropped run in the middle of the
    input glues the fragments on either side of it into one line the parser
    cannot make sense of, so it is the visible consequence of an overflow on
    the response side."""
    text = raw.decode("utf-8", "replace")
    lines = [ln for ln in text.replace("\r\n", "\n").replace("\r", "\n").split("\n") if ln]
    counts = {"OK": 0, "ERROR": 0, "+MTERR": 0, "+MTATTR": 0, "other": 0}
    for ln in lines:
        if ln == "OK":
            counts["OK"] += 1
        elif ln == "ERROR":
            counts["ERROR"] += 1
        elif ln.startswith("+MTERR:"):
            counts["+MTERR"] += 1
        elif ln.startswith("+MTATTR:"):
            counts["+MTATTR"] += 1
        else:
            counts["other"] += 1
    return lines, counts


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("MT_PORT"),
                    help="the AT link, a /dev/serial/by-id path")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--lines", type=int, default=300,
                    help="command lines to write back to back (default 300)")
    ap.add_argument("--command", default=DEFAULT_COMMAND,
                    help="the line to repeat (default %r)" % DEFAULT_COMMAND)
    ap.add_argument("--toggle-node", default=None,
                    help="commissioned node id to toggle from chip-tool "
                         "during the burst, e.g. 0x4902; omit for no "
                         "controller load")
    ap.add_argument("--chip-tool", default=DEFAULT_CHIPTOOL)
    ap.add_argument("--storage-dir", default="/tmp/ct-hearth-mg24")
    ap.add_argument("--pace-ms", type=float, default=0.0,
                    help="milliseconds to wait between lines instead of "
                         "writing them back to back. 0 (the default) is the "
                         "burst; a non-zero value is the control that shows "
                         "the same volume is answered in full when nothing "
                         "upstream is over-driven")
    ap.add_argument("--quiet-for", type=float, default=5.0,
                    help="stop draining after this many seconds of silence "
                         "(default 5)")
    ap.add_argument("--max-drain", type=float, default=120.0,
                    help="hard cap on the drain window (default 120 s)")
    args = ap.parse_args()

    if not args.port:
        ap.error("--port (or MT_PORT) is required")
    if "/dev/serial/by-id/" not in args.port:
        ap.error("--port must be a /dev/serial/by-id path, not %r" % args.port)

    port = open_port(args.port, args.baud)
    print("+MTREADY after open: %s"
          % ("seen" if wait_ready(port, 25.0) else "NOT SEEN"))
    port.reset_input_buffer()

    toggler = None
    if args.toggle_node:
        toggler = Toggler(args.chip_tool, args.toggle_node, args.storage_dir)
        toggler.start()
        time.sleep(1.0)         # let the first toggle get going

    one = (args.command + "\r\n").encode("ascii")
    payload = one * args.lines
    t_write = time.monotonic()
    if args.pace_ms > 0:
        for _ in range(args.lines):
            port.write(one)
            port.flush()
            time.sleep(args.pace_ms / 1000.0)
    else:
        port.write(payload)
        port.flush()
    t_written = time.monotonic()

    raw = bytearray()
    last_byte = time.monotonic()
    hard_stop = last_byte + args.max_drain
    while True:
        now = time.monotonic()
        if now > hard_stop or now - last_byte > args.quiet_for:
            break
        chunk = port.read(4096)
        if chunk:
            raw += chunk
            last_byte = time.monotonic()
    t_drained = time.monotonic()

    if toggler:
        toggler.stop.set()
        toggler.join(timeout=70)
    port.close()

    lines, counts = classify(bytes(raw))
    terminals = counts["OK"] + counts["ERROR"]
    line_len = len(args.command) + 2

    print("")
    print("sent      %d line(s) of %d B = %d B, written in %.2f s (%s)"
          % (args.lines, line_len, len(payload), t_written - t_write,
             "back to back" if args.pace_ms <= 0
             else "paced %.1f ms apart" % args.pace_ms))
    print("drained   %.1f s (stopped after %.1f s of silence; cap %.0f s)"
          % (t_drained - t_written, args.quiet_for, args.max_drain))
    print("back      %d B, %d complete line(s)" % (len(raw), len(lines)))
    print("          OK %d, ERROR %d, +MTERR %d, +MTATTR %d, unrecognised %d"
          % (counts["OK"], counts["ERROR"], counts["+MTERR"],
             counts["+MTATTR"], counts["other"]))
    print("terminals %d of %d command line(s) answered" % (terminals, args.lines))
    if toggler:
        inside = sum(1 for t in toggler.done if t <= t_drained)
        print("toggles   %d completed inside the drain window (%d in all)"
              % (inside, len(toggler.done)))
        print("          so %d of the %d +MTATTR line(s) are controller URCs "
              "and %d are command echoes"
              % (inside, counts["+MTATTR"], counts["+MTATTR"] - inside))
    print("")
    print("Now read the console (the Debug Probe's UART CDC, DTR asserted) for")
    print("  W link: rx ring overflow, dropped N byte(s)")
    print("The device's reported drops sum to the true total: the counter is")
    print("per byte and monotonic, and each report is the delta since the last")
    print("hearth_link_read() (port/hearth_port_sl.c).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
