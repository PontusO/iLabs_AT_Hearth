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

A census of the attribute value types used across all 20 endpoint classes in
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
| D3 | Device types are identified by their standard Matter device type IDs, read from `esp_matter` rather than transcribed | Every `esp_matter` endpoint namespace exposes `get_device_type_id()`, so the firmware table derives its IDs from the SDK and cannot drift from it. Keeps the remaining device types as table entries, not design work. See §6. |
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

**Step 3 must run before step 4, and this is not merely conventional.**
esp-matter persists its `min_unused_endpoint_id` counter, but only for endpoints
created after `esp_matter::start()` (§12.1, P1 verdict). Creating the composition
before start keeps the counter in RAM, so every boot allocates from the same base
and reproduces the same IDs. If a future change ever applied a composition live
instead of rebooting, the counter would persist, IDs would climb monotonically on
every change, and this invariant would break immediately. That is the second,
independent reason `AT+MTEPAPPLY` reboots rather than applying in place: the
first is Matter data-model stability (§5.1).

Verified on hardware 2026-07-27: six cold power cycles, identical IDs each time.

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

**Provisional**: the no-commissioning-window behaviour below depends on open
question P2 (§12.1). Read it as intent until P2 concludes.

A factory-fresh C6, or one whose composition was cleared, has no endpoints. It:

- starts the Matter stack with the Root Node only,
- **does not open a commissioning window**,
- answers `AT+MTEP?` with zero `+MTEP:` lines,
- rejects `AT+MTATTR` and `AT+MTCOMMISSION` with `+MTERR:9`.

Without this, a user could commission an empty node and the host would then add
endpoints underneath a live fabric, which is the data-model instability Matter
forbids. First boot is host-driven; every boot after that is autonomous.

## 6. Device type table

### 6.1 Shape

One firmware table maps a Matter device type ID to a creation thunk. The ID is
**not transcribed**: every `esp_matter` endpoint namespace exposes
`get_device_type_id()`, so the table reads it from the SDK and cannot drift from
whatever esp-matter revision we build against.

A thunk per type is required because each namespace has its own `config_t`, so
the `create()` calls do not share a signature:

```cpp
static endpoint_t *mk_on_off_light(node_t *n) {
    on_off_light::config_t c;
    return on_off_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static const mt_devtype_t s_devtypes[] = {
    { on_off_light::get_device_type_id(),            mk_on_off_light },
    { dimmable_light::get_device_type_id(),          mk_dimmable_light },
    { color_temperature_light::get_device_type_id(), mk_color_temp_light },
    { temperature_sensor::get_device_type_id(),      mk_temperature_sensor },
};
```

So the cost per device type is a four-line thunk plus one table row, not one
line. The point stands that adding types is mechanical rather than design work,
but it is not free.

### 6.2 Verified mapping

Verified against **esp-matter release/v1.5 (v1.5.1, commit `21aa3d1`)**, which is
what this firmware builds against, and against the 20 endpoint classes in
arduino-esp32 **3.3.8** `libraries/Matter`. IDs come from
`components/esp_matter/data_model/esp_matter_endpoint.h`; the namespace each
Arduino class uses comes from its `::create(node::get()` call site.

Slice entries, all four confirmed present in v1.5.1:

| Device type ID | Arduino class | esp_matter namespace | Types exercised |
|---|---|---|---|
| `0x0100` | `MatterOnOffLight` | `on_off_light` | `bool`, bidirectional writes |
| `0x0101` | `MatterDimmableLight` | `dimmable_light` | `u8`, two clusters on one endpoint |
| `0x010C` | `MatterColorTemperatureLight` | `color_temperature_light` | `u16`, three clusters on one endpoint |
| `0x0302` | `MatterTemperatureSensor` | `temperature_sensor` | `i16`, read-only device-to-controller |

Between them these cover `bool`, `u8`, `u16` and `i16`, single and multi-cluster
endpoints, and both data directions. `u32` appears almost entirely in read-only
metadata such as FeatureMap and arrives free with the table.

Remaining types, mapping clean, added as table rows:

| Device type ID | Arduino class | esp_matter namespace (v1.5.1) |
|---|---|---|
| `0x000F` | `MatterGenericSwitch` | `generic_switch` |
| `0x0015` | `MatterContactSensor` | `contact_sensor` |
| `0x002B` | `MatterFan` | `fan` |
| `0x0041` | `MatterWaterFreezeDetector` | `water_freeze_detector` |
| `0x0043` | `MatterWaterLeakDetector` | `water_leak_detector` |
| `0x0044` | `MatterRainSensor` | `rain_sensor` |
| `0x0071` | `MatterTemperatureControlledCabinet` | `temperature_controlled_cabinet` |
| `0x0107` | `MatterOccupancySensor` | `occupancy_sensor` |
| `0x0305` | `MatterPressureSensor` | `pressure_sensor` |
| `0x0307` | `MatterHumiditySensor` | `humidity_sensor` |

### 6.3 Namespace drift between esp-matter revisions

arduino-esp32 3.3.8 bundles **esp_matter 1.4.1**; this firmware pins **v1.5.1**.
Several namespaces were renamed between them, so the Arduino library's call
sites are not a usable guide to our namespace names:

| Arduino class | Namespace in 1.4.1 | Namespace in v1.5.1 | Device type ID |
|---|---|---|---|
| `MatterOnOffPlugin` | `on_off_plugin_unit` | `on_off_plug_in_unit` | `0x010A` |
| `MatterDimmablePlugin` | `dimmable_plugin_unit` | `dimmable_plug_in_unit` | `0x010B` |
| `MatterWindowCovering` | `window_covering_device` | `window_covering` | `0x0202` |

Three Arduino classes call namespaces that exist in **neither** revision. Their
`::create()` targets are referenced only by the Arduino library's own sources,
which means those three classes do not build against the esp_matter that
arduino-esp32 3.3.8 ships with either:

| Arduino class | Namespace it calls | Status | Likely target |
|---|---|---|---|
| `MatterColorLight` | `rgb_color_light` | Not in 1.4.1 or v1.5.1 | `extended_color_light`, `0x010D` |
| `MatterEnhancedColorLight` | `enhanced_color_light` | Not in 1.4.1 or v1.5.1 | `extended_color_light`, `0x010D` |
| `MatterThermostat` | `multi_mode_thermostat` | Not in 1.4.1 or v1.5.1 | `thermostat`, `0x0301` |

These three must be resolved before they are scheduled, not while implementing
them. The "likely target" column is inference from the class behaviour and is
explicitly unverified. Note that `MatterColorLight` and `MatterEnhancedColorLight`
would both resolve to `0x010D`; that is harmless, since two host classes
producing an identical endpoint reconcile as identical, which they are.

This drift is also a live risk for phase C4: a host library written against the
arduino-esp32 class surface talks to a firmware built on a different esp-matter
revision than the one those classes were written for.

**Accepted while prototyping; a release blocker.** For prototyping, working
around the drift per device type as we add it is fine, and that is the agreed
position. It does not survive delivery: shipping a host library whose class
surface is defined by esp_matter 1.4.1 against firmware pinned to v1.5.1 means
every future core or SDK bump can silently rename a namespace out from under a
device type that used to work. Before anything is delivered we need a settled
answer on which revision is normative and how the two are kept in step, whether
that is pinning the Arduino core, tracking a single esp-matter, or declaring our
own device type names and mapping them internally. Recorded here so the decision
is made deliberately rather than discovered during a release.

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
| 24–26 | Thread | connectivity change, state change, interface state change |
| 27–31 | Reserved | future events |

Bits 24 to 26 are **allocated now although Thread is not built** (§13). The
firmware does not emit them on a WiFi image, and a host may set them harmlessly.
Fixing the layout while the mask is unpublished costs nothing; renumbering it
after a host library ships is a breaking change, and Thread is a build-time
variant that will otherwise arrive after C4 and want bits.

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

## 8.1 Network transport query

```
AT+MTNET?   ->  +MTNET:<transport>,<enabled>,<connected>
```

- `<transport>`: `WIFI` or `THREAD`, fixed at build time (§13).
- `<enabled>`: `1` when the transport is compiled in and started.
- `<connected>`: `1` when the operational network is up.

This backs five reference-API getters that currently have **no** `AT+MT`
equivalent at all: `isWiFiStationEnabled()`, `isWiFiConnected()`,
`isThreadEnabled()`, `isThreadConnected()` and `isDeviceConnected()`. That gap
exists independently of Thread; Thread only makes it obvious.

It also tells a host which image it is talking to. Matter-over-Thread is a
Kconfig option (`ENABLE_MATTER_OVER_THREAD`, gated on `OPENTHREAD_ENABLED`), so
transport is a **build-time** choice and the two variants are separate images
selected by reflash, the same model already used for the ESP-NOW and Matter
personalities. A runtime selector would be wrong even if the SDK offered one:
the Root Node's NetworkCommissioning cluster advertises WiFi or Thread features,
so transport is part of the data model, and changing it on a commissioned device
has the same consequences as changing the endpoint composition (§5).

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

### 12.1 C1 preconditions and open questions

Two assumptions in this design are load-bearing and unverified. They are handled
differently: P1 blocks, P2 is tracked.

**P1: PASSED (2026-07-27).** Six cold power cycles on the C6 all reported
`+MTSPIKE:1,2,3` for an on/off light, dimmable light and temperature sensor
created in that order. Confirmed mechanically as well as empirically, see the
verdict note after the description below.

**P1 (was blocking): endpoint IDs are assigned sequentially in creation order,
and stably.**
§5.3 depends on rebuilding the composition in stored order reproducing the same
endpoint IDs on every boot. Persisted attribute values and the controller's
cached Descriptor `PartsList` are both keyed on endpoint ID, so if esp-matter
ever renumbers, reuses, or reserves IDs differently across boots, the whole
persistence scheme is unsound. Verify by building a three-endpoint composition,
recording the IDs, power-cycling, and comparing. Also confirm the behaviour when
an endpoint fails to create mid-sequence: a partial build must not silently shift
the IDs of everything after it.

**Verdict and mechanism (2026-07-27).** Read from
`esp_matter/data_model/esp_matter_data_model.cpp` at v1.5.1:

- `endpoint::create()` assigns `endpoint->endpoint_id =
  current_node->min_unused_endpoint_id++` at line 1668. Every failure path (null
  node 1655, endpoint cap 1658, allocation failure 1665) returns **before** that
  line, so **a failed create consumes no endpoint ID**. An endpoint after a
  failed one therefore slides down into its ID. The boot rebuild in §5.3 must
  abort the whole composition on any single failure rather than skipping the
  failed entry, which is what the C1 plan already specifies.
- `min_unused_endpoint_id` is persisted to NVS, but `store_min_unused_endpoint_id()`
  (line 1678) and `read_min_unused_endpoint_id()` (line 175) are both gated on
  `esp_matter::is_started()`. Endpoints created **before** `esp_matter::start()`
  increment the counter in RAM only, so every boot starts from the same base and
  reproduces the same IDs. Endpoints created **after** start persist the counter,
  and their IDs climb monotonically across reboots and are never reused.

**P2 (tracked, not blocking): the boot commissioning window can be suppressed.**
§5.5 requires an unconfigured device to open no commissioning window, but CHIP
auto-opens one at boot when the node is uncommissioned. The likely mechanism is
building with `CHIP_DEVICE_CONFIG_ENABLE_PAIRING_AUTOSTART` disabled and having
the firmware open the window itself once a composition exists. That is
inference, not a verified fact about this esp-matter revision.

P2 has a consequence beyond §5.5 even when it succeeds: with autostart off, a
**configured but uncommissioned** device also stops opening its own window, so
the firmware must open one explicitly at the end of the §5.3 boot sequence. That
promotes `AT+MTCOMMISSION` from "reopen a window on an already-commissioned
device", as `AT_MT_SPEC.md` §3.5 currently describes it, to part of the normal
path. If P2 fails, §5.5 needs redesigning, most likely by letting the window open
and having the host clear the composition, which is a worse story.

**How P2 is handled.** It does not gate the composition work: nothing in
`AT+MTEP`, the NVS store, the boot rebuild or the device type table depends on
the answer. So C1 proceeds, with the window policy isolated behind a single
function (`mt_matter_boot_window_policy()`) called at the end of the §5.3 boot
sequence, and observations recorded as they come up:

| Date | Observation |
|---|---|
| 2026-07-26 | Raised. `CHIP_DEVICE_CONFIG_ENABLE_PAIRING_AUTOSTART` identified as the likely knob, unverified against esp-matter v1.5.1. |

The conclusion is a deliberate, targeted effort rather than a discovery made
mid-implementation, and it has a deadline: **P2 must be settled before the
`TESTING.md` Phase 2 assertions for §5.5 are written** (phase C5), because those
assertions encode whichever answer we land on. Until then §5.5 is provisional
and should be read as intent, not as settled behaviour.

### 12.2 Phases

| Phase | Deliverable |
|---|---|
| **C1** | Spike P1 (§12.1) first, then endpoint composition: `AT+MTEP` / `MTEPCLEAR` / `MTEPAPPLY`, NVS persistence, boot rebuild, unconfigured-state gating behind the P2 seam, and the four slice device types. |
| **C2** | `AT+MTATTR` mode parameter and the `+MTERR` code allocation across all existing handlers. |
| **C3** | Event mask, `+MTEVT`, removal of `+MTCOMMISSION`, `+MTIDENT`, `AT+MTNET?` (§8.1), and the resulting `AT_MT_SPEC.md` edits. |
| **C4** | `iLabs_Matter` host library: `ATLink` extraction, `ArduinoMatter`, `MatterEndPoint`, and the four endpoint classes. |
| **C5** | `TESTING.md` update and the regression harness (T1 and T2) covering the new surface. |

C1 through C3 are firmware-only and independently useful: they can be verified
with a terminal and `chip-tool` before any host library exists.

## 13. Out of scope

- The remaining 16 device types (§6.2), plus the three needing resolution in
  §6.3. Table rows against the C1 table, not design work.
- `AT+MTATTRX` implementation. Lands with Temperature Controlled Cabinet.
- **Thread transport.** Deferred per `ARCHITECTURE.md`, and best done after C4
  so there is a host library to exercise it. It is a build-time variant
  (`ENABLE_MATTER_OVER_THREAD`), so the two images differ only in configuration,
  and §8.1 plus the event bits at 24 to 26 are already shaped for it.

  Two preconditions, in this order:

  1. **Switch to a single-app partition first.** The dual-OTA table from B2
     bring-up is still in place: `ota_0` and `ota_1` at `0x1e0000` each. Today's
     binary leaves 15% free in that partition, and Thread's measured +164 KB
     would cut it to about 7%. That fits, but bringing up a new transport with
     7% headroom is needlessly unpleasant, and single-app removes the constraint
     entirely. The switch was decided during B2 and never executed.
  2. **A Thread Border Router on the bench.** A rig change `TESTING.md` §2 has
     to absorb, and a hard dependency: without one, a Thread image cannot reach
     a fabric at all.
- `AT+MTOTA`. Covered by `FIRMWARE_UPDATE_SPEC.md`.
- Composed endpoints and bridged devices. The reference library's
  `MatterComposedLights` example uses ordinary endpoints, so nothing here blocks
  it, but endpoint parent/child relationships are not modelled.
- Per-device commissioning credentials. Still the esp-matter test DAC.
