# C5 Regression Harness T4 (Thread-image Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the existing Phase 2 chain (2.1-2.12) against the Thread image
by parameterizing the gate and the pairing verb by transport, and record
`test/baselines/thread-lifecycle.json`.

**Architecture:** Approach A per the design spec
(`docs/superpowers/specs/2026-07-31-c5-regression-harness-t4-design.md`):
`phase2_gate` gains the link, detects the transport from `AT+MTNET?`, and
branches its preconditions (WiFi credentials vs OTBR liveness plus a live
`ot-ctl` dataset). Steps 2.3 and 2.11 build their pairing argv through one
transport-keyed helper. Everything else in the chain is untouched.

**Tech Stack:** Python 3 stdlib + pyserial. chip-tool and ot-ctl as
subprocesses. No new dependencies.

## Global Constraints

- **No em dashes** anywhere. Colon, comma, parentheses or a full stop.
- **No new Python dependencies.**
- **MT_PSK never appears** in any file, fixture, baseline, report or log
  excerpt copied into a report. The Thread dataset is bench-local and MAY
  appear in run logs, but is NOT recorded in any committed file (fixture
  excepted: the committed ot-ctl fixture is a REAL dataset of the bench
  network, which is acceptable because that network is a lab fixture; if
  the operator objects during Task 1, regenerate the network afterwards).
- Hardware is touched ONLY in Tasks 1 and 5. Tasks 2-4 must keep
  `cd test && python3 test_mt_regression.py` green with no device attached
  (123 tests at plan time).
- Commit messages explain why and end with exactly:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

- Harness file `test/mt_regression.py`, self-tests
  `test/test_mt_regression.py`, 4-space indent, match the file's voice.
- Bench commands use the by-id port path. The OTBR bring-up gotchas are
  graph F36: run `otbr-agent` directly (never `script/setup`), and the
  D-Bus policy file must be installed or the agent aborts obscurely.
- Thread build commands per CLAUDE.md: the SDKCONFIG redirect is mandatory
  (`-D SDKCONFIG=build_thread/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.thread"`)
  or the Thread config silently takes over the WiFi build.

---

### Task 1: Bench preflight: pin the four unknowns, flash the Thread image

Bench-facing. Record everything verbatim in
`.superpowers/sdd/2026-07-31-c5-regression-harness-t4/task-1-findings.md`.
The bench starts on the WiFi image, factory-fresh with the composition.

**Files:**
- Create: `test/fixtures/otctl_dataset_active.txt` (real capture)
- Create: the findings file above

**Interfaces:**
- Produces: four pinned facts Tasks 2-5 transcribe: (a) the exact ot-ctl
  invocation that works (path, sudo or not), (b) whether the Thread
  network survives an otbr-agent restart or the re-form one-liner,
  (c) the `ble-thread` pairing argv shape on this chip-tool binary,
  (d) the mismatch-boot sequence of `build_thread` over WiFi-era NVS and
  that `AT+MTRESET` normalizes it. Plus the committed fixture.

- [ ] **Step 1: OTBR bring-up and the ot-ctl invocation**

Locate the binaries in `/mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/src/git/ot-br-posix/build/otbr/`
(`src/agent/otbr-agent`, and `ot-ctl` under
`third_party/openthread/repo/src/posix/`). Verify the D-Bus policy file is
installed (`/usr/share/dbus-1/system.d/otbr-agent.conf` or equivalent; if
missing, copy `src/agent/otbr-agent.conf` there with sudo, per F36). Start
the agent against the ZBT-2:

```bash
ZBT=$(ls /dev/serial/by-id/usb-Nabu_Casa_ZBT-2_*-if00)
sudo <otbr-agent-path> -I wpan0 -B lo "spinel+hdlc+uart://$ZBT?uart-baudrate=460800" &
```

(The exact radio URL and baud were used 2026-07-29; if the agent rejects
them, check `ps` history or try 115200 and record what works.) Then pin the
ot-ctl invocation: try `<ot-ctl-path> state` as the normal user, then with
sudo; record which answers. Record `state` (expect leader/router after the
dataset restores) and whether the July network came back on its own
(finding (b)); if `state` is disabled/detached, re-form:
`dataset init new`, `dataset commit active`, `ifconfig up`, `thread start`,
and record that as the re-form procedure.

- [ ] **Step 2: capture the dataset fixture**

```bash
<pinned-ot-ctl> dataset active -x | tee /tmp/dataset.txt
cp /tmp/dataset.txt test/fixtures/otctl_dataset_active.txt
git add test/fixtures/otctl_dataset_active.txt
git commit -m "test: capture real ot-ctl dataset output for the T4 parser

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc"
```

Record the output shape (the hex line, any `Done` trailer, prompt echoes):
finding (a)'s fixture and the parser's ground truth.

- [ ] **Step 3: ble-thread argv sanity**

```bash
BIN=~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool
$BIN pairing ble-thread --help 2>&1 | head -20
```

Record the positional signature (expected
`ble-thread node-id operational-dataset setup-pin-code discriminator`,
dataset as `hex:<...>`): finding (c).

- [ ] **Step 4: flash the Thread image and observe the mismatch boot**

```bash
source ~/esp/esp-idf-v5.4.1/export.sh && source ~/esp/esp-matter/export.sh
idf.py -B build_thread -D SDKCONFIG=build_thread/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.thread" build
BYID=$(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)
python3 fw/flash.py --build-dir build_thread --port "$BYID" --bridge espnow
```

Then with a probe script (reuse `from mt_regression import ATLink,
cmd_retry`): the bench WiFi image was factory-fresh (fabric 0), so a
mismatch (`fabric_count > 0 && !provisioned`) may not arise at all; record
what `AT+MTNET?` actually reports on first boot (expect
`+MTNET:THREAD,...`), whether any `+MTEVT:27` appears, then `AT+MTRESET`
and confirm the normalized factory-fresh state (`+MTFABRICS:0`,
`+MTSTATE:1,0`, composition intact via `AT+MTEP?`): finding (d). Leave the
bench ON the Thread image, factory-fresh, and note that deviation from the
bench convention in the findings (Task 5 restores WiFi at the end).

- [ ] **Step 5: write the findings file** with the four one-sentence
conclusions and the OTBR bring-up runbook (including whether the agent is
left running for Task 5; recommended: leave it running).

### Task 2: OtCtl runner and the dataset parser

**Files:**
- Modify: `test/mt_regression.py` (after the ChipTool section)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: Task 1 findings (a) and the committed fixture.
- Produces: `DEFAULT_OTCTL` (module constant, from the findings; the
  `MT_OTCTL` env override applied in main's argparse);
  `otctl_run(args, binary, runner=None, timeout=10) -> (rc, out)` with an
  injectable runner exactly like `ChipTool._default_runner` (TimeoutExpired
  converted to a nonzero rc, same rationale);
  `parse_dataset(text) -> str|None` returning the bare hex string.

- [ ] **Step 1: failing tests**

```python
class TestOtCtl(unittest.TestCase):
    def test_run_argv_and_result(self):
        calls = []
        def runner(argv, timeout):
            calls.append((argv, timeout))
            return 0, "hex\nDone\n"
        rc, out = otctl_run(["dataset", "active", "-x"], "/x/ot-ctl",
                            runner=runner)
        self.assertEqual(rc, 0)
        self.assertEqual(calls[0][0], ["/x/ot-ctl", "dataset", "active", "-x"])

    def test_parse_dataset_fixture(self):
        text = fixture("otctl_dataset_active.txt")
        ds = parse_dataset(text)
        self.assertIsNotNone(ds)
        self.assertRegex(ds, r"^[0-9a-fA-F]{16,}$")

    def test_parse_dataset_garbage_is_none(self):
        self.assertIsNone(parse_dataset("Error 35: InvalidState\nDone\n"))
```

Tighten `test_parse_dataset_fixture` to the literal hex the fixture holds.
If the findings pinned a sudo prefix, `otctl_run` takes the FULL argv list
from `DEFAULT_OTCTL` being a list, not a string; adapt the constant's type
to the findings and keep the tests matching.

- [ ] **Step 2: red run** (`cd test && python3 test_mt_regression.py TestOtCtl`)

- [ ] **Step 3: implement**

```python
def otctl_run(args, binary, runner=None, timeout=10):
    """One-shot ot-ctl invocation with the ChipTool runner contract:
    a hung ot-ctl becomes a nonzero rc, not a raw TimeoutExpired."""
    argv = (list(binary) if isinstance(binary, (list, tuple))
            else [binary]) + list(args)
    runner = runner or ChipTool._default_runner
    return runner(argv, timeout)


def parse_dataset(text):
    """The active dataset hex from `ot-ctl dataset active -x` output:
    the first line that is entirely hex and plausibly long. Validated
    against the Task 1 fixture."""
    for line in (text or "").splitlines():
        line = line.strip().lstrip("> ")
        if re.fullmatch(r"[0-9a-fA-F]{16,}", line):
            return line
    return None
```

(`DEFAULT_OTCTL` from the findings; reuse `ChipTool._default_runner` as
shown so the timeout conversion is shared, or extract a module-level
`_subprocess_runner` if the reviewer of Task 2 prefers; either is
acceptable as long as both call sites share one implementation.)

- [ ] **Step 4: green, full suite, commit**
(`test: ot-ctl runner and dataset parser for the Thread gate` + why-body +
trailers; only the two test/ files.)

### Task 3: Transport detection and the gate branches

**Files:**
- Modify: `test/mt_regression.py` (phase2_gate signature and body; main's
  ordering; argparse; Phase2Context)
- Modify: `test/test_mt_regression.py`

**Interfaces:**
- Consumes: `otctl_run`, `parse_dataset`, existing gate checks.
- Produces: `phase2_gate(chip, args, link) -> (problem, transport, dataset)`
  where problem is None on success; `Phase2Context.transport` (default
  "WIFI") and `.dataset` (default None); argparse `--transport`
  (choices WIFI/THREAD, default None = detect), `--dataset`
  (default `MT_DATASET` env), `--ot-ctl` (default `MT_OTCTL` env or
  `DEFAULT_OTCTL`); main reordered: open port, phase0, THEN phase2_gate
  with the link, then run_phase2 with ctx.transport/ctx.dataset set.
  Exit code 2 on gate abort, unchanged.

- [ ] **Step 1: failing tests.** Gate tests currently monkeypatch
`shutil.which` and build args namespaces; read them first, then add:

```python
class TestPhase2GateTransport(unittest.TestCase):
    # helpers: a FakeLink whose AT+MTNET? answers the chosen transport,
    # an args namespace, a ChipTool with the usual fake runner, and a
    # fake otctl runner. Follow the existing TestPhase2Gate plumbing.

    def test_wifi_detected_requires_credentials(self):
        # AT+MTNET? -> +MTNET:WIFI,0,0,0 ; no ssid/psk -> problem names
        # MT_SSID; transport returned "WIFI"

    def test_thread_detected_skips_credentials_requires_otbr(self):
        # AT+MTNET? -> +MTNET:THREAD,0,0,0 ; ssid/psk absent is FINE;
        # fake otctl: state -> "router\nDone", dataset -> fixture text;
        # problem None, transport "THREAD", dataset equals the fixture hex

    def test_thread_dead_otbr_aborts(self):
        # otctl state rc != 0 -> problem mentions otbr-agent and the
        # bring-up remedy

    def test_thread_bad_role_aborts(self):
        # state -> "disabled\nDone" -> problem mentions the Thread
        # network being down

    def test_dataset_override_skips_otctl_fetch(self):
        # args.dataset set -> no otctl dataset call, returned as-is

    def test_transport_override_beats_detection(self):
        # args.transport = "THREAD" with a link answering WIFI -> THREAD
        # branch runs (and the mismatch is the operator's business)
```

- [ ] **Step 2: red run**

- [ ] **Step 3: implement the gate**

```python
def phase2_gate(chip, args, link):
    """Phase-2-only preflight, before anything destructive. Detects the
    transport from the device and branches: WiFi needs credentials,
    Thread needs a live border router and its active dataset (design
    spec T4 section 2). Returns (problem, transport, dataset)."""
    if shutil.which("openocd") is None:
        return ("openocd not on PATH: the 2.8 warm reboot resets the "
                "RP2350 over SWD (see graph N22)", None, None)
    transport = getattr(args, "transport", None)
    if transport is None:
        res, lines = cmd_retry(link, "AT+MTNET?")
        m = re.match(r"\+MTNET:(WIFI|THREAD),", lines[0]) \
            if res == 0 and lines else None
        if not m:
            return ("cannot detect the transport: AT+MTNET? gave %r"
                    % (lines,), None, None)
        transport = m.group(1)
    dataset = None
    if transport == "THREAD":
        rc, out = otctl_run(["state"], args.ot_ctl)
        if rc != 0:
            return ("otbr-agent is not answering (ot-ctl state failed): "
                    "start it per the T4 runbook (graph F36: run "
                    "otbr-agent directly, D-Bus policy required)",
                    transport, None)
        role = (out or "").strip().splitlines()[0].strip().lstrip("> ")
        if role not in ("leader", "router", "child"):
            return ("the Thread network is down (ot-ctl state: %s); "
                    "bring it up before a Thread phase 2 run" % role,
                    transport, None)
        dataset = getattr(args, "dataset", None)
        if not dataset:
            rc, out = otctl_run(["dataset", "active", "-x"], args.ot_ctl)
            dataset = parse_dataset(out) if rc == 0 else None
        if not dataset:
            return ("no usable Thread dataset (ot-ctl dataset active -x "
                    "gave nothing; set MT_DATASET to override)",
                    transport, None)
    else:
        if not getattr(args, "ssid", None) or not getattr(args, "psk", None):
            return ("phase 2 needs WiFi credentials: export MT_SSID and "
                    "MT_PSK (or pass --ssid/--psk)", transport, None)
    if not (os.path.isfile(chip.binary)
            and os.access(chip.binary, os.X_OK)):
        return ("chip-tool not executable: %s (set MT_CHIPTOOL or "
                "--chip-tool)" % chip.binary, transport, dataset)
    try:
        rc, out = chip.run(
            ["payload", "parse-setup-payload", GATE_REFERENCE_QR],
            timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        return ("chip-tool failed to run: %s" % exc, transport, dataset)
    if rc != 0 or parse_setup_payload(out) is None:
        return ("chip-tool cannot parse the reference setup payload; "
                "check it runs as this user (root-owned /tmp/chip_*.ini "
                "is the known cause)", transport, dataset)
    return None, transport, dataset
```

Main reorder: move the `serial.Serial` open and `link = ATLink(port)`
ABOVE the phase-2 block; run `phase0` first (inside the try, as now), then:

```python
        if args.phase == 2:
            problem, transport, dataset = phase2_gate(chip, args, link)
            if problem:
                print("ABORT: " + problem)
                return 2
            header["node_id"] = "0x%X" % args.node_id
            chip.wipe_storage()
            ctx = Phase2Context(link, chip, suite, args)
            ctx.transport = transport
            ctx.dataset = dataset
            ...
```

(The existing chip2 wiring, relink wiring and run_phase2 call follow
unchanged. The `chip = ChipTool(...)` construction stays before the try;
only the gate call moves. Phase 0's early `return 2` path is inside the
try and the guarded finally still closes the link: verify by reading
main once before editing.)

Argparse additions:

```python
    ap.add_argument("--transport", choices=["WIFI", "THREAD"], default=None,
                    help="override transport detection (default: ask the "
                         "device via AT+MTNET?)")
    ap.add_argument("--dataset", default=os.environ.get("MT_DATASET"),
                    help="Thread operational dataset hex (default: fetch "
                         "from ot-ctl at the gate)")
    ap.add_argument("--ot-ctl", dest="ot_ctl",
                    default=os.environ.get("MT_OTCTL", DEFAULT_OTCTL))
```

`Phase2Context.__init__` gains `self.transport = "WIFI"` and
`self.dataset = None`.

- [ ] **Step 4: update existing gate tests** (they call the two-argument
form; give them a FakeLink answering WIFI and unpack the tuple), run the
full suite green.

- [ ] **Step 5: commit** (`test: phase 2 gate detects the transport and
checks the border router` + why-body + trailers).

### Task 4: The pairing argv helper, 2.3/2.11 rewiring, and the docs

**Files:**
- Modify: `test/mt_regression.py` (pairing_argv helper; step_2_3_commission;
  step_2_11_two_resets)
- Modify: `test/test_mt_regression.py`
- Modify: `docs/TESTING.md`

**Interfaces:**
- Consumes: `ctx.transport`, `ctx.dataset`, `ctx.passcode`,
  `ctx.discriminator`.
- Produces: `pairing_argv(ctx) -> list` used by both steps.

- [ ] **Step 1: failing tests**

```python
class TestPairingArgv(unittest.TestCase):
    def _ctx(self, transport, dataset=None):
        ctx = fresh_ctx()
        ctx.transport = transport
        ctx.dataset = dataset
        ctx.passcode = 20202021
        ctx.discriminator = 3840
        return ctx

    def test_wifi_argv(self):
        argv = pairing_argv(self._ctx("WIFI"))
        self.assertEqual(argv[:2], ["pairing", "ble-wifi"])
        self.assertIn("0x4845", argv)

    def test_thread_argv(self):
        argv = pairing_argv(self._ctx("THREAD", dataset="0e08abc123"))
        self.assertEqual(argv[:2], ["pairing", "ble-thread"])
        self.assertIn("hex:0e08abc123", argv)
        self.assertNotIn(None, argv)
```

Plus: TestStep23 and TestStep211 gain one THREAD-transport case each
(ctx.transport = "THREAD", ctx.dataset set; assert the runner's pairing
argv used ble-thread; all checks still pass with the same URC releases).
Adapt to those classes' existing plumbing; the WIFI defaults in fresh_ctx
keep every existing test unchanged.

- [ ] **Step 2: red run**

- [ ] **Step 3: implement**

```python
def pairing_argv(ctx):
    """chip-tool pairing argv for the detected transport. ble-thread's
    shape was pinned by the T4 Task 1 preflight; adjust here if the
    findings differ, not at the call sites."""
    node = "0x%X" % ctx.node_id
    if ctx.transport == "THREAD":
        return ["pairing", "ble-thread", node, "hex:" + ctx.dataset,
                str(ctx.passcode), str(ctx.discriminator)]
    return ["pairing", "ble-wifi", node, ctx.opts.ssid, ctx.opts.psk,
            str(ctx.passcode), str(ctx.discriminator)]
```

In `step_2_3_commission`: set `ctx.passcode, ctx.discriminator` (already
done) BEFORE the pairing call, then replace the inline ble-wifi argv with
`chip.run(pairing_argv(ctx), timeout=120)`. Same replacement in
`step_2_11_two_resets`. Nothing else in either step changes.

- [ ] **Step 4: TESTING.md ride-alongs.** Section 7 preamble: a short
paragraph on transport parameterization (detection via AT+MTNET, the
Thread gate preconditions, the pairing verb difference, the second
baseline file). Section 9: the Thread phase 2 run command next to the WiFi
one (`--phase 2 --include-slow --include-manual` on the by-id port with
otbr up; no env credentials needed on Thread).

- [ ] **Step 5: green, full suite, commit** (single commit, four files,
why-body + trailers).

### Task 5: Hardware verification and the Thread lifecycle baseline

Bench-facing. The bench is on the Thread image, factory-fresh (Task 1 left
it so); OTBR should be running per the Task 1 runbook (restart it per the
findings if not). Operator present for the 2.9 power cycles.

**Files:**
- Create: `test/baselines/thread-lifecycle.json`

- [ ] **Step 1: self-tests green; phase 0 against the Thread image**
(`python3 -u test/mt_regression.py --port $BYID --phase 0`; the Thread
image needs its boot settling time, graph F37: one retry allowed).

- [ ] **Step 2: two full runs, byte-identical**

```bash
BYID=$(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00)
python3 -u test/mt_regression.py --port "$BYID" --phase 2 \
  --include-slow --include-manual 2>&1 | tee /tmp/t4-run1.log
python3 -u test/mt_regression.py --port "$BYID" --phase 2 \
  --include-slow --include-manual 2>&1 | tee /tmp/t4-run2.log
diff <(grep -E "\[(PASS|FAIL|SKIP)\]" /tmp/t4-run1.log) \
     <(grep -E "\[(PASS|FAIL|SKIP)\]" /tmp/t4-run2.log)
```

Expected: 89 passed, 0 failed, both exit 0, empty diff. No MT_SSID/MT_PSK
needed. Run commands in the background per the 600 s tool cap, as in T3.
A failed check that is not clearly commissioning-flake class: STOP,
gather evidence, report BLOCKED (the coordinator rules on flakes).

- [ ] **Step 3: third run records the baseline**

```bash
python3 -u test/mt_regression.py --port "$BYID" --phase 2 \
  --include-slow --include-manual \
  --baseline test/baselines/thread-lifecycle.json 2>&1 | tee /tmp/t4-run3.log
python3 -c "import json, collections; d=json.load(open('test/baselines/thread-lifecycle.json')); print(d['header']['transport'], d['header']['node_id'], len(d['results']), collections.Counter(d['results'].values()))"
```

Expected: transport THREAD, 89 entries, all PASS, `ssid` null in the
header, no dataset anywhere in the file.

- [ ] **Step 4: default run proves the gates on Thread** (no flags: exit 0,
2 gated) and the WiFi-credentials absence stays a non-issue.

- [ ] **Step 5: restore the bench convention**

```bash
python3 fw/flash.py --build-dir build_b4 --port "$BYID" --bridge espnow
# probe: AT+MTNET? reports WIFI; AT+MTRESET if needed so the bench is
# factory-fresh; AT+MTEP? shows the composition; stop otbr-agent.
```

- [ ] **Step 6: commit the baseline** (`test: Thread lifecycle baseline
from three clean phase 2 runs` + why-body recording the runs and the
restored bench + trailers), and document the bench end state in the task
report (WiFi image, factory-fresh, composition, storages wiped, OTBR
stopped, port).
