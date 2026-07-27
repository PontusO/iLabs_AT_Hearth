# AT+MT Command Specification (iLabs AT Matter, ESP32-C6)

Status: **draft, tracks the implementation** (Phase B4). This is the host ↔ C6
contract for the Matter personality. It mirrors the ESP-NOW firmware's `AT+EN`
conventions (same `at_core` engine and transport) so a host that speaks one
speaks the other; the two namespaces are disjoint and only one personality is
flashed at a time (mode-switch by host reflash).

## 1. Transport & line conventions

- **Link:** UART, `115200 8N1`, no flow control by default. The AT link is on
  the C6's UART1 (host-bridge pins GPIO16/17); the console/logs live on a
  separate UART (GPIO2). See `ARCHITECTURE.md`.
- **Command framing:** a command line is terminated by CR (`\r`) or LF (`\n`).
- **Response framing:** every response and URC line is terminated by CRLF.
- **Command forms:**
  - `AT+CMD`          execute
  - `AT+CMD?`         query
  - `AT+CMD=<p1>,<p2>,…`  set (comma-separated parameters)
- **Terminal responses:** each command terminates with `OK` or `ERROR`. A
  specific fault prints `+MTERR:<code>` on the line *before* `ERROR` (see §5).
- **URCs** (unsolicited result codes, §4) may arrive at any time, including
  between a command and its terminal response. The host parser must dispatch
  `+MT…` lines out-of-band.
- **Echo:** off at boot. `ATE1` enables character echo, `ATE0` disables it.
- **Boot marker:** the firmware emits `+MTREADY` once when the AT interface is
  up (after boot or after `AT+MTRESET`), so the host can resynchronize.
- **Number formats:** integers are decimal unless noted. Cluster and attribute
  IDs in `AT+MTATTR` accept hex (`0x0006`) or decimal (`6`).
- **One command in flight:** wait for `OK`/`ERROR` before sending the next
  command. URCs are independent of this ordering.

## 2. Command summary

| Command | Form | Response |
|---|---|---|
| `AT` | exec | `OK` |
| `ATE0` / `ATE1` | exec | `OK` (echo off / on) |
| `AT+CGMI` | exec | `<manufacturer>` → `OK` |
| `AT+CGMM` | exec | `<model>` → `OK` |
| `AT+CGMR` | exec | `<fw_version>` → `OK` |
| `AT+MTVER?` | query | `+MTVER:<fw_version>` → `OK` |
| `AT+MTSTATE?` | query | `+MTSTATE:<state>,<fabrics>` → `OK` |
| `AT+MTFABRICS?` | query | `+MTFABRICS:<count>` → `OK` |
| `AT+MTCOMMISSION[=<timeout_s>]` | exec/set | `OK` (window opened; `+MTCOMMISSION` URCs) |
| `AT+MTCODES?` | query | `+MTCODES:<qr>,<manual>` → `OK` |
| `AT+MTRESET` | exec | `OK` → Matter reset (fabrics/credentials) + reboot |
| `AT+MTFRESET` | exec | `OK` → full factory reset (adds the composition) + reboot |
| `AT+MTATTR=<ep>,<cl>,<attr>` | set (3) | `+MTATTR:<ep>,<cl>,<attr>,<val>` → `OK` (read) |
| `AT+MTATTR=<ep>,<cl>,<attr>,<val>` | set (4) | `OK` (write) |
| `AT+MTEP?` | query | `+MTEP:<idx>,<ep_id>,<devtype>` per endpoint → `OK` |
| `AT+MTEP=<devtype>` | set | `OK` (append to the staged composition) |
| `AT+MTEPCLEAR` | exec | `OK` (begin staging an empty composition) |
| `AT+MTEPAPPLY` | exec | `OK` → persist + reboot |
| `AT+MTEVT?` | query | `+MTEVTMASK:<hex32>` → `OK` |
| `AT+MTEVT=<hexmask>` | set | `OK` (subscribe to platform events) |
| `AT+MTNET?` | query | `+MTNET:<transport>,<enabled>,<connected>` → `OK` |

## 3. Command reference

### 3.1 Identity: `AT+CGMI` / `AT+CGMM` / `AT+CGMR`
LTE-modem-style (3GPP TS 27.007) execute commands. Each prints one identity
line then `OK`: manufacturer (`iLabs Electronics`), model (`ESP32-C6 Matter`),
firmware revision (= `AT+MTVER?`'s first field).

### 3.2 `AT+MTVER?`
`+MTVER:<fw_version>`. Firmware version string.

### 3.3 `AT+MTSTATE?`
`+MTSTATE:<state>,<fabrics>`
- `<state>`: `0` uninitialized (no fabric, no open window), `1` commissioning
  (a commissioning window is open), `2` operational (≥ 1 fabric).
- `<fabrics>`: number of commissioned fabrics.

### 3.4 `AT+MTFABRICS?`
`+MTFABRICS:<count>`: number of commissioned fabrics.

### 3.5 `AT+MTCOMMISSION[=<timeout_s>]`
Opens a **basic commissioning window** (BLE + DNS-SD advertising) so a
controller can commission (or additionally commission) the device.
- `<timeout_s>`: optional window lifetime, 30–900 s (default 300).
- Returns `OK` once the window request is accepted.
- Progress is reported asynchronously via the `+MTCOMMISSION:*` URCs (§4).
- Note: a factory-fresh device opens a window automatically at boot; this
  command is for (re)opening one on an already-commissioned device.

### 3.6 `AT+MTCODES?`
`+MTCODES:<qr_payload>,<manual_pairing_code>`
- `<qr_payload>`: the Matter onboarding QR payload string (e.g. `MT:Y.K90…`).
- `<manual_pairing_code>`: the 11-digit manual pairing code.
Derived from the device's commissionable data (discriminator/passcode) and
vendor/product IDs.

### 3.7 `AT+MTRESET`: Matter reset
Erases all **Matter** data (fabrics, credentials, attribute persistence) and
reboots. Emits `OK`, then reboots; the host resynchronizes on the next
`+MTREADY`.

**The endpoint composition survives.** This is the end-user operation, "unpair
this device from my home", and the composition is a product definition supplied
by the host firmware rather than user data: a board that is a dimmable light
plus a temperature sensor is still that afterwards, and comes back immediately
commissionable with the correct data model instead of inert until the host
re-declares it.

To erase the composition as well, use `AT+MTFRESET` (§3.10). To change it
without a full erase, stage a new one with `AT+MTEPCLEAR` / `AT+MTEP=` /
`AT+MTEPAPPLY` (§3.9); applying an empty composition returns the device to
unconfigured.

After reset the device opens a commissioning window automatically, subject to
the unconfigured-device policy still being settled (design spec §12.1, P2).

### 3.8 `AT+MTATTR`: data model access
Read or write a single Matter attribute.
- **Read:** `AT+MTATTR=<ep>,<cluster>,<attr>` (3 params)
  → `+MTATTR:<ep>,<cluster>,<attr>,<val>` → `OK`
- **Write:** `AT+MTATTR=<ep>,<cluster>,<attr>,<val>[,<mode>]` (4 or 5 params) → `OK`

`<cluster>` and `<attr>` accept hex (`0x0006`) or decimal. `<val>` is an
integer; it is interpreted according to the attribute's own type
(bool/enum/bitmap/intN/uintN). String/array/float attributes are not
supported over this command (`+MTERR:5`).

`<mode>` selects how a write is published, default `1`:

| Mode | Behaviour |
|---|---|
| `1` | Subscribers and bound devices see the change. The normal case for a host-driven change. |
| `0` | The value changes locally with no report. |

Mode `0` exists so a host that is **reflecting** a change which came from a
controller does not echo it back to the fabric and loop. It corresponds to
`setAttributeVal` in the arduino-esp32 Matter API, against `updateAttributeVal`
for mode `1`.

A failed access reports which level was wrong: `+MTERR:2` unknown endpoint,
`+MTERR:3` unknown cluster, `+MTERR:4` unknown attribute, `+MTERR:5` an
attribute whose type this command cannot carry.

A write echoes a `+MTATTR` URC (the attribute callback confirming the change),
then `OK`. A controller-driven change to the light endpoint also raises a
`+MTATTR` URC (§4) so the host observes external state changes.

Example, the on/off light (endpoint 1, OnOff cluster `0x0006`, attribute `0x0000`):
```
AT+MTATTR=1,6,0        -> +MTATTR:1,6,0,0   (read: off)
AT+MTATTR=1,6,0,1      -> OK                 (turn on)
AT+MTATTR=1,6,0        -> +MTATTR:1,6,0,1
```

### 3.9 `AT+MTEP` / `AT+MTEPCLEAR` / `AT+MTEPAPPLY`: endpoint composition

The host declares which endpoints the device presents. The composition is
persisted in NVS and rebuilt by the firmware at every boot, so the device
rejoins its fabric after a power cut without host involvement.

- `AT+MTEP?` lists the **live** composition, one line per endpoint:
  `+MTEP:<index>,<endpoint_id>,<device_type>`. Zero lines means the device is
  unconfigured. It never reports a staged composition.
- `AT+MTEPCLEAR` opens a staging session holding an empty composition.
- `AT+MTEP=<device_type>` appends one endpoint. `<device_type>` is a standard
  Matter device type ID, hex or decimal. Rejected with `+MTERR:10` outside a
  staging session or past the 16-endpoint cap, and with `+MTERR:6` for a device
  type this firmware does not implement. A rejected append consumes no slot.
- `AT+MTEPAPPLY` persists the staged composition, emits `OK`, and reboots. The
  host resynchronizes on the next `+MTREADY`.

Staging lives in RAM, so a reboot discards an open session and leaves the stored
composition untouched rather than half-written.

Endpoint IDs are assigned sequentially from 1 in declaration order and are
stable across boots, because the composition is rebuilt before the Matter stack
starts. Applying a composition therefore always reboots rather than taking
effect in place.

An **unconfigured** device (no stored composition) presents only the Root Node.
Changing the composition of a commissioned device invalidates controller caches
and may require re-commissioning.

Device types implemented so far: `0x0100` On/Off Light, `0x0101` Dimmable Light,
`0x010C` Colour Temperature Light, `0x0302` Temperature Sensor.

Example:
```
AT+MTEPCLEAR           -> OK
AT+MTEP=0x0100         -> OK
AT+MTEP=0x0302         -> OK
AT+MTEPAPPLY           -> OK, then reboot and +MTREADY
AT+MTEP?               -> +MTEP:0,1,0x0100
                          +MTEP:1,2,0x0302
                          OK
```

### 3.10 `AT+MTFRESET`: full factory reset

Everything `AT+MTRESET` erases, **plus the endpoint composition**, leaving a
blank unconfigured board. Emits `OK`, then reboots; the host resynchronizes on
the next `+MTREADY`, after which `AT+MTEP?` reports zero endpoints.

This is a manufacturing and development operation, not an end-user one. The two
resets exist separately because they have different audiences and different
correct answers about the composition:

| | `AT+MTRESET` | `AT+MTFRESET` |
|---|---|---|
| Fabrics, credentials, attribute persistence | erased | erased |
| Endpoint composition (`mt_ep` namespace) | **kept** | **erased** |
| `fctry` partition (device attestation) | untouched | untouched |
| Typical caller | end user, unpairing | factory line, developer |

**The `fctry` partition is never touched by either.** It holds the per-device
attestation certificate and passcode provisioned at manufacture; erasing it
would destroy the unit's identity, and no AT command should be able to do that.

Note that `AT+MTFRESET` is deliberately a separate command rather than a
parameter on `AT+MTRESET`. A form like `AT+MTRESET=1` would give meaning to an
input that is currently required to be rejected, and that rejection is an
intentional tripwire on the most destructive command in the set.

### 3.11 `AT+MTEVT`: platform event subscription

```
AT+MTEVT?           ->  +MTEVTMASK:0x0000003F
AT+MTEVT=<hexmask>  ->  OK
```

The firmware surfaces CHIP platform events as `+MTEVT:<bit>[,<detail>]`, but
only for bits the host has subscribed to. Default mask `0x0000003F`, the
commissioning group, which reproduces exactly what the firmware emitted before
the mask existed.

| Bits | Group | Contents |
|---|---|---|
| 0–5 | Commissioning | `0` window opened, `1` session started, `2` session stopped, `3` complete, `4` window closed, `5` fail-safe expired |
| 6–9 | Fabric | `6` will be removed, `7` removed, `8` committed, `9` updated |
| 10–15 | Connectivity | `10` WiFi, `11` internet, `12` interface IP changed, `13` operational network started, `14` DNS-SD initialised, `15` server ready |
| 16–19 | BLE | `16` connected, `17` disconnected, `18` advertising change, `19` deinitialised |
| 20–23 | Misc | `20` OTA state, `21` bindings changed, `22` time sync, `23` reserved |
| 24–26 | Thread | `24` connectivity, `25` state change, `26` interface state change |
| 27–31 | Reserved | |

Bits `10`, `11` and `24` carry a `<detail>` of `1` or `0` for up and down.

Thread bits are **allocated but never emitted on a WiFi image**: transport is a
build-time choice (§3.12), and a host may subscribe to them harmlessly. Fixing
the layout before it is published avoids renumbering later.

The query answers `+MTEVTMASK`, not `+MTEVT`, deliberately. URCs may arrive
between a command and its terminal response, so a `+MTEVT:<n>` reply would be
indistinguishable from an event landing at that moment.

### 3.12 `AT+MTNET?`: network transport

```
AT+MTNET?  ->  +MTNET:<transport>,<enabled>,<connected>
```

- `<transport>`: `WIFI` or `THREAD`.
- `<enabled>`: `1` when the transport is compiled in and started.
- `<connected>`: `1` when the operational network is up.

Transport is fixed at build time. Matter-over-Thread is a Kconfig option
(`ENABLE_MATTER_OVER_THREAD`, gated on `OPENTHREAD_ENABLED`), so the two
variants are separate images selected by reflash, the same model used for the
ESP-NOW and Matter personalities. There is deliberately no command to change it:
the Root Node's NetworkCommissioning cluster advertises the transport's
features, so it is part of the data model, and changing it on a commissioned
device has the same consequences as changing the endpoint composition (§3.9).

## 4. Unsolicited result codes (URCs)

| URC | Meaning |
|---|---|
| `+MTREADY` | AT interface up after boot, `AT+MTRESET`, `AT+MTFRESET` or `AT+MTEPAPPLY`. |
| `+MTEVT:<bit>[,<detail>]` | A subscribed platform event fired (§3.11). |
| `+MTATTR:<ep>,<cluster>,<attr>,<val>` | An attribute changed on one of the declared endpoints (controller-driven or local). The root endpoint (0) is intentionally not reported, to keep boot-time init noise off the link. |
| `+MTIDENT:<ep>,<enabled>` | The Identify cluster started (`1`) or stopped (`0`) on an endpoint. Backs the per-endpoint identify callback; the host decides how to indicate it. |

**`+MTCOMMISSION:STARTED` / `:COMPLETE` / `:FAILED` were removed** in phase C3
and are now `+MTEVT:0`, `+MTEVT:3` and `+MTEVT:5`. All three are in the default
event mask, so a host that sets no mask sees the same three events under the new
name and nothing else changes. Carrying two URC families for one event would
have been permanent; nothing depended on the old names yet.

## 5. Error codes: `+MTERR:<n>`

On a specific fault the firmware prints `+MTERR:<n>` then `ERROR`. Generic
faults print a bare `ERROR` (no `+MTERR` line).

| Code | Meaning |
|---|---|
| `1` | Bad parameter: malformed, out of range, or the wrong number of them. |
| `2` | Unknown endpoint. |
| `3` | Unknown cluster on that endpoint. |
| `4` | Unknown attribute in that cluster. |
| `5` | Attribute type not supported by this command (string, array, float). |
| `6` | Unknown or unsupported device type. |
| `7` | Persistence (NVS) failure. |
| `8` | Unknown / unsupported command (version-skew detection). |
| `9` | Not ready: no composition declared, or the stack is not started. |
| `10` | Composition change rejected: nothing staged, or the endpoint limit was reached. |
| (bare `ERROR`) | **Wrong command form**, or an unclassified runtime failure. |

**A bare `ERROR` now means the command form was wrong**, for example a SET on a
query-only command or a QUERY on an exec-only one. Bad *parameter values* carry
`+MTERR:1` and a failed data-model lookup carries `2` to `5`, so a host can tell
"you asked the wrong way" from "you asked for something that is not there".
Before phase C2 both collapsed into a bare `ERROR`.

**Code-space policy (integration-plan contract C2):** `+MTERR` and `+ENERR`
have distinct prefixes, so each may use the `1–99` range independently; codes
`≥ 100` map to a bare `ERROR`. As `AT+MT` faults gain specific codes they are
allocated here and kept semantically stable for a future merged binary.

## 6. Commissioning credentials

- **Development / bring-up:** esp-matter test credentials: discriminator
  `3840`, setup passcode `20202021`. `AT+MTCODES?` returns the matching QR and
  manual codes. Suitable for `chip-tool` and phone controllers on a test fabric.
- **Production:** per-device Device Attestation Certificate + passcode
  provisioned into `fctry` / `esp_secure_cert` (via `esp-matter-mfg-tool`).
  Manufacturing flow: TBD.

## 7. Data model (v1)

- Transport: **Matter-over-WiFi** (2.4 GHz). BLE is used for commissioning only.
- Endpoints are **declared by the host** over `AT+MTEP` and persisted on the
  device (§3.9). Endpoint `0` is the mandatory Root Node. A factory-fresh device
  presents no application endpoints until the host declares them.
- `AT+MTATTR` provides generic integer attribute read/write across the data
  model.

## 8. Planned / reserved (not yet implemented)

- `AT+MTOTA=<url>` (or an `AT+`-namespaced HTTP-download command): fetch a
  firmware image over the C6's WiFi and stream it to the host for a
  host-driven serial-flash update. See `FIRMWARE_UPDATE_SPEC.md`.
- `AT+MTATTRX`: read/write array, octet-string and character-string attributes,
  hex-encoded. Specified in the design spec; implemented with the first device
  type that needs it.
- `+MTERR` codes `2` to `5` for specific attribute faults (phase C2).
- Additional device types for the table in §3.9 (phase C-later).

## 9. Relationship to `AT+EN`

`AT+MT` and `AT+EN` share the `at_core` engine, transport, framing and error
conventions, and disjoint command namespaces. Only one personality is flashed
at a time; the host reflashes the C6 to switch. This keeps a future single
binary (registering both command tables) a mechanical merge. See the
[ESP-NOW + Matter integration plan](https://github.com/PontusO/iLabs_AT_ESP-now/blob/main/docs/matter-integration-plan.md).
