# iLabs AT Matter: regression test plan

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
| 1 | **Challenger RP2350 WiFi6/BLE5** | The C6 runs the `iLabs_AT_Matter` firmware; the RP2350 runs the `RP2350USB2Serial` bridge (already shipped as `fw/RP2350USB2Serial.ino.uf2`). |
| 1 | USB-C cable | Power + the bridged AT link to the PC |
| 1 | Linux PC | Runs the harness, `chip-tool`, and BlueZ. Needs a working Bluetooth adapter for BLE commissioning. |
| 1 | 2.4 GHz WiFi AP | The C6 is WiFi-only (Thread is deferred, see `ARCHITECTURE.md`). The PC and the C6 must land on the **same L2 segment**, or operational discovery over mDNS fails. |
| 1 | USB-TTL probe (optional) | On C6 **GPIO2** for the console/log UART. Not required by the harness, but it is where the CHIP stack explains itself when a test fails. |

Unlike the ESP-NOW rig there is no second board and no board-to-board spacing
requirement, but there **is** a network requirement: client isolation / AP
isolation on the test SSID will break everything after commissioning. Note the
SSID used in the baseline report.

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
`+MTCOMMISSION:COMPLETE` or `+MTATTR:1,6,0,0` without racing the command stream.
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
- `AT+CGMM` reports `ESP32-C6 Matter`. If it reports `ESP32-C6 ESP-NOW` the board
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
| `AT+CGMM` | `ESP32-C6 Matter` then `OK` |
| `AT+CGMR` | version line then `OK`, equal to `AT+MTVER?`'s field |
| `AT+MTVER?` | line starts `+MTVER:` then `OK` |
| `AT+MTSTATE?` | matches `+MTSTATE:<0\|1\|2>,<n>` then `OK` |
| `AT+MTFABRICS?` | matches `+MTFABRICS:<n>` then `OK` |
| `AT+MTCODES?` | `+MTCODES:<qr>,<manual>`; `<qr>` starts `MT:`, `<manual>` is 11 digits |
| `at+mtver?` (lower case) | `OK` (dispatch is case-insensitive) |
| `AT+MTATTR=1,6,0` | `+MTATTR:1,6,0,<0\|1>` then `OK` |
| `AT+MTATTR=1,0x0006,0x0000` | same value as the decimal form (hex parsing) |
| `AT+MTATTR=0,0x0028,0x0002` | `OK` with an integer value (root-endpoint read works: VendorID) |

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
| `AT+MTCOMMISSION?` | bare `ERROR` (query form is not accepted) |
| `AT+MTATTR?` | bare `ERROR` |
| `AT+MTATTR` | bare `ERROR` (exec form, no arguments) |

The two `AT+MTRESET` cases are the highest-value negatives in the suite: a
dispatch regression there wipes the fabric, so they are worth their own explicit
no-reboot assertion rather than just a result-code check.

**`AT+MTCOMMISSION` range and parse guards** (all rejected before the window
opens, so they are state-safe):

| Command | Expected |
|---|---|
| `AT+MTCOMMISSION=29` | bare `ERROR` (below the 30 s minimum) |
| `AT+MTCOMMISSION=901` | bare `ERROR` (above the 900 s maximum) |
| `AT+MTCOMMISSION=abc` | bare `ERROR` (not an integer) |
| `AT+MTCOMMISSION=` | bare `ERROR` (empty argument) |
| `AT+MTCOMMISSION=300x` | bare `ERROR` (trailing garbage after the integer) |

Follow each with `AT+MTSTATE?` and assert the state is unchanged, which is the
assertion that actually proves "rejected before any side effect".

**`AT+MTATTR` argument validation:**

| Command | Expected |
|---|---|
| `AT+MTATTR=1,6` | bare `ERROR` (fewer than 3 parameters) |
| `AT+MTATTR=1,6,0,1,2` | bare `ERROR` (more than 4 parameters: `at_split_args` returns -1) |
| `AT+MTATTR=x,6,0` | bare `ERROR` (endpoint not numeric) |
| `AT+MTATTR=1,zz,0` | bare `ERROR` (cluster not numeric) |
| `AT+MTATTR=1,6,0,z` | bare `ERROR` (value not numeric) |
| `AT+MTATTR=99,6,0` | bare `ERROR` (unknown endpoint) |
| `AT+MTATTR=1,0xFFFF,0` | bare `ERROR` (unknown cluster) |
| `AT+MTATTR=1,6,0xFFFF` | bare `ERROR` (unknown attribute) |
| `AT+MTATTR=0,0x0028,0x0005` | bare `ERROR` (NodeLabel is a string: non-integer types are unsupported by design, spec §3.8) |

### 6.3 A note on the `+MTERR` code space

Today only code `8` exists, so every negative above except the unknown-command
case asserts a **bare `ERROR`**. That is faithful to the current spec (§5), and
the suite must assert exactly that. It is also a weakness: the ESP-NOW suite can
tell a bad LMK (`+ENERR:6`) from a bad MAC (`ERROR`), and this one cannot tell a
bad endpoint from a bad cluster from a wrong command form.

When specific `+MTERR` codes are allocated (spec §8 lists this as planned), these
assertions tighten from "rejected" to "rejected for the documented reason", and
the suite becomes as diagnostic as the ESP-NOW one. Until then, treat this as a
known coverage limit rather than a gap in the tests.

## 7. Phase 2: Matter lifecycle (device + controller)

Stateful and destructive: it factory-resets, commissions, and re-commissions. It
runs in a fixed order, and each step's preconditions are established by the step
before it.

**2.1 Factory-fresh baseline**
`AT+MTRESET` returns `OK`, the device reboots, and `+MTREADY` arrives within 15 s.
Then `AT+MTFABRICS?` is `0` and `AT+MTSTATE?` is `1` (a fresh device opens a
window automatically), with a `+MTCOMMISSION:STARTED` URC observed after the
reboot.

**2.2 Onboarding codes are usable**
`AT+MTCODES?` after the reset returns the same codes as before it (they derive
from provisioned commissionable data, not from session state).

**2.3 Commission over BLE + WiFi**
`chip-tool pairing ble-wifi <node> <ssid> <psk> <passcode> <discriminator>`, with
the passcode and discriminator taken from the build's credentials. Assert:
- `chip-tool` exits 0,
- `+MTCOMMISSION:COMPLETE` arrives on the AT link,
- `AT+MTFABRICS?` becomes `1`,
- `AT+MTSTATE?` becomes `2`.

The AT-side and controller-side views agreeing is the point of the test: either
one alone can pass while the device is unusable.

**2.4 Attribute round-trip, host to controller**
`AT+MTATTR=1,6,0,1` returns `OK`; a `+MTATTR:1,6,0,1` URC is observed; then
`chip-tool onoff read on-off <node> 1` reports `1`. Repeat for `0`.

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
`AT+MTCOMMISSION=60` returns `OK`, `+MTCOMMISSION:STARTED` arrives, and
`AT+MTSTATE?` becomes `1` while the window is open. Commission a **second
fabric** with a second `chip-tool` storage directory, then assert
`AT+MTFABRICS?` is `2`. Remove it with
`chip-tool operationalcredentials remove-fabric <index>` and assert it returns
to `1`. This exercises the fabric accounting that `AT+MTSTATE?`/`AT+MTFABRICS?`
report, which nothing else covers.

**2.8 Persistence across a warm reboot**
Set the light on, reset the board **without** a factory reset (RP2350-driven
reset, the same path `fw/flash.py` uses), wait for `+MTREADY`, and assert the
fabric count and the attribute value both survived.

**2.9 Commissioning window expiry (slow, opt-in)**
`AT+MTCOMMISSION=30` with no controller attaching: assert `+MTCOMMISSION:FAILED`
arrives after the fail-safe expires, and that `AT+MTSTATE?` returns to `2`. Gated
behind `--include-slow` because of the ~90 s wall clock.

**2.10 Factory reset clears everything**
`AT+MTRESET`, then `AT+MTFABRICS?` is `0`, `chip-tool` can no longer reach the old
node ID, and the device is advertising as commissionable again. This leaves the
rig in the factory-fresh state that 2.1 expects, so re-runs are clean.

## 8. Interpreting failures

| Symptom | Likely cause |
|---|---|
| Phase 0 aborts on `AT+CGMM` | Wrong personality flashed (ESP-NOW firmware on the board), or the RP2350 bridge is not running. |
| All of Phase 1 fails, Phase 0 passed | An `at_core` grammar/dispatch regression. Run the ESP-NOW RegressionSuite: if its Phase 1 also fails, the bug is in `at_core`, not in `mt_at.c`. |
| One named Phase 1 test fails | Its name points at the command. Compare against the JSON baseline. |
| 2.3 fails with `chip-tool` timing out on BLE | BlueZ busy or the adapter is claimed by another process. Restart `bluetooth.service` and re-run. |
| 2.3 commissions but 2.4/2.5 fail | Operational discovery over mDNS. Check AP client isolation and that the PC and C6 are on the same segment. This is the single most common false alarm on a new bench. |
| A `+MTATTR` URC never arrives, but the read agrees | The attribute callback or `mt_at_urc()` path, not the data model. |
| Everything after 2.1 fails | Factory reset left the device in a bad state. Power-cycle and re-run before investigating. |

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

## 10. Build-out order

| Step | Deliverable |
|---|---|
| **T1** | `test/mt_regression.py` with `ATLink`, the URC queue, `check()`, the report and the JSON baseline. Phase 0 and Phase 1 only. No `chip-tool` dependency, so it runs on any bench in seconds. |
| **T2** | Phase 2.1–2.5 (reset, commission, both attribute directions). This is where the `chip-tool` subprocess wrapper and its output parsing land. |
| **T3** | Phase 2.6–2.10 (multi-fabric, persistence, expiry, final reset). |
| **T4** | Optional: fold Phase 1 into an RP2350 sketch once a host library exists, so the AT conformance half can run on the real host MCU the way the ESP-NOW suite does. |

T1 is worth having on its own: it is the part that runs after every edit to
`mt_at.c`, and it needs nothing but the board and a USB cable.

## 11. Known gaps

- **No host-library layer to test.** The ESP-NOW suite's Phase 2 exercises a C++
  API; there is no Matter equivalent yet. If one is written, it gets its own
  phase here.
- **`AT+MTOTA` is unimplemented** (spec §8), so the firmware-update path in
  `FIRMWARE_UPDATE_SPEC.md` is untested by this suite.
- **Thread is not built**, so the commissioning matrix is WiFi + BLE only.
- **Non-integer attributes** are unsupported by design and are only tested for
  correct rejection, not for behaviour.
- **Single endpoint.** The data-model coverage is one on/off light; `AT+MTEP`
  (runtime endpoint creation, spec §8) will need its own cases.
- **Production credentials.** Everything here assumes the esp-matter test DAC.
  A unit provisioned via `esp-matter-mfg-tool` needs its own baseline (different
  onboarding codes, different discriminator).

## 12. Suggested workflow

Run T1 (Phase 0 + 1) before every commit to `mt_at.c`, `main.cpp` or `at_core`.
Run the full suite including Phase 2 before tagging a release, and check the JSON
baseline in next to the commit hashes, exactly as the ESP-NOW rig does. When
`at_core` changes, run the ESP-NOW RegressionSuite too, and note both results in
the same place.
