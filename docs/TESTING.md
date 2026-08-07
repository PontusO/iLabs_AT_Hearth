# iLabs AT Hearth: regression test plan

Status: **plan** (the harness described here is not written yet; see §10 for the
build-out order). This is the Matter counterpart to the ESP-NOW firmware's
**RegressionSuite** (`iLabs_ESP-NOW/examples/RegressionSuite` + rig notes in
`extras/REGRESSION.md`). Same idea, same report format, same "run it after every
change" discipline. The differences come entirely from the fact that the partner
on the far side is a Matter controller on a PC, not a second Challenger board.

## 1. What this pins, and why it looks different from ESP-NOW

The ESP-NOW suite is a **two-board self-test**: the same Arduino sketch is
flashed to two Challengers, the lower MAC becomes TESTER, and the whole thing
runs unattended over the air. That works because ESP-NOW's peer is another
ESP-NOW node, and because there is a host C++ library (`iLabs_ESP-NOW`) whose
API is half of what needs testing.

Matter has neither property:

- The peer is a **Matter controller** (`chip-tool`) that only runs on a PC, over
  WiFi and BLE, against a real fabric. A second Challenger cannot play that role.
- There is **no host library yet**. The only host contract today is the raw
  `AT+MT` line protocol, so the whole surface under test is the AT link plus the
  Matter behaviour behind it.

So the harness is a **Python program on the PC** that drives two things at once:
the AT link (via serial) and `chip-tool` (via subprocess), asserting that the two
views of the device agree. The AT link is reachable from the PC because the
RP2350 already runs the `RP2350USB2Serial` bridge that `fw/flash.py` installs:
after flashing, leave the bridge in place and `/dev/ttyACM0` **is** the C6's
`AT+MT` link on GPIO16/17. No extra wiring.

What the suite is a tripwire for, in priority order:

1. **`AT+MT` spec drift** (`docs/AT_MT_SPEC.md`): wrong terminal response, a
   missing `+MTERR:8` on an unknown command, an accepted out-of-range argument,
   a renamed or reshaped URC.
2. **`at_core` regressions**, which are shared with the ESP-NOW firmware: grammar,
   dispatch, echo, line-length handling. See §9 on running both suites.
3. **Matter lifecycle regressions**: commissioning, fabric accounting, attribute
   read/write in both directions, persistence across reboot, factory reset.

## 2. Rig / bill of materials

| Qty | Item | Notes |
|----|------|-------|
| 1 | **Challenger RP2350 WiFi6/BLE5** | The C6 runs the `iLabs_AT_Hearth` firmware; the RP2350 runs the `RP2350USB2Serial` bridge (already shipped as `fw/RP2350USB2Serial.ino.uf2`). |
| 1 | USB-C cable | Power + the bridged AT link to the PC |
| 1 | Linux PC | Runs the harness, `chip-tool`, and BlueZ. Needs a working Bluetooth adapter for BLE commissioning. |
| 1 | 2.4 GHz WiFi AP | The C6 is WiFi-only (Thread is deferred, see `ARCHITECTURE.md`). The PC and the C6 must land on the **same L2 segment**, or operational discovery over mDNS fails. |
| 0 | USB-TTL probe (superseded) | The RP2350 bridge sketch now carries a software UART on the pin C6 **GPIO2** is wired to, so the console arrives over the same USB cable as the AT link. No probe needed. |

**Keep the two streams separate.** The bridge exposes the C6 console (GPIO2) and
the AT link (GPIO16/17), and they must reach the harness as distinct streams. A
merged stream interleaves them character by character:

```
WIFI_EVENT_STA_CON+NMETCRTEEADD
```

which is `WIFI_EVENT_STA_CON` from the console and `+MTREADY` from the AT link in
one buffer. Every URC assertion in Phases 1 and 2 would fail intermittently and
look like a firmware fault. The harness reads the AT stream only; the console
stream is for diagnosis when something fails.

Unlike the ESP-NOW rig there is no second board and no board-to-board spacing
requirement, but there **is** a network requirement: client isolation / AP
isolation on the test SSID will break everything after commissioning. Note the
SSID used in the baseline report.

### Scripted power cuts (replaces the operator unplug)

The bench hub (RSHTECH RSH-A10, cascaded RTS5411, all segments `ppps`)
gives software-controlled VBUS per port through `uhubctl`, verified
2026-08-07: cutting the Challenger's port makes the board vanish from
`/dev/serial/by-id/` for exactly the commanded interval and
re-enumerate in about 2.3 s, a true full-board power cycle (the board
is bus-powered). Any test step that used to read "chime, operator
unplugs, holds, replugs" is now:

```sh
# Challenger full power cycle, 5 s off (transport-mismatch tests,
# power-cut recovery, the B103-class CDC-reconnect scenarios)
uhubctl -l 3-1.3 -p 3 -a cycle -d 5
```

Port map as cabled today: Challenger at hub `3-1.3` port 3, Debug
Probe at `3-1.3` port 4, ZBT-2 at `3-1` port 2. The USB2-face hub
locations (`3-1*`) are the ones that control VBUS. A udev rule
(`/etc/udev/rules.d/52-uhubctl.rules`, vendor 0bda) grants
unprivileged access. Port numbers follow the cabling, not the device:
re-run `uhubctl` and re-verify the map after any bench rewiring before
trusting a scripted cut, per the known-start-state rule.

## 3. One-time setup

### 3.1 Device

```sh
# build against the esp-matter toolchain (IDF v5.4.1, not the ESP-NOW v5.5.4)
idf.py set-target esp32c6
idf.py build

# flash, board in BOOTSEL (this also leaves the USB2Serial bridge running)
python3 fw/flash.py
```

Record the firmware commit in the report header, so a regression can be bisected
against a known-good firmware. The same applies to the `at_core` commit in
`iLabs_AT_ESP-now`, since it is pulled in via `EXTRA_COMPONENT_DIRS`.

### 3.2 Controller

`chip-tool` from the same connectedhomeip revision that `esp-matter release/v1.5`
pins. Build it once and keep it on `PATH`. The harness uses an isolated storage
directory per run so a stale fabric cannot leak between runs:

```sh
export MT_CHIPTOOL=chip-tool
export MT_CHIPTOOL_STORAGE=/tmp/mt-regression   # wiped at the start of each run
export MT_SSID=... MT_PSK=...
export MT_PORT=/dev/ttyACM0
```

### 3.3 Harness

`pip install pyserial`. Nothing else: `chip-tool` is driven as a subprocess and
parsed from its stdout, so there is no Python CHIP binding to install.

## 4. Harness architecture

One program, `test/mt_regression.py`, structured to mirror the ESP-NOW sketch so
the two reports read the same.

**`ATLink`** wraps the serial port and reproduces the result mapping that
`ESP_NOW.command()` uses on the ESP-NOW side, so the assertion style carries over
unchanged:

| Return | Meaning |
|---|---|
| `0` | terminated with `OK` |
| `n > 0` | terminated with `+MTERR:<n>` then `ERROR` |
| `-1` | terminated with a bare `ERROR` (no `+MTERR` line) |
| `-2` | no terminal response before the timeout |

It also keeps a **URC queue**: any `+MT…` line that is not the current command's
own intermediate response is pushed aside with a timestamp, so tests can await
`+MTEVT:3` (commissioning complete) or `+MTATTR:1,6,0,0` without racing the
command stream.
This matters more here than on ESP-NOW: commissioning URCs arrive seconds after
the command that caused them, and controller-driven `+MTATTR` URCs arrive with no
command at all.

**`check(name, cond)`** scores and prints exactly like the sketch:

```
  [PASS] [AT+] MTVER? emits +MTVER:
  [FAIL] [AT-] MTCOMMISSION=901 -> ERROR
```

**Reporting**: same summary line (`===== RESULT: N passed, M failed =====`), plus
two things the sketch cannot do: a **non-zero exit code** on failure so it can be
wired into CI, and a **JSON baseline** (`--baseline out.json`) recording every
test name, result, firmware/`at_core` commits and the SSID, so a regression is a
`diff` away rather than a re-read of two terminal scrollbacks.

**Selection**: `--phase 1`, `-k <substring>`, and `--include-slow` (the
commissioning-window-timeout test costs ~90 s and is off by default).

## 5. Phase 0: link preflight

Not scored, but a hard gate: if these fail, the rest of the report is noise.

- Port opens at 115200 8N1.
- `AT` answers `OK` within 1 s (the parser task is alive).
- `AT+CGMM` reports `ESP32-C6 Hearth`. If it reports `ESP32-C6 ESP-NOW` the board
  is running the **wrong personality**, which is the single most likely operator
  error on a bench that carries both firmwares. Abort with that message rather
  than emitting 40 failures.
- Firmware version from `AT+CGMR` is captured into the report header.

## 6. Phase 1: raw AT protocol conformance

The direct analogue of the ESP-NOW suite's Phase 1, and the real spec tripwire.

**Design rule, inherited from ESP-NOW and important here: Phase 1 must not change
device state.** Every negative case is rejected before any side effect, and the
positive cases are queries, identity commands, and attribute *reads*.
`AT+MTCOMMISSION` with valid arguments opens a real window and `AT+MTATTR` with
four parameters performs a real write, so both are deferred to Phase 2. Phase 1
can therefore run on a commissioned device without disturbing it.

### 6.1 Positive

| Command | Expected |
|---|---|
| `AT` | `OK` |
| `ATE1` then `ATE0` | `OK` each; echo observed on and off (the harness reads echo-aware) |
| `AT+CGMI` | `iLabs Electronics` then `OK` |
| `AT+CGMM` | `ESP32-C6 Hearth` then `OK` |
| `AT+CGMR` | version line then `OK`, equal to `AT+MTVER?`'s field |
| `AT+MTVER?` | line starts `+MTVER:` then `OK` |
| `AT+MTSTATE?` | matches `+MTSTATE:<0\|1\|2>,<n>` then `OK` |
| `AT+MTFABRICS?` | matches `+MTFABRICS:<n>` then `OK` |
| `AT+MTCODES?` | `+MTCODES:<qr>,<manual>`; `<qr>` starts `MT:`, `<manual>` is 11 digits |
| `at+mtver?` (lower case) | `OK` (dispatch is case-insensitive) |
| `AT+MTATTR=1,6,0` | `+MTATTR:1,6,0,<0\|1>` then `OK` |
| `AT+MTATTR=1,0x0006,0x0000` | same value as the decimal form (hex parsing) |
| `AT+MTATTR=0,0x0028,0x0002` | `OK` with an integer value (root-endpoint read works: VendorID) |
| `AT+MTEVT?` | `+MTEVTMASK:0x0800003F` at boot (commissioning group plus bit 27) |
| `AT+MTNET?` | matches `+MTNET:(WIFI\|THREAD),[01],[01]` |
| `AT+MTBAUD?` | `+MTBAUD:115200` at boot |
| `AT+MTFLOW?` | `+MTFLOW:0` (no board routes RTS/CTS) |
| `AT+MTFLOW=0` | `OK` (the only mode this hardware accepts) |

The event mask is the one piece of state Phase 1 may change, because it is AT
state rather than device state and it is trivially restorable. Set it, read it
back, restore it:

```
AT+MTEVT=0xFFFFFFFF   -> OK
AT+MTEVT?             -> +MTEVTMASK:0xFFFFFFFF
AT+MTEVT=0x0800003F   -> OK        (restore before Phase 2)
```

**Restoring matters.** Leaving the mask wide open makes every Phase 2 URC
assertion race against connectivity and BLE chatter that the tests do not expect,
which presents as intermittent Phase 2 failures with no obvious cause.

`AT+MTNET?` on a WiFi image should report `WIFI,1,1` once associated. A
`<connected>` of `0` late in a run is worth noticing: it means the C6 dropped its
AP association, and the Phase 2 mDNS-dependent tests are about to fail for
reasons that have nothing to do with the firmware.

Cross-checks worth scoring as their own tests:

- `AT+MTSTATE?`'s state and `AT+MTFABRICS?`'s count are **consistent**: state `2`
  implies count > 0, state `0` implies count == 0. (State `1` may hold either,
  since an open window outranks the fabric count in `mt_matter_state()`.)
- `AT+MTCODES?` is **stable** across two consecutive calls.
- The manual code matches the recorded baseline. For a development build using
  the esp-matter test credentials (discriminator 3840, passcode 20202021) this is
  the well-known CHIP test pairing code; assert against the baseline value rather
  than hard-coding it, so provisioned production units do not fail spuriously.

### 6.2 Negative

Derived from `at_parser.c` (grammar) and `mt_at.c` (per-command validation).

**Grammar and dispatch (shared `at_core`):**

| Command | Expected |
|---|---|
| `AT+MTBOGUS` | `+MTERR:8` (unknown command; version-skew signal) |
| `HELLO` | bare `ERROR` (not an `AT+` line) |
| `AT+MTVER?X` | bare `ERROR` (trailing character after `?`) |
| a line longer than `MT_AT_LINE_MAX` (512) | bare `ERROR` (overflow path) |
| a bare CR (empty line) | **no response at all**, i.e. `-2`. This is deliberate parser behaviour and is easy to break; pin it. |
| the same command terminated by CR, LF, and CRLF | `OK` in all three cases |

**Wrong command form:**

| Command | Expected |
|---|---|
| `AT+MTVER=1` | bare `ERROR` (SET on a query-only command) |
| `AT+CGMI?` | bare `ERROR` (QUERY on an exec-only command) |
| `AT+MTSTATE` | bare `ERROR` (EXEC on a query-only command) |
| `AT+MTFABRICS=1` | bare `ERROR` |
| `AT+MTCODES=1` | bare `ERROR` |
| `AT+MTRESET?` | bare `ERROR`, **and the device does not reboot** (assert no `+MTREADY` follows) |
| `AT+MTRESET=1` | bare `ERROR`, same no-reboot assertion |
| `AT+MTFRESET?` | bare `ERROR`, same no-reboot assertion |
| `AT+MTFRESET=1` | bare `ERROR`, same no-reboot assertion |
| `AT+MTCOMMISSION?` | bare `ERROR` (query form is not accepted) |
| `AT+MTATTR?` | bare `ERROR` |
| `AT+MTATTR` | bare `ERROR` (exec form, no arguments) |

The four reset cases are the highest-value negatives in the suite: a
dispatch regression there wipes the fabric, so they are worth their own explicit
no-reboot assertion rather than just a result-code check.

**`AT+MTCOMMISSION` range and parse guards** (all rejected before the window
opens, so they are state-safe):

| Command | Expected |
|---|---|
| `AT+MTCOMMISSION=179` | `+MTERR:1` (below the 180 s minimum) |
| `AT+MTCOMMISSION=901` | `+MTERR:1` (above the 900 s maximum) |
| `AT+MTCOMMISSION=abc` | `+MTERR:1` (not an integer) |
| `AT+MTCOMMISSION=` | `+MTERR:1` (empty argument) |
| `AT+MTCOMMISSION=300x` | `+MTERR:1` (trailing garbage after the integer) |

Follow each with `AT+MTSTATE?` and assert the state is unchanged, which is the
assertion that actually proves "rejected before any side effect".

**`AT+MTATTR` argument validation:**

| Command | Expected |
|---|---|
| `AT+MTATTR=1,6` | `+MTERR:1` (fewer than 3 parameters) |
| `AT+MTATTR=1,6,0,1,2` | `+MTERR:1` (mode out of range: 5 params is now a valid form) |
| `AT+MTATTR=1,6,0,1,0,9` | `+MTERR:1` (more than 5 parameters: `at_split_args` returns -1) |
| `AT+MTATTR=x,6,0` | `+MTERR:1` (endpoint not numeric) |
| `AT+MTATTR=1,zz,0` | `+MTERR:1` (cluster not numeric) |
| `AT+MTATTR=1,6,0,z` | `+MTERR:1` (value not numeric) |
| `AT+MTATTR=99,6,0` | `+MTERR:2` (unknown endpoint) |
| `AT+MTATTR=1,0xFFFF,0` | `+MTERR:3` (unknown cluster) |
| `AT+MTATTR=1,6,0xFFFF` | `+MTERR:4` (unknown attribute) |
| `AT+MTATTR=0,0x0028,0x0005` | `+MTERR:5` (NodeLabel is a string: non-integer types are unsupported by design, spec §3.8) |
| `AT+MTATTR?` | bare `ERROR` (wrong command form, not a bad parameter) |

**`AT+MTSWITCH` argument validation:**

| Command | Expected |
|---|---|
| `AT+MTSWITCH?` | bare `ERROR` (query form not accepted) |
| `AT+MTSWITCH` | bare `ERROR` (exec form without arguments) |
| `AT+MTSWITCH=` | `+MTERR:1` (empty argument) |
| `AT+MTSWITCH=zz` | `+MTERR:1` (endpoint not numeric) |
| `AT+MTSWITCH=99` | `+MTERR:2` (unknown endpoint) |
| `AT+MTSWITCH=0` | `+MTERR:3` (root endpoint has no Switch cluster) |
| `AT+MTSWITCH=1` | `OK` (success: endpoint has Switch cluster; event fired on fabric) |

Note that `AT+MTSWITCH` does not return a value to the host; the event fire is fire-and-forget over the fabric. Controller-side observation is the positive check.

**`AT+MTCMDRESP` / the verdict mailbox (command forwarding, C1):** the state
machine (`mt_cmdbox.c`) is host-tested exhaustively (`test/host/test_mt_cmdbox.c`).
The FreeRTOS glue around it in `mt_at.c` (`mt_cmd_forward`'s 1000 ms wait, the
two non-blocking semaphore drains that make a timeout-boundary race
self-healing rather than a permanent instant-deny, the critical sections
across two tasks) has no host-test coverage by construction: none of it
exists without FreeRTOS. It is a bench/inspection concern, verified by
reading the timing and by exercising `AT+MTCMDRESP` against a live forwarded
command once Task C2 wires a caller in, not by an automated regression here.

**`AT+MTCMDRESP` argument validation:**

| Command | Expected |
|---|---|
| `AT+MTCMDRESP?` | bare `ERROR` (query form not accepted, `AT+MTSWITCH` pattern: no "current pending command" to report) |
| `AT+MTCMDRESP` | bare `ERROR` (exec form without arguments) |
| `AT+MTCMDRESP=1` | `+MTERR:1` (fewer than 2 parameters) |
| `AT+MTCMDRESP=1,1,1` | `+MTERR:1` (more than 2 parameters) |
| `AT+MTCMDRESP=zz,1` | `+MTERR:1` (seq not numeric) |
| `AT+MTCMDRESP=1,zz` | `+MTERR:1` (verdict not numeric) |
| `AT+MTCMDRESP=1,2` | `+MTERR:1` (verdict outside `{0,1}`) |
| `AT+MTCMDRESP=99,1` (no forward pending) | `+MTERR:1` (seq not the one currently `PENDING`) |
| `AT+MTCMDRESP=<seq>,1` (seq already answered or expired) | `+MTERR:1` (same code; wrong, stale, future and already-answered are not distinguished, per the wire contract) |

**`AT+MTLOCK` argument validation:**

| Command | Expected |
|---|---|
| `AT+MTLOCK?` | bare `ERROR` (query form not accepted, `AT+MTSWITCH`/`AT+MTTEMPLEVELS` pattern) |
| `AT+MTLOCK` | bare `ERROR` (exec form without arguments) |
| `AT+MTLOCK=1` | `+MTERR:1` (fewer than 2 parameters) |
| `AT+MTLOCK=1,1,1,1` | `+MTERR:1` (more than 3 parameters) |
| `AT+MTLOCK=zz,1` | `+MTERR:1` (endpoint not numeric) |
| `AT+MTLOCK=1,zz` | `+MTERR:1` (state not numeric) |
| `AT+MTLOCK=1,3` | `+MTERR:1` (state outside `0..2`) |
| `AT+MTLOCK=1,1,zz` | `+MTERR:1` (source not numeric) |
| `AT+MTLOCK=1,1,11` | `+MTERR:1` (source above `mt_matter_lock_source_max()`, i.e. outside `0..10`) |
| `AT+MTLOCK=99,1` | `+MTERR:2` (unknown endpoint) |
| `AT+MTLOCK=<non-door-lock ep>,1` | `+MTERR:3` (endpoint has no `DoorLock` cluster) |
| `AT+MTLOCK=<door-lock ep>,1` | `OK` (state accepted; follow with `AT+MTATTR` read of `LockState` to confirm) |

**`AT+MTVALVE` argument validation:**

| Command | Expected |
|---|---|
| `AT+MTVALVE?` | bare `ERROR` (query form not accepted, `AT+MTLOCK` pattern) |
| `AT+MTVALVE` | bare `ERROR` (exec form without arguments) |
| `AT+MTVALVE=1` | `+MTERR:1` (fewer than 2 parameters) |
| `AT+MTVALVE=1,1,1,1` | `+MTERR:1` (more than 3 parameters) |
| `AT+MTVALVE=zz,1` | `+MTERR:1` (endpoint not numeric) |
| `AT+MTVALVE=1,zz` | `+MTERR:1` (state not numeric) |
| `AT+MTVALVE=1,3` | `+MTERR:1` (state outside `0..2`) |
| `AT+MTVALVE=1,1,zz` | `+MTERR:1` (level not numeric) |
| `AT+MTVALVE=1,1,101` | `+MTERR:1` (level above `100`) |
| `AT+MTVALVE=99,1` | `+MTERR:2` (unknown endpoint) |
| `AT+MTVALVE=<non-valve ep>,1` | `+MTERR:3` (endpoint has no `ValveConfigurationAndControl` cluster) |
| `AT+MTVALVE=<valve ep>,1` | `OK` (state accepted; follow with `AT+MTATTR` read of `CurrentState` to confirm) |
| `AT+MTVALVE=<valve ep>,1,50` | `OK` (state and level both accepted; per `AT_MT_SPEC.md` §3.19 the level does not publish as an attribute on this SDK revision, so there is nothing to read back for it) |

**Chatty-host bench case: a bridge command already in flight when a forward
opens.** Exercises `AT_MT_SPEC.md` §3.17's own by-construction limitation and
the `iLabs_Hearth` library README's "Hearth originals" section from the other
side of the link: have the host poll `AT+MTFABRICS?` on every pass of its own
loop (standing in for any chatty per-loop status query, e.g.
`Matter.isDeviceCommissioned()`), then have the controller invoke `LockDoor`
so a `+MTCMD` opens while that poll is in flight. Expected: the AT parser
task is still inside the `AT+MTFABRICS?` handler, blocked taking
`ChipStackLock` (held for the wait by the CHIP task processing the invoke, see
`ARCHITECTURE.md` §8.4), so `AT+MTCMDRESP` cannot reach `cmd_mtcmdresp()` in
time even if the host sends it; the window expires, the firmware
default-denies, `+MTCMDTO:<seq>` is raised, and the controller observes
`Status::Failure` on the `LockDoor` invoke (door-lock-server.cpp's
`OperationErrorEnum::kUnspecified` path). Not a bug to chase: the fix is a
host-side one (keep chatty per-loop status polling off any loop that also
needs to answer forwarded commands), so this case exists to confirm the
documented outcome actually reproduces on the bench, not to find a firmware
regression.

**`AT+MTEVT` and `AT+MTNET`:**

| Command | Expected |
|---|---|
| `AT+MTEVT` | bare `ERROR` (exec form not accepted) |
| `AT+MTEVT=zz` | `+MTERR:1` (not a number) |
| `AT+MTEVT=` | `+MTERR:1` (empty argument) |
| `AT+MTNET` | bare `ERROR` (exec form on a query-only command) |
| `AT+MTNET=1` | bare `ERROR` (set form on a query-only command) |

`AT+MTNET` is query-only by design and there is deliberately no way to set the
transport: it is a build-time choice, and the Root Node's NetworkCommissioning
cluster advertises it, so it is part of the data model (spec §3.12). A future
`AT+MTNET=` that appeared to work would be a serious bug, which is why the SET
form is pinned as rejected here rather than left unspecified.

**`AT+MTBAUD` and `AT+MTFLOW`:**

| Command | Expected |
|---|---|
| `AT+MTBAUD` | bare `ERROR` (exec form not accepted) |
| `AT+MTBAUD=12345` | `+MTERR:1` (not a standard rate) |
| `AT+MTBAUD=zz` | `+MTERR:1` (not a number) |
| `AT+MTBAUD=1843200` | `+MTERR:1` (above the 921600 cap) |
| `AT+MTFLOW` | bare `ERROR` (exec form not accepted) |
| `AT+MTFLOW=4` | `+MTERR:1` (above the highest mode) |
| `AT+MTFLOW=1` | `+MTERR:1` (RTS/CTS not wired on this board) |
| `AT+MTFLOW=3` | `+MTERR:1` (same) |

The three rejected `AT+MTFLOW` modes are the assertion that matters here.
`at_core` can drive RTS/CTS and the ESP-NOW image accepts all four modes
through `AT+ENFLOW`, but no C6 board routes the pair (spec §3.14). A build
that started accepting mode `1` or `3` would gate the C6's transmitter on an
unbonded pin: the symptom is not an error but a link that stops mid-answer
and needs a reset, which is exactly the kind of failure a regression suite
should catch before a bench session does.

Do not add a positive `AT+MTBAUD=<rate>` case to this phase. A rate switch
leaves the harness and the device disagreeing until the harness reopens its
port, so it belongs in a dedicated test that owns the reconnect, not in a
table of one-line assertions. `AT+MTBAUD?` reading back `115200` (§6.1)
covers the query side.

The last four rows are the point of the C2 retrofit. `2` through `5` walk
endpoint, cluster, attribute, type in order, so a host that gets `+MTERR:3`
knows its endpoint was fine and its cluster was not. Before C2 all four, plus
every parameter mistake above them, returned an identical bare `ERROR`.

Assert the **exact** code, never merely "an error". A regression that collapses
`3` back into `2`, or into a bare `ERROR`, is precisely what this table exists
to catch, and it is invisible to a test that only checks for failure.

### 6.3 What a bare `ERROR` still means

After the C2 retrofit a bare `ERROR` means exactly one thing: **the command form
was wrong**. A SET on a query-only command, a QUERY on an exec-only one, a
trailing character after `?`. Everything else carries a code.

That division is worth asserting in both directions. The form negatives in §6.2
must stay bare, and the parameter and lookup negatives must stay coded. A change
that starts returning `+MTERR:1` for `AT+MTVER=1` is as much a regression as one
that returns a bare `ERROR` for a bad endpoint, because it destroys the
host's ability to distinguish "you asked the wrong way" from "you asked for
something that is not there".

This section previously recorded the absence of specific codes as a known
coverage limit. C2 closed it, and the suite is now as diagnostic as the ESP-NOW
one, which can already tell a bad LMK (`+ENERR:6`) from a bad MAC (`ERROR`).

## 7. Phase 2: Matter lifecycle (device + controller)

Stateful and destructive: it factory-resets, commissions, and re-commissions. It
runs in a fixed order, and each step's preconditions are established by the step
before it. Two steps are opt-in and skip by default: `--include-slow` admits 2.10
(the ~200 s window-expiry wait) and `--include-manual` admits 2.9 (needs an
operator at the bench); a step sat out this way counts as gated in the run
summary, separately from a failure or an abort-skip, because it is operator
intent and not a truncated run.

Since T4 this whole chain also runs against the Thread image, unmodified: the
gate detects the transport by asking `AT+MTNET?` (override with
`--transport`) and branches its preconditions on the answer. On WiFi it still
needs `MT_SSID`/`MT_PSK`; on Thread it instead needs a live `otbr-agent`
(bring-up gotchas: `ARCHITECTURE.md` section 3, graph F36) answering an
active role (leader, router or child) on `ot-ctl state`, and fetches the
active dataset live via `ot-ctl dataset active -x` (override with
`--dataset`/`MT_DATASET`). The only step code that differs by transport is
the pairing verb in 2.3 and 2.11: `chip-tool pairing ble-wifi <node> <ssid>
<psk> <passcode> <discriminator>` on WiFi, `chip-tool pairing ble-thread
<node> hex:<dataset> <passcode> <discriminator>` on Thread. Every other
assertion below is transport-neutral and unchanged. A full Thread run
records its own baseline, `test/baselines/thread-lifecycle.json`, next to
the WiFi one.

The combined image (`build_combined`, `AT+MTTRANSPORT` selects the active
stack, spec §3.12.2) needs no baseline of its own: hardware-verified
2026-08-02, a full run in each mode is PASS-identical to that mode's
existing single-image baseline, and the transport switch/mismatch contract
(spec §3.12.1) is proven in both directions on real hardware.

**2.1 Factory-fresh baseline**
`AT+MTRESET` returns `OK`, the device reboots, and `+MTREADY` arrives within 15 s.
Then `AT+MTFABRICS?` is `0` and `AT+MTSTATE?` is `1` (a fresh device opens a
window automatically), with a `+MTEVT:0` URC (window opened) observed after the
reboot.

**2.2 Onboarding codes are usable**
`AT+MTCODES?` after the reset returns the same codes as before it (they derive
from provisioned commissionable data, not from session state).

**2.3 Commission over BLE + WiFi**
`chip-tool pairing ble-wifi <node> <ssid> <psk> <passcode> <discriminator>`, with
the passcode and discriminator taken from the build's credentials. Assert:
- `chip-tool` exits 0,
- `+MTEVT:3` (commissioning complete) arrives on the AT link,
- `AT+MTFABRICS?` becomes `1`,
- `AT+MTSTATE?` becomes `2`.

The AT-side and controller-side views agreeing is the point of the test: either
one alone can pass while the device is unusable.

Since 2026-07-30 (commit eb15d0f, decision DE24) events `0` and `4` are a
pair: exactly one `+MTEVT:4` is raised per reported `+MTEVT:0`, after
`+MTEVT:3` on a successful commissioning. The harness asserts the order and
the absence of a duplicate; both leaks it guards against existed before the
fix (spec `AT_MT_SPEC.md` section 3.11).

**2.4 Attribute round-trip, host to controller**
`AT+MTATTR=1,6,0,1` returns `OK`; a `+MTATTR:1,6,0,1` URC is observed; then
`chip-tool onoff read on-off <node> 1` reports `1`. Repeat for `0`.

Then the write modes (spec §3.8). Subscribe first, with
`chip-tool interactive start` and a subscription on OnOff, so a report is
observable rather than inferred:

- `AT+MTATTR=1,6,0,1,1` (explicit notify) behaves exactly as the default form:
  `OK`, a `+MTATTR` URC, a report at the controller, and a read of `1`.
- `AT+MTATTR=1,6,0,0,0` (local only) returns `OK` and a subsequent
  `AT+MTATTR=1,6,0` reads `0`, but the controller sees **no report**. A
  `chip-tool onoff read` afterwards does show `0`, since the value really did
  change; it is the report that is suppressed, not the write.

Mode `0` existing at all is what lets a host reflect a controller-driven change
without echoing it back to the fabric. A regression that makes mode `0` notify
turns that reflection into a loop, and nothing else in this suite would catch
it, because every other observable stays correct.

**2.5 Attribute round-trip, controller to host**
`chip-tool onoff off <node> 1`, then assert a `+MTATTR:1,6,0,0` URC arrives on
the AT link within 2 s **unprompted**, and that a subsequent `AT+MTATTR=1,6,0`
read agrees. Repeat with `on` and with `toggle`.

This is the closest thing the Matter suite has to the ESP-NOW OTA round-trip: it
proves the whole path (radio, CHIP stack, attribute callback, URC, AT link) end
to end in both directions.

**2.6 Root-endpoint URCs stay off the link**
Across the whole of Phase 2, no `+MTATTR:0,…` URC is ever seen. Endpoint 0 is
intentionally suppressed (spec §4), and regressing that floods the host with
boot-time init noise.

**2.7 Additional commissioning window on an operational device**
`AT+MTCOMMISSION=180` returns `OK`, `+MTEVT:0` arrives, and
`AT+MTSTATE?` becomes `1` while the window is open. Commission a **second
fabric** with a second `chip-tool` storage directory, then assert
`AT+MTFABRICS?` is `2`. Remove it with
`chip-tool operationalcredentials remove-fabric <index>` and assert it returns
to `1`. This exercises the fabric accounting that `AT+MTSTATE?`/`AT+MTFABRICS?`
report, which nothing else covers.

**2.8 Persistence across a warm reboot**
Set the light on, reset the board **without** a factory reset (RP2350-driven
reset, the same path `fw/flash.py` uses), wait for `+MTREADY`, and assert the
fabric count survived.

The attribute value survives since the B63 fix (commit 8100af4,
2026-07-31), and the harness asserts it: the post-reboot read must equal the
written value. History worth keeping: esp-matter's lighting feature config
defaults StartUpOnOff to 0 ("always boot Off") instead of null ("previous
value"), so before the fix every boot forcibly persisted a 0 over the
otherwise healthy NVS restore, measured as boots-to-0 with 0, 6 and 15
second write-to-reset gaps. The fix passes null in the light device type
create thunks; this check is its regression guard.

**2.9 Cold boot with a state change at init (regression, real bug)**
Set the light **on**, then **remove power** rather than resetting, and assert the
device reaches `+MTREADY` within 15 s. Then assert `AT+MTATTR=1,6,0` reads `0`.

This looks like a duplicate of 2.8 and is not. A commissioned device whose OnOff
state must change at init applies `StartUpOnOff` **during**
`esp_matter::start()`, which fires `app_attribute_update_cb`, which calls
`mt_at_urc()`. `app_main` runs `esp_matter::start()` before `mt_at_start()`, so
at that moment the AT UART and its TX mutex do not exist yet. Before commit
`b7d9b0e` this asserted in `xSemaphoreTake(NULL)` and rebooted, forever:

```
I (1488) chip[ZCL]: Toggle ep1 on/off from state 1 to 0
assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
```

The bug shipped in B4.3 and survived bring-up because it needs the device
commissioned **and** the state to actually change at init. 2.8 misses it because
a warm reset preserves the state, so nothing changes and no URC fires. Only a
cold boot with a pending state change reaches the path.

The `AT+MTATTR` read is not decoration: it proves the toggle actually fired. If
it reads `1`, no state change occurred and the test proved nothing, so treat
that as an inconclusive run rather than a pass.

Post-B63 note (see 2.8): with StartUpOnOff null since commit 8100af4, no
state changes at init at all, so the assertion is now that the value
**survives** the cold boot (a read of the written value), the cold-boot half
of the B63 regression guard. The original state-change-at-init path this
test was written around only arms if a controller explicitly configures
StartUpOnOff to change state at boot; exercising that configuration is
future work, and the boot-loop protection itself (the `s_at_up` URC gate)
is what keeps it safe when it does.

**2.10 Commissioning window expiry (slow, opt-in)**
`AT+MTCOMMISSION=180` with no controller attaching: assert the window's end is
reported and that `AT+MTSTATE?` returns to `2`. Gated behind `--include-slow`
because of the ~200 s wall clock. (The 180 s floor is CHIP's Matter minimum;
values below it are rejected with `+MTERR:1` since commit eb15d0f. Pinned on
hardware 2026-07-31: a never-attached window ends with exactly one
`+MTEVT:4` and no `+MTEVT:5`, about 180 s after the `OK`; `+MTEVT:5` is the
fail-safe timer, which never arms without a PASE session.)

**2.11 The two resets differ in exactly one respect**
Run with a composition applied and a fabric commissioned.

- `AT+MTRESET`, wait for `+MTREADY`. Assert `AT+MTFABRICS?` is `0` **and**
  `AT+MTEP?` still lists the same endpoints with the same IDs.
- Re-commission, then `AT+MTFRESET`, wait for `+MTREADY`. Assert
  `AT+MTFABRICS?` is `0` **and** `AT+MTEP?` now returns zero `+MTEP:` lines.

The composition surviving the first and not the second is the whole distinction
between the two commands (spec §3.10). A regression that made `AT+MTRESET` erase
it would look harmless in every other test here, and would silently turn an
end-user unpair into a bricked product that presents nothing until its host
re-declares the data model.

**2.12 Factory reset returns the rig to a known state**
After the `AT+MTFRESET` in 2.11, `chip-tool` can no longer reach the old node ID
and the device advertises as commissionable again. This leaves the rig in the
factory-fresh state that 2.1 expects, so re-runs are clean.

Note that `AT+MTRESET` alone does **not** do this, which it did before the
composition existed. A run that resets with the wrong command starts the next
iteration with endpoints already declared, and 2.1's assertions no longer mean
what they say.

The T3 harness restores the captured composition after this test (staging
grammar, spec 3.9), so a full run still ends in the
factory-fresh-with-composition state 2.1 expects.

## 8. Interpreting failures

| Symptom | Likely cause |
|---|---|
| Phase 0 aborts on `AT+CGMM` | Wrong personality flashed (ESP-NOW firmware on the board), or the RP2350 bridge is not running. |
| Phase 0 aborts on the very first `AT` right after a reflash or bridge reboot | Settling: the first command after a reboot can time out once. Re-run before investigating; the parser task needs a moment after the bridge re-enumerates. |
| All of Phase 1 fails, Phase 0 passed | An `at_core` grammar/dispatch regression. Run the ESP-NOW RegressionSuite: if its Phase 1 also fails, the bug is in `at_core`, not in `mt_at.c`. |
| One named Phase 1 test fails | Its name points at the command. Compare against the JSON baseline. |
| 2.3 fails with `chip-tool` timing out on BLE | BlueZ busy or the adapter is claimed by another process. Restart `bluetooth.service` and re-run. |
| 2.3 commissions but 2.4/2.5 fail | Operational discovery over mDNS. Check AP client isolation and that the PC and C6 are on the same segment. This is the single most common false alarm on a new bench. |
| A `+MTATTR` URC never arrives, but the read agrees | The attribute callback or `mt_at_urc()` path, not the data model. |
| Everything after 2.1 fails | Factory reset left the device in a bad state. Power-cycle and re-run before investigating. |
| 2.9 boot-loops, or any boot loop with `rst:0xc (SW_CPU)` every ~2 s | Something raised a URC before `mt_at_start()` ran. Check the GPIO2 console for `assert failed: xQueueSemaphoreTake`. The guard added in `b7d9b0e` covers `mt_at_urc()`; a new call site that writes the AT UART directly from an esp_matter callback would reintroduce it. |
| The console shows only ROM output (`ESP-ROM:`, `load:`, `entry`) and nothing else | You are reading GPIO16/17, not GPIO2. The ROM prints on the C6's default UART0 pins and knows nothing about the custom console pin, so the bridge port carries ROM chatter only. Bootloader and app logs are on GPIO2. |

The console UART on GPIO2 carries the CHIP stack's own explanation for every
Phase 2 failure. Capture it alongside the report when filing anything.

## 9. Shared `at_core`: run both suites

`at_core` lives in `iLabs_AT_ESP-now` and is compiled into both firmwares. A
change to `at_parser.c`, `at_uart.c` or `link_mgr.c` is **not** validated by this
suite alone: it needs the ESP-NOW RegressionSuite on its two-board rig as well.

Keep the grammar assertions in §6.2 aligned with the ESP-NOW suite's equivalents.
Where both suites test the same `at_core` behaviour (unknown command, non-AT
line, trailing character after `?`, wrong command form) they must expect the same
shape, differing only in the `+MTERR` / `+ENERR` prefix. A divergence between the
two suites is itself a finding.

The same "run it against both" discipline applies to Phase 2 across the two
firmware images, since T4 made the chain transport-neutral (§7). Both use the
by-id port so a re-enumerated `/dev/ttyACM*` cannot silently point the run at
the wrong device:

```sh
# WiFi image
export MT_SSID=... MT_PSK=...
python3 test/mt_regression.py \
  --port $(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00) \
  --phase 2 --include-slow --include-manual

# Thread image: otbr-agent already up (bring-up runbook, ARCHITECTURE.md
# section 3, graph F36); no MT_SSID/MT_PSK needed, the transport is detected
# from AT+MTNET?
python3 test/mt_regression.py \
  --port $(ls /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_*-if00) \
  --phase 2 --include-slow --include-manual
```

**otbr-agent bring-up, two things that are not obvious from the flags alone**
(bug B72, found 2026-08-01):

- Start `otbr-agent` with `-B` set to the host's **real LAN interface**
  (`enp42s0` on this bench), never `lo`. Avahi ignores loopback, so with
  `-B lo` BLE and the Thread join both look healthy and chip-tool only dies
  later, at operational discovery, with "Avahi resolve failed". The gate's
  own preflight cannot catch this: it asks `ot-ctl state`, which is happy on
  `lo` too.
- The D-Bus policy file install (graph F36) is a one-time step, but the
  control socket ACL is not: after **every** `otbr-agent` start, re-grant it
  with `sudo setfacl -m u:<user>:rw /run/openthread-wpan0.sock`. The agent
  recreates the socket `0755 root:root` on each start, and this `ot-ctl`
  build is a plain `AF_UNIX` client, so the D-Bus policy does nothing for
  it; only the ACL grant does.

## 10. Build-out order

| Step | Deliverable |
|---|---|
| **T1** | `test/mt_regression.py` with `ATLink`, the URC queue, `check()`, the report and the JSON baseline. Phase 0 and Phase 1 only. No `chip-tool` dependency, so it runs on any bench in seconds. |
| **T2** | Phase 2.1–2.5 (reset, commission, both attribute directions). This is where the `chip-tool` subprocess wrapper and its output parsing land. |
| **T3** | Phase 2.6–2.12 (multi-fabric, persistence, cold-boot URC regression, expiry, both resets). |
| **T4** | Optional: fold Phase 1 into an RP2350 sketch once a host library exists, so the AT conformance half can run on the real host MCU the way the ESP-NOW suite does. |

T1 is worth having on its own: it is the part that runs after every edit to
`mt_at.c`, and it needs nothing but the board and a USB cable.

## 11. Known gaps

- **No host-library layer to test.** The ESP-NOW suite's Phase 2 exercises a C++
  API; there is no Matter equivalent yet. If one is written, it gets its own
  phase here.
- **`AT+MTOTA` is unimplemented** (spec §8), so the firmware-update path in
  `FIRMWARE_UPDATE_SPEC.md` is untested by this suite.
- **Thread is built** (hardware-verified 2026-07-29) and Phase 1 runs on both
  images with committed baselines. Phase 2's Thread commissioning matrix is
  hardware-verified too (2026-08-01): three clean 89/89 `--include-slow
  --include-manual` runs plus one clean 79/79-with-2-gated default run,
  baseline committed at `test/baselines/thread-lifecycle.json`.
- **Non-integer attributes** are unsupported by design and are only tested for
  correct rejection (`+MTERR:5`), not for behaviour. `AT+MTATTRX` will need its
  own cases when it lands.
- **Four device types.** The table covers on/off light, dimmable light, colour
  temperature light and temperature sensor. The other 16 in the reference API
  are table entries and get cases as they are added.
- **Production credentials.** Everything here assumes the esp-matter test DAC.
  A unit provisioned via `esp-matter-mfg-tool` needs its own baseline (different
  onboarding codes, different discriminator).

## 12. Suggested workflow

Run T1 (Phase 0 + 1) before every commit to `mt_at.c`, `main.cpp` or `at_core`.
Run the full suite including Phase 2 before tagging a release, and check the JSON
baseline in next to the commit hashes, exactly as the ESP-NOW rig does. When
`at_core` changes, run the ESP-NOW RegressionSuite too, and note both results in
the same place.
