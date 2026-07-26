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
| `AT+MTRESET` | exec | `OK` → factory reset + reboot |
| `AT+MTATTR=<ep>,<cl>,<attr>` | set (3) | `+MTATTR:<ep>,<cl>,<attr>,<val>` → `OK` (read) |
| `AT+MTATTR=<ep>,<cl>,<attr>,<val>` | set (4) | `OK` (write) |

## 3. Command reference

### 3.1 Identity — `AT+CGMI` / `AT+CGMM` / `AT+CGMR`
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
`+MTFABRICS:<count>` — number of commissioned fabrics.

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

### 3.7 `AT+MTRESET`
Factory-resets the device (erases all Matter data: fabrics, credentials,
attribute persistence) and reboots. Emits `OK`, then reboots; the host
resynchronizes on the next `+MTREADY`. After reset the device is factory-fresh
and opens a commissioning window automatically.

### 3.8 `AT+MTATTR` — data model access
Read or write a single Matter attribute.
- **Read:** `AT+MTATTR=<ep>,<cluster>,<attr>` (3 params)
  → `+MTATTR:<ep>,<cluster>,<attr>,<val>` → `OK`
- **Write:** `AT+MTATTR=<ep>,<cluster>,<attr>,<val>` (4 params) → `OK`

`<cluster>` and `<attr>` accept hex (`0x0006`) or decimal. `<val>` is an
integer; it is interpreted according to the attribute's own type
(bool/enum/bitmap/intN/uintN). String/array/float attributes are not
supported over this command (returns `ERROR`).

A write echoes a `+MTATTR` URC (the attribute callback confirming the change),
then `OK`. A controller-driven change to the light endpoint also raises a
`+MTATTR` URC (§4) so the host observes external state changes.

Example — the on/off light (endpoint 1, OnOff cluster `0x0006`, attribute `0x0000`):
```
AT+MTATTR=1,6,0        -> +MTATTR:1,6,0,0   (read: off)
AT+MTATTR=1,6,0,1      -> OK                 (turn on)
AT+MTATTR=1,6,0        -> +MTATTR:1,6,0,1
```

## 4. Unsolicited result codes (URCs)

| URC | Meaning |
|---|---|
| `+MTREADY` | AT interface up after boot or `AT+MTRESET`. |
| `+MTCOMMISSION:STARTED` | A commissioning window opened. |
| `+MTCOMMISSION:COMPLETE` | Commissioning finished successfully. |
| `+MTCOMMISSION:FAILED` | Commissioning failed (fail-safe timer expired). |
| `+MTATTR:<ep>,<cluster>,<attr>,<val>` | An attribute on the light endpoint changed (controller-driven or local). The root endpoint (0) is intentionally not reported, to keep boot-time init noise off the link. |

## 5. Error codes — `+MTERR:<n>`

On a specific fault the firmware prints `+MTERR:<n>` then `ERROR`. Generic
faults print a bare `ERROR` (no `+MTERR` line).

| Code | Meaning |
|---|---|
| `8` | Unknown / unsupported command (version-skew detection). |
| (bare `ERROR`) | Bad parameters, wrong command form, or a runtime failure. |

**Code-space policy (integration-plan contract C2):** `+MTERR` and `+ENERR`
have distinct prefixes, so each may use the `1–99` range independently; codes
`≥ 100` map to a bare `ERROR`. As `AT+MT` faults gain specific codes they are
allocated here and kept semantically stable for a future merged binary.

## 6. Commissioning credentials

- **Development / bring-up:** esp-matter test credentials — discriminator
  `3840`, setup passcode `20202021`. `AT+MTCODES?` returns the matching QR and
  manual codes. Suitable for `chip-tool` and phone controllers on a test fabric.
- **Production:** per-device Device Attestation Certificate + passcode
  provisioned into `fctry` / `esp_secure_cert` (via `esp-matter-mfg-tool`).
  Manufacturing flow: TBD.

## 7. Data model (v1)

- Transport: **Matter-over-WiFi** (2.4 GHz). BLE is used for commissioning only.
- One endpoint: an **on/off light** on endpoint `1` (OnOff cluster `0x0006`).
  Endpoint `0` is the mandatory Root Node.
- `AT+MTATTR` provides generic integer attribute read/write across the data
  model.

## 8. Planned / reserved (not yet implemented)

- `AT+MTOTA=<url>` (or an `AT+`-namespaced HTTP-download command): fetch a
  firmware image over the C6's WiFi and stream it to the host for a
  host-driven serial-flash update. See `FIRMWARE_UPDATE_SPEC.md`.
- `AT+MTEP=<devtype>`: create additional endpoints at runtime.
- Additional `+MTERR` codes for specific Matter faults.

## 9. Relationship to `AT+EN`

`AT+MT` and `AT+EN` share the `at_core` engine, transport, framing and error
conventions, and disjoint command namespaces. Only one personality is flashed
at a time; the host reflashes the C6 to switch. This keeps a future single
binary (registering both command tables) a mechanical merge. See the
[ESP-NOW + Matter integration plan](https://github.com/PontusO/iLabs_AT_ESP-now/blob/main/docs/matter-integration-plan.md).
