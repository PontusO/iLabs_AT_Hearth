# AT+MT Command Specification (iLabs AT Hearth, ESP32-C6)

Status: **draft, tracks the implementation** (Phase B4). This is the host ↔ C6
contract for the Matter personality. It mirrors the ESP-NOW firmware's `AT+EN`
conventions (same `at_core` engine and transport) so a host that speaks one
speaks the other; the two namespaces are disjoint and only one personality is
flashed at a time (mode-switch by host reflash).

## 1. Transport & line conventions

- **Link:** UART, `115200 8N1`, no flow control. The AT link is on the C6's
  UART1 (host-bridge pins GPIO16/17); the console/logs live on a separate
  UART (GPIO2). See `ARCHITECTURE.md`. The rate is changeable at runtime with
  `AT+MTBAUD` (§3.13) and is not persisted, so every reset returns the link to
  115200. Hardware flow control has a command (`AT+MTFLOW`, §3.14) but no
  wiring on any board built so far.
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
  up (after boot or after `AT+MTRESET`), so the host can resynchronize. It is
  the **first line of a new session**: no URC can precede it. URCs raised
  before the interface is up are dropped rather than queued, because the host
  is not listening yet and resynchronizes on the marker. A URC arriving ahead
  of `+MTREADY` is a firmware bug, not something a host must tolerate.
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
| `AT+MTCOMMISSION[=<timeout_s>]` | exec/set | `OK` (window opened; commissioning event bits) |
| `AT+MTCODES?` | query | `+MTCODES:<qr>,<manual>` → `OK` |
| `AT+MTRESET` | exec | `OK` → Matter reset (fabrics/credentials) + reboot |
| `AT+MTFRESET` | exec | `OK` → full factory reset (adds the composition) + reboot |
| `AT+MTATTR=<ep>,<cl>,<attr>` | set (3) | `+MTATTR:<ep>,<cl>,<attr>,<val>` → `OK` (read) |
| `AT+MTATTR=<ep>,<cl>,<attr>,<val>` | set (4) | `OK` (write) |
| `AT+MTSWITCH=<ep>[,<action>]` | set | `OK` (switch event emitted) |
| `AT+MTTEMPLEVELS=<ep>,"<l1>"[,...]` | set | `OK` (temperature level labels stored) |
| `AT+MTEP?` | query | `+MTEP:<idx>,<ep_id>,<devtype>[,<variant>]` per endpoint → `OK` |
| `AT+MTEP=<devtype>[,<variant>]` | set | `OK` (append to the staged composition) |
| `AT+MTEPCLEAR` | exec | `OK` (begin staging an empty composition) |
| `AT+MTEPAPPLY` | exec | `OK` → persist + reboot |
| `AT+MTEVT?` | query | `+MTEVTMASK:<hex32>` → `OK` |
| `AT+MTEVT=<hexmask>` | set | `OK` (subscribe to platform events) |
| `AT+MTNET?` | query | `+MTNET:<transport>,<enabled>,<connected>` → `OK` |
| `AT+MTTRANSPORT?` | query | `+MTTRANSPORT:<active>,<stored>` → `OK` (combined image only) |
| `AT+MTTRANSPORT=<WIFI\|THREAD>` | set | `OK` (persisted, no reboot; combined image only) |
| `AT+MTBAUD?` | query | `+MTBAUD:<baud>` → `OK` |
| `AT+MTBAUD=<baud>` | set | `OK` at the old rate, then the link switches |
| `AT+MTFLOW?` | query | `+MTFLOW:<mode>` → `OK` |
| `AT+MTFLOW=<mode>` | set | `OK` at the old setting, then the link switches |
| `AT+MTVALVE=<ep>,<state>[,<level>]` | set | `OK` (valve state/level reported) |
| `AT+MTMODES=<ep>,<mode>,"<label>"[,...]` | set | `OK` (ModeSelect SupportedModes list stored) |
| `AT+MTOPSTATE=<ep>,<state>` | set | `OK` (OperationalState transition reported) |
| `AT+MTALARM=<ep>,<field>,<value>` | set | `OK` (SmokeCoAlarm state field reported) |
| `AT+MTCHIMESOUNDS=<ep>,<id>,"<name>"[,...]` | set | `OK` (Chime InstalledChimeSounds list stored) |
| `AT+MTCHIME=<ep>,<what>,<value>` | set | `OK` (SelectedChime/Enabled set) |

## 3. Command reference

### 3.1 Identity: `AT+CGMI` / `AT+CGMM` / `AT+CGMR`
LTE-modem-style (3GPP TS 27.007) execute commands. Each prints one identity
line then `OK`: manufacturer (`iLabs Electronics`), model (`ESP32-C6 Hearth`),
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
- `<timeout_s>`: optional window lifetime, 180–900 s (default 300). The floor
  is Matter's 3-minute minimum announcement duration, which CHIP enforces
  (`CommissioningWindowManager::MinCommissioningTimeout`); values below it are
  rejected with `+MTERR:1`. Until 2026-07-30 this spec said 30, which the AT
  layer accepted and CHIP then refused at runtime, so 30–179 returned a bare
  `ERROR` plus a spurious `+MTEVT:4`.
- Returns `OK` once the window request is accepted.
- Progress is reported asynchronously via the commissioning event bits 0, 3 and 5 (§3.11), which are in the default event mask.
- Note: a factory-fresh device opens a window automatically at boot; this
  command is for (re)opening one on an already-commissioned device.

> #### DEFECT D1: on a commissioned device this command succeeded and did nothing
>
> **Status: FIXED 2026-07-29.** Found on hardware 2026-07-28; affected every
> build up to and including tag `0.1.0`. Kept on record rather than deleted,
> because the failure mode (a command reporting success in every observable
> way while doing nothing) is worth recognising if it recurs.
>
> The command reported success in full. It returned `OK`, raised `+MTEVT:0`,
> and `AT+MTSTATE?` reported `1` (commissioning). CHIP really had opened a
> window in its own bookkeeping. **Nothing was advertised.** A commissioner
> could not see the device, and the host had no way to tell the difference.
>
> Cause: `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=y`, whose documented behaviour
> is to deinitialise BLE on successful commissioning and to skip BLE init at
> boot when the device is already provisioned. `BLEManagerImpl` calls
> `esp_bt_mem_release`, handing the controller memory back to the heap, so
> when CHIP's BLE state machine later decides to advertise there is no
> controller to advertise with. It logs `bleAdv Timeout : Start slow
> advertisement` and NimBLE logs nothing at all, which is the tell: NimBLE
> announces every GAP procedure it actually performs.
>
> DNS-SD cannot cover for it either. On a device whose transport has no
> network yet the mDNS publish fails too:
> `chip[DIS]: Failed to advertise commissionable node: 3`.
>
> **This is exactly the case the note above says the command exists for**, so
> the command is specified for a job the build configuration makes impossible.
>
> Fix: `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=n` in `sdkconfig.defaults`,
> keeping the BLE stack resident so a window can genuinely be reopened.
>
> The cost was measured on hardware rather than estimated: **35,800 bytes**,
> leaving 173 KB of free heap with BLE resident against 209 KB without. Cheap,
> and not a speculative configuration: the device already ran in exactly this
> state for the first seconds of every boot and throughout every commissioning.
> The firmware now logs both figures on every boot (`kBLEDeinitialized` in
> `main.cpp`) so the trade can be re-checked after an SDK bump instead of taken
> on trust. The image also shrank by 1,504 bytes, since the teardown path
> compiles out.
>
> Consequence: the BLE radio stays initialised for the life of the device. It
> does not advertise unless a window is open, but it is powered.
>
> `AT+MTFRESET` was the workaround before the fix, at the cost of the fabric
> and the composition. Note the counter-argument: standard Matter
> multi-admin adds a controller over the *operational* network rather than
> BLE, so an ordinary device does not need BLE after commissioning. This is
> not an ordinary device, since the host can change its transport underneath
> it and `AT+MTCOMMISSION` exists precisely so the host can demand a window.
>
> Decide together with open question P2 (§12.1 of the design spec). Both ask
> the same question: what should this device do when it believes it is
> commissioned but cannot actually be reached?

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

Nullable numeric attributes (for example `CurrentLevel` or
`TargetPositionLiftPercent100ths`) are supported the same way as their
non-nullable counterparts. The one exception is reading one that currently
holds null: the AT grammar has no null literal, so that read reports
`+MTERR:5`, indistinguishable from an unsupported type. The write path here
does not add a way to write null on purpose, but Matter's own nullable
encoding reserves one sentinel value per type to mean null (255 for a u8,
the type minimum for a signed type, and so on). A host that writes that
exact sentinel gets a null back, same as if a controller had written it, and
the next read then answers `+MTERR:5` for that reason.

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
  `+MTEP:<index>,<endpoint_id>,<device_type>[,<variant>]`. The fourth field
  appears **only when the variant is nonzero**, so a host that has never seen
  a variant reads byte-identical output to before this field existed. Zero
  lines means the device is unconfigured. It never reports a staged
  composition.
- `AT+MTEPCLEAR` opens a staging session holding an empty composition.
- `AT+MTEP=<device_type>[,<variant>]` appends one endpoint. `<device_type>` is
  a standard Matter device type ID, hex or decimal. `<variant>` is an optional
  per-device-type sub-selector, decimal, defaulting to `0`; every device type
  in the table below has a `<variant>` range of `0` only except the
  Temperature Controlled Cabinet, where `0` and `1` pick two structurally
  different clusters (see its row's note). Rejected with `+MTERR:10` outside a
  staging session or past the 16-endpoint cap, with `+MTERR:6` for a device
  type this firmware does not implement, and with `+MTERR:1` for a `<variant>`
  the device type does not define (including a value that does not parse as
  an unsigned integer). A rejected append consumes no slot.
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

Supported device types. `AT+MTEP=<id>` accepts any ID in this table; any ID
outside it is rejected with `+MTERR:6`. This is the firmware's
`main/mt_devtypes.cpp` table, rendered here, and it grows by rows as new
device types are added.

| ID (hex) | Device type |
|---|---|
| `0x0100` | On/Off Light |
| `0x0101` | Dimmable Light |
| `0x010C` | Colour Temperature Light |
| `0x0302` | Temperature Sensor |
| `0x010A` | On/Off Plug-in Unit |
| `0x010B` | Dimmable Plug-in Unit |
| `0x0015` | Contact Sensor |
| `0x0107` | Occupancy Sensor |
| `0x0307` | Humidity Sensor |
| `0x0305` | Pressure Sensor |
| `0x0044` | Rain Sensor |
| `0x0041` | Water Freeze Detector |
| `0x0043` | Water Leak Detector |
| `0x002B` | Fan |
| `0x0202` | Window Covering |
| `0x0301` | Thermostat |
| `0x010D` | Extended Colour Light |
| `0x000F` | Generic Switch |
| `0x0071` | Temperature Controlled Cabinet |
| `0x000A` | Door Lock |
| `0x0106` | Light Sensor |
| `0x0306` | Flow Sensor |
| `0x002C` | Air Quality Sensor |
| `0x010F` | Mounted On/Off Control |
| `0x0110` | Mounted Dimmable Load Control |
| `0x002D` | Air Purifier |
| `0x007A` | Extractor Hood |
| `0x0072` | Room Air Conditioner |
| `0x0078` | Cooktop |
| `0x0303` | Pump |
| `0x0042` | Water Valve |
| `0x0027` | Mode Select |
| `0x0073` | Laundry Washer |
| `0x0075` | Dishwasher |
| `0x007C` | Laundry Dryer |
| `0x0076` | Smoke/CO Alarm |
| `0x0011` | Power Source |
| `0x0146` | Chime |

`0x010D` Extended Colour Light diverges from stock esp-matter: the firmware
bolts the HueSaturation feature onto the standard ColorControl configuration,
so a host sees `CurrentHue` and `CurrentSaturation` alongside the XY and
colour-temperature (mireds) attributes esp-matter enables by default. All
three colour representations are live on the same endpoint at once;
`AT+MTATTR` reaches whichever one a controller actually wrote.

`0x000F` Generic Switch has no writable attribute. `switch_cluster::create()`
unconditionally creates `NumberOfPositions` (default `2`) and
`CurrentPosition` (default `0`), both read-only, so a host can still read
`CurrentPosition` over `AT+MTATTR` (cluster `0x003B`) like any other
attribute; there is just nothing on this device type `AT+MTATTR` can write.
The command that drives it is `AT+MTSWITCH` (§3.15), which raises a Switch
cluster event rather than writing state.

`0x0071` Temperature Controlled Cabinet is the first device type with a
non-trivial variant. `AT+MTEP=0x0071` (variant `0`) builds a TemperatureNumber
cabinet: `TemperatureSetpoint`, `MinTemperature`, `MaxTemperature` and `Step`
are ordinary attributes reachable over `AT+MTATTR`, like any other numeric
device type. `AT+MTEP=0x0071,1` builds a TemperatureLevel cabinet instead: the
device exposes `SelectedTemperatureLevel` (an ordinary `u8`, also
`AT+MTATTR`-reachable) alongside `SupportedTemperatureLevels`, a list of
strings that is not an `AT+MTATTR` attribute at all. Its content is set with
`AT+MTTEMPLEVELS` (§3.16). The two variants are mutually exclusive at the CHIP
level: a cabinet endpoint always has exactly one of them, never both and never
neither.

`0x000A` Door Lock is Hearth's first device type beyond arduino-esp32 parity:
upstream has no door lock class or example. Its `LockDoor`/`UnlockDoor`
commands are **not** answered directly from the data model: they are
forwarded to the host for adjudication (§3.17) and the host reports the
outcome back with `AT+MTLOCK` (§3.18). `AT+MTATTR` still reads `LockState`
(cluster `0x0101`, attribute `0x0000`) like any other attribute; it is only
the two commands that leave the ordinary `AT+MTATTR` path. Feature map `0`:
no PIN/user/credential (COTA) surface in this round, so controllers may only
send bare `LockDoor`/`UnlockDoor`, never a PIN-carrying command.

`0x002C` Air Quality Sensor's `AirQuality` attribute (cluster `0x005B`,
attribute `0x0000`) is served through a per-endpoint CHIP server instance
rather than esp-matter's generic attribute store (bug B139: esp-matter marks
the attribute managed-internally and nothing behind it answers a read).
`AT+MTATTR` behaves identically on the wire for reads and for the value
grammar: an integer `0`-`6`, exactly like any other attribute. One
exception, hardware-verified: a write to this attribute never echoes the
`+MTATTR` URC, in either mode. The echo comes from esp-matter's attribute
callback, which managed-internally attributes bypass; the server instance
reports the change to fabric subscribers instead, so controllers still see
it, but the host must rely on the `OK` alone. The `<mode>` field is
accepted and has no effect here.

`0x0076` Smoke/CO Alarm enables both the `SmokeAlarm` and `CoAlarm` feature
bits, so both `SmokeState` and `COState` exist on every endpoint of this
type; there is no variant to pick one over the other. `AT+MTALARM` (§3.22)
is the only way to drive its state: the cluster's eleven `Set*` methods fire
the spec's events and the critical-alarm auto-unmute, which a raw
`AT+MTATTR` write to the same ember-managed storage would silently skip, so
`AT+MTATTR` is not the intended path to this cluster's state even though
some of its attributes are, mechanically, plain integers.

`0x0011` Power Source is a **flat sibling**, not a device composed onto
another endpoint: the Smoke/CO Alarm device type's Matter specification
mandates a power source be present somewhere on the node, and that mandate
is node-scoped, not endpoint-scoped, so a standalone Power Source endpoint
elsewhere in the composition satisfies it with no composition tree needed
(design spec decision log, 2026-08-07). The endpoint enables the `Battery`
feature only (`Wired`/`Battery` are mutually exclusive, spec F5), which
publishes `BatChargeLevel`, `BatReplacementNeeded` and `BatReplaceability`
alongside the base `Status`/`Order`/`Description`, all ordinary
`AT+MTATTR`-reachable integers. `BatPercentRemaining` is hand-added by the
thunk (`0`-`200` in half-percent steps per the Matter spec, nullable,
defaulting to null) since no endpoint-creation path in this SDK revision
wires it on its own; it too is a plain `AT+MTATTR` attribute once added.

`0x0146` Chime carries an SDK-gap workaround, not a firmware design choice:
`ESPMatterChimeClusterServerInitCallback`, the one function that actually
constructs and registers this cluster's server object with esp-matter's data
model provider, has no call site anywhere in the pinned esp-matter/
connectedhomeip tree (`cluster::chime::create()` wires only the delegate
stash). The firmware calls it itself, in the same pre-start registration
window the `AT+MTTEMPLEVELS` delegate and the Air Quality Sensor's server
instances use (`main.cpp`); see the round's decision record in
`ARCHITECTURE.md` for the full trail. `InstalledChimeSounds` and
`SelectedChime`/`Enabled` are set with the dedicated `AT+MTCHIMESOUNDS`
(§3.23) and `AT+MTCHIME` (§3.24) commands, not `AT+MTATTR`: this cluster is
registered directly with the data model provider rather than through
esp-matter's generic attribute store, so there is no `AT+MTATTR` path to
either attribute. `PlayChimeSound` forwards to the host over `+MTCMD` with
the reserved fifth payload field carrying the requested `chimeID` (§3.17);
unlike the water valve (§3.19) or the OperationalState trio (§3.21), the
host's verdict reaches the controller exactly as given, `Status::Success` on
allow or `Status::Failure` on deny, with no SDK-side remapping.

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

**On `build_combined` (§3.12.2), "credentials" in the table means the active
transport's.** The Matter fabric is always erased, by both resets, regardless
of which stack is running. The network credentials are not symmetric: CHIP's
per-transport erase call only reaches the stack that is actually initialized,
so a reset issued while Thread is active clears the Thread network key and
leaves any previously stored WiFi SSID/PSK exactly as they were, and a reset
issued while WiFi is active leaves a previously stored Thread network key
untouched in the same way. A host that commissioned a device over WiFi,
switched it to Thread with `AT+MTTRANSPORT`, and then issued `AT+MTRESET`
should not assume the WiFi credentials are gone: they are not, until a reset
is issued back on WiFi. This asymmetry does not exist on `build_b4` or
`build_thread`, which have only one transport's credentials to erase in the
first place.

Note that `AT+MTFRESET` is deliberately a separate command rather than a
parameter on `AT+MTRESET`. A form like `AT+MTRESET=1` would give meaning to an
input that is currently required to be rejected, and that rejection is an
intentional tripwire on the most destructive command in the set.

### 3.11 `AT+MTEVT`: platform event subscription

```
AT+MTEVT?           ->  +MTEVTMASK:0x0800003F
AT+MTEVT=<hexmask>  ->  OK
```

The firmware surfaces CHIP platform events as `+MTEVT:<bit>[,<detail>]`, but
only for bits the host has subscribed to. Default mask `0x0800003F`: the
commissioning group (bits 0-5), which reproduces exactly what the firmware
emitted before the mask existed, plus bit 27.

**The mask is not persistent.** It lives in RAM and resets to the default on
every boot, which has a consequence worth stating outright: an event that only
ever fires *during* boot can never be subscribed to in time, because there is
no moment at which a host could set a mask that would catch it. Such an event
is either in the default mask or it is unreachable. Bit 27 (§3.12.1) is the
only one in this protocol, which is why it is in the default despite not being
part of the commissioning group. Any future boot-only event must join it.

| Bits | Group | Contents |
|---|---|---|
| 0–5 | Commissioning | `0` window opened, `1` session started, `2` session stopped, `3` complete, `4` window closed, `5` fail-safe expired |
| 6–9 | Fabric | `6` will be removed, `7` removed, `8` committed, `9` updated |
| 10–15 | Connectivity | `10` WiFi, `11` internet, `12` interface IP changed, `13` operational network started, `14` DNS-SD initialised, `15` server ready |
| 16–19 | BLE | `16` connected, `17` disconnected, `18` advertising change, `19` deinitialised |
| 20–23 | Misc | `20` OTA state, `21` bindings changed, `22` time sync, `23` reserved |
| 24–26 | Thread | `24` connectivity, `25` state change, `26` interface state change |
| 27 | Device state | `27` transport mismatch (§3.12.1) |
| 28–31 | Reserved | |

Bits `10`, `11` and `24` carry a `<detail>` of `1` or `0` for up and down.

Events `0` and `4` are a **pair**: exactly one `+MTEVT:4` is raised per
`+MTEVT:0` the host actually received, at the moment the window truly ends
(commissioning complete, window timeout, or the 20-failed-attempts limit).
CHIP's underlying "window closed" callback really means "stopped advertising
for PASE", and it also fires when a commissioner establishes a session (the
window is paused, not gone) and when a failed open cleans up a window that
never existed. Both are deliberately suppressed. Before 2026-07-30 both
leaked through: a successful commissioning showed two `+MTEVT:4`, and a
failed `AT+MTCOMMISSION` showed one with no matching `+MTEVT:0`. A window
that expires with no controller attach raises its single `+MTEVT:4` at the
timeout; event `5` (fail-safe expired) does not fire, because the fail-safe
only arms once a PASE session exists.

Thread bits are **allocated but never emitted while WiFi is the active
transport**, and do emit whenever Thread is active: transport is a build-time
choice on the two single-stack images and a boot-time choice on the combined
image (§3.12), and a host may subscribe to them harmlessly against any of the
three. Fixing the layout before it is published avoids renumbering later.

The query answers `+MTEVTMASK`, not `+MTEVT`, deliberately. URCs may arrive
between a command and its terminal response, so a `+MTEVT:<n>` reply would be
indistinguishable from an event landing at that moment.

### 3.12 `AT+MTNET?`: network transport

```
AT+MTNET?  ->  +MTNET:<transport>,<enabled>,<connected>[,<mismatch>]
```

- `<transport>`: `WIFI` or `THREAD`.
- `<enabled>`: `1` when the transport is compiled in and started.
- `<connected>`: `1` when the operational network is up.
- `<mismatch>`: `1` when a stored fabric was commissioned on a *different*
  transport than this image provides (§3.12.1). Optional trailing field: a
  host that parses the first three and ignores extras stays correct, which is
  why it was added at the end rather than inserted.

Transport is fixed at build time on the two single-stack images (`build_b4`,
`build_thread`). Matter-over-Thread is a Kconfig option
(`ENABLE_MATTER_OVER_THREAD`, gated on `OPENTHREAD_ENABLED`), so those two
variants are separate images selected by reflash, the same model used for the
ESP-NOW and Matter personalities. There is deliberately no command to change it
on those images: the Root Node's NetworkCommissioning cluster advertises the
transport's features, so it is part of the data model, and changing it on a
commissioned device has the same consequences as changing the endpoint
composition (§3.9).

**On the combined image** (`build_combined`, §3.12.2) both stacks are compiled
in, and exactly one is active per boot. `<transport>` here reports the
**active** stack, not a build-time constant, so a host or the regression
harness reading `AT+MTNET?` needs no change to work against either image.
`AT+MTTRANSPORT` (§3.12.2) is the command that changes which stack is active,
and it exists only on this image for exactly the reason above: changing the
active transport carries the same commissioned-device consequences that
changing the composition does.

#### 3.12.1 Transport mismatch: a fabric from the other image

**Status: implemented and hardware-verified 2026-07-29.** Depended on defect
D1 (§3.5), fixed the same day, without which a window opened in this state
would advertise to nobody.

Verified against a device commissioned over Thread, with its WiFi credentials
erased and verified absent, then reflashed to the WiFi image. It reported
`+MTREADY`, `+MTEVT:0`, `+MTEVT:27`, and:

```
AT+MTNET?      ->  +MTNET:WIFI,1,0,1     mismatch flagged, not connected
AT+MTFABRICS?  ->  +MTFABRICS:1          fabric preserved, nothing erased
AT+MTEP?       ->  +MTEP:0,1,0x0100      composition preserved
AT+MTSTATE?    ->  +MTSTATE:1,1          window OPEN while holding a fabric
```

The last line is the load-bearing one: CHIP had already logged `Fabric already
commissioned. Disabling BLE advertisement`, so a commissioning window open on a
device with one fabric is the policy overriding it.

Reflashing between the WiFi and Thread images leaves NVS untouched by design
(`ARCHITECTURE.md` §4, verified on hardware), so a device commissioned under
one image still holds that fabric under the other. Fabric credentials are
transport-independent in Matter, so nothing about them is invalid. They are
simply unreachable: the device has no credentials for the new transport, so it
joins no network, and a commissioner cannot deliver any because it cannot
reach the device.

On the combined image (§3.12.2) the identical state is reachable without a
reflash: `AT+MTTRANSPORT=` followed by a reboot changes which stack is active
exactly as a reflash would, and the mismatch detection, `+MTEVT:27`, and the
`AT+MTNET?` `<mismatch>` flag behave the same either way, since both routes
end at the same condition, a fabric present on a transport that is not
provisioned.

Observed on hardware 2026-07-28, and the failure is the worst available shape.
CHIP finds the fabric, logs `Fabric already commissioned. Disabling BLE
advertisement`, and tears BLE down. The device then reports itself commissioned
through `AT+MTSTATE?` while being reachable by no route at all: no network, no
BLE, no mDNS. It looks configured and is not.

**Detection.** The firmware asks CHIP whether this transport is provisioned
(`IsWiFiStationProvisioned()` / `IsThreadProvisioned()`) and treats a device
holding a fabric on an unprovisioned transport as unreachable.

An earlier design recorded which transport a fabric was commissioned on and
compared it at boot. That was replaced after hardware showed it answers the
wrong question. It asks "where did this fabric come from" when what matters is
"can this device reach a network". A device commissioned over Thread and then
reflashed to the WiFi image joined WiFi perfectly well, on credentials left by
an earlier WiFi commissioning, and served its Thread-era fabric over mDNS. A
marker would have called that a mismatch and opened a pointless window on a
working device. Fabric credentials are transport-independent; **provisioning is
the thing that is not**.

The check also needs no storage of its own, so it cannot drift, and it is
correct on a device upgraded to this firmware without any migration step.

On a mismatch the firmware **erases nothing**:

- the fabric is kept, so reflashing the original image restores a working
  commissioned device, which matters because flipping images is routine during
  development, and because the credentials are not actually invalid;
- the composition is kept, since it is transport-independent and is a product
  definition rather than user data (the same reasoning that makes it survive
  `AT+MTRESET`, §3.8);
- `mt_boot_window_policy()` treats the device as **not commissioned for the
  purpose of the boot commissioning window**, so it advertises and can be
  commissioned onto the new transport;
- `+MTEVT:27` is raised, and `AT+MTNET?` reports `<mismatch>` as `1` so a host
  that connected later can still discover the condition;
- the condition clears on `kCommissioningComplete`, since commissioning on
  this transport is precisely what provisions it.

The alternative of auto-erasing on mismatch was considered and rejected: it
destroys a working fabric on any transport flip, including a brief one, and
"looks configured but is not" is a reporting problem rather than a reason to
delete a user's commissioning.

**This resolves open question P2** (design spec §12.1), which asked whether the
boot commissioning window can be suppressed on an unconfigured device. The
question was the wrong way round. What matters is not whether a window can be
suppressed but that "configured" was the wrong predicate: it must mean *has a
fabric usable on this transport*, not merely *has a fabric*.

Blocked on D1 because a device in this state has already had BLE released, so
opening a window achieves nothing until BLE stays resident.

### 3.12.2 `AT+MTTRANSPORT`: transport selection (combined image only)

```
AT+MTTRANSPORT?               ->  +MTTRANSPORT:<active>,<stored>
AT+MTTRANSPORT=<WIFI|THREAD>  ->  OK
```

Exists only on `build_combined`, the one image carrying both the WiFi and the
Thread stack with exactly one active per boot. `build_b4` and `build_thread`
never register this command, so `AT+MTTRANSPORT` there reaches the ordinary
"unknown command" path and answers `+MTERR:8`, the same as any other command
this firmware does not implement; there is no combined-image-only stub that
always reports one fixed transport.

- `<active>`: `WIFI` or `THREAD`, the stack this boot actually launched.
  Latched once at boot, before `esp_matter::start()`, from whatever was
  stored at that moment: it does not change while the device is running,
  even if `AT+MTTRANSPORT=` stages a different value in the meantime.
- `<stored>`: the persisted choice, which takes effect on the next boot.
  Equal to `<active>` except while a switch is staged and not yet applied.
- `AT+MTTRANSPORT=<WIFI|THREAD>` validates the argument (`+MTERR:1` for
  anything else), persists it to NVS, and answers `OK`. **It has no reboot
  side effect.** Unlike `AT+MTEPAPPLY`, which stages-then-reboots the
  composition, this command only writes the choice; the host owns the
  reboot (`AT+MTRESET`, `AT+MTFRESET`, the reset line, or power), the same
  way `AT+MTEP=`/`AT+MTEPCLEAR` stage without reloading endpoints. An NVS
  write failure answers `+MTERR:7`.

**Persistence.** The stored choice survives both `AT+MTRESET` and
`AT+MTFRESET`, like the endpoint composition (§3.9, §3.10): it is a product
setting, not user data, and neither reset is a reason to fall back to WIFI on
a device deliberately configured for Thread.

**Switching with a live fabric is always allowed**, and deliberately not
gated on commissioning state. The transport-mismatch flow (§3.12.1) already
covers what happens next: the fabric is kept, a commissioning window opens
because the newly active transport cannot reach it yet, and `AT+MTNET?`
reports the mismatch until the device is commissioned on the transport it
now runs. Switching back revives the original fabric with nothing to redo.
Refusing to switch until a factory reset was considered and rejected for the
same reason §3.12.1 rejected erasing on mismatch.

Fresh-device default: `WIFI`, matching the single-stack images' shipping
behaviour and the C4 host library's expectations.

### 3.13 `AT+MTBAUD`: AT link rate

```
AT+MTBAUD?         ->  +MTBAUD:<baud>
AT+MTBAUD=<baud>   ->  OK, then the link switches
```

Accepted rates, the same table the ESP-NOW image's `AT+ENBAUD` uses:

```
1200 2400 4800 9600 19200 38400 57600 115200 230400 460800 921600
```

Anything else is `+MTERR:1`.

The `OK` goes out at the **old** rate: the switch drains TX first, so the
acknowledgement is complete before the divisor changes. The host reconfigures
its own side after seeing it. A URC that happens to land in that gap also goes
out at the old rate, for the same reason.

**The rate is not persisted.** It lives in RAM, so a reset (`AT+MTRESET`,
`AT+MTEPAPPLY`, the reset line, power) returns the link to 115200. That is
what gives a host that loses sync a guaranteed way back: pulse reset and
resynchronize on `+MTREADY`. Persisting the rate would make one bad switch
unrecoverable without a serial-download reflash.

### 3.14 `AT+MTFLOW`: hardware flow control

```
AT+MTFLOW?         ->  +MTFLOW:<mode>
AT+MTFLOW=<mode>   ->  OK, then the link switches
```

| Mode | Meaning |
|---|---|
| `0` | none, three-wire TX/RX/GND |
| `1` | RTS only: the C6 signals the host to pause |
| `2` | CTS only: the host signals the C6 to pause |
| `3` | full RTS/CTS |

`OK` goes out at the old setting, for the same reason as `AT+MTBAUD`, and
mattering more here: enabling CTS can gate this device's own transmitter.

**No board built so far wires RTS/CTS to the C6, so only mode `0` is
accepted; `1` to `3` answer `+MTERR:1`.** The shared `at_core` can drive them
(`AT_UART_RTS_PIN` = GPIO19, `AT_UART_CTS_PIN` = GPIO18 on C6) and the ESP-NOW
firmware exposes the full range through `AT+ENFLOW`, but on the Challenger
RP2350 WiFi6/BLE5 and the Slice RP2350 WiFi6 the ESP32-C6-MINI-1-H4 uses only
`RXD0`/`TXD0`, the esp-hosted SPI, boot, strapping, USB and `EN`. GPIO18 and
GPIO19 are not connected. The pair *is* routed on the older ESP32-C3
Connectivity board, which is where `at_core`'s pin assignment comes from.

Accepting a mode the board cannot honour would not degrade gracefully: it
would gate the transmitter on an unbonded input and the link would stop
mid-answer, needing a reset. `MT_UART_FLOWCTRL_WIRED` in `mt_at_config.h` is
the one macro a board revision that routes the pair has to flip.

### 3.15 `AT+MTSWITCH`: switch event emission

Emits a Switch cluster event on `<ep>` rather than writing an attribute: the
first command on this surface that raises an event instead of changing state.

```
AT+MTSWITCH=<ep>[,<action>]  ->  OK
```

- `<ep>`: the endpoint id.
- `<action>`: optional, default `0`. `0` is InitialPress at position `1`,
  matching the upstream arduino-esp32 class's `click()`. Any other value is
  reserved for the switch's richer feature set (MultiPress, LongPress, and so
  on, none implemented yet) and answers `+MTERR:1`.

**Set-only.** `AT+MTSWITCH?` or a bare `AT+MTSWITCH` is the wrong command
form and answers a plain `ERROR` (§5), the same convention every other
set-only command in this spec follows.

A failed access reports which level was wrong, the same two codes
`AT+MTATTR` uses for the same lookups: `+MTERR:2` unknown endpoint,
`+MTERR:3` an endpoint without a Switch cluster. A send failure at the
esp_matter/CHIP layer itself (the event fails to queue, for example) is not
one of the specific faults and answers a bare `ERROR`, §5's "unclassified
runtime failure" case, the same as an unexpected `AT+MTATTR` write failure.

**Events are fire-and-forget.** `AT+MTSWITCH` only tells the firmware to
raise the InitialPress event toward whichever controllers are subscribed to
it on this endpoint. It never echoes as a `+MTATTR` or any other URC on the
AT link, and there is no command that reads an event back afterward: `OK`
means the event was handed to the Matter stack, not that any controller
received or acted on it.

Example, a generic switch on endpoint 3:
```
AT+MTSWITCH=3     -> OK         (InitialPress, position 1)
AT+MTSWITCH=3,0   -> OK         (equivalent, explicit default)
AT+MTSWITCH=3,1   -> +MTERR:1   (reserved action)
AT+MTSWITCH=9     -> +MTERR:2   (no endpoint 9)
```

### 3.16 `AT+MTTEMPLEVELS`: temperature level labels

Stores the label list backing a TemperatureLevel-variant cabinet's
`SupportedTemperatureLevels` attribute (§3.9, `AT+MTEP=0x0071,1`). Unlike
every other attribute in this firmware, `SupportedTemperatureLevels` is a list
of strings served by CHIP's own delegate mechanism, not by esp_matter's
attribute store, so it has no `AT+MTATTR` path at all: this is the only way a
host can set it.

```
AT+MTTEMPLEVELS=<ep>,"<label1>"[,"<label2>",...,"<labelN>"]  ->  OK
```

- `<ep>`: the endpoint id, a bare unsigned token (hex or decimal) ending at
  the first comma.
- `<label1>..<labelN>`: `1..16` double-quoted labels. Each label is `1..16`
  bytes of printable ASCII (`0x20`..`0x7E`) and may not contain a `"`
  character. A comma **inside** a quoted label is legal and is part of the
  label's text; a comma **between** labels separates them. Violating any of
  these (wrong count, empty or oversized label, non-printable byte, a bare
  label with no quotes, an unterminated quote, or anything trailing after a
  label's closing quote other than a comma or end of line) answers
  `+MTERR:1`.

**Set-only.** `AT+MTTEMPLEVELS?` or a bare `AT+MTTEMPLEVELS` is the wrong
command form and answers a plain `ERROR` (§5), the same convention
`AT+MTSWITCH` (§3.15) follows.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `TemperatureControl` cluster;
`+MTERR:4` the cluster is present but is a TemperatureNumber-variant cabinet,
so there is no `SupportedTemperatureLevels` attribute to set. A bare `ERROR`
covers an unclassified runtime failure, the same as `AT+MTATTR` and
`AT+MTSWITCH`.

**On success the stored labels replace whatever was there before**, and the
attribute is reported dirty so an active subscription sees the new list on
its next report. There is no append or partial-update form: each
`AT+MTTEMPLEVELS` call is a full replacement of the label list.

**Labels are not persisted.** The store lives in RAM and starts empty every
boot, the same policy the AT link rate (§3.13) and every attribute's runtime
value follow: `SupportedTemperatureLevels` is current display text a host
sets after `+MTREADY`, not a product definition like the endpoint composition
(§3.9), which does survive a reboot. A `SupportedTemperatureLevels` read
before the host has sent `AT+MTTEMPLEVELS` for that endpoint returns an empty
list.

Example, a TemperatureLevel cabinet on endpoint 4:
```
AT+MTTEMPLEVELS=4,"Low","Medium","High"  -> OK
AT+MTTEMPLEVELS=4,"Wine, red","Wine, white"  -> OK   (commas inside labels)
AT+MTTEMPLEVELS=4                        -> +MTERR:1 (no labels)
AT+MTTEMPLEVELS=4,Low                    -> +MTERR:1 (missing quotes)
AT+MTTEMPLEVELS=4,"Low","Lo\"w"          -> +MTERR:1 (quote inside a label)
AT+MTTEMPLEVELS=9,"Low"                  -> +MTERR:2 (no endpoint 9)
AT+MTTEMPLEVELS=1,"Low"                  -> +MTERR:3 (ep 1 has no TemperatureControl cluster)
AT+MTTEMPLEVELS=4,"Low"                  -> +MTERR:4 (ep 4 built as variant 0, TemperatureNumber)
```

### 3.17 Command forwarding: `+MTCMD` / `AT+MTCMDRESP`

Some Matter commands need an app-level decision the firmware cannot make on
its own; the Door Lock's `LockDoor`/`UnlockDoor` (§3.9, `0x000A`) are the
first consumers. The firmware forwards the command to the host over a URC
and waits for a verdict, rather than deciding on its own or refusing the
command outright. This is a generic frame: the door lock is its first
registered consumer, not the protocol's shape, so a future app-adjudicated
command reuses it rather than inventing a second one.

```
+MTCMD:<seq>,<ep>,<cluster>,<command>          (URC, decimal fields)
+MTCMD:<seq>,<ep>,<cluster>,<command>,<payload> (URC, decimal fields, reserved fifth field)
+MTCMD:0,<ep>,<cluster>,<command>              (URC, decimal fields, notify-only)
AT+MTCMDRESP=<seq>,<verdict>  ->  OK
+MTCMDTO:<seq>                                 (URC, only on a missed window)
```

- `+MTCMD:<seq>,<ep>,<cluster>,<command>`: raised when a controller invokes a
  command that needs adjudication. `<seq>` identifies this specific request;
  `<ep>` the endpoint; `<cluster>` and `<command>` the Matter cluster and
  command IDs, decimal, the same convention `AT+MTATTR` (§3.8) uses for its
  own `<cluster>`/`<attr>` fields. The door lock registers cluster `257`
  (`0x0101`) commands `0` (`LockDoor`) and `1` (`UnlockDoor`); the water
  valve (§3.19) registers cluster `129` (`0x0081`) commands `0` (`Open`) and
  `1` (`Close`) the same way, though per §3.19 its verdict never changes the
  wire response, only whether the host actually actuates. The OperationalState
  trio (§3.21, Laundry Washer / Dishwasher / Laundry Dryer) registers cluster
  `96` (`0x0060`) commands `0` (`Pause`), `1` (`Stop`), `2` (`Start`), `3`
  (`Resume`): unlike the valve, here the verdict IS the wire response (the
  SDK copies the filled error straight into the command's reply), so a deny
  genuinely fails the command, the same as the door lock.
- `+MTCMD:<seq>,<ep>,<cluster>,<command>,<payload>`: the same adjudicated
  form, plus one reserved fifth field for a command that carries a single
  value the host needs to see. `<payload>` is decimal, the same convention as
  the other fields. This is the extension the four-field form above always
  reserved: a host parser that reads exactly four fields and ignores anything
  past the fourth comma keeps working unchanged when a consumer adds the
  fifth. First consumer: chime's `PlayChimeSound` `chimeID`.
- `+MTCMD:0,<ep>,<cluster>,<command>` (notify-only): raised for a command
  whose ember callback has nothing to report back up the stack, so there is
  no verdict to wait for and none is requested. Sequence `0` is reserved for
  this form and is never issued by the adjudicated forms above; the firmware
  opens no mailbox slot, blocks nothing, and returns as soon as the line is
  queued. `AT+MTCMDRESP=0,...` always answers `+MTERR:1`, the same as any
  other seq the mailbox does not recognise as currently pending, because
  there is structurally nothing pending under seq `0` to answer. First
  consumer: the Smoke/CO Alarm's `SelfTestRequest` (§3.22), cluster `92`
  (`0x005C`) command `0`; the SDK has already answered the controller before
  the ember callback this fires from ever runs (§3.22's self-test lifecycle
  note), which is exactly why there is no verdict left for the host to give.
- `AT+MTCMDRESP=<seq>,<verdict>`: the host's answer. `<verdict>` is `1`
  (allow) or `0` (deny). **Set-only**; a bare or query form answers a plain
  `ERROR` (§5), the same convention `AT+MTSWITCH` (§3.15) follows, since
  there is no "current pending command" to report beyond the seq the URC
  already named. `+MTERR:1` on bad grammar, a verdict outside `{0,1}`, or a
  `<seq>` that is not the one currently pending (wrong, stale, already
  answered, or already timed out).
- **The verdict deadline is exactly 1000 ms**, measured from the moment the
  `+MTCMD` line is **queued for transmission** (`mt_at_urc()`'s
  `at_uart_write_line()` call, which copies the line into the UART driver's
  TX ring buffer), not from the line actually reaching the wire. Those two
  moments are usually indistinguishable, but not always: anything already
  ahead of it in the ring (a backlog of URCs or command responses the host
  has not yet drained) delays the physical send, and the clock is already
  running for the whole of that delay. A host under enough incoming traffic
  to build a TX backlog therefore sees a shorter effective window than 1000
  ms, not the full one.
- **Default-deny.** A timeout, the AT link not being up, or no host ever
  having answered `AT+MTCMDRESP` for this firmware session: all are treated
  identically to an explicit deny. A lock fails closed, never open, on every
  path that is not an explicit allow inside the window.
- **Concurrent bridge commands queue behind the open window.** Matter
  serializes command invokes on the CHIP event-loop task, so at most one
  forward is ever in flight, but while it is, the CHIP stack lock is held for
  the whole wait (worst case 1 s): any other `mt_matter_*` bridge call
  (`AT+MTATTR` and friends) blocks until the verdict lands or the deadline
  expires. `AT+MTCMDRESP` itself is exempt (it never takes that lock, see
  below), so the AT link stays responsive enough to actually answer the
  command that is holding everything else up.
- **A forward that opens while a host bridge command is already at the
  parser forfeits the window, by construction.** "Already at the parser"
  means the AT parser task is inside an `mt_matter_*` handler for a command
  the host sent moments earlier (any AT-backed query or setter, e.g.
  `AT+MTFABRICS?`), which needs the CHIP stack lock to answer; if the
  `+MTCMD` this section describes opened because the CHIP task is holding
  that same lock for its own up-to-1000-ms wait, the parser task blocks
  taking it and cannot get back to reading the next line, including the
  host's own eventual `AT+MTCMDRESP`, until the lock frees. There is no
  fix inside this window: the deadline expires, the firmware default-denies
  as documented above, and `+MTCMDTO:<seq>` follows. A host that needs
  verdicts to land reliably keeps bridge traffic off whatever loop also
  needs to answer forwarded commands; see `docs/TESTING.md`'s chatty-host
  bench case and the `iLabs_Hearth` library README's "Hearth originals"
  section for the same limitation from the host side.

**`AT+MTCMDRESP` never takes the CHIP stack lock**, unlike every other
`mt_matter_*` bridge call in this firmware. The CHIP task is blocked inside
the cluster callback that raised `+MTCMD`, waiting on `AT+MTCMDRESP`; if the
handler tried to take the same lock, it would block on the very task it is
trying to answer, a guaranteed deadlock lasting the full 1000 ms until
`mt_cmd_forward()`'s own timeout gives up. `AT+MTCMDRESP` only touches the
verdict mailbox and a semaphore, never the CHIP stack, by design.

Example, a controller sending `LockDoor` to endpoint 5:
```
                                    +MTCMD:1,5,257,0
AT+MTCMDRESP=1,1                -> OK              (allow; controller sees Status::Success)
AT+MTCMDRESP=1,1                -> +MTERR:1         (seq 1 already answered)
AT+MTCMDRESP=99,1               -> +MTERR:1         (no such seq)
```
Silent host (deadline missed):
```
                                    +MTCMD:2,5,257,0
                                    +MTCMDTO:2       (1000 ms later; controller sees Status::Failure)
AT+MTCMDRESP=2,1                -> +MTERR:1         (seq 2 already expired)
```

### 3.18 `AT+MTLOCK`: door lock state reporting

Reports the Door Lock cluster's `LockState` on `<ep>`, typically once the
host has actually driven the physical lock. This is the other half of the
door lock's split ownership: the firmware never decides *whether* a lock
moves (that is `AT+MTCMDRESP`'s job, §3.17) and never decides *that* it has
moved either (spec F4: `LockState` is deliberately not written on an allowed
verdict; the app owns the actuation and its timing may not be immediate). A
host that allows `LockDoor` and then calls `AT+MTLOCK` once the bolt has
actually thrown is the intended flow; `LockState` reads keep working over
`AT+MTATTR` (§3.8) exactly as any other attribute unchanged by this command.

```
AT+MTLOCK=<ep>,<state>[,<source>]  ->  OK
```

- `<ep>`: the endpoint id.
- `<state>`: `0` NotFullyLocked, `1` Locked, `2` Unlocked (`DlLockState`
  protocol values fixed by the Matter spec). Anything else `+MTERR:1`.
- `<source>`: optional, decimal, defaults to `1` (`OperationSourceEnum::
  kManual`). Accepted values are `0`..`10` (every defined `OperationSourceEnum`
  value except the SDK's internal `kUnknownEnumValue`, which is not a valid
  source to report); outside that range `+MTERR:1`.

Driving `SetLockState` with a source and a real state change is what makes
the cluster emit its `LockOperation` event, which is what a subscribed
controller actually sees change; writing `LockState` through `AT+MTATTR`
instead would change the attribute silently, with no event, which is why
`AT+MTLOCK` exists as its own command rather than folding into `AT+MTATTR`.

**Set-only.** `AT+MTLOCK?` or a bare `AT+MTLOCK` is the wrong command form
and answers a plain `ERROR` (§5), the same convention `AT+MTSWITCH` (§3.15)
and `AT+MTTEMPLEVELS` (§3.16) follow.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `DoorLock` cluster. A bare `ERROR`
covers an unclassified runtime failure (the `SetLockState` call itself
reporting failure), the same as `AT+MTATTR` and `AT+MTSWITCH`.

**The firmware never calls this on its own.** Neither an allowed
`AT+MTCMDRESP` verdict nor anything else in the firmware triggers
`AT+MTLOCK`'s effect automatically: actuation timing belongs entirely to the
host, which is expected to call `AT+MTLOCK` itself once its own lock
mechanism confirms the new state.

Example, a door lock on endpoint 5:
```
AT+MTLOCK=5,1        -> OK         (Locked, source defaults to kManual)
AT+MTLOCK=5,2,1      -> OK         (Unlocked, kManual explicit)
AT+MTLOCK=5,3        -> +MTERR:1   (3 is not a valid DlLockState for this command)
AT+MTLOCK=5,1,99     -> +MTERR:1   (99 exceeds the highest valid OperationSourceEnum value)
AT+MTLOCK=9,1        -> +MTERR:2   (no endpoint 9)
AT+MTLOCK=1,1        -> +MTERR:3   (ep 1 has no DoorLock cluster)
```

### 3.19 `AT+MTVALVE`: water valve state reporting

Reports the `ValveConfigurationAndControl` cluster's `CurrentState` (and,
optionally, `CurrentLevel`) on `<ep>`, typically once the host has actually
moved the physical valve following an allowed `+MTCMD` verdict for `Open` or
`Close` (cluster `0x0081`, commands `0`/`1`; §3.17). This is the other half
of the valve's split ownership, the same shape as the door lock (§3.18): the
firmware forwards the command for adjudication but never decides that the
valve has moved, or reports it moving, on its own.

**The verdict cannot fail the command on the wire.** Unlike the door lock,
where a deny surfaces as `Status::Failure` to the controller,
`ValveConfigurationAndControl`'s own server calls the delegate's
`HandleOpenValve`/`HandleCloseValve` synchronously and discards what they
return: the controller sees `Status::Success` whether the `+MTCMD` verdict
was an allow or a deny. The verdict gates only whether the host actually
opens or closes the valve; the wire response is fixed before the delegate is
ever consulted, unconditionally. This is an SDK property
(`TEMPORARY_RETURN_IGNORED` at both delegate call sites,
`valve-configuration-and-control-cluster.cpp`), not a firmware choice, and
there is no return path on this side of the bridge that could change it.

```
AT+MTVALVE=<ep>,<state>[,<level>]  ->  OK
```

- `<ep>`: the endpoint id.
- `<state>`: `0` Closed, `1` Open, `2` Transitioning (`ValveStateEnum` wire
  values). Anything else `+MTERR:1`.
- `<level>`: optional, `0`..`100`. Outside that range `+MTERR:1`.

Reported through the cluster's own `UpdateCurrentState()`/
`UpdateCurrentLevel()` calls (not a raw attribute write), so the
`ValveStateChanged` event a subscribed controller expects is actually
emitted, the same reason `AT+MTLOCK` drives `SetLockState()` rather than
writing `LockState` directly.

**`<level>` publishes nowhere on this SDK revision.** esp-matter
release/v1.5.1's `water_valve` endpoint fixes `ValveConfigurationAndControl`'s
`FeatureMap` at `0` (`valve_configuration_and_control::create()` always calls
`global::attribute::create_feature_map(cluster, 0)`, and its `config_t` has
no field to opt the Level feature in), so `CurrentLevel`/`TargetLevel` are
never created as attributes at all. `UpdateCurrentLevel()` is safe to call
regardless (it checks the feature before touching `CurrentLevel` and returns
success either way), so a `<level>` argument neither errors nor does
anything a controller can observe. Documented rather than silently accepted:
a host expecting `AT+MTATTR` to read back a level it just reported over
`AT+MTVALVE` gets `+MTERR:4` (unknown attribute), not the value.

**Set-only.** `AT+MTVALVE?` or a bare `AT+MTVALVE` is the wrong command form
and answers a plain `ERROR` (§5), the `AT+MTLOCK`/`AT+MTSWITCH` convention.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `ValveConfigurationAndControl`
cluster. A bare `ERROR` covers an unclassified runtime failure, the same as
`AT+MTATTR` and `AT+MTLOCK`.

**The firmware never calls this on its own.** Same split as `AT+MTLOCK`
(§3.18): actuation timing belongs entirely to the host, which is expected to
call `AT+MTVALVE` itself once its own valve mechanism confirms the new
state.

Example, a water valve on endpoint 6:
```
AT+MTVALVE=6,1        -> OK         (Open, no level reported)
AT+MTVALVE=6,1,75     -> OK         (Open, level 75; not readable back, see above)
AT+MTVALVE=6,0        -> OK         (Closed)
AT+MTVALVE=6,3        -> +MTERR:1   (3 is not a valid ValveStateEnum for this command)
AT+MTVALVE=6,1,101    -> +MTERR:1   (101 is out of range for <level>)
AT+MTVALVE=9,1        -> +MTERR:2   (no endpoint 9)
AT+MTVALVE=1,1        -> +MTERR:3   (ep 1 has no ValveConfigurationAndControl cluster)
```

### 3.20 `AT+MTMODES`: Mode Select supported-modes list

Stores the `ModeSelect` cluster's `SupportedModes` list on `<ep>`: up to 8
`<mode>,"<label>"` pairs, each pair a `uint8` mode value and the display
label a controller shows for it. Like `AT+MTTEMPLEVELS` (§3.16), this
attribute is served by CHIP's own `SupportedModesManager` mechanism rather
than esp_matter's attribute store, so it has no `AT+MTATTR` path: this is the
only way a host can set it.

```
AT+MTMODES=<ep>,<mode>,"<label>"[,<mode>,"<label>",...]  ->  OK
```

- `<ep>`: the endpoint id, a bare unsigned token (hex or decimal) ending at
  the first comma.
- `<mode>,"<label>"`: `1..8` pairs. `<mode>` is a bare unsigned token (hex or
  decimal), `0..255`; no two pairs in the same command may repeat a mode
  value. `<label>` is `1..32` bytes of double-quoted printable ASCII
  (`0x20`..`0x7E`) and may not contain a `"` character. A comma **inside** a
  quoted label is legal and is part of the label's text; a comma **between**
  pairs separates them. Violating any of these (wrong count, a repeated mode
  value, an empty or oversized label, a non-printable byte, a bare label with
  no quotes, an unterminated quote, or anything trailing after a label's
  closing quote other than a comma or end of line) answers `+MTERR:1`.

**Set-only.** `AT+MTMODES?` or a bare `AT+MTMODES` is the wrong command form
and answers a plain `ERROR` (§5), the same convention `AT+MTTEMPLEVELS`
follows.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `ModeSelect` cluster. A bare `ERROR`
covers an unclassified runtime failure, the same as `AT+MTATTR` and
`AT+MTTEMPLEVELS`.

**On success the stored list replaces whatever was there before**, and
`SupportedModes` is reported dirty so an active subscription sees the new
list on its next report. There is no append or partial-update form: each
`AT+MTMODES` call is a full replacement of the mode list.

**Not persisted.** The store lives in RAM and starts empty every boot, the
same policy `AT+MTTEMPLEVELS` follows: `SupportedModes` is current display
content a host sets after `+MTREADY`, not a product definition like the
endpoint composition (§3.9). A controller's `ChangeToMode` command against a
mode value not currently in the list is rejected by the SDK itself
(`InvalidCommand`) before this firmware sees it; a `SupportedModes` read
before the host has sent `AT+MTMODES` for that endpoint returns an empty
list, and `ChangeToMode` against an empty list is rejected the same way
(`UnsupportedCluster`).

**`CurrentMode` is a plain attribute, not part of this command.** Unlike
`SupportedModes`, `CurrentMode` is a normal esp_matter-managed `uint8`
attribute: readable and writable over `AT+MTATTR` like any other integer
attribute (cluster `80`/`0x0050`, attribute `3`/`0x0003`). A controller's
`ChangeToMode` command sets `CurrentMode` itself, inside the SDK, after
validating the requested mode is in the `SupportedModes` list; the host sees
that the ordinary way, a `+MTATTR` URC (§4) from the attribute-change
callback, not a URC specific to this command.

Example, a Mode Select endpoint on endpoint 7:
```
AT+MTMODES=7,0,"Quiet"                          -> OK   (one mode)
AT+MTMODES=7,0,"Quiet",1,"Normal",2,"Boost"      -> OK   (replaces the list above)
AT+MTMODES=7,0,"Eco, low"                        -> OK   (comma inside a label)
AT+MTMODES=7                                     -> +MTERR:1 (no pairs)
AT+MTMODES=7,0,Quiet                             -> +MTERR:1 (missing quotes)
AT+MTMODES=7,0,"Quiet",0,"Silent"                -> +MTERR:1 (mode 0 repeated)
AT+MTMODES=9,0,"Quiet"                           -> +MTERR:2 (no endpoint 9)
AT+MTMODES=1,0,"Quiet"                           -> +MTERR:3 (ep 1 has no ModeSelect cluster)
AT+MTATTR=7,80,3                                 -> +MTATTR:7,80,3,0   (read CurrentMode)
AT+MTATTR=7,80,3,1                               -> OK                 (write CurrentMode directly)
```

### 3.21 `AT+MTOPSTATE`: OperationalState transition reporting

Reports the `OperationalState` cluster's `OperationalState` attribute on
`<ep>`, typically once the host has actually finished executing an allowed
`Pause`/`Resume`/`Start`/`Stop` `+MTCMD` verdict (cluster `96`/`0x0060`,
commands `0`-`3`; §3.17). This is the device type behind Laundry Washer
(`0x0073`), Dishwasher (`0x0075`) and Laundry Dryer (`0x007C`) (§3.9), all
three of which wire the identical base `OperationalState` cluster with no
device-specific extension.

Unlike the water valve (§3.19), where the SDK discards the delegate's
verdict and always answers the controller `Status::Success`, here the
adjudication verdict **is** the wire response: the SDK copies the filled
`GenericOperationalError` straight into the command's
`OperationalCommandResponse`. `+MTCMDRESP=<seq>,0` (deny) therefore fails
the `Pause`/`Resume`/`Start`/`Stop` command the controller sees, with
`ErrorStateID` `0x02` (`UnableToCompleteOperation`); an allow answers
`0x00` (`NoError`). `AT+MTOPSTATE` is the other half of the split, the same
shape as `AT+MTLOCK`/`AT+MTVALVE`: the firmware never decides *that* the
appliance has actually reached the new state, or reports it reaching one, on
its own; the host calls this once its own control loop confirms the
transition really happened.

```
AT+MTOPSTATE=<ep>,<state>  ->  OK
```

- `<ep>`: the endpoint id.
- `<state>`: `0` Stopped, `1` Running, `2` Paused (`OperationalStateEnum`
  wire values). `3` (`Error`) is rejected with `+MTERR:1`: `Error` is
  reserved for the device's own fault-detection path
  (`OnOperationalErrorDetected`, not exposed on the AT surface this round),
  never a state this command may set directly. Anything else outside
  `0`..`3` is also `+MTERR:1`.

Reported through the cluster's own `Instance::SetOperationalState()` (not a
raw attribute write), so the `OperationalState` attribute report a
subscribed controller expects is actually emitted, the same reasoning
`AT+MTLOCK`/`AT+MTVALVE` follow for their own cluster's setter.

**`PhaseList` ships null.** This firmware defines no phases for any of the
three device types: `GetOperationalPhaseAtIndex` answers
`CHIP_ERROR_NOT_FOUND` at index `0` unconditionally, which is what the SDK
defines as "the `PhaseList` attribute is null" rather than an empty list.
`CurrentPhase` is likewise never set. A v2 surface once a consumer actually
needs phases, alongside `CountdownTime` (also unconditionally null this
round: no consumer needs it yet) and `OnOperationalErrorDetected` (the
device-fault path, distinct from the host-adjudicated commands this section
covers).

**Set-only.** `AT+MTOPSTATE?` or a bare `AT+MTOPSTATE` is the wrong command
form and answers a plain `ERROR` (§5), the `AT+MTLOCK`/`AT+MTVALVE`
convention.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `OperationalState` cluster. A bare
`ERROR` covers an unclassified runtime failure, the same as `AT+MTATTR` and
`AT+MTLOCK`/`AT+MTVALVE`.

**Every `OperationalState` attribute is managed internally.** All six
(`PhaseList`, `CurrentPhase`, `OperationalStateList`, `OperationalState`,
`OperationalError`, and the derived-cluster-only `CountdownTime`) are served
by the SDK's own `Instance`, not esp-matter's generic attribute store, so
none of them is reachable over `AT+MTATTR`: `AT+MTOPSTATE` is the only way a
host observes or sets the current state.

**The firmware never calls this on its own.** Same split as
`AT+MTLOCK`/`AT+MTVALVE` (§3.18/§3.19): actuation timing belongs entirely to
the host, which is expected to call `AT+MTOPSTATE` itself once its own
control loop confirms the appliance has reached the new state.

Example, a laundry washer on endpoint 8:
```
AT+MTOPSTATE=8,1        -> OK         (Running)
AT+MTOPSTATE=8,2        -> OK         (Paused)
AT+MTOPSTATE=8,0        -> OK         (Stopped)
AT+MTOPSTATE=8,3        -> +MTERR:1   (3/Error is reserved, not settable here)
AT+MTOPSTATE=8,4        -> +MTERR:1   (4 is not a valid OperationalStateEnum value)
AT+MTOPSTATE=99,1       -> +MTERR:2   (no endpoint 99)
AT+MTOPSTATE=1,1        -> +MTERR:3   (ep 1 has no OperationalState cluster)
```

### 3.22 `AT+MTALARM`: Smoke/CO Alarm state reporting

Reports an event-emitting `SmokeCoAlarm` cluster (`92`/`0x005C`) state change
on `<ep>`, through one of the cluster's own eleven `Set*` methods rather than
a raw `AT+MTATTR` write to the same attribute: the setters fire the cluster's
spec-mandated events (`SmokeAlarm`, `COAlarm`, `MuteEnded`, `HardwareFault`,
`EndOfService`, `AllClear`, `LowBattery`, `SelfTestComplete`) and the
critical-alarm auto-unmute, both of which a raw write would silently skip.

```
AT+MTALARM=<ep>,<field>,<value>  ->  OK
```

- `<ep>`: the endpoint id.
- `<field>`: which state to set, `1`-`11`:

| `<field>` | State | Setter | `<value>` |
|---|---|---|---|
| `1` | SmokeState | `SetSmokeState` | `AlarmStateEnum`: `0` Normal, `1` Warning, `2` Critical |
| `2` | COState | `SetCOState` | `AlarmStateEnum` (as above) |
| `3` | BatteryAlert | `SetBatteryAlert` | `AlarmStateEnum` (as above) |
| `4` | DeviceMuted | `SetDeviceMuted` | `MuteStateEnum`: `0` NotMuted, `1` Muted |
| `5` | TestInProgress | `SetTestInProgress` | `0` or `1` (bool); `0` after a self-test notify is the completion path, below |
| `6` | HardwareFaultAlert | `SetHardwareFaultAlert` | `0` or `1` (bool) |
| `7` | EndOfServiceAlert | `SetEndOfServiceAlert` | `EndOfServiceEnum`: `0` Normal, `1` Expired |
| `8` | InterconnectSmokeAlarm | `SetInterconnectSmokeAlarm` | `AlarmStateEnum` (as above) |
| `9` | InterconnectCOAlarm | `SetInterconnectCOAlarm` | `AlarmStateEnum` (as above) |
| `10` | ContaminationState | `SetContaminationState` | `ContaminationStateEnum`: `0` Normal, `1` Low, `2` Warning, `3` Critical |
| `11` | SmokeSensitivityLevel | `SetSmokeSensitivityLevel` | `SensitivityEnum`: `0` High, `1` Standard, `2` Low |

`<field>` `0` (`ExpressedState`) is not in this table because it is derived
by the server from the ten states above and is never settable directly: `0`
and anything outside `1`-`11` answer `+MTERR:1`. `<value>` outside the range
its own field's enum defines (or, for the two boolean fields, outside
`0`/`1`) also answers `+MTERR:1`; the range comes from the SDK's own enum
(its `kUnknownEnumValue` bound), never a literal copied into this firmware.

**The self-test lifecycle.** A controller's `SelfTestRequest` command
(cluster `0x005C`, command `0`) is not adjudicated the way `AT+MTLOCK`'s or
`AT+MTOPSTATE`'s commands are: `SmokeCoAlarmServer::HandleRemoteSelfTestRequest`
sets `TestInProgress` true and `ExpressedState` `Testing`, answers the
controller `Status::Success`, and only *then* calls the app-level hook this
firmware wires to the notify-only `+MTCMD` form (§3.17):
`+MTCMD:0,<ep>,92,0` arrives with no verdict to give, since the wire response
is already sent. The host runs whatever self-test it actually performs, then
reports completion with `AT+MTALARM=<ep>,5,0` (`TestInProgress` false), which
the SDK's own `SetTestInProgress` recognises as the field's true-to-false
edge and fires `SelfTestComplete` on the fabric. `AT+MTCMDRESP=0,...` always
answers `+MTERR:1`: there is nothing pending seq `0` could ever answer.

**Set-only.** `AT+MTALARM?` or a bare `AT+MTALARM` is the wrong command form
and answers a plain `ERROR` (§5), the `AT+MTLOCK`/`AT+MTVALVE`/`AT+MTOPSTATE`
convention.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `SmokeCoAlarm` cluster. A bare
`ERROR` covers an unclassified runtime failure, for example a setter
returning false because the field already held that value, the same as
`AT+MTATTR` and the rest of this family.

Example, a smoke/CO alarm on endpoint 9:
```
AT+MTALARM=9,1,1        -> OK          (SmokeState Warning; fires SmokeAlarm)
AT+MTALARM=9,1,2        -> OK          (SmokeState Critical; fires SmokeAlarm + MuteEnded)
AT+MTALARM=9,4,1        -> OK          (DeviceMuted; silences the alarm)
AT+MTALARM=9,5,0        -> OK          (self-test complete; fires SelfTestComplete)
AT+MTALARM=9,1,3        -> +MTERR:1    (3 is not a valid AlarmStateEnum value)
AT+MTALARM=9,0,0        -> +MTERR:1    (field 0/ExpressedState is derived, not settable)
AT+MTALARM=9,12,0       -> +MTERR:1    (field outside 1..11)
AT+MTALARM=99,1,1       -> +MTERR:2    (no endpoint 99)
AT+MTALARM=1,1,1        -> +MTERR:3    (ep 1 has no SmokeCoAlarm cluster)
```

### 3.23 `AT+MTCHIMESOUNDS`: Chime installed-sound list

Stores the `Chime` cluster's `InstalledChimeSounds` list on `<ep>`: up to 8
`<id>,"<name>"` pairs, each pair a `uint8` chime ID and the display name a
controller shows for it. The `AT+MTMODES` grammar (§3.20) with IDs in place
of mode values: like `AT+MTMODES`/`AT+MTTEMPLEVELS`, this list is served by
the cluster's own `ChimeDelegate` mechanism rather than esp-matter's generic
attribute store, so it has no `AT+MTATTR` path - this is the only way a host
can set it.

```
AT+MTCHIMESOUNDS=<ep>,<id>,"<name>"[,<id>,"<name>",...]  ->  OK
```

- `<ep>`: the endpoint id, a bare unsigned token (hex or decimal) ending at
  the first comma.
- `<id>,"<name>"`: `1..8` pairs. `<id>` is a bare unsigned token (hex or
  decimal), `0..255`; no two pairs in the same command may repeat an id.
  `<name>` is `1..32` bytes of double-quoted printable ASCII (`0x20`..`0x7E`)
  and may not contain a `"` character. A comma **inside** a quoted name is
  legal and is part of the name's text; a comma **between** pairs separates
  them. Violating any of these (wrong count, a repeated id, an empty or
  oversized name, a non-printable byte, a bare name with no quotes, an
  unterminated quote, or anything trailing after a name's closing quote other
  than a comma or end of line) answers `+MTERR:1`.

**Set-only.** `AT+MTCHIMESOUNDS?` or a bare `AT+MTCHIMESOUNDS` is the wrong
command form and answers a plain `ERROR` (§5), the `AT+MTMODES` convention.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `Chime` cluster. A bare `ERROR`
covers an unclassified runtime failure, the same as `AT+MTATTR` and
`AT+MTMODES`.

**On success the stored list replaces whatever was there before**, and
`InstalledChimeSounds` is reported dirty so an active subscription sees the
new list on its next report. There is no append or partial-update form: each
`AT+MTCHIMESOUNDS` call is a full replacement of the sound list.

**Not persisted.** The store lives in RAM and starts empty every boot, the
same policy `AT+MTMODES`/`AT+MTTEMPLEVELS` follow. `AT+MTCHIME=<ep>,0,<id>`
(§3.24) against an id not currently in this list is rejected
(`ChimeCluster::SetSelectedChime()` answers `Status::NotFound`, mapped to
`+MTERR:1`).

Example, a chime endpoint on endpoint 10:
```
AT+MTCHIMESOUNDS=10,1,"Doorbell"                    -> OK   (one sound)
AT+MTCHIMESOUNDS=10,1,"Doorbell",2,"Alert, urgent"   -> OK   (replaces the list above; comma inside a name)
AT+MTCHIMESOUNDS=10                                  -> +MTERR:1 (no pairs)
AT+MTCHIMESOUNDS=10,1,Doorbell                       -> +MTERR:1 (missing quotes)
AT+MTCHIMESOUNDS=10,1,"Doorbell",1,"Chime"           -> +MTERR:1 (id 1 repeated)
AT+MTCHIMESOUNDS=99,1,"Doorbell"                     -> +MTERR:2 (no endpoint 99)
AT+MTCHIMESOUNDS=1,1,"Doorbell"                      -> +MTERR:3 (ep 1 has no Chime cluster)
```

### 3.24 `AT+MTCHIME`: Chime SelectedChime / Enabled

Sets one of the `Chime` cluster's two plain attributes on `<ep>` through the
cluster's own `SetSelectedChime()`/`SetEnabled()` (design spec F3), not a raw
attribute write: like `InstalledChimeSounds` (§3.23), this cluster is
registered directly with esp-matter's data model provider rather than through
its generic attribute store, so `AT+MTATTR` has no path to either attribute
either.

```
AT+MTCHIME=<ep>,<what>,<value>  ->  OK
```

- `<ep>`: the endpoint id.
- `<what>`: `0` `SelectedChime`, `1` `Enabled`. Anything else `+MTERR:1`.
- `<value>`: for `<what>` `0`, a chime ID; it must already be one of the ids
  `AT+MTCHIMESOUNDS` (§3.23) installed on this endpoint, or `+MTERR:1`
  (`SetSelectedChime()` itself answers `Status::NotFound` for an unknown id).
  For `<what>` `1`, `0` or `1` (bool); anything else `+MTERR:1`.

**Set-only.** `AT+MTCHIME?` or a bare `AT+MTCHIME` is the wrong command form
and answers a plain `ERROR` (§5), the `AT+MTOPSTATE`/`AT+MTALARM` convention.

**Lookup errors follow the established division.** `+MTERR:2` unknown
endpoint; `+MTERR:3` the endpoint has no `Chime` cluster. A bare `ERROR`
covers an unclassified runtime failure, the same as `AT+MTATTR` and the rest
of this family.

**`SetSelectedChime()`/`SetEnabled()` persist their own value** through
CHIP's attribute persistence provider (unlike `AT+MTCHIMESOUNDS`'s list,
which is not persisted): a chosen chime and the enabled flag both survive
`AT+MTRESET` the same way any other `SafeAttributePersistenceProvider`-backed
attribute does.

Example, a chime endpoint on endpoint 10 with sounds `1`/`2` installed
(§3.23):
```
AT+MTCHIME=10,0,1        -> OK          (SelectedChime = 1)
AT+MTCHIME=10,1,1        -> OK          (Enabled = true)
AT+MTCHIME=10,0,9        -> +MTERR:1    (9 is not an installed chime id)
AT+MTCHIME=10,1,2        -> +MTERR:1    (2 is not a valid bool for Enabled)
AT+MTCHIME=10,2,1        -> +MTERR:1    (2 is not a valid <what>)
AT+MTCHIME=99,0,1        -> +MTERR:2    (no endpoint 99)
AT+MTCHIME=1,0,1         -> +MTERR:3    (ep 1 has no Chime cluster)
```

## 4. Unsolicited result codes (URCs)

| URC | Meaning |
|---|---|
| `+MTREADY` | AT interface up after boot, `AT+MTRESET`, `AT+MTFRESET` or `AT+MTEPAPPLY`. |
| `+MTEVT:<bit>[,<detail>]` | A subscribed platform event fired (§3.11). |
| `+MTATTR:<ep>,<cluster>,<attr>,<val>` | An attribute changed on one of the declared endpoints (controller-driven or local). The root endpoint (0) is intentionally not reported, to keep boot-time init noise off the link. |
| `+MTIDENT:<ep>,<enabled>` | The Identify cluster started (`1`) or stopped (`0`) on an endpoint. Backs the per-endpoint identify callback; the host decides how to indicate it. |
| `+MTCMD:<seq>,<ep>,<cluster>,<command>[,<payload>]` | A command needing an app-level verdict was invoked; answer with `AT+MTCMDRESP` within 1000 ms (§3.17). |
| `+MTCMD:0,<ep>,<cluster>,<command>` | Notify-only: the command was invoked with no verdict to give; `AT+MTCMDRESP=0,...` always answers `+MTERR:1` (§3.17). |
| `+MTCMDTO:<seq>` | The `AT+MTCMDRESP` window for `<seq>` closed with no answer; the command was denied by default (§3.17). |

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

## 9. Relationship to `AT+EN`

`AT+MT` and `AT+EN` share the `at_core` engine, transport, framing and error
conventions, and disjoint command namespaces. The two transport commands are
deliberately parallel: `AT+MTBAUD` and `AT+MTFLOW` (§3.13, §3.14) drive the
same `at_uart_set_baud()` / `at_uart_set_flowctrl()` as `AT+ENBAUD` and
`AT+ENFLOW`, with the same rate table and mode numbering, so a host that can
retune one link can retune the other. They diverge in two places only: a bad
value is `+MTERR:1` here and a bare `ERROR` there, following each namespace's
own grammar, and `AT+MTFLOW` refuses the modes this hardware cannot honour. Only one personality is flashed
at a time; the host reflashes the C6 to switch. This keeps a future single
binary (registering both command tables) a mechanical merge. See the
[ESP-NOW + Matter integration plan](hearth-integration-plan.md).
