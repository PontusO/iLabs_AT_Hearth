#!/usr/bin/env python3
"""mt_catalogue_proof.py: the per-batch fabric proof for a catalogue batch.

Stages a batch's device types after the on/off light (endpoint 1, the
regression anchor), applies, commissions with chip-tool into the bench's
Thread fabric, and proves each endpoint's primary attribute in both
directions: an AT+MTATTR write read back by the controller, a controller
command or write raising the +MTATTR URC, and a second AT read agreeing.
Writes a harness-shaped result file (--baseline). Imports the harness's
pieces (test/mt_regression.py) and adds nothing to it.

Serial devices by /dev/serial/by-id only; never /dev/ttyACM<n> (on this
bench that is the Thread RCP). otbr-agent is never touched.
"""
import argparse
import json
import os
import re
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mt_regression as H  # noqa: E402

CHIP_CLUSTERS = {"onoff", "levelcontrol", "booleanstate", "occupancysensing",
                 "relativehumiditymeasurement", "pressuremeasurement",
                 "illuminancemeasurement", "flowmeasurement"}

def _type(devtype, name, revision, chip_cluster, cluster, attr, chip_attr,
          at_value, parse="int", controller=None, extra_reads=()):
    return {"devtype": devtype, "name": name, "revision": revision,
            "chip_cluster": chip_cluster,
            "primary": {"cluster": cluster, "attr": attr, "chip_attr": chip_attr,
                        "at_value": at_value, "parse": parse},
            "controller": controller, "extra_reads": list(extra_reads)}

# Endpoint 1 is always the on/off light (the regression harness's own
# anchor, PHASE3_COMPOSITION's convention too): staged ahead of every
# batch so the fabric looks like the bench's real deployed composition,
# but never re-proven here, since the existing harness already exercises
# it exhaustively. It is therefore not one of BATCHES's rows: only
# composition_for() knows about it.
ANCHOR_DEVTYPE = "0x0100"
ANCHOR_NAME = "on/off light (anchor)"

# controller = ("command", [chip-tool args after the cluster name, before node/ep],
#               expected +MTATTR value) or None for a read-only cluster.
BATCHES = {
    "mg24-batch1": [
        _type("0x0101", "dimmable light", 3, "levelcontrol", 8, 0, "current-level", 100,
              controller=("command", ["move-to-level", "200", "0", "0", "0"], 200)),
        _type("0x0015", "contact sensor", 2, "booleanstate", 69, 0, "state-value", 1, parse="bool",
              extra_reads=[("read-event", "state-change")]),
        _type("0x0044", "rain sensor", 1, "booleanstate", 69, 0, "state-value", 1, parse="bool"),
        _type("0x0041", "water freeze detector", 1, "booleanstate", 69, 0, "state-value", 1, parse="bool"),
        _type("0x0043", "water leak detector", 1, "booleanstate", 69, 0, "state-value", 1, parse="bool"),
        _type("0x0107", "occupancy sensor", 4, "occupancysensing", 1030, 0, "occupancy", 1,
              extra_reads=[("read", "feature-map", 2)]),
        _type("0x0307", "humidity sensor", 2, "relativehumiditymeasurement", 1029, 0, "measured-value", 5000),
        _type("0x0305", "pressure sensor", 2, "pressuremeasurement", 1027, 0, "measured-value", 1013),
        _type("0x0106", "light sensor", 3, "illuminancemeasurement", 1024, 0, "measured-value", 12345),
        _type("0x0306", "flow sensor", 2, "flowmeasurement", 1028, 0, "measured-value", 250),
        _type("0x010A", "on/off plug-in unit", 4, "onoff", 6, 0, "on-off", 1,
              controller=("command", ["on"], 1)),
        _type("0x010B", "dimmable plug-in unit", 5, "levelcontrol", 8, 0, "current-level", 100,
              controller=("command", ["move-to-level", "200", "0", "0", "0"], 200)),
    ],
}

def composition_for(batch):
    return [(1, ANCHOR_DEVTYPE)] + [(i + 2, t["devtype"])
                                     for i, t in enumerate(BATCHES[batch])]

def rows_for(batch):
    return BATCHES[batch]

def parse_bool_attr(out):
    m = re.search(r":\s+(TRUE|FALSE)\s*$", out or "", re.M)
    return None if m is None else m.group(1) == "TRUE"

def parse_device_types(out):
    """Every (device type id, revision) pair from a Descriptor
    DeviceTypeList chip-tool read, in order. The two values never share
    a line in chip-tool's real output: DataModelLogger prints
    "DeviceType: 777 (Heat Pump)", the id followed by a parenthesised
    name, then a separate "Revision: N" line with no id on it. Reuses
    the harness's own id parser (H.parse_device_types, already tolerant
    of the parenthesised name) and pairs it positionally with each
    entry's Revision line."""
    ids = H.parse_device_types(out)
    revs = [int(m) for m in re.findall(r"Revision:\s*(\d+)\s*$", out or "", re.M)]
    return list(zip(ids, revs))

def _onboarding_codes(link):
    """The AT+MTCODES? extraction step_3_5_commission uses
    (test/mt_regression.py ~1959-1962), lifted here since it is inline
    there rather than a separate importable helper. Returns (qr, manual)
    or (None, None); neither value is a credential, both are the
    device's own public onboarding codes."""
    res, lines = link.command("AT+MTCODES?")
    if res == 0 and lines:
        m = re.fullmatch(r"\+MTCODES:(.+),(\d{11})", lines[0])
        if m:
            return m.group(1), m.group(2)
    return None, None

def build_parser():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("MT_PORT"))
    ap.add_argument("--batch", required=True, choices=sorted(BATCHES))
    ap.add_argument("--bridge", choices=sorted(H.BRIDGE_LINES), default="challenger")
    ap.add_argument("--chip-tool", default=os.environ.get("MT_CHIPTOOL", H.DEFAULT_CHIPTOOL))
    ap.add_argument("--storage", default=os.environ.get("MT_CHIPTOOL_STORAGE",
                                                         "/tmp/mt-regression"))
    ap.add_argument("--transport", choices=["WIFI", "THREAD"], default=None)
    ap.add_argument("--dataset", default=os.environ.get("MT_DATASET"))
    ap.add_argument("--ot-ctl", dest="ot_ctl", default=H.DEFAULT_OTCTL)
    ap.add_argument("--ssid", default=os.environ.get("MT_SSID"))
    ap.add_argument("--psk", default=os.environ.get("MT_PSK"))
    ap.add_argument("--openocd-config", dest="openocd_config", default=None)
    ap.add_argument("--node-id", type=lambda x: int(x, 0), default=0x4845)
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--restore", default="0x0100,0x0302",
                    help="composition to stage after the proof (the bench's "
                         "documented start state); bare device types only, "
                         "no <devtype>,<variant> suffix support yet")
    ap.add_argument("--dry-run", action="store_true")
    return ap

def print_plan(batch):
    comp = composition_for(batch)
    print("%s: %d endpoints" % (batch, len(comp)))
    print("  ep  1 %s %s: staged only (the regression harness's own "
          "anchor, not re-proven here)" % (ANCHOR_DEVTYPE, ANCHOR_NAME))
    for (ep, dt), t in zip(comp[1:], rows_for(batch)):
        print("  ep %2d %s %s rev %d: AT write %s -> chip-tool %s read %s; controller %s"
              % (ep, dt, t["name"], t["revision"], t["primary"]["at_value"],
                 t["chip_cluster"], t["primary"]["chip_attr"],
                 "n/a (read-only cluster)" if t["controller"] is None else t["controller"][1]))

class Ctx:  # the fields pairing_argv() reads
    pass

def prove_endpoint(link, chip, s, node, ep, t):
    p = t["primary"]
    tag = "%s %s" % (t["devtype"], t["name"])
    rc, out = chip.run(["descriptor", "read", "device-type-list", node, str(ep)], timeout=30)
    s.check("%s ep%d device-type-list carries (%d, %d)"
            % (tag, ep, int(t["devtype"], 16), t["revision"]),
            rc == 0 and (int(t["devtype"], 16), t["revision"]) in parse_device_types(out))
    res, _ = link.command("AT+MTATTR=%d,%d,%d,%s" % (ep, p["cluster"], p["attr"], p["at_value"]))
    s.check("%s ep%d AT write %s -> OK" % (tag, ep, p["at_value"]), res == 0)
    link.drain(0.3)   # the local write raises its own +MTATTR; not this row's
    rc, out = chip.run([t["chip_cluster"], "read", p["chip_attr"], node, str(ep)], timeout=30)
    if p["parse"] == "bool":
        got = parse_bool_attr(out)
        want = bool(p["at_value"])
    else:
        got = H.parse_int_attr(out)
        want = p["at_value"]
    s.check("%s ep%d controller reads %s = %s after the AT write" % (tag, ep, p["chip_attr"], want),
            rc == 0 and got == want)
    for extra in t["extra_reads"]:
        if extra[0] == "read-event":
            rc, out = chip.run([t["chip_cluster"], "read-event", extra[1], node, str(ep)], timeout=30)
            s.check("%s ep%d %s event present" % (tag, ep, extra[1]),
                    rc == 0 and "StateChange" in out)
        else:
            rc, out = chip.run([t["chip_cluster"], "read", extra[1], node, str(ep)], timeout=30)
            s.check("%s ep%d %s = %s" % (tag, ep, extra[1], extra[2]), rc == 0 and H.parse_int_attr(out) == extra[2])
    if t["controller"] is None:
        s.not_applicable("%s ep%d controller write" % (tag, ep), "read-only cluster")
        last = p["at_value"]
    else:
        kind, args, want_urc = t["controller"]
        link.drain(0.2)
        rc, out = chip.run([t["chip_cluster"]] + args + [node, str(ep)], timeout=30)
        s.check("%s ep%d controller %s exits 0" % (tag, ep, " ".join(args)), rc == 0)
        got = link.await_urc(r"\+MTATTR:%d,%d,%d,%d$" % (ep, p["cluster"], p["attr"], want_urc), 10.0)
        s.check("%s ep%d +MTATTR:%d,%d,%d,%d on the AT link" % (tag, ep, ep, p["cluster"], p["attr"], want_urc),
                got is not None)
        rc, out = chip.run([t["chip_cluster"], "read", p["chip_attr"], node, str(ep)], timeout=30)
        s.check("%s ep%d controller reads back %d" % (tag, ep, want_urc),
                rc == 0 and (H.parse_int_attr(out) == want_urc if p["parse"] != "bool" else parse_bool_attr(out) == bool(want_urc)))
        last = want_urc
    res, lines = link.command("AT+MTATTR=%d,%d,%d" % (ep, p["cluster"], p["attr"]))
    s.check("%s ep%d second AT read agrees (%s)" % (tag, ep, last),
            res == 0 and lines == ["+MTATTR:%d,%d,%d,%s" % (ep, p["cluster"], p["attr"], last)])

def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.dry_run:
        print_plan(args.batch)
        return 0
    if not args.port:
        build_parser().error("--port is required (or MT_PORT): a /dev/serial/by-id path")
    import serial
    chip = H.ChipTool(args.chip_tool, args.storage)
    port = H.open_at_port(args.port, serial, args.bridge)
    link = H.ATLink(port)
    if args.bridge == "cpico":
        H.wait_boot_marker(link)
    header = {"port": args.port, "batch": args.batch,
              "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
              "fw_repo_head": H.repo_head(H.REPO_ROOT), "ssid": None,
              "node_id": "0x%X" % args.node_id, "chip_tool": args.chip_tool}
    s = H.Suite()
    problem, transport, dataset = H._transport_gate(chip, args, link, H.otctl_run)
    if problem:
        print("ABORT: " + problem)
        return 2
    res, lines = link.command("AT+CGMR")
    header["cgmr"] = lines[0] if res == 0 and lines else None
    comp = composition_for(args.batch)

    # stage, apply, read back (the harness's 3.1 pattern, minus the
    # persistence-across-a-second-reboot proof, which is not this
    # script's job)
    res, _ = link.command("AT+MTFRESET", timeout=5.0)
    s.check("compose: MTFRESET -> OK", res == 0)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("compose: +MTREADY after MTFRESET", ready is not None)
    devtypes = [dt for _ep, dt in comp]
    staged = H.stage_composition(link, devtypes)
    s.check("compose: composition staged", staged)
    link.drain(0.3)
    res, _ = link.command("AT+MTEPAPPLY", timeout=5.0)
    s.check("compose: MTEPAPPLY -> OK", res == 0)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("compose: +MTREADY after MTEPAPPLY", ready is not None)
    # the first command after a reboot can time out once (graph N22);
    # every analogous readback in the harness retries through this
    res, lines = H.cmd_retry(link, "AT+MTEP?")
    expected = ["+MTEP:%d,%d,%s" % (i, ep, dt) for i, (ep, dt) in enumerate(comp)]
    s.check("compose: composition readback exact", res == 0 and lines == expected)

    # commission: capture the onboarding codes, parse them, pair
    qr, manual = _onboarding_codes(link)
    have_codes = s.check("commission: onboarding codes captured", qr is not None)
    passcode = discriminator = None
    if have_codes:
        rc, out = chip.run(["payload", "parse-setup-payload", qr], timeout=15)
        parsed = H.parse_setup_payload(out) if rc == 0 else None
        have_codes = s.check("commission: QR payload parses", parsed is not None)
        if parsed is not None:
            passcode, discriminator = parsed
    paired = False
    if have_codes:
        chip.wipe_storage()
        ctx = Ctx()
        ctx.node_id, ctx.transport, ctx.dataset = args.node_id, transport, dataset
        ctx.passcode, ctx.discriminator = passcode, discriminator
        ctx.opts = args  # pairing_argv's WIFI branch reads ctx.opts.ssid/.psk
        rc, out = chip.run(H.pairing_argv(ctx), timeout=120)
        paired = s.check("commission: chip-tool pairing exits 0", rc == 0)
        if not paired:
            print(H._pairing_tail(out, getattr(args, "psk", None)))
    else:
        print("ABORT: cannot capture onboarding codes, skipping commissioning")

    node = "0x%X" % args.node_id
    if paired:
        for (ep, _), t in zip(comp[1:], rows_for(args.batch)):
            prove_endpoint(link, chip, s, node, ep, t)
    else:
        print("ABORT: pairing failed, skipping the endpoint proofs")

    # restore: factory-reset back to the bench's documented start state
    res, _ = link.command("AT+MTFRESET", timeout=5.0)
    s.check("restore: MTFRESET -> OK", res == 0)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("restore: +MTREADY after MTFRESET", ready is not None)
    restore_devtypes = args.restore.split(",")
    ok = H.stage_composition(link, restore_devtypes)
    s.check("restore: composition staged", ok)
    link.drain(0.3)
    res, _ = link.command("AT+MTEPAPPLY", timeout=5.0)
    s.check("restore: MTEPAPPLY -> OK", res == 0)
    ready = link.await_urc(r"\+MTREADY$", timeout=15.0)
    s.check("restore: +MTREADY after MTEPAPPLY", ready is not None)
    expected_restore = ["+MTEP:%d,%d,%s" % (i, i + 1, dt)
                        for i, dt in enumerate(restore_devtypes)]
    res, lines = H.cmd_retry(link, "AT+MTEP?")
    s.check("restore: composition %s staged" % args.restore,
            res == 0 and lines == expected_restore)
    res, lines = H.cmd_retry(link, "AT+MTFABRICS?")
    s.check("restore: fabrics 0", res == 0 and lines == ["+MTFABRICS:0"])
    chip.wipe_storage()

    passed = sum(1 for _, ok, _ in s.results if ok); failed = len(s.results) - passed
    print("%s: %d passed, %d failed, %d not applicable" % (args.batch, passed, failed, len(s.na)))
    if args.baseline:
        H.write_baseline(args.baseline, header, s)
    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
