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
import types
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mt_regression as H  # noqa: E402

CHIP_CLUSTERS = {"onoff", "levelcontrol", "booleanstate", "occupancysensing",
                 "relativehumiditymeasurement", "pressuremeasurement",
                 "illuminancemeasurement", "flowmeasurement",
                 "colorcontrol", "thermostat", "fancontrol",
                 "windowcovering", "airquality",
                 "doorlock", "valveconfigurationandcontrol",
                 "powersource", "smokecoalarm", "operationalstate",
                 "modeselect", "chime",
                 "switch", "pumpconfigurationandcontrol",
                 "rvcrunmode", "rvccleanmode", "rvcoperationalstate"}

# The MG24 Hearth build's product id. Two Hearth boards on the bench share
# discriminator 0xF00, so chip-tool's BLE scan can pair the other one
# (graph B573, finding F579); a proof on the wrong board proves nothing.
EXPECTED_PRODUCT_ID = "productId=0x8010"

def _check(cluster, attr, chip_attr, at_value, parse="int", controller=None,
           echo=False, null_read=False, chip_cluster=None, urc_chip_attr=None,
           at_cmd=None):
    """One proven attribute. controller = (kind, chip-tool args after the
    cluster name, (urc_cluster, urc_attr, urc_value)), kind "command" or
    "write"; the URC may land on a different attribute from the AT write's
    (the window covering's command writes Target, not Current); kind
    "verdict" instead of the URC triple is (fwd_cluster, fwd_command),
    the answer 1 (allow) / 0 (deny) / None (unanswered, +MTCMDTO), the
    want dict {"rc0": bool, "status": int (optional), "error_state":
    int (optional: the chip-tool response's ErrorStateID, read by
    H.parse_status, which answers it for every response shape since the
    trio's allow/deny/refusal all exit 0)} and an optional payload (batch
    4) passed to CmdResponder.expect(payload=) (the chime's PlayChimeSound
    7); kind "notify" is (args, (fwd_cluster, fwd_command),
    payload or None, [follow-up at_cmd format strings],
    ("read-event", event_name)) (batch 4): the controller command is
    invoked threaded, the seq-0 forward is asserted by
    CmdResponder.expect_notify and NEVER answered, then the host's own
    AT completion line(s) and the event read (AT_MT_SPEC 3.22's
    self-test: the notify, then AT+MTALARM=<ep>,5,0, then the
    SelfTestComplete event); kind "no_forward" is (args, want) (batch 4):
    after the controller command no +MTCMD for the cluster's own commands
    may arrive within 3s (the chime disabled, the trio's Pause from
    Stopped, refused by the SDK server before any forward). echo asserts
    the AT write's own +MTATTR echo instead of draining it (DE531); null_read
    asserts +MTERR:5 on an AT read before the write (AT_MT_SPEC 3.8's null
    rule). chip_cluster overrides the type's for a check on another cluster.
    at_cmd, when set, is the format string (with %(ep)d) of the AT line the
    write step sends instead of the AT+MTATTR write, e.g. "AT+MTLOCK=%(ep)d,1"."""
    return {"cluster": cluster, "attr": attr, "chip_attr": chip_attr,
            "at_value": at_value, "parse": parse, "controller": controller,
            "echo": echo, "null_read": null_read, "chip_cluster": chip_cluster,
            "urc_chip_attr": urc_chip_attr, "at_cmd": at_cmd}

def _multi(devtype, name, revision, chip_cluster, checks, extra_reads=(),
           setup=()):
    """The batch 2+ row shape; setup (batch 4) is a
    ("setup", [at_cmd format strings], (cluster_arg, attr_arg, [substrings]))
    tuple, or None: run once before the type's checks, each AT line must
    answer OK, and the following controller list read must name every
    substring (AT_MT_SPEC 3.20/3.23's set-only list stores)."""
    return {"devtype": devtype, "name": name, "revision": revision,
            "chip_cluster": chip_cluster, "checks": list(checks),
            "extra_reads": list(extra_reads), "setup": setup}

def _type(devtype, name, revision, chip_cluster, cluster, attr, chip_attr,
          at_value, parse="int", controller=None, extra_reads=()):
    """Batch 1's one-attribute shape; its controller tuple's URC is the
    same attribute, (kind, args, value)."""
    if controller is not None:
        kind, args, value = controller
        controller = (kind, args, (cluster, attr, value))
    return _multi(devtype, name, revision, chip_cluster,
                  [_check(cluster, attr, chip_attr, at_value, parse, controller)],
                  extra_reads)

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
#
# Two rules the bench taught this table on 2026-09-22, both of them Matter
# facts rather than port behaviour, and both of them things the design spec
# (section 4 step 3) got wrong when it named `move-to-level` and `on`:
#
#  * The controller command must move the attribute AWAY from the value the
#    AT write of step 2 put there. A command that asks for the value already
#    held is a no-op, raises no MatterPostAttributeChangeCallback and so no
#    +MTATTR URC, and the row waits for something that can never arrive.
#    Observed on the console as "Endpoint c On/off already set to new value".
#  * LevelControl's four "without On/Off" commands (Move, MoveToLevel, Stop,
#    Step) are gated by the Options processing: with the endpoint's OnOff
#    cluster present, its OnOff attribute FALSE and the Options ExecuteIfOff
#    bit clear, the command returns SUCCESS and changes nothing
#    (level-control.cpp's shouldExecuteIfOff(), quoting ZCL7 3.10.2.2.8.1;
#    the caller at :889 returns Status::Success on the refusal, so chip-tool
#    exits 0 and the row sees a silent no-op). A freshly composed dimmable
#    light is off, so the "with On/Off" variant is the command a real dimmer
#    sends and the only one that can prove the URC path here.
BATCHES = {
    "mg24-batch1": [
        _type("0x0101", "dimmable light", 3, "levelcontrol", 8, 0, "current-level", 100,
              controller=("command", ["move-to-level-with-on-off", "200", "0", "0", "0"], 200)),
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
        # OnOff is a BOOLEAN attribute, so chip-tool prints "OnOff: TRUE"
        # and parse="bool" is what reads it; parse_int_attr finds no
        # integer on that line and answers None.
        _type("0x010A", "on/off plug-in unit", 4, "onoff", 6, 0, "on-off", 1, parse="bool",
              controller=("command", ["off"], 0)),
        _type("0x010B", "dimmable plug-in unit", 5, "levelcontrol", 8, 0, "current-level", 100,
              controller=("command", ["move-to-level-with-on-off", "200", "0", "0", "0"], 200)),
    ],
    "mg24-batch2": [
        _multi("0x010C", "colour temperature light", 4, "colorcontrol", [
            _check(0x0300, 0x0007, "color-temperature-mireds", 300,
                   controller=("command", ["move-to-color-temperature", "400", "0", "1", "1"],
                               (0x0300, 0x0007, 400))),
        ], extra_reads=[("read", "feature-map", 16), ("read", "color-capabilities", 16)]),
        _multi("0x010D", "extended colour light", 4, "colorcontrol", [
            _check(0x0300, 0x0007, "color-temperature-mireds", 300,
                   controller=("command", ["move-to-color-temperature", "400", "0", "1", "1"],
                               (0x0300, 0x0007, 400))),
            _check(0x0300, 0x0000, "current-hue", 60,
                   controller=("command", ["move-to-hue", "120", "0", "0", "1", "1"],
                               (0x0300, 0x0000, 120))),
        ], extra_reads=[("read", "feature-map", 25), ("read", "color-capabilities", 25)]),
        _multi("0x0301", "thermostat", 4, "thermostat", [
            _check(0x0201, 0x0000, "local-temperature", -500, null_read=True),
            _check(0x0201, 0x0012, "occupied-heating-setpoint", 2000,
                   controller=("command", ["setpoint-raise-lower", "0", "10"],
                               (0x0201, 0x0012, 2100))),
        ]),
        _multi("0x002B", "fan", 4, "fancontrol", [
            _check(0x0202, 0x0002, "percent-setting", 30,
                   controller=("write", ["write", "percent-setting", "70"],
                               (0x0202, 0x0002, 70))),
        ]),
        _multi("0x0202", "window covering", 5, "windowcovering", [
            _check(0x0102, 0x000E, "current-position-lift-percent100ths", 2500,
                   controller=("command", ["go-to-lift-percentage", "7500"],
                               (0x0102, 0x000B, 7500)),
                   urc_chip_attr="target-position-lift-percent100ths"),
        ]),
        _multi("0x002C", "air quality sensor", 1, "airquality", [
            _check(0x005B, 0x0000, "air-quality", 3, echo=True),
        ], extra_reads=[("read", "feature-map", 15)]),
    ],
    "mg24-batch3": [
        _multi("0x000A", "door lock", 3, "doorlock", [
            _check(0x0101, 0x0000, "lock-state", 1, null_read=True,
                   at_cmd="AT+MTLOCK=%(ep)d,1"),
            _check(0x0101, 0x0000, "lock-state", 1, at_cmd="AT+MTLOCK=%(ep)d,1",
                   controller=("verdict", ["lock-door", "--timedInteractionTimeoutMs", "5000"],
                               (257, 0), 1, {"rc0": True, "status": 0x0})),
            _check(0x0101, 0x0000, "lock-state", 1, at_cmd="AT+MTLOCK=%(ep)d,1",
                   controller=("verdict", ["unlock-door", "--timedInteractionTimeoutMs", "5000"],
                               (257, 1), 0, {"rc0": False, "status": 0x1})),
            _check(0x0101, 0x0000, "lock-state", 1, at_cmd="AT+MTLOCK=%(ep)d,1",
                   controller=("verdict", ["lock-door", "--timedInteractionTimeoutMs", "5000"],
                               (257, 0), None, {"rc0": False, "status": 0x1})),
        ]),
        _multi("0x0042", "water valve", 1, "valveconfigurationandcontrol", [
            _check(0x0081, 0x0004, "current-state", 1, null_read=True, echo=True,
                   at_cmd="AT+MTVALVE=%(ep)d,1",
                   controller=("verdict", ["open"], (129, 0), 1, {"rc0": True})),
            _check(0x0081, 0x0004, "current-state", 1, at_cmd="AT+MTVALVE=%(ep)d,1",
                   controller=("verdict", ["close"], (129, 1), 0, {"rc0": True})),
        ], extra_reads=[("read-event", "valve-state-changed")]),
    ],
    # Batch 4 (catalogue batch 4 design spec section 4): the rows run in
    # registry order, endpoints 2-8 behind the anchor 0x0100 (ep 1), and
    # every new kind appears here at least once: verdict payload and
    # error_state (the trio), at_refused (the trio's Instance-served
    # write), notify (the smoke/CO self-test), setup (mode select, chime)
    # and no_forward (the trio's Pause from Stopped, the chime disabled).
    "mg24-batch4": [
        _multi("0x0011", "power source", 1, "powersource", [
            _check(47, 12, "bat-percent-remaining", 80),
        ], extra_reads=[("read", "feature-map", 2)]),
        _multi("0x0076", "smoke/CO alarm", 1, "smokecoalarm", [
            _check(92, 1, "smoke-state", 1, at_cmd="AT+MTALARM=%(ep)d,1,1"),
            _check(92, 1, "smoke-state", 0,
                   at_cmd="AT+MTALARM=%(ep)d,1,0",
                   controller=("notify", ["self-test-request"], (92, 0), None,
                               ["AT+MTALARM=%(ep)d,5,0"],
                               ("read-event", "self-test-complete"))),
        ]),
        _multi("0x0073", "laundry washer", 2, "operationalstate", [
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0"),
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0",
                   controller=("verdict", ["start"], (96, 2), 1,
                               {"rc0": True, "error_state": 0})),
            _check(96, 4, "operational-state", 1, at_cmd="AT+MTOPSTATE=%(ep)d,1",
                   controller=("verdict", ["pause"], (96, 0), 0,
                               {"rc0": True, "error_state": 2})),
            _check(96, 4, "operational-state", 1, at_cmd="AT+MTOPSTATE=%(ep)d,1",
                   controller=("verdict", ["stop"], (96, 1), None,
                               {"rc0": True, "error_state": 2})),
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0",
                   controller=("no_forward", ["pause"],
                               {"rc0": True, "error_state": 3})),
            _check(96, 4, "operational-state", 0,
                   controller=("at_refused", ["AT+MTATTR=%(ep)d,96,4,2"], 11)),
        ]),
        _multi("0x0075", "dishwasher", 2, "operationalstate", [
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0"),
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0",
                   controller=("verdict", ["start"], (96, 2), 1,
                               {"rc0": True, "error_state": 0})),
            _check(96, 4, "operational-state", 1, at_cmd="AT+MTOPSTATE=%(ep)d,1",
                   controller=("verdict", ["pause"], (96, 0), 0,
                               {"rc0": True, "error_state": 2})),
            _check(96, 4, "operational-state", 1, at_cmd="AT+MTOPSTATE=%(ep)d,1",
                   controller=("verdict", ["stop"], (96, 1), None,
                               {"rc0": True, "error_state": 2})),
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0",
                   controller=("no_forward", ["pause"],
                               {"rc0": True, "error_state": 3})),
            _check(96, 4, "operational-state", 0,
                   controller=("at_refused", ["AT+MTATTR=%(ep)d,96,4,2"], 11)),
        ]),
        _multi("0x007C", "laundry dryer", 2, "operationalstate", [
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0"),
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0",
                   controller=("verdict", ["start"], (96, 2), 1,
                               {"rc0": True, "error_state": 0})),
            _check(96, 4, "operational-state", 1, at_cmd="AT+MTOPSTATE=%(ep)d,1",
                   controller=("verdict", ["pause"], (96, 0), 0,
                               {"rc0": True, "error_state": 2})),
            _check(96, 4, "operational-state", 1, at_cmd="AT+MTOPSTATE=%(ep)d,1",
                   controller=("verdict", ["stop"], (96, 1), None,
                               {"rc0": True, "error_state": 2})),
            _check(96, 4, "operational-state", 0, at_cmd="AT+MTOPSTATE=%(ep)d,0",
                   controller=("no_forward", ["pause"],
                               {"rc0": True, "error_state": 3})),
            _check(96, 4, "operational-state", 0,
                   controller=("at_refused", ["AT+MTATTR=%(ep)d,96,4,2"], 11)),
        ]),
        _multi("0x0027", "mode select", 1, "modeselect", [
            _check(80, 3, "current-mode", 0,
                   controller=("command", ["change-to-mode", "1"],
                               (80, 3, 1))),
        ], setup=("setup", ["AT+MTMODES=%(ep)d,0,\"Quiet\",1,\"Normal\",2,\"Boost\""],
                  ("modeselect", "supported-modes",
                   ["Quiet", "Normal", "Boost"]))),
        _multi("0x0146", "chime", 1, "chime", [
            _check(1366, 1, "selected-chime", 7, at_cmd="AT+MTCHIME=%(ep)d,0,7"),
            _check(1366, 2, "enabled", 1, parse="bool", at_cmd="AT+MTCHIME=%(ep)d,1,1",
                   controller=("verdict", ["play-chime-sound"], (1366, 0), 1,
                               {"rc0": True}, 7)),
            _check(1366, 2, "enabled", 0, parse="bool", at_cmd="AT+MTCHIME=%(ep)d,1,0",
                   controller=("no_forward", ["play-chime-sound"], {"rc0": True})),
        ], setup=("setup", ["AT+MTCHIMESOUNDS=%(ep)d,7,\"Ding\""],
                  ("chime", "installed-chime-sounds", ["Ding"]))),
    ],
    # Batch 5a (catalogue batch 5a design spec section 3): the six ember-only
    # types in registry order, endpoints 2-7 behind the anchor. Existing row
    # kinds only. The pump and the room AC share one featureless OnOff list;
    # their OnOff FeatureMaps (0 and 2) prove the seed table's per-type
    # qualifier.
    "mg24-batch5a": [
        _multi("0x002D", "air purifier", 2, "fancontrol", [
            _check(0x0202, 0x0002, "percent-setting", 30,
                   controller=("write", ["write", "percent-setting", "70"],
                               (0x0202, 0x0002, 70))),
        ]),
        _type("0x010F", "mounted on/off control", 2, "onoff", 6, 0, "on-off", 1, parse="bool",
              controller=("command", ["off"], 0)),
        _type("0x0110", "mounted dimmable load control", 2, "levelcontrol", 8, 0, "current-level", 100,
              controller=("command", ["move-to-level-with-on-off", "200", "0", "0", "0"], 200)),
        _multi("0x000F", "generic switch", 3, "switch", [
            _check(0x003B, 0x0001, "current-position", 1),
            _check(0x003B, 0x0000, "number-of-positions", 2, at_cmd="AT+MTSWITCH=%(ep)d"),
            _check(0x003B, 0x0001, "current-position", 1,
                   controller=("at_refused", ["AT+MTSWITCH=%(ep)d,1"], 1)),
        ], extra_reads=[("read-event", "initial-press"), ("read", "feature-map", 2)]),
        _multi("0x0303", "pump", 3, "onoff", [
            _check(6, 0, "on-off", 1, parse="bool",
                   controller=("command", ["off"], (6, 0, 0))),
            _check(0x0200, 0x0020, "operation-mode", 0,
                   chip_cluster="pumpconfigurationandcontrol",
                   controller=("write", ["write", "operation-mode", "1"],
                               (0x0200, 0x0020, 1))),
        ], extra_reads=[("read", "feature-map", 0)]),
        _multi("0x0072", "room air conditioner", 3, "onoff", [
            _check(6, 0, "on-off", 1, parse="bool",
                   controller=("command", ["off"], (6, 0, 0))),
            _check(0x0201, 0x0012, "occupied-heating-setpoint", 2000,
                   chip_cluster="thermostat",
                   controller=("command", ["setpoint-raise-lower", "0", "10"],
                               (0x0201, 0x0012, 2100))),
        ], extra_reads=[("read", "feature-map", 2)]),
    ],
    # Batch 5b (catalogue batch 5b design spec section 4): the robotic
    # vacuum cleaner, endpoint 2 behind the anchor. ModeBase CurrentMode is
    # Instance-served, so its rows are mode_verdict (no write step);
    # RvcOperationalState uses the batch 4 kinds.
    "mg24-batch5b": [
        _multi("0x0074", "robotic vacuum cleaner", 4, "rvcrunmode", [
            _check(84, 1, "current-mode", 1,
                   controller=("mode_verdict", ["change-to-mode", "1"], (84, 0), 1,
                               {"mode_status": 0, "current_mode": 1}, 1)),
            _check(84, 1, "current-mode", 1,
                   controller=("mode_verdict", ["change-to-mode", "0"], (84, 0), 0,
                               {"mode_status": 2, "current_mode": 1}, 0)),
            _check(84, 1, "current-mode", 1,
                   controller=("mode_verdict", ["change-to-mode", "5"], (84, 0), "none",
                               {"mode_status": 1, "current_mode": 1}, None)),
            _check(85, 1, "current-mode", 1, chip_cluster="rvccleanmode",
                   controller=("mode_verdict", ["change-to-mode", "1"], (85, 0), 1,
                               {"mode_status": 0, "current_mode": 1}, 1)),
            _check(97, 4, "operational-state", 1, chip_cluster="rvcoperationalstate",
                   at_cmd="AT+MTOPSTATE=%(ep)d,1",
                   controller=("verdict", ["pause"], (97, 0), 1,
                               {"rc0": True, "error_state": 0})),
            _check(97, 4, "operational-state", 2, chip_cluster="rvcoperationalstate",
                   at_cmd="AT+MTOPSTATE=%(ep)d,2",
                   controller=("verdict", ["resume"], (97, 3), 0,
                               {"rc0": True, "error_state": 2})),
            _check(97, 4, "operational-state", 66, chip_cluster="rvcoperationalstate",
                   at_cmd="AT+MTOPSTATE=%(ep)d,0x42",
                   controller=("no_forward", ["go-home"],
                               {"rc0": True, "error_state": 3})),
            _check(97, 4, "operational-state", 0, chip_cluster="rvcoperationalstate",
                   controller=("at_refused", ["AT+MTATTR=%(ep)d,97,4,1"], 11)),
        ], setup=("setup", ["AT+MTMODES=%(ep)d,84,0,16384,\"Idle\",1,0,\"Cleaning\"",
                            "AT+MTMODES=%(ep)d,85,0,0,\"Vacuum\",1,0,\"Mop\""],
                  [("rvcrunmode", "supported-modes", ["Idle", "Cleaning"]),
                   ("rvccleanmode", "supported-modes", ["Vacuum", "Mop"])])),
    ],
}

def composition_for(batch):
    return [(1, ANCHOR_DEVTYPE)] + [(i + 2, t["devtype"])
                                     for i, t in enumerate(BATCHES[batch])]

def rows_for(batch):
    return BATCHES[batch]

def parse_bool_attr(out):
    """The last plain `<Label>: TRUE`/`FALSE` value chip-tool prints,
    mirroring parse_int_attr's discipline (mt_regression.py:5294):
    anchored on chip-tool's `]  Label: VALUE` line shape (the `]` closes
    the `[TOO]` tag, then 2-or-more spaces) so a single-space summary
    line cannot supply a false hit, and taking the LAST match so an
    earlier TRUE/FALSE on a different label cannot win. None on no
    match, never raises."""
    vals = re.findall(r"\]\s{2,}[A-Za-z_]\w*:\s*(TRUE|FALSE)\s*$", out or "", re.M)
    return None if not vals else vals[-1] == "TRUE"

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

def paired_board_ok(out):
    """The commissioning guard's test, factored for the self-tests:
    chip-tool's pairing output names the device it paired by product id,
    so the board under test is the one paired only when its own product
    id is in the output."""
    return EXPECTED_PRODUCT_ID in out

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
        setup = t.get("setup")
        if setup:
            _kind, atlines, reads = setup
            # Batch 4's form is one (cluster, attr, names) read; batch 5b's
            # is a list of them. Both print one line per read; the listed
            # form names the chip cluster in each row (both of the RVC's
            # reads are supported-modes).
            if not isinstance(reads, list):
                reads = [reads]
            for line in atlines:
                print("  ep %2d %s %s rev %d: setup %s -> OK"
                      % (ep, dt, t["name"], t["revision"], line % {"ep": ep}))
            for scc, sattr, substrs in reads:
                what = ("%s %s" % (scc, sattr)) if isinstance(setup[2], list) else sattr
                print("  ep %2d %s %s rev %d: setup chip-tool %s read %s carries %s"
                      % (ep, dt, t["name"], t["revision"], what, sattr,
                         " and ".join(substrs)))
        for i, c in enumerate(t["checks"]):
            urc = "n/a (no controller action, read-only attribute)"
            if c["controller"] is not None:
                if c["controller"][0] == "verdict":
                    # The 5-element tuple is the batch 1-3 shape, the
                    # 6th element the batch 4 payload.
                    kind, args, (fcl, fcmd), answer, want = c["controller"][:5]
                    payload = c["controller"][5] if len(c["controller"]) > 5 else None
                    # The verdict's forward and answer stand in for the
                    # URC triple: command, forward, answer or "unanswered".
                    cmd = " ".join(str(a) for a in args)
                    urc = f"{cmd} -> +MTCMD:{fcl},{fcmd} answer {answer if answer is not None else 'unanswered'}"
                    if payload is not None:
                        urc += f" payload {payload}"
                elif c["controller"][0] == "notify":
                    kind, args, (fcl, fcmd), payload, atlines, event = c["controller"]
                    cmd = " ".join(str(a) for a in args)
                    follow = " then " + " then ".join(
                        line % {"ep": ep} for line in atlines)
                    urc = (f"{cmd} -> +MTCMD:{fcl},{fcmd} notify, not answered"
                           + follow + f" then read-event {event[1]}")
                elif c["controller"][0] == "no_forward":
                    kind, args, want = c["controller"]
                    urc = "%s -> no +MTCMD within 3s" % " ".join(str(a) for a in args)
                elif c["controller"][0] == "at_refused":
                    kind, atlines, err = c["controller"]
                    urc = "host write %s -> +MTERR:%d (refused, no forward)" % (
                        atlines[0] % {"ep": ep}, err)
                elif c["controller"][0] == "mode_verdict":
                    _kind, args, (fcl, fcmd), answer, want, _payload = c["controller"]
                    cmd = " ".join(str(a) for a in args)
                    ans = "allow" if answer == 1 else "deny" if answer == 0 else "unlisted"
                    urc = "%s -> ChangeToMode status %d (%s; %s)" % (
                        cmd, want["mode_status"], ans,
                        "no +MTCMD for the command" if answer == "none"
                        else "+MTCMD:%d,%d answered %s" % (fcl, fcmd, ans))
                else:
                    kind, args, (u_cluster, u_attr, u_val) = c["controller"]
                    # The write's args already carry the "write" kind word
                    # ("write percent-setting 70"); printing the kind in front
                    # of them doubles it, so the command text stands alone.
                    cmd = " ".join(str(a) for a in args)
                    urc = f"{cmd} -> +MTATTR:{u_cluster},{u_attr},{u_val}"
            flags = "".join(
                f" {name}" for name, on in (("echo", c["echo"]),
                                            ("null-read", c["null_read"])) if on)
            # at_cmd sends the type's own AT command (AT+MTLOCK/AT+MTVALVE),
            # not the AT+MTATTR write, so the plan shows that line.
            atline = c["at_cmd"] % {"ep": ep} if c.get("at_cmd") else "AT write %s" % c["at_value"]
            print("  ep %2d %s %s rev %d: %s%s -> chip-tool %s read %s; controller %s"
                  % (ep, dt, t["name"], t["revision"], atline, flags,
                     c["chip_cluster"] or t["chip_cluster"], c["chip_attr"], urc))

class Ctx:  # the fields pairing_argv() reads
    pass

def prove_endpoint(link, chip, s, node, ep, t):
    tag = "%s %s" % (t["devtype"], t["name"])
    rc, out = chip.run(["descriptor", "read", "device-type-list", node, str(ep)], timeout=30)
    s.check("%s ep%d device-type-list carries (%d, %d)"
            % (tag, ep, int(t["devtype"], 16), t["revision"]),
            rc == 0 and (int(t["devtype"], 16), t["revision"]) in parse_device_types(out))
    # The per-attribute row name gains " <chip_attr>" only when a type has
    # more than one check: batch 1's single-check rows keep their existing
    # names byte-for-byte (including the N/A row's literal "controller write"),
    # so a batch 1 re-run's result file stays comparable. A check whose
    # controller is a verdict gains a further " <command> <answer>" suffix
    # (e.g. "lock-state lock-door allow"): several checks can share one
    # attribute (the door lock's four checks all prove LockState) and every
    # row must name the command and the adjudication it proves, while batch
    # 1's and batch 2's row names stay byte-for-byte (neither carries a
    # verdict kind).
    if t.get("setup"):
        _kind, atlines, reads = t["setup"]
        # Batch 4's form is one (cluster, attr, names) read; batch 5b's is a
        # list of them (the RVC's two mode lists). The list form names the
        # chip cluster in each row, because both reads are supported-modes.
        listed = isinstance(reads, list)
        if not listed:
            reads = [reads]
        for line in atlines:
            wcmd = line % {"ep": ep}
            res, wlines = link.command(wcmd)
            s.check("%s ep%d setup %s -> OK" % (tag, ep, wcmd), res == 0)
        for scc, sattr, substrs in reads:
            rc, out = chip.run([scc, "read", sattr, node, str(ep)], timeout=30)
            what = ("%s %s" % (scc, sattr)) if listed else sattr
            s.check("%s ep%d setup %s read carries %s"
                    % (tag, ep, what, " and ".join(substrs)),
                    rc == 0 and all(x in out for x in substrs))
    multi = len(t["checks"]) > 1
    for c in t["checks"]:
        prefix = "%s ep%d" % (tag, ep) + (" %s" % c["chip_attr"] if multi else "")
        if c["controller"] is not None:
            if c["controller"][0] == "verdict":
                kind, args, (fcl, fcmd), answer, want = c["controller"][:5]
                prefix += " %s %s" % (args[0],
                                      "allow" if answer == 1 else
                                      "deny" if answer == 0 else "unanswered")
                # Only the chime's play-chime-sound verdict carries the 6th
                # tuple element (the forwarded chimeID); the trio's verdict
                # tuples carry none, so the payload word is chime-only.
                if len(c["controller"]) > 5:
                    prefix += " payload %s" % c["controller"][5]
            elif c["controller"][0] == "notify":
                prefix += " self-test-request"
            elif c["controller"][0] == "no_forward":
                prefix += " %s no-forward" % c["controller"][1][0]
            elif c["controller"][0] == "at_refused":
                prefix += " AT write refused"
            elif c["controller"][0] == "mode_verdict":
                _kind, args, _fwd, answer, _want = c["controller"][:5]
                prefix += " %s %s %s %s" % (
                    c["chip_cluster"] or t["chip_cluster"], args[0], args[1],
                    "allow" if answer == 1 else "deny" if answer == 0 else "unlisted")
        prove_check(link, chip, s, node, ep, t, c, prefix)
    for extra in t["extra_reads"]:
        if extra[0] == "read-event":
            rc, out = chip.run([t["chip_cluster"], "read-event", extra[1], node, str(ep)], timeout=30)
            # chip-tool prints the event under its Matter name
            # (BooleanState's StateChange, the valve's ValveStateChanged);
            # the pass test is that printed name, not a cluster-specific
            # constant.
            s.check("%s ep%d %s event present" % (tag, ep, extra[1]),
                    rc == 0 and extra[1].replace("-", " ").title().replace(" ", "") in out)
        else:
            rc, out = chip.run([t["chip_cluster"], "read", extra[1], node, str(ep)], timeout=30)
            s.check("%s ep%d %s = %s" % (tag, ep, extra[1], extra[2]), rc == 0 and H.parse_int_attr(out) == extra[2])

def _second_at_read(link, s, prefix, at, own):
    res, lines = link.command("AT+MTATTR=" + at)
    s.check("%s second AT read agrees (%s)" % (prefix, own),
            res == 0 and lines == ["+MTATTR:%s,%s" % (at, own)])

def prove_check(link, chip, s, node, ep, t, c, prefix):
    cc = c["chip_cluster"] or t["chip_cluster"]
    at = "%d,%d,%d" % (ep, c["cluster"], c["attr"])
    if c["controller"] is not None and c["controller"][0] == "at_refused":
        # The trio's Instance-served OperationalState row: AT_MT_SPEC 3.21
        # and 3.8's DE270 rule -- the attribute exists but is served by the
        # cluster's own Instance, so the AT write is refused with the given
        # +MTERR code; there is no write step before it, no forward, no read
        # and no second read.
        _kind, atlines, err = c["controller"]
        link.drain(0.2)
        wcmd = atlines[0] % {"ep": ep}
        res, wlines = link.command(wcmd)
        s.check("%s %s -> +MTERR:%d" % (prefix, wcmd, err), res == err)
        return
    if c["controller"] is not None and c["controller"][0] == "mode_verdict":
        # ModeBase CurrentMode is Instance-served (an AT write is refused
        # +MTERR:11), so there is no write step: the controller's
        # ChangeToMode is the only way the mode moves (AT_MT_SPEC 3.20.1).
        # The SDK answers an unlisted mode itself (Status 1) before any
        # forward; a listed one forwards the mode as payload and the host's
        # verdict lands in the ChangeToModeResponse Status (0 allow, 2 deny).
        _kind, args, (fcl, fcmd), answer, want, payload = c["controller"]
        cc = c["chip_cluster"] or t["chip_cluster"]
        link.drain(0.2)
        ctx = types.SimpleNamespace(chip=chip, chip_call=None)
        # ChangeToMode's NewMode is a positional command argument, so it
        # precedes the destination, as the harness's own step 3.15 passes it.
        handle = H.invoke_chip(ctx, [cc] + args + [node, str(ep)], timeout=30)
        rc, out = handle.join(30)
        if answer == "none":
            # The response has landed, so any forward this command raised
            # is already on the link: the check is a short window for a
            # +MTCMD for this endpoint and cluster (the queue is checked
            # before the wire, so an arrival the join already carried is
            # seen), not a race against chip-tool's own exit.
            fwd = link.await_urc(r"^\+MTCMD:\d+,%d,%d,\d+(,|$)" % (ep, fcl), 1.0)
            s.check("%s no +MTCMD for the command" % prefix, fwd is None)
        else:
            responder = H.CmdResponder(link)
            fwd = responder.expect(cluster=fcl, command=fcmd, verdict=answer,
                                   payload=payload, timeout=5.0)
            s.check("%s forward %d/%d answered %s" % (prefix, fcl, fcmd,
                                                     "allow" if answer else "deny"),
                    fwd is not None)
        s.check("%s ChangeToMode status %d" % (prefix, want["mode_status"]),
                rc == 0 and H.parse_change_to_mode_status(out) == want["mode_status"])
        if "current_mode" in want:
            rc, out = chip.run([cc, "read", "current-mode", node, str(ep)], timeout=30)
            s.check("%s current-mode = %d after" % (prefix, want["current_mode"]),
                    rc == 0 and H.parse_int_attr(out) == want["current_mode"])
        return
    if c["null_read"]:
        # AT_MT_SPEC 3.8's null rule: reading an attribute that has never
        # been set answers +MTERR:5. ATLink._collect() consumes the
        # +MTERR:<n> line and returns n as the result code, so the error
        # line is NOT in `lines` and the check is res == 5.
        res, lines = link.command("AT+MTATTR=" + at)
        s.check("%s AT read of the null seed -> +MTERR:5" % prefix, res == 5)
    # The write step: at_cmd, when set, sends the type's own AT command
    # (AT+MTLOCK/AT+MTVALVE) instead of the AT+MTATTR write; the row name
    # says the command.
    #
    # Drain before the write, in both forms: the AT layer may have queued a
    # stale +MTATTR for the same endpoint, cluster and attribute (a URC from
    # an earlier check), and the echo row below must prove that THIS write
    # produced its own +MTATTR. A stale URC would let a silent write pass,
    # so the queue has to be empty when the write goes out.
    link.drain(0.2)
    if c.get("at_cmd"):
        wcmd = c["at_cmd"] % {"ep": ep}
        res, wlines = link.command(wcmd)
        s.check("%s %s -> OK" % (prefix, wcmd), res == 0)
    else:
        res, wlines = link.command("AT+MTATTR=%s,%s" % (at, c["at_value"]))
        s.check("%s AT write %s -> OK" % (prefix, c["at_value"]), res == 0)
    if c["echo"]:
        # DE531: a local write must raise its own +MTATTR echo.
        # AT_MT_SPEC 3.8: the echo arrives BEFORE the write's OK.
        # ATLink._derive_expect gives an AT+MTATTR write the +MTATTR:
        # expect prefix, and _collect appends any line starting with
        # the expect prefix to the command's lines, so the echo
        # normally lands in the write's own returned lines and never
        # in the URC queue; the await_urc below is the wait for the
        # late case.
        echo_line = "+MTATTR:%s,%s" % (at, c["at_value"])
        got = (echo_line in wlines) or \
              link.await_urc(r"\+MTATTR:%s,%s$" % (at, c["at_value"]), 3.0)
        s.check("%s AT write echoes +MTATTR:%s,%s" % (prefix, at, c["at_value"]), got is not None)
    else:
        link.drain(0.3)   # without echo=True the write's own +MTATTR is not this row's
    rc, out = chip.run([cc, "read", c["chip_attr"], node, str(ep)], timeout=30)
    if c["parse"] == "bool":
        got, want = parse_bool_attr(out), bool(c["at_value"])
    else:
        got, want = H.parse_int_attr(out), c["at_value"]
    s.check("%s controller reads %s = %s after the AT write" % (prefix, c["chip_attr"], want),
            rc == 0 and got == want)
    own = c["at_value"]
    if c["controller"] is None:
        s.not_applicable("%s controller write" % prefix, "no controller action (read-only attribute)")
        _second_at_read(link, s, prefix, at, own)
    elif c["controller"][0] == "notify":
        _kind, args, (fcl, fcmd), payload, atlines, event = c["controller"]
        link.drain(0.2)
        ctx = types.SimpleNamespace(chip=chip, chip_call=None)
        handle = H.invoke_chip(ctx, [cc] + args[:1] + [node, str(ep)] + args[1:], timeout=30)
        responder = H.CmdResponder(link)
        # Seq 0 is notify-only: expect_notify asserts the forward and never
        # sends AT+MTCMDRESP; the host's own completion line(s) below are
        # the answer the firmware expects (AT_MT_SPEC 3.22).
        fwd = responder.expect_notify(cluster=fcl, command=fcmd,
                                      payload=payload, timeout=5.0)
        s.check("%s forward %d/%d is seq 0, not answered" % (prefix, fcl, fcmd),
                fwd is not None)
        for line in atlines:
            wcmd = line % {"ep": ep}
            res, wlines = link.command(wcmd)
            s.check("%s %s -> OK" % (prefix, wcmd), res == 0)
        if event[0] == "read-event":
            rc, out = chip.run([cc, "read-event", event[1], node, str(ep)], timeout=30)
            # Same printed-name rule as the type's own extra_reads rows:
            # chip-tool prints SelfTestComplete, the pass test is that
            # name, not a cluster-specific constant.
            s.check("%s %s event present" % (prefix, event[1]),
                    rc == 0 and event[1].replace("-", " ").title().replace(" ", "") in out)
        else:
            rc, out = chip.run([cc, "read", event[1], node, str(ep)], timeout=30)
            s.check("%s %s = %s after the notify" % (prefix, event[1], event[2]),
                    rc == 0 and H.parse_int_attr(out) == event[2])
        rc, out = handle.join(30)
        s.check("%s chip-tool %s exits 0" % (prefix, " ".join(args)), rc == 0)
    elif c["controller"][0] == "no_forward":
        # The SDK short-circuits before the delegate: no +MTCMD for this
        # cluster's commands may arrive within 3s (the chime with Enabled
        # false, the trio's Pause from Stopped, refused server-side). One
        # 3 s window, one await: the pattern names the endpoint and the
        # cluster but not the command, so a forward on any command of the
        # cluster fails this row, including one this row does not itself
        # send. (The earlier shape awaited each command id for 0.75 s in
        # turn, so a forward arriving after its own slice sat in the queue
        # unseen and the row passed: the final review's I1.)
        _kind, args, want = c["controller"]
        link.drain(0.2)
        ctx = types.SimpleNamespace(chip=chip, chip_call=None)
        handle = H.invoke_chip(ctx, [cc] + args[:1] + [node, str(ep)] + args[1:], timeout=30)
        fwd = link.await_urc(r"^\+MTCMD:\d+,%d,%d,\d+(,|$)" % (ep, c["cluster"]), 3.0)
        s.check("%s no +MTCMD within 3s" % prefix, fwd is None)
        rc, out = handle.join(30)
        s.check("%s chip-tool %s" % (prefix, "exits 0" if want["rc0"] else "fails"),
                (rc == 0) == want["rc0"])
        if "status" in want:
            s.check("%s wire status 0x%X" % (prefix, want["status"]),
                    H.parse_status(out) == want["status"])
        if "error_state" in want:
            s.check("%s ErrorStateID %d" % (prefix, want["error_state"]),
                    H.parse_status(out) == want["error_state"])
    elif c["controller"][0] == "verdict":
        # Ordering rule for verdict checks: an allowed command may move the
        # attribute (an opened valve's CurrentState to Transitioning or Open,
        # a locked door's LockState to Locked), and the host, not the
        # controller, reports actuation. The "second AT read agrees" row
        # runs BEFORE the controller action, and the attribute after the
        # action is deliberately not asserted: nothing here reads it back,
        # and the host, not this proof, owns what the actuation became.
        _second_at_read(link, s, prefix, at, own)
        kind, args, (fcl, fcmd), answer, want = c["controller"][:5]
        # The 6th tuple element (batch 4) is the forwarded payload the
        # forward must carry (the chime's PlayChimeSound 7); None keeps the
        # batch 1-3 shape's no-payload-filter behaviour of expect().
        payload = c["controller"][5] if len(c["controller"]) > 5 else None
        ctx = types.SimpleNamespace(chip=chip, chip_call=None)
        # The argv shape puts the command name first, then node and
        # endpoint, then the flags: chip-tool's --help usage lines read
        # "<cluster> <command> destination-id endpoint-id ... [flags]".
        handle = H.invoke_chip(ctx, [cc] + args[:1] + [node, str(ep)] + args[1:], timeout=30)
        responder = H.CmdResponder(link)
        if answer is None:
            # Seen, deliberately not answered: the firmware must time out
            # the interaction and raise +MTCMDTO on its own seq.
            fwd = responder._match(fcl, fcmd, None, 5.0)
            s.check("%s forward %d/%d seen" % (prefix, fcl, fcmd), fwd is not None)
            to = fwd and link.await_urc(r"\+MTCMDTO:%d$" % fwd["seq"], 3.0)
            s.check("%s unanswered -> +MTCMDTO" % prefix, bool(to))
        else:
            fwd = responder.expect(cluster=fcl, command=fcmd, verdict=answer,
                                   payload=payload, timeout=5.0)
            s.check("%s forward %d/%d answered %s" % (prefix, fcl, fcmd,
                                                     "allow" if answer else "deny"),
                    fwd is not None)
        rc, out = handle.join(30)
        s.check("%s chip-tool %s" % (prefix, "exits 0" if want["rc0"] else "fails"),
                (rc == 0) == want["rc0"])
        if "status" in want:
            s.check("%s wire status 0x%X" % (prefix, want["status"]),
                    H.parse_status(out) == want["status"])
        # error_state reads the same response as status (H.parse_status
        # answers both the `status = 0xNN` and the `ErrorStateID: N` wire
        # shapes, last match wins) but the row name says ErrorStateID,
        # because the trio's allow/deny/refusal all exit 0 and only the
        # ErrorStateID separates them.
        if "error_state" in want:
            s.check("%s ErrorStateID %d" % (prefix, want["error_state"]),
                    H.parse_status(out) == want["error_state"])
    else:
        kind, args, (ucl, uat, uval) = c["controller"]
        link.drain(0.2)
        rc, out = chip.run([cc] + args + [node, str(ep)], timeout=30)
        s.check("%s controller %s exits 0" % (prefix, " ".join(args)), rc == 0)
        got = link.await_urc(r"\+MTATTR:%d,%d,%d,%d$" % (ep, ucl, uat, uval), 10.0)
        s.check("%s +MTATTR:%d,%d,%d,%d on the AT link" % (prefix, ep, ucl, uat, uval), got is not None)
        rc, out = chip.run([cc, "read", c["urc_chip_attr"] or c["chip_attr"], node, str(ep)], timeout=30)
        if c["parse"] == "bool":
            ok = parse_bool_attr(out) == bool(uval)
        else:
            ok = H.parse_int_attr(out) == uval
        s.check("%s controller reads back %d" % (prefix, uval), rc == 0 and ok)
        if (ucl, uat) == (c["cluster"], c["attr"]):
            own = uval
        else:
            res, lines = link.command("AT+MTATTR=%d,%d,%d" % (ep, ucl, uat))
            s.check("%s AT read of %d/%d agrees (%s)" % (prefix, ucl, uat, uval),
                    res == 0 and lines == ["+MTATTR:%d,%d,%d,%s" % (ep, ucl, uat, uval)])
        _second_at_read(link, s, prefix, at, own)

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
        if paired:
            paired = s.check("commission: paired the board under test (%s)" % EXPECTED_PRODUCT_ID,
                             paired_board_ok(out))
            if not paired:
                print("ABORT: chip-tool paired another board (no %s in its output)"
                      % EXPECTED_PRODUCT_ID)
        if not paired:
            print(H._pairing_tail(out, getattr(args, "psk", None)))
    else:
        print("ABORT: cannot capture onboarding codes, skipping commissioning")

    node = "0x%X" % args.node_id
    try:
        if paired:
            for (ep, _), t in zip(comp[1:], rows_for(args.batch)):
                prove_endpoint(link, chip, s, node, ep, t)
        else:
            print("ABORT: pairing failed, skipping the endpoint proofs")
    finally:
        # restore: factory-reset back to the bench's documented start
        # state, in a finally so a serial drop (or any other exception)
        # mid-proof still runs it: without this, an exception out of the
        # loop above would skip straight to the interpreter and leave the
        # bench commissioned with all thirteen catalogue endpoints instead
        # of the documented 0x0100,0x0302.
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
