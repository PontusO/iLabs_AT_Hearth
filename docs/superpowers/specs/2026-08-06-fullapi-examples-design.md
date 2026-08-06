# FullAPI examples design: one reference sketch per class

Date: 2026-08-06. Adds a complete-API reference example for every one
of the library's 31 endpoint classes, without touching the existing
example tiers. Library-only round; no firmware change.

## 1. Decisions taken with the user (2026-08-06)

- **Keep both sets.** The 19 byte-identical upstream examples stay at
  the examples root untouched: they are the compile-parity proof. The
  new set is ADDED.
- **Menu layout**: `examples/FullAPI/<ClassName>/<ClassName>.ino`, one
  per class, rendering as a single FullAPI submenu in the IDE.
- **Per class, not per wire ID** where they differ: MatterColorLight
  and MatterEnhancedColorLight each get an example (both ride 0x010D;
  the API difference is exactly what an example teaches).
- The two scenario showcases (MatterDoorLockAdjudicated,
  HearthSensorsAndAppliances) stay as they are; the FullAPI door lock
  example supersedes neither.

## 2. The sketch template (uniform across all 31)

1. **Banner comment = coverage checklist.** Lists the class's complete
   public surface: constructor, every begin() variant, every
   setter/getter, every callback, Hearth extensions, public constants;
   each line notes where in the sketch it is exercised. A reviewer
   checks the banner against the header's public section: the "covers
   all aspects" claim is mechanically checkable.
2. **setup()**: begin() with documented arguments; every callback
   registered, each printing to Serial. Mutually exclusive begin
   variants (the cabinet's TN/TL) select via a #define with both paths
   present in code.
3. **loop()**: `Hearth.poll()` unconditionally first with the standard
   why-comment, then a single-character CDC menu exercising every
   setter and reading back through every getter; '?' prints the menu.
   No blocking waits anywhere.
4. **House truths embedded where they apply**: poll-latency warning on
   adjudicated classes; OffOnly on Cooktop (its menu has no "on");
   dead-front on Room AC; the AirQuality no-echo note; one commented
   lastError() usage pattern per sketch.

## 3. Scope of "complete public surface"

Everything declared public in the class header except: base-class
plumbing a sketch never calls directly (attributeChangeCB,
hearthAttrTypeFor, hearthOnForwardedCommand, hearthOnReconciled,
operator overloads), and end() (demonstrated in comments only, since
tearing an endpoint down mid-demo is not a usable demo). Where a
method's effect is only observable controller-side, the sketch prints
what to run in chip-tool to observe it (one line, house command
style).

## 4. Verification

- Every one of the 31 sketches compile-checked for
  pico:rp2040:challenger_2350_wifi6_ble5 (scripted sweep, one
  arduino-cli loop; discovery-daemon cleanup after; flash/RAM numbers
  recorded per sketch in the task report).
- Reviewer gate per task: banner-vs-header coverage per class.
- Round close: bench spot-run of two representative sketches (one
  adjudicated: the FullAPI MatterDoorLock; one sensor) proving the
  template's loop behaves on hardware. No per-sketch bench.
- README: an "Examples" section documenting the three tiers (parity
  proofs at the root, FullAPI references, scenario showcases).
- library.properties 0.4.1. No keywords.txt changes (no new API).

## 5. Execution shape

Four authoring tasks batched by family, one integration task:
- E1: lights + plugs + mounted (OnOffLight, DimmableLight,
  ColorTemperatureLight, ColorLight, EnhancedColorLight, OnOffPlugin,
  DimmablePlugin, MountedOnOffControl, MountedDimmableLoadControl): 9.
- E2: sensors (Temperature, Humidity, Pressure, Light, Flow,
  AirQuality, Occupancy, Contact, Rain, WaterFreeze, WaterLeak): 11.
- E3: appliances + climate (Fan, AirPurifier, ExtractorHood, Cooktop,
  Thermostat, RoomAirConditioner, Pump, WindowCovering,
  TemperatureControlledCabinet): 9. Cabinet carries the #define mode
  switch and the labels extension demo.
- E4: adjudicated + event (DoorLock incl verdict callbacks and
  setLockState sources, GenericSwitch incl click()): 2, plus the
  README section and library.properties 0.4.1 and the compile-all
  sweep re-run as the integration gate.
- E5: bench spot-run (two sketches) + round close.

## 6. Out of scope, recorded

- Rewriting or annotating the upstream parity examples (byte-identity
  is load-bearing).
- Firmware changes of any kind.
- Translating examples or generating documentation sites from them.
