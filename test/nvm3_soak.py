#!/usr/bin/env python3
"""nvm3_soak.py: the baseline soak for the nvm3 repack round.

Repeatedly reboots the bridge through its chime endpoint (AT+MTCHIME),
watches the boot console's nvm3 object count line, and after each
churn phase commissions the bridge into the bench's fabric the same way
the catalogue proof does, so the fabric records nvm3 writes on the
device. The free-byte line is the watch value: when it drops below
--threshold the soak stops early with reached=true, and the round's
fix round gets its failure to run against. If every cycle ends with
free above the threshold, the baseline never failed and the round
stops and reports that instead.

The Thread dataset is a credential: it enters only through the
harness's own transport gate and leaves only inside chip-tool's argv
(H.pairing_argv); it is never printed and the JSON header lists the
arguments with --dataset and --psk removed. The console (the Debug
Probe's UART
CDC, 115200, DTR asserted: platform/silabs/README.md, the console
section) is read for one line per boot, the nvm3 line.

The device is left at 0x0100,0x0302 in a finally, so a serial drop or
a KeyboardInterrupt mid-cycle still runs the restore.
"""
import argparse
import json
import os
import re
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial as _serial  # noqa: E402
import mt_regression as H  # noqa: E402
from mt_catalogue_proof import Ctx, _onboarding_codes  # noqa: E402

# The boot console's nvm3 line: `I boot: nvm3: <n> object(s), <free> B
# free of <size> B at 0x<adr>, erase count <e>`.
_NVM3_RE = re.compile(
    r"^I boot: nvm3: (\d+) object\(s\), (\d+) B free of (\d+) B at 0x[0-9A-Fa-f]+,"
    r" erase count (\d+)\s*$")

# The composition run_cycle stages: the regression anchor plus the
# chime, which churn_until toggles through the chime endpoint.
CHIME_COMPOSITION = ["0x0100", "0x0146"]
CHIME_EP = 2


def parse_nvm3_line(line):
    """The boot console's nvm3 line to {"objects", "free", "size",
    "erase"} (ints), or None for any other line (including the
    `meminfo rc` error form)."""
    m = _NVM3_RE.match(line if isinstance(line, str) else "")
    if not m:
        return None
    return {"objects": int(m.group(1)), "free": int(m.group(2)),
            "size": int(m.group(3)), "erase": int(m.group(4))}


def churn_until(link, console, ep, threshold, block, max_blocks):
    """Churn the device's chime endpoint and read the nvm3 line after
    each reboot. One block is `block` pairs of
    AT+MTCHIME=<ep>,1,1 / AT+MTCHIME=<ep>,1,0 (a chime write to nvm3
    either way), then AT+MTRESET and the wait for +MTREADY; the console
    is drained immediately before each reset (every boot prints an nvm3
    line, and a reading taken before the reset would be the previous
    boot's), so the next nvm3 line off the console is the just-reset
    boot's free-byte reading. Stops when free < threshold
    (reached=true) or after max_blocks blocks (reached=false). Returns
    {"blocks", "writes", "free", "erase", "reached"}; free and erase
    are None when the console never sent a line before the block cap."""
    blocks = 0
    writes = 0
    free = None
    erase = None
    reached = False
    while blocks < max_blocks:
        for _ in range(block):
            link.command("AT+MTCHIME=%d,1,1" % ep)
            link.command("AT+MTCHIME=%d,1,0" % ep)
        writes += 2 * block
        blocks += 1
        console.drain()
        link.command("AT+MTRESET", timeout=5.0)
        if link.await_urc(r"\+MTREADY$", timeout=15.0) is None:
            break
        info = console.read_nvm3(30.0)
        if info is not None:
            free = info["free"]
            erase = info["erase"]
        if free is not None and free < threshold:
            reached = True
            break
    return {"blocks": blocks, "writes": writes, "free": free,
            "erase": erase, "reached": reached}


def run_cycle(link, console, chip, args, transport, dataset):
    """One soak cycle: factory reset, stage the chime composition,
    apply, churn, then the proof script's commissioning path (the
    onboarding codes via AT+MTQR?/manual capture, a fresh chip-tool
    storage, and the pairing run), then a final factory reset so the
    next cycle starts clean. Returns {"cycle", "churn", "paired",
    "rc", "tail"}; tail is H._pairing_tail(out) and is present only on
    a failed pairing. The dataset reaches chip-tool only through
    H.pairing_argv and never the return value."""
    link.command("AT+MTFRESET", timeout=5.0)
    if link.await_urc(r"\+MTREADY$", timeout=15.0) is None:
        return {"cycle": None, "churn": None, "paired": False, "rc": None,
                "tail": "ABORT: no +MTREADY after the cycle's MTFRESET"}
    if not H.stage_composition(link, CHIME_COMPOSITION):
        return {"cycle": None, "churn": None, "paired": False, "rc": None,
                "tail": "ABORT: composition " + ",".join(CHIME_COMPOSITION) +
                " did not stage"}
    link.drain(0.3)
    link.command("AT+MTEPAPPLY", timeout=5.0)
    if link.await_urc(r"\+MTREADY$", timeout=15.0) is None:
        return {"cycle": None, "churn": None, "paired": False, "rc": None,
                "tail": "ABORT: no +MTREADY after MTEPAPPLY"}
    churn = churn_until(link, console, CHIME_EP, args.threshold,
                        args.block, args.max_blocks)
    # the commissioning path, the proof script's step_3_5 shape
    # (mt_catalogue_proof.py main): the onboarding codes, the parsed
    # passcode and discriminator, a wiped chip-tool storage, and the
    # pairing run with the transport the gate detected
    qr, manual = _onboarding_codes(link)
    rc = None
    out = None
    paired = False
    tail = None
    if qr is not None:
        rc, out = chip.run(["payload", "parse-setup-payload", qr], timeout=15)
        parsed = H.parse_setup_payload(out) if rc == 0 else None
        if parsed is None:
            rc, out = -1, None
        else:
            passcode, discriminator = parsed
            chip.wipe_storage()
            ctx = Ctx()
            ctx.node_id, ctx.transport, ctx.dataset = args.node_id, transport, dataset
            ctx.passcode, ctx.discriminator = passcode, discriminator
            ctx.opts = args  # pairing_argv's WIFI branch reads ctx.opts.ssid/.psk
            rc, out = chip.run(H.pairing_argv(ctx), timeout=120)
            paired = rc == 0
    else:
        rc, out = -1, None
    if not paired:
        tail = H._pairing_tail(out, getattr(args, "psk", None))
    link.command("AT+MTFRESET", timeout=5.0)
    return {"cycle": None, "churn": churn, "paired": paired, "rc": rc,
            "tail": tail}


class ConsoleReader:
    """The boot console: pyserial on the Debug Probe's UART CDC at
    115200, DTR asserted before and after the open (the CDC discards
    output while the host holds DTR low; the controller's scratch
    runner does `open, c.dtr = True`). Reads lines and hands the
    first one parse_nvm3_line accepts back, the rest dropped."""

    def __init__(self, path, baud=115200, serial_mod=None):
        self.serial_mod = serial_mod or _serial
        self.path = path
        self.baud = baud
        self._port = None
        self._buf = b""

    def open(self):
        port = self.serial_mod.Serial()
        port.port = self.path
        port.baudrate = self.baud
        port.timeout = 0.05
        port.dtr = True
        port.open()
        port.dtr = True
        self._port = port
        return port

    def close(self):
        if self._port is not None:
            self._port.close()
            self._port = None

    def drain(self):
        """Discard whatever the console already holds, so the next
        read_nvm3 is the next boot's line. A no-op when the port is not
        open."""
        if self._port is not None:
            self._port.reset_input_buffer()
        self._buf = b""

    def read_nvm3(self, timeout):
        """The next nvm3 line, as a dict, or None on timeout. Every
        other line on the console is dropped."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._read_line()
            if line is None:
                continue
            info = parse_nvm3_line(line)
            if info is not None:
                return info
        return None

    def _read_line(self):
        while b"\n" not in self._buf:
            if self._port is None:
                return None
            data = self._port.read(64)
            if not data:
                return None
            self._buf += data
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("MT_PORT"),
                    help="the bridge's AT port, a /dev/serial/by-id path")
    ap.add_argument("--console", default=None,
                    help="the boot console (Debug Probe UART CDC), a "
                         "/dev/serial/by-id path; 115200, DTR asserted")
    ap.add_argument("--bridge", choices=sorted(H.BRIDGE_LINES), default="cpico")
    ap.add_argument("--chip-tool",
                    default=os.environ.get("MT_CHIPTOOL", H.DEFAULT_CHIPTOOL))
    ap.add_argument("--storage",
                    default=os.environ.get("MT_CHIPTOOL_STORAGE",
                                           "/tmp/mt-regression"))
    ap.add_argument("--node-id", type=lambda x: int(x, 0), default=0x4845)
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--threshold", type=int, default=2500,
                    help="free bytes below which the churn stops early")
    ap.add_argument("--block", type=int, default=100,
                    help="chime pairs per churn block")
    ap.add_argument("--max-blocks", type=int, default=40)
    ap.add_argument("--out", default=None, help="the JSON result path")
    # the harness's transport arguments, as mt_catalogue_proof.build_parser
    # has them: the dataset enters only through the gate, never printed
    # and removed from the JSON header with the psk
    ap.add_argument("--transport", choices=["WIFI", "THREAD"], default=None)
    ap.add_argument("--dataset", default=os.environ.get("MT_DATASET"))
    ap.add_argument("--ot-ctl", dest="ot_ctl", default=H.DEFAULT_OTCTL)
    ap.add_argument("--ssid", default=os.environ.get("MT_SSID"))
    ap.add_argument("--psk", default=os.environ.get("MT_PSK"))
    return ap


def _restore(link, composition="0x0100,0x0302"):
    """The bench's documented start state, in a finally so a serial
    drop (or any other exception) mid-cycle still runs it."""
    link.command("AT+MTFRESET", timeout=5.0)
    if link.await_urc(r"\+MTREADY$", timeout=15.0) is None:
        return
    H.stage_composition(link, composition.split(","))
    link.drain(0.3)
    link.command("AT+MTEPAPPLY", timeout=5.0)
    link.await_urc(r"\+MTREADY$", timeout=15.0)


def main(argv=None):
    args = build_parser().parse_args(argv)
    if not args.port:
        build_parser().error("--port is required (or MT_PORT): a "
                             "/dev/serial/by-id path")
    if not args.console:
        build_parser().error("--console is required: a /dev/serial/by-id path "
                             "to the Debug Probe's UART CDC")
    chip = H.ChipTool(args.chip_tool, args.storage)
    port = H.open_at_port(args.port, _serial, args.bridge)
    link = H.ATLink(port)
    if args.bridge == "cpico":
        H.wait_boot_marker(link)
    console = ConsoleReader(args.console)
    console.open()
    header = {"port": args.port, "timestamp":
              datetime.now(timezone.utc).isoformat(timespec="seconds"),
              "fw_repo_head": H.repo_head(H.REPO_ROOT)}
    s = H.Suite()
    problem, transport, dataset = H._transport_gate(chip, args, link, H.otctl_run)
    if problem:
        print("ABORT: " + problem)
        return 2
    res, lines = link.command("AT+CGMR")
    header["cgmr"] = lines[0] if res == 0 and lines else None
    # the arguments, except the credentials: the Thread dataset and
    # the wifi psk are credentials and never enter the result file
    header["args"] = {k: v for k, v in vars(args).items()
                      if k not in ("dataset", "psk")}
    cycles = []
    commissioned = 0
    try:
        for i in range(1, args.cycles + 1):
            cyc = run_cycle(link, console, chip, args, transport, dataset)
            cyc["cycle"] = i
            cycles.append(cyc)
            if cyc["paired"]:
                commissioned += 1
            c = cyc["churn"]
            free = c["free"] if c else None
            writes = c["writes"] if c else 0
            erase = c["erase"] if c else None
            print("cycle %d: free %s B after %s writes (erase %s), "
                  "paired %s" % (i, free, writes, erase,
                                 "yes" if cyc["paired"] else "no"))
            if c and c["reached"]:
                print("threshold %d B reached in cycle %d, stopping"
                      % (args.threshold, i))
                break
    finally:
        _restore(link)
        console.close()
        try:
            port.close()
        except Exception:
            pass
    print("nvm3 soak: %d of %d commissioned" % (commissioned, len(cycles)))
    if args.out:
        doc = {"header": header, "cycles": cycles}
        with open(args.out, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
    return 0 if commissioned == len(cycles) else 1


if __name__ == "__main__":
    sys.exit(main())
