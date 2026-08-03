# iLabs_Hearth library: transport support and 0.2.0 contract catch-up

Date: 2026-08-03. Companion to firmware 0.2.0 (the C6 combined image,
`2026-08-01-c6-combined-image-design.md`). The library lives at
`Dropbox/Arduino/libraries/iLabs_Hearth` (its own git repo, branch `main`);
this spec lives in the firmware repo with the rest of the C-phase designs.

## 1. Why

The host library was built against the C4-era AT contract. Firmware 0.2.0
changed that contract in three ways the library does not yet know about:

- `AT+MTTRANSPORT` exists on the combined image: a persisted transport
  selection, applied at next boot, reported as `+MTTRANSPORT:<active>,<stored>`.
- The P2 mismatch surface: `AT+MTNET?` grew a fourth field (the mismatch
  flag) and `+MTEVT:27` signals a transport mismatch at boot. Today the
  library drops the fourth field on the floor (`src/Hearth.cpp:572`, the
  `strtol` never looks past the third field) and bounds-checks event 27
  out of existence (`src/Hearth.cpp:277`, `bit >= 27`).
- The reset-semantics documentation in the library is wrong: `README.md`
  and `Hearth.h:149-150` claim `AT+MTRESET` "does not touch the Matter
  fabric", but `AT+MTRESET` is the Matter reset (fabric, credentials and
  attribute persistence erased; composition and transport setting
  survive). `decommission()` (`src/Hearth.cpp:849`) sends it and works
  precisely BECAUSE it erases the fabric; the current comment says it
  relies on a reboot, which is false.

## 2. Decisions taken with the user (2026-08-03)

- **Full Hearth extension API**: setTransport / transport / mismatch
  surface on `HearthClass`, never on `ArduinoMatter`. The parity class
  stays byte-identical to upstream arduino-esp32.
- **Event 27 routes to `onLinkEvent`** as a new `HEARTH_TRANSPORT_MISMATCH`
  code, beside `HEARTH_COPROCESSOR_REBOOTED`. The parity `onEvent()` enum
  is not extended.
- **Lazy compat**: no probing, no version parsing. Transport calls issue
  the command and map the firmware's `+MTERR:8` (unknown command, the
  natural answer on single-stack images) to a new not-supported error
  code. Sketches that never touch transport pay nothing.

## 3. API (Hearth extensions, `src/Hearth.h`)

```cpp
typedef enum {
    HEARTH_TRANSPORT_WIFI = 0,
    HEARTH_TRANSPORT_THREAD = 1,
} HearthTransport;

/* Stage the transport for the next boot. Returns true when the firmware
 * confirms the setting is stored. The host owns the reboot: nothing
 * changes until the co-processor restarts. On single-transport firmware
 * this returns false with lastError() == HEARTH_ERR_NOT_SUPPORTED. */
bool setTransport(HearthTransport t);

/* Read the active and stored transport. A pending switch shows as
 * active != stored. Same not-supported mapping as setTransport(). */
bool transport(HearthTransport *active, HearthTransport *stored);

/* The mismatch flag from the last AT+MTNET? round-trip: true when the
 * device holds a fabric but the active transport is unprovisioned
 * (firmware spec 3.12.1). Any is*Connected()/is*Enabled() call, or
 * transportMismatch() itself, refreshes it. */
bool transportMismatch();
```

- `HEARTH_ERR_NOT_SUPPORTED` joins the existing `lastError()` code space.
- Wire grammar (authoritative in firmware `AT_MT_SPEC.md` 3.12.2):
  `AT+MTTRANSPORT=WIFI|THREAD` answers `OK` or `+MTERR:1`;
  `AT+MTTRANSPORT?` answers `+MTTRANSPORT:<active>,<stored>` then `OK`;
  absent command answers `+MTERR:8`. Names are upper-case exact.
- `transportMismatch()` issues `AT+MTNET?` (reusing `hearthQueryNet()`)
  so it is a live answer, consistent with the library's
  everything-is-a-live-query policy (no cached commissioning state).

## 4. Parser and event changes (`src/Hearth.cpp`)

- `hearthOnNetLine()` (`:550-574`): parse an optional fourth field into
  the net-query context. Three-field lines (older firmware) must parse
  exactly as today, mismatch defaulting to 0. The stale comment at
  `:540-542` ("transport is fixed at build time") is rewritten: one
  transport per BOOT, fixed per build only on single-stack images.
- Event table (`:237-265`): index 27 added; bounds check at `:277`
  becomes `bit >= 28`. Bit 27's action is NOT a parity-callback dispatch:
  it raises `HEARTH_TRANSPORT_MISMATCH` through the link-event callback.
  Unknown bits 28-31 stay silently dropped.
- `+MTERR:8` after `AT+MTTRANSPORT` maps to `HEARTH_ERR_NOT_SUPPORTED`
  in the command-response path; other commands' error mapping unchanged.

## 5. Documentation truth-pass

- `README.md` and `Hearth.h` reset-semantics text corrected: `AT+MTRESET`
  is the Matter reset (fabric, credentials, attribute persistence erased;
  composition and the stored transport survive); `AT+MTFRESET` adds the
  composition. `decommission()`'s doc comment states it erases the fabric
  via `AT+MTRESET`, which is the mechanism, not a side effect.
- Combined-image note where decommission is documented: network
  credential erasure fires only for the ACTIVE stack (firmware spec 3.10),
  so a dormant transport's credentials survive a decommission.
- `HearthCompat.h:35-66` comment updated for the three-variant firmware
  reality. `CONFIG_ENABLE_CHIPOBLE 1` stays force-defined: BLE is
  resident on all three images, that claim remains true.

## 6. Testing (MockStream harness, `test/host/`)

New cases, one new binary `test_transport` plus additions to `test_hearth`:

- `+MTNET` with 3 fields and with 4 fields: field values land correctly,
  3-field behavior byte-identical to today (closes the existing
  zero-coverage gap on `hearthOnNetLine`).
- `+MTEVT:27` dispatches `HEARTH_TRANSPORT_MISMATCH` to the link-event
  callback and does NOT invoke the parity `onEvent()` callback; bits 28+
  still silently dropped.
- `setTransport()` sends `AT+MTTRANSPORT=THREAD` and returns true on OK;
  `+MTERR:1` and `+MTERR:8` map to the right lastError codes.
- `transport()` parses `+MTTRANSPORT:WIFI,THREAD` into active/stored
  (pending-switch case) and `WIFI,WIFI` (steady state).
- `decommission()` semantics test pinning the corrected claim (sends
  AT+MTRESET, documented as fabric-erasing).

All existing tests keep passing (`make -C test/host run`).

## 7. Out of scope, recorded

- Host-driven firmware flashing to switch variants (FIRMWARE_UPDATE_SPEC
  territory; the combined image makes it less necessary, not more).
- Exposing factory reset (`AT+MTFRESET`) to sketches.
- Thread-specific commissioning helpers: BLE commissioning is
  transport-agnostic from the host's seat.
- Example sketch changes: they stay byte-identical to upstream.
- Version/capability negotiation beyond the lazy `+MTERR:8` mapping.
