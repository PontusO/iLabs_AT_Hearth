#!/usr/bin/env python3
"""Measure the largest endpoint composition an image can actually serve.

Why this exists
---------------
The combined WiFi+Thread image (`build_combined`) links both network stacks
and starts about 39.5 KB of heap below `build_wifi`, so the 28-endpoint
`PHASE3_COMPOSITION` does not fit in it: the device accepts the whole
composition, may even commission, and then fails under controller traffic
with lwIP `ERR_MEM` (CHIP error `0x3000001`) on `SendMessage`, followed by
retransmission exhaustion and a CASE timeout. Nothing short of real
operational traffic sees this, which is why the criterion below is not
"did AT+MTEPAPPLY answer OK".

The cap that came out of this on 2026-08-20 (firmware 0.12.0, WiFi-active)
is in this repository's `README.md`, under "Endpoint capacity": the whole
measured curve, the heap floor that is the real rule, and the per-type
costs. Its user-facing half lives in the `iLabs_Hearth` Arduino library's
`fw/README.md`. The committed evidence is
`test/baselines/combined-devicetypes.json`,
whose header carries the `max_endpoints` the Phase 3 baseline was recorded
at. Re-run this after an SDK bump, a cluster-gate change or a
`sdkconfig.defaults*` edit: all three move heap, and the cap is a heap
limit wearing an endpoint count as a proxy.

What one trial does
-------------------
1. `AT+MTFRESET`, stage the first N entries of `PHASE3_COMPOSITION`,
   `AT+MTEPAPPLY`, confirm `AT+MTEP?` reads back N lines.
2. Read `free heap at startup` off the C6 console, tied to the endpoint
   count the SAME boot printed, so a figure can never drift onto a
   neighbouring boot. Needs --console: the AT link cannot report it.
3. Commission with chip-tool (ble-wifi or ble-thread, per the transport
   the device reports), require `+MTEVT:3` and exactly one fabric.
4. Drive operational traffic: three rounds of a root PartsList read (its
   response grows with the composition, so it is the read most likely to
   hit a buffer wall), an OnOff toggle and read-back at endpoint 1, and a
   device-type-list read at the HIGHEST endpoint; then a live subscription
   that must deliver at least two reports while the host drives the
   attribute.

A trial passes only if 4 completes.

Usage
-----
    python3 test/mt_endpoint_cap.py --port /dev/serial/by-id/... \\
        --console /dev/serial/by-id/...-if01 --out caps.json 14 21 25

Bisect rather than sweep: each trial costs a commissioning cycle. Feed it
the counts you want, in the order you want them.
"""
import argparse
import json
import os
import re
import sys
import time
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mt_regression import (ATLink, ChipTool, DEFAULT_CHIPTOOL, DEFAULT_OTCTL,
                           PHASE3_COMPOSITION, Subscriber, cmd_retry,
                           parse_setup_payload, phase3_gate,
                           stage_composition)

# The console (GPIO2 via the bridge) is a SEPARATE stream from the AT link
# (GPIO16/17). Merging them makes every URC assertion flaky, so this reads
# the console as a plain tail rather than sharing the link's transport.
HEAP_RE = re.compile(r"free heap at startup: (\d+)")


def boot_heap(console_path, endpoints, wait=8.0):
    """Free heap from the last boot block whose endpoint count matches.

    +MTREADY is written BEFORE the heap line (mt_at_start() runs first in
    app_main), so a caller that has just seen +MTREADY can be here before
    the figure exists. Poll rather than sample once.
    """
    deadline = time.monotonic() + wait
    want = "Matter started with %d endpoint(s)" % endpoints
    while True:
        try:
            with open(console_path, "rb") as f:
                txt = f.read().decode("utf-8", "replace")
        except OSError:
            txt = ""
        best = None
        for m in re.finditer(re.escape(want), txt):
            h = HEAP_RE.search(txt[m.end():m.end() + 400])
            if h:
                best = int(h.group(1))
        if best is not None or time.monotonic() > deadline:
            return best
        time.sleep(0.5)


class Trial:
    def __init__(self, link, chip, args, transport, dataset):
        self.link = link
        self.chip = chip
        self.args = args
        self.transport = transport
        self.dataset = dataset

    def compose(self, n):
        res, _ = cmd_retry(self.link, "AT+MTFRESET", timeout=5.0)
        if res != 0:
            return "AT+MTFRESET refused"
        if self.link.await_urc(r"\+MTREADY$", timeout=20.0) is None:
            return "no +MTREADY after AT+MTFRESET"
        if not stage_composition(
                self.link, [dt for _slot, dt in PHASE3_COMPOSITION[:n]]):
            return "staging refused"
        self.link.drain(0.3)
        res, _ = self.link.command("AT+MTEPAPPLY", timeout=5.0)
        if res != 0:
            return "AT+MTEPAPPLY refused"
        if self.link.await_urc(r"\+MTREADY$", timeout=20.0) is None:
            return "no +MTREADY after AT+MTEPAPPLY"
        res, lines = cmd_retry(self.link, "AT+MTEP?")
        if res != 0 or len(lines) != n:
            return ("AT+MTEP? read back %s lines, wanted %d"
                    % (len(lines) if res == 0 else "ERROR", n))
        return None

    def commission(self):
        node = "0x%X" % self.args.node_id
        res, lines = cmd_retry(self.link, "AT+MTCODES?")
        m = (re.fullmatch(r"\+MTCODES:(.+),(\d{11})", lines[0])
             if res == 0 and lines else None)
        if not m:
            return "no onboarding codes"
        rc, out = self.chip.run(
            ["payload", "parse-setup-payload", m.group(1)], timeout=20)
        parsed = parse_setup_payload(out) if rc == 0 else None
        if not parsed:
            return "QR payload did not parse"
        passcode, disc = parsed
        self.chip.wipe_storage()
        self.link.drain(0.3)
        if self.transport == "THREAD":
            argv = ["pairing", "ble-thread", node, "hex:" + self.dataset,
                    str(passcode), str(disc)]
        else:
            argv = ["pairing", "ble-wifi", node, self.args.ssid,
                    self.args.psk, str(passcode), str(disc)]
        rc, _ = self.chip.run(argv, timeout=180)
        if rc != 0:
            return "chip-tool pairing rc=%d" % rc
        if self.link.await_urc(r"\+MTEVT:3$", timeout=90.0) is None:
            return "no +MTEVT:3"
        res, lines = cmd_retry(self.link, "AT+MTFABRICS?")
        if res != 0 or lines != ["+MTFABRICS:1"]:
            return "fabric count did not settle at 1"
        return None

    def operational(self, n):
        node = "0x%X" % self.args.node_id
        for rnd in (1, 2, 3):
            rc, out = self.chip.run(
                ["descriptor", "read", "parts-list", node, "0"], timeout=45)
            if rc != 0:
                return "round %d: parts-list read rc=%d" % (rnd, rc)
            m = re.search(r"PartsList:\s*(\d+)\s*entries", out)
            got = int(m.group(1)) if m else -1
            if got < n:
                return ("round %d: root PartsList reported %d entries, "
                        "wanted %d" % (rnd, got, n))
            rc, _ = self.chip.run(["onoff", "toggle", node, "1"], timeout=45)
            if rc != 0:
                return "round %d: onoff toggle rc=%d" % (rnd, rc)
            rc, _ = self.chip.run(["onoff", "read", "on-off", node, "1"],
                                  timeout=45)
            if rc != 0:
                return "round %d: on-off read rc=%d" % (rnd, rc)
            rc, _ = self.chip.run(["descriptor", "read", "device-type-list",
                                   node, str(n)], timeout=45)
            if rc != 0:
                return ("round %d: device-type-list read at ep %d rc=%d"
                        % (rnd, n, rc))
        sub = Subscriber(self.chip, self.args.node_id, endpoint=1,
                         min_s=0, max_s=5)
        if not sub.start(settle=25.0):
            sub.stop()
            return "subscription never produced a priming report"
        try:
            self.link.command("AT+MTATTR=1,6,0,1")
            time.sleep(3)
            self.link.command("AT+MTATTR=1,6,0,0")
            time.sleep(3)
            if len(sub.reports()) < 2:
                return ("subscription produced %d reports, wanted at least 2"
                        % len(sub.reports()))
        finally:
            sub.stop()
        return None


def run_trial(trial, console, n):
    print("  --- N=%d ---" % n, flush=True)
    rec = {"n": n, "heap": None, "verdict": "FAIL", "stage": None,
           "detail": None}
    problem = trial.compose(n)
    if problem:
        rec.update(stage="compose", detail=problem)
        return rec
    rec["heap"] = boot_heap(console, n)
    print("      free heap at startup: %s" % rec["heap"], flush=True)
    for stage, fn in (("commission", lambda: trial.commission()),
                      ("operational", lambda: trial.operational(n))):
        problem = fn()
        if problem:
            rec.update(stage=stage, detail=problem)
            return rec
        print("      %s ok" % stage, flush=True)
    rec.update(verdict="PASS", stage="operational")
    return rec


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("counts", nargs="+", type=int,
                    help="endpoint counts to try, in order")
    ap.add_argument("--port", default=os.environ.get("MT_PORT"),
                    help="AT link; a /dev/serial/by-id path, never ttyACM<n>")
    ap.add_argument("--console", required=True,
                    help="C6 console stream (a file being tailed, or a "
                         "device node): the heap figure is only printed "
                         "there")
    ap.add_argument("--out", default=None, help="write results as JSON")
    ap.add_argument("--transport", choices=["WIFI", "THREAD"], default=None,
                    help="override detection (default: ask AT+MTNET?)")
    ap.add_argument("--chip-tool",
                    default=os.environ.get("MT_CHIPTOOL", DEFAULT_CHIPTOOL))
    ap.add_argument("--ot-ctl", dest="ot_ctl",
                    default=os.environ.get("MT_OTCTL", DEFAULT_OTCTL))
    ap.add_argument("--dataset", default=os.environ.get("MT_DATASET"))
    ap.add_argument("--storage", default="/tmp/mt-endpoint-cap")
    ap.add_argument("--ssid", default=os.environ.get("MT_SSID"))
    ap.add_argument("--psk", default=os.environ.get("MT_PSK"))
    ap.add_argument("--node-id", type=lambda x: int(x, 0), default=0x4845)
    args = ap.parse_args(argv)
    if not args.port:
        ap.error("--port is required (or MT_PORT): pass a "
                 "/dev/serial/by-id path, never /dev/ttyACM<n>")
    limit = len(PHASE3_COMPOSITION)
    for n in args.counts:
        if not 1 <= n <= limit:
            ap.error("counts must be between 1 and %d; got %d" % (limit, n))

    import serial
    port = serial.Serial(args.port, 115200, timeout=0.05)
    link = ATLink(port)
    chip = ChipTool(args.chip_tool, args.storage)
    gate_args = types.SimpleNamespace(
        transport=args.transport, dataset=args.dataset, ot_ctl=args.ot_ctl,
        ssid=args.ssid, psk=args.psk)
    problem, transport, dataset = phase3_gate(chip, gate_args, link)
    if problem:
        print("ABORT: " + problem)
        return 2
    print("  transport: %s" % transport, flush=True)
    trial = Trial(link, chip, args, transport, dataset)
    results = []
    for n in args.counts:
        rec = run_trial(trial, args.console, n)
        rec["transport"] = transport
        results.append(rec)
        print("  N=%-3d %-4s heap=%-7s %s"
              % (n, rec["verdict"], rec["heap"], rec["detail"] or ""),
              flush=True)
        if args.out:
            with open(args.out, "w") as f:
                json.dump(results, f, indent=1)
    port.close()
    return 0 if all(r["verdict"] == "PASS" for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
