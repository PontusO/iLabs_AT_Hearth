# AT+MT extensions for an arduino-esp32-parity Matter host library

Date: 2026-07-26
Status: **approved design**, not yet implemented. Implementation phases in §12.

Related documents:
- `docs/AT_MT_SPEC.md` (the wire protocol this extends)
- `docs/ARCHITECTURE.md` (decision record; §11 here feeds it)
- `docs/TESTING.md` (regression plan; §11 here amends it)

## 1. Goal

Let an unmodified arduino-esp32 Matter sketch run on a Challenger RP2350
WiFi6/BLE5, with the RP2350 as the Arduino host and the ESP32-C6 running the
Matter stack behind the `AT+MT` link.

```cpp
#include <Matter.h>
MatterOnOffLight OnOffLight;

void setup() {
  OnOffLight.begin();
  OnOffLight.onChange(onOffLightCallback);
  Matter.begin();
}
```

That source must compile and behave identically whether it targets an ESP32
directly or a Challenger through `AT+MT`. This is the same contract
`iLabs_ESP-NOW` holds against `ESP32_NOW.h`, applied to Matter.

The reference implementation is Espressif's `Matter` library, shipped in the
arduino-esp32 core (verified here against **3.3.8**, at
`libraries/Matter/`). It is the right reference because it sits on
**esp-matter**, the same stack our C6 firmware runs, so the mapping is
mechanical rather than interpretive.

## 2. What the reference API actually needs

A census of the attribute value types used across all 21 endpoint classes in
`src/MatterEndpoints/*.cpp`:

| Union field | Uses |
|---|---|
| `val.u8` | 99 |
| `val.u16` | 71 |
| `val.b` | 42 |
| `val.i16` | 31 |
| `val.u32` | 18 |
| `val.a` (array) | 1 |

No floats, no strings, no octet strings, no structs. `ColorFormat.h` is a
host-side RGB/HSV helper in the Arduino core, not an attribute type: hue and
saturation cross the wire as `u8`, X/Y and colour temperature as `u16`.

**Consequence:** the existing `AT+MTATTR`, which already handles `bool`,
`int8..64`, `uint8..64`, `enum` and `bitmap`, covers essentially the whole
parity surface. The single array consumer is
`TemperatureControlledCabinet::SupportedTemperatureLevels`.

The real gap is elsewhere. Every endpoint class creates its endpoint at
**runtime** in `begin()`, before the stack starts:

```cpp
bool MatterOnOffLight::begin(bool initialState) {
  ArduinoMatter::_init();
  on_off_light::config_t light_config;
  light_config.on_off.on_off = initialState;
  endpoint_t *endpoint = on_off_light::create(node::get(), &light_config,
                                              ENDPOINT_FLAG_NONE, (void *)this);
  setEndPointId(endpoint::get_id(endpoint));
  ...
}
```

`AT+MT` has no command for this at all. Adding one is the bulk of this design.

## 3. Design decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | Mirror the arduino-esp32 `Matter` API, not the Silicon Labs one | Same underlying esp-matter stack; matches the `ESP32_NOW.h` precedent set by `iLabs_ESP-NOW`. |
| D2 | Endpoint composition persists in the C6's NVS | The C6 must rejoin its fabric after a power cut without waiting on the host. See §5.3. |
| D3 | Device types are identified by their standard Matter device type IDs | Makes the firmware table one line per type and the host mapping one constant per class. Keeps the remaining 17 types as data, not code. |
| D4 | No `AT+MTSTART` command | The C6 rebuilds from NVS and starts itself at boot, which D2 requires anyway. See §5.3. |
| D5 | An unconfigured C6 opens no commissioning window | Prevents commissioning an empty node and then mutating the data model underneath a live fabric. See §5.5. |
| D6 | Events are subscribed by bit mask | ~30 event codes on a 115200 link shared with attribute traffic; most are stack chatter no sketch acts on. |
| D7 | `+MTCOMMISSION` is removed and folded into `+MTEVT` | Two URCs for one event would be permanent. Nothing ships against the current spec yet, so the change is free now and never again. |
| D8 | Specific `+MTERR` codes are allocated | Closes the diagnostic weakness recorded in `TESTING.md` §6.3, which the new commands would otherwise widen. |

## 4. Architecture

```
sketch (unmodified arduino-esp32 Matter source)
  |
  +- iLabs_Matter        arduino-pico host library, mirrors class names verbatim
       |
       +- ATLink         shared line-protocol transport (host-side twin of at_core)
            |
            +- AT+MT over UART, 115200 8N1, C6 GPIO16/17
                 |
                 +- mt_at.c -> mt_matter bridge -> esp-matter on the C6
```

The C6 is the single source of truth. The host library holds no Matter state
beyond endpoint IDs, cached last-known attribute values, and callback
registrations.

`ATLink.cpp` / `ATLink.h` already exist inside `iLabs_ESP-NOW` as its transport.
They get extracted into a shared component so both host libraries use one
line-protocol implementation, mirroring the `at_core` extraction on the firmware
side (integration-plan contracts C3 and C4).

## 5. Endpoint composition

### 5.1 Commands

```
AT+MTEP?                    query the live composition
+MTEP:<index>,<endpoint_id>,<device_type>      (one line per endpoint)
OK

AT+MTEPCLEAR                discard any staged composition, begin staging empty
OK

AT+MTEP=<device_type>       append one endpoint to the staged composition
OK

AT+MTEPAPPLY                persist the staged composition to NVS, then reboot
OK
```

`<device_type>` is a standard Matter device type ID, hex or decimal.

**Staging is explicit and lives in RAM.** `AT+MTEPCLEAR` opens a staging
session, `AT+MTEP=` appends to it, and `AT+MTEPAPPLY` closes it. `AT+MTEP=`
without an open session is rejected with `+MTERR:10`, as is `AT+MTEPAPPLY`.
A reboot discards any open session, so an interrupted host leaves the persisted
composition untouched rather than half-written.

`AT+MTEP?` always reports the **live** composition, the one currently built and
serving the fabric. It never reports the staged one. A host that wants to know
what it staged already knows.

`AT+MTEPAPPLY` writes NVS, emits `OK`, drains the UART, then reboots. The host
resynchronises on the next `+MTREADY`, exactly as it does after `AT+MTRESET`.
Applying an empty staged composition is legal and returns the device to the
unconfigured state of §5.5.

### 5.2 NVS format

Namespace `mt_ep`, key `comp`: a blob holding a `uint16` count followed by that
many `uint32` device type IDs, in declaration order. Maximum 16 endpoints, a
RAM-driven cap, configurable in `mt_at_config.h`.

### 5.3 Boot sequence

1. Read the composition blob from NVS.
2. If the count is zero, go to §5.5.
3. Create each endpoint in stored order via the device-type table (§6).
4. `esp_matter::start()`.
5. Emit `+MTREADY`.

esp-matter assigns endpoint IDs sequentially from 1 as endpoints are created.
Recreating them in the stored order therefore reproduces the same IDs on every
boot. **This is a load-bearing invariant**: persisted attribute values and the
controller's cached Descriptor `PartsList` are both keyed on endpoint ID. Any
future change to creation order or to the table must preserve it.

### 5.4 Host reconcile flow

`Matter.begin()` in the host library:

1. Send `AT+MTEP?` and collect the listing.
2. Compare the returned device-type sequence against the sequence the sketch
   declared through its endpoint constructors.
3. **Identical:** adopt the returned endpoint IDs, apply uncommissioned initial
   state (§7.3), return. One query, zero writes, no reboot. This is the steady
   state on every boot after the first.
4. **Different:** `AT+MTEPCLEAR`, one `AT+MTEP=` per declared endpoint,
   `AT+MTEPAPPLY`, wait for `+MTREADY` (15 s timeout), re-query, then continue
   at step 3.

When the composition differs **and** `AT+MTFABRICS?` is non-zero, the library
prints a warning on the host's `Serial` before applying: the change invalidates
controller caches and may require re-commissioning. It still applies, because
the sketch is the declaration of intent, but it does not do so silently.

### 5.5 The unconfigured state

A factory-fresh C6, or one whose composition was cleared, has no endpoints. It:

- starts the Matter stack with the Root Node only,
- **does not open a commissioning window**,
- answers `AT+MTEP?` with zero `+MTEP:` lines,
- rejects `AT+MTATTR` and `AT+MTCOMMISSION` with `+MTERR:9`.

Without this, a user could commission an empty node and the host would then add
endpoints underneath a live fabric, which is the data-model instability Matter
forbids. First boot is host-driven; every boot after that is autonomous.

## 6. Device type table

One firmware table maps a Matter device type ID to its `esp_matter` create
function. Slice entries:

| Device type ID | Device type | esp_matter namespace | Types exercised |
|---|---|---|---|
| `0x0100` | On/Off Light | `on_off_light` | `bool`, bidirectional writes |
| `0x0101` | Dimmable Light | `dimmable_light` | `u8`, two clusters on one endpoint |
| `0x010C` | Colour Temperature Light | `color_temperature_light` | `u16`, three clusters on one endpoint |
| `0x0302` | Temperature Sensor | `temperature_sensor` | `i16`, read-only device-to-controller |

Between them these cover `bool`, `u8`, `u16` and `i16`, single and multi-cluster
endpoints, and both data directions. `u32` appears almost entirely in read-only
metadata such as FeatureMap and arrives free with the table.

The remaining 17 types (plug-in units, generic switch, door lock, the other
sensors, fan, thermostat, window covering, air quality, leak/freeze/rain
detectors, temperature controlled cabinet) are table entries added later. Their
device type IDs must be checked against the Matter Device Library Specification
as each is added rather than taken from this document.

## 7. Attribute layer

### 7.1 Mode parameter

```
AT+MTATTR=<ep>,<cl>,<attr>,<val>[,<mode>]
```

`<mode>`: `0` sets the value locally without notifying subscribers
(`setAttributeVal`), `1` updates and notifies (`updateAttributeVal`). Default
`1`, which is the current behaviour, so this is backward compatible. The
distinction exists in the reference API and some endpoint classes rely on it to
avoid feedback loops when reflecting a controller-driven change.

### 7.2 Opaque attributes

```
AT+MTATTRX=<ep>,<cl>,<attr>,<hex>       write
AT+MTATTRX=<ep>,<cl>,<attr>             read -> +MTATTRX:<ep>,<cl>,<attr>,<hex>
```

Covers array, octet string and character string attributes, hex-encoded with the
same convention as `AT+ENSEND`. Specified now for completeness; implemented with
`TemperatureControlledCabinet`, its only consumer in the reference library. Not
in the slice.

### 7.3 Initial state

`MatterOnOffLight::begin(true)` and equivalents need no protocol support. The
host library issues an `AT+MTATTR` write after start, and only when
`AT+MTFABRICS?` reports zero. On a commissioned device the value esp-matter
persisted takes precedence, which is the correct Matter semantic.

### 7.4 Unchanged

`+MTATTR` URCs on controller-driven change keep their current behaviour,
including the deliberate suppression of endpoint 0.

## 8. Event layer

```
AT+MTEVT=<hex32mask>        subscribe
AT+MTEVT?                   -> +MTEVT:<hex32mask>
+MTEVT:<bit>[,<detail>]     URC
```

Bit groups, chosen so a host can enable a whole class cheaply:

| Bits | Group | Contents |
|---|---|---|
| 0–5 | Commissioning | window opened, session started, session stopped, complete, window closed, fail-safe expired |
| 6–9 | Fabric | will be removed, removed, committed, updated |
| 10–15 | Connectivity | WiFi connectivity, internet connectivity, interface IP changed, operational network started, DNS-SD initialised, server ready |
| 16–19 | BLE | CHIPoBLE connected, disconnected, advertising change, BLE deinitialised |
| 20–23 | Misc | OTA state changed, bindings changed via cluster, time sync change, reserved |
| 24–31 | Reserved | Thread and future events |

Default mask `0x0000003F` (the commissioning group), which reproduces exactly
what the firmware emits today.

The host library declares `matterEvent_t` with the **same enumerator names and
values as arduino-esp32**, so a sketch's `case MATTER_COMMISSIONING_COMPLETE:`
compiles unchanged. The bit-to-value mapping stays inside the library, and
`Matter.onEvent()` sets the mask from what the sketch registered.

**Known parity limitation.** The reference callback signature is
`void(matterEvent_t, const chip::DeviceLayer::ChipDeviceEvent *)`. The raw CHIP
struct cannot cross a UART, so the library passes `nullptr` for the second
argument. Sketches that switch on the event code port verbatim; the rare one
that dereferences the event struct does not. This goes in the library README.

## 9. Identify

```
+MTIDENT:<ep>,<enabled>
```

Backs the per-endpoint `onIdentify()` callback declared on `MatterEndPoint` and
exercised by the reference `MatterOnIdentify` example. Small, but part of the
endpoint base class, so omitting it would make the class hierarchy dishonest.

## 10. Error codes

Replaces the current table in `AT_MT_SPEC.md` §5.

| Code | Meaning |
|---|---|
| `1` | Bad parameter or out of range |
| `2` | Unknown endpoint |
| `3` | Unknown cluster |
| `4` | Unknown attribute |
| `5` | Attribute type not supported by this command |
| `6` | Unknown or unsupported device type |
| `7` | Persistence (NVS) failure |
| `8` | Unknown or unsupported command *(existing, unchanged)* |
| `9` | Not ready: no composition declared, or stack not started |
| `10` | Composition change rejected: nothing staged, or endpoint limit exceeded |

`8` keeps its meaning so host-side version-skew detection is untouched. The
existing policy stands: codes `1–99` carry a `+MTERR` line, `≥ 100` collapses to
a bare `ERROR`, and the `+MTERR` / `+ENERR` prefixes keep the two code spaces
independent.

## 11. Impact on existing documents

### 11.1 Breaking change

`+MTCOMMISSION:STARTED` / `:COMPLETE` / `:FAILED` are **removed**. Their
information moves to `+MTEVT` bits 0, 3 and 5, which are in the default mask, so
a host that sets no mask sees the same three events under the new name.

Affected: `AT_MT_SPEC.md` §4 (URC table) and §5 (error codes), and
`TESTING.md` §7 (three Phase 2 assertions). Both are edited as part of Phase C3.

### 11.2 Testing

- **Phase 1** gains `AT+MTEP`, `AT+MTEVT` and `AT+MTATTRX` negatives. Every
  existing negative tightens from "expect a bare `ERROR`" to "expect the
  documented code". `TESTING.md` §6.3, which records the absence of specific
  codes as a known limit, is deleted.
- **Phase 2** gains a composition lifecycle group: stage, apply, reboot, verify
  the rebuild; change the composition on a commissioned device and assert both
  the warning and the fabric consequence; assert an unconfigured C6 opens no
  commissioning window and rejects `AT+MTATTR` with `+MTERR:9`.
- **A new host-library phase** becomes possible, the direct analogue of the
  ESP-NOW suite's Phase 2. `TESTING.md` §11 currently lists its absence as a
  known gap. Because the host library runs on the RP2350, that phase is an
  Arduino sketch, so the two-board rig pattern partly returns.

### 11.3 Architecture decision record

D2, D3, D4, D5 and D7 are added to the decision log in `ARCHITECTURE.md`.

## 12. Implementation phases

Continues the repository's phase lettering (Phase A: `at_core` extraction; Phase
B: Matter bring-up and the initial `AT+MT` command set).

| Phase | Deliverable |
|---|---|
| **C1** | Endpoint composition: `AT+MTEP` / `MTEPCLEAR` / `MTEPAPPLY`, NVS persistence, boot rebuild, unconfigured-state gating, and the four slice device types. |
| **C2** | `AT+MTATTR` mode parameter and the `+MTERR` code allocation across all existing handlers. |
| **C3** | Event mask, `+MTEVT`, removal of `+MTCOMMISSION`, `+MTIDENT`, and the resulting `AT_MT_SPEC.md` edits. |
| **C4** | `iLabs_Matter` host library: `ATLink` extraction, `ArduinoMatter`, `MatterEndPoint`, and the four endpoint classes. |
| **C5** | `TESTING.md` update and the regression harness (T1 and T2) covering the new surface. |

C1 through C3 are firmware-only and independently useful: they can be verified
with a terminal and `chip-tool` before any host library exists.

## 13. Out of scope

- The remaining 17 device types. Data entries against the C1 table.
- `AT+MTATTRX` implementation. Lands with Temperature Controlled Cabinet.
- Thread transport. Deferred per `ARCHITECTURE.md`.
- `AT+MTOTA`. Covered by `FIRMWARE_UPDATE_SPEC.md`.
- Composed endpoints and bridged devices. The reference library's
  `MatterComposedLights` example uses ordinary endpoints, so nothing here blocks
  it, but endpoint parent/child relationships are not modelled.
- Per-device commissioning credentials. Still the esp-matter test DAC.
