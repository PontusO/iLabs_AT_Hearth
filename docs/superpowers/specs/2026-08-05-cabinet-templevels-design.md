# Temperature-controlled cabinet design: variants, the level delegate, and label transport

Date: 2026-08-05. Completes the upstream class set (20 of 20) by adding
MatterTemperatureControlledCabinet across firmware and library. Replaces
the reserved-but-never-designed generic AT+MTATTRX with what the SDK
facts actually support; the ATTRX findings are recorded in section 8 so
the reservation stays honest.

## 1. Decisions taken with the user (2026-08-05)

- **Composition variants**: AT+MTEP gains an optional per-endpoint
  variant byte, persisted in a v2 composition blob. Generic mechanism;
  the cabinet is its first consumer (variant 0 = TemperatureNumber,
  1 = TemperatureLevel). Chosen over TN-only shipping and over a
  parallel staging command.
- **Level labels cross the wire as strings, this round.** Initially
  scoped as numeric identifiers with generated labels; reversed once
  the controller-visibility fact was on the table (controllers read
  SupportedTemperatureLevels FROM the device; host-side strings can
  never reach a controller UI). Generated-label-only was judged
  unusable in practice.
- **Command-forwarding (door-lock family) deferred** to a future round;
  recorded as a parked idea (the lamp-vs-lock asymmetry: cluster
  servers resolve lamp-class commands autonomously, lock-class commands
  demand a synchronous app verdict across the AT link).

## 2. SDK facts this design is built on (explorer evidence, 2026-08-05)

- esp-matter 21aa3d1's public attribute API REJECTS ARRAY-typed values
  in both directions (get_val at esp_matter_data_model.cpp:932,
  set_val/update at :1083). String/octet types work with deep-copy
  semantics and a 512-byte TLV bound.
- SupportedTemperatureLevels is not in the attribute table at all:
  ATTRIBUTE_FLAG_MANAGED_INTERNALLY, served by CHIP's
  temperature-control-server through a globally registered
  SupportedTemperatureLevelsIteratorDelegate (SetInstance). esp-matter
  ships no default delegate; the application must provide one.
- temperature_control::create() is abort trap number five:
  VALIDATE_FEATURES_EXACT_ONE(TemperatureNumber, TemperatureLevel) at
  esp_matter_cluster.cpp:2849-2850; a default config asserts at boot.
  The mode is therefore fixed at endpoint creation, i.e. it must live
  in the persisted composition.
- The TN feature's attributes (TemperatureSetpoint, MinTemperature,
  MaxTemperature, Step) and TL's SelectedTemperatureLevel are ordinary
  integer attributes already served by the existing AT+MTATTR
  converters.
- The AT link comfortably carries the label payload on one line
  (Matter caps each label at 16 chars, we cap the list at 16 entries;
  the ESP-NOW sibling runs 8 KB command lines as precedent).
- Upstream arduino-esp32's cabinet API is NUMERIC
  (begin(uint8_t *levels, count) / setSupportedTemperatureLevels):
  upstream never sends label strings from the sketch either; its
  bundled newer esp-matter differs structurally, so its internals are
  not a porting reference, only its API surface is.

## 3. Composition format v2 (firmware)

- The blob gains one variant byte per entry; the format version bumps.
  The decoder accepts v1 blobs and reads every entry as variant 0;
  encoding always writes v2. No in-place migration: the stored blob
  upgrades the next time a composition is applied.
- mt_composition.c stays pure and host-testable; the v1/v2 decode
  matrix (v1 blob, v2 blob, v2 with all-zero variants, truncated/corrupt
  of each) gets host-test coverage.
- Grammar: `AT+MTEP=<id>[,<variant>]`, default 0. A variant the device
  type does not define answers +MTERR:1 at staging time (the table
  knows each type's valid variant range; today: cabinet 0..1, every
  other type 0 only).
- `AT+MTEP?` reports `+MTEP:<index>,<ep_id>,<devtype>[,<variant>]` with
  the variant field present ONLY when nonzero: byte-identical output
  for every existing composition, so no existing host parser changes
  behavior.
- mt_devtypes gains variant-aware creation: the thunk signature grows a
  variant argument (all existing thunks ignore it); the cabinet thunk
  branches on it to set exactly one of the two features.

## 4. The level delegate and AT+MTTEMPLEVELS (firmware)

- The firmware registers one SupportedTemperatureLevelsIteratorDelegate
  at startup (CHIP's design is a global singleton queried per
  endpoint), backed by a small per-endpoint store: up to 16 labels of
  up to 16 bytes for each TL-variant cabinet endpoint.
- `AT+MTTEMPLEVELS=<ep>,"<label1>"[,...,"<labelN>"]`:
  - Set-only; quoted labels; N 1..16; label length 1..16 bytes,
    printable ASCII, no double-quote character inside a label
    (+MTERR:1 otherwise). Commas inside labels are legal (the handler
    parses quotes itself; at_core is untouched).
  - +MTERR:2 unknown endpoint; +MTERR:3 endpoint without a
    TemperatureControl cluster; +MTERR:4 cluster present but no
    SupportedTemperatureLevels (a number-variant cabinet), keeping the
    established lookup-code semantics; on success the store updates and
    the attribute reports dirty so subscribed controllers refresh.
  - Labels are NOT persisted: the sketch re-sends them after every
    boot (same philosophy as attribute state; the store starts empty
    and controllers read an empty list until the host sets it).
- SelectedTemperatureLevel is an ordinary u8 attribute: reads, writes
  and URCs already work through AT+MTATTR unchanged.
- MT_FW_VERSION 0.3.2.

## 5. Library

- MatterTemperatureControlledCabinet mirrors upstream's full API:
  - `begin(double setpoint, double min, double max, double step)`:
    declares 0x0071 variant 0; drives the four i16 attributes
    (hundredths, conversion arithmetic copied exactly from upstream).
  - `begin(uint8_t *supportedLevels, uint16_t levelCount,
    uint8_t selectedLevel)`: declares 0x0071 variant 1; at reconcile
    sends AT+MTTEMPLEVELS with GENERATED labels ("Level <n>" per
    identifier), then writes SelectedTemperatureLevel.
  - setters/getters per upstream (setTemperatureSetpoint, setStep,
    setSelectedTemperatureLevel, setSupportedTemperatureLevels,
    getSupportedTemperatureLevelsCount, ...); internal cap 16.
- Hearth extension (documented addition, DE102 pattern):
  `setSupportedTemperatureLevelLabels(const char *const *labels,
  uint16_t count)`: real names on controller UIs; sends the strings
  verbatim through AT+MTTEMPLEVELS. Callable instead of or after the
  numeric path; the numeric identifiers remain the sketch-side handle
  either way (SelectedTemperatureLevel is the list index).
- Registry/reconcile variant plumbing: hearthDeclare carries a variant
  (default 0); reconcile emits `AT+MTEP=<id>,<variant>` when nonzero
  and includes the variant in the adopt-vs-rebuild comparison, parsing
  the optional fourth +MTEP? field.
- library.properties 0.3.2. Upstream example copied byte-identical and
  compile-checked.

## 6. Verification

- Host: codec v1/v2 matrix; cabinet class tests per the established
  recipe in both modes incl. the label path (exact AT+MTTEMPLEVELS
  wire strings, quote handling) and variant plumbing through the
  reconcile tests.
- Bench (one operator session, no unplugs): compose one cabinet per
  mode (fifth trap live both ways); chip-tool reads
  SupportedTemperatureLevels and shows the host-set labels verbatim;
  controller writes SelectedTemperatureLevel and the sketch callback
  fires; number-mode setpoint round-trip; a v1-blob decode check runs
  host-side (crafted blob through the codec tests, not on hardware);
  negative pins for MTTEMPLEVELS (+MTERR:1/2/3 cases and bare-ERROR
  wrong form); bench restore.

## 7. Out of scope, recorded

- Command-forwarding for app-adjudicated commands (door-lock family):
  deferred round, parked with the design asymmetry documented; flagged
  by the user as a welcome future challenge.
- Persisting level labels on the C6 (rejected: labels are sketch state,
  re-sent per boot, matching the attribute-state philosophy).
- Any further AT+MTATTRX surface: see section 8.

## 8. The ATTRX findings, recorded so the reservation stays honest

The reserved AT+MTATTRX was scoped this round and deliberately NOT
built. What the investigation established: a generic opaque-attribute
transport on this SDK pin could only ever serve char/octet string
attributes (real but currently consumer-less), because esp-matter's
public API rejects ARRAY values outright and every list attribute in
sight (cabinet levels, occupancy HoldTimeLimits) is
MANAGED_INTERNALLY behind a per-cluster delegate that no generic
transport can reach. List support is therefore always per-cluster
firmware work (as done here for the cabinet), and ATTRX should be
revisited only if a concrete string-attribute consumer appears
(NodeLabel-class). Raising esp-matter's ARRAY/AAI limitations upstream
can ride along with the already-parked runtime-driver-selection
proposal at project release (graph I90).
