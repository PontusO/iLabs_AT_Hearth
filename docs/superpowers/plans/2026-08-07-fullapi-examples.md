# FullAPI Examples Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One complete-API reference sketch for each of the library's 31
endpoint classes under examples/FullAPI/, library 0.4.1.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-06-fullapi-examples-design.md`).
Documentation-only round in the library repo: a uniform CDC-driven
sketch template instantiated 31 times, batched by recipe family so a
reviewer's banner-vs-header coverage check falls between genuinely
different shapes. No firmware work; no library source changes.

**Tech Stack:** arduino-pico sketches, arduino-cli compile checks for
pico:rp2040:challenger_2350_wifi6_ble5.

## Global Constraints

- **No em dashes** anywhere, sketches and comments included.
- All work in /home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth
  on branch `fullapi-examples` (created at E1 dispatch from main
  1fe482f). Dropbox: work in place, never symlink.
- Layout: `examples/FullAPI/<ClassName>/<ClassName>.ino` (folder name
  equals sketch name, one per class). The 19 upstream parity examples
  at the examples root are UNTOUCHABLE (byte-identity is load-bearing);
  the two scenario showcases stay as they are.
- Coverage contract (spec section 3): everything public in the class
  header EXCEPT attributeChangeCB, hearthAttrTypeFor,
  hearthOnForwardedCommand, hearthOnReconciled, operator overloads,
  and end() (comment-only). Controller-observable effects print a
  one-line chip-tool command.
- Template (spec section 2) is binding: banner-as-checklist, all
  callbacks registered with prints, Hearth.poll() first in loop with
  the why-comment, single-char CDC menu, '?' help, no blocking waits,
  no delay() longer than 20 ms.
- House truths where they apply: poll-latency warning (DoorLock),
  OffOnly (Cooktop: menu has no on), dead-front (RoomAC), AQ no-echo
  note, one commented lastError() pattern per sketch.
- Every sketch compile-checked with arduino-cli for
  pico:rp2040:challenger_2350_wifi6_ble5; flash/RAM recorded per
  sketch in the report; discovery daemons killed after arduino-cli
  (pkill -f serial-discovery; pkill -f mdns-discovery; pkill -f
  dfu-discovery) and verified with ps -eo pid,comm | grep -i discovery
  (pgrep -f self-matches).
- No binaries in commits (compile in a scratch copy or clean build
  artifacts; check git show --stat); NEW commits never --amend;
  2-space /* */ style in sketches.
- Commit messages explain why and end exactly with:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

## The template, worked instance (MatterOnOffLight)

Every sketch is this shape; adapt the banner, globals, callbacks, and
menu to the class. Read the class header FIRST and build the banner
from its public section; the banner is the reviewer's checklist.

```cpp
/*
 * FullAPI reference: MatterOnOffLight
 *
 * Demonstrates the complete public API of this class. The banner below
 * is the coverage checklist: every public member, and where this
 * sketch exercises it.
 *
 *   MatterOnOffLight()            global object below
 *   begin(bool initialState)      setup()
 *   setOnOff(bool)                menu '1' / '0'
 *   getOnOff()                    menu 's'
 *   toggle()                      menu 't'
 *   onChange(cb)                  setup(), prints on any change
 *   onChangeOnOff(cb)             setup(), prints the new state
 *   updateAccessory()             menu 'u' (see its header comment)
 *   end()                         not called: tearing the endpoint
 *                                 down mid-demo is not a usable demo;
 *                                 call it when retiring the endpoint
 *
 * Observe controller-side:  chip-tool onoff read on-off <node> <ep>
 *
 * Error handling pattern (applies to every setter in this library):
 *   if (!Light.setOnOff(true)) {
 *     // Hearth.lastError() holds the +MTERR code (0 after a bare
 *     // ERROR); see the library README's error section.
 *   }
 */
#include <Matter.h>

MatterOnOffLight Light;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 8000) {}

  Light.onChange([](bool state) {
    Serial.print("onChange: ");
    Serial.println(state);
    return true;
  });
  Light.onChangeOnOff([](bool state) {
    Serial.print("onChangeOnOff: ");
    Serial.println(state);
    return true;
  });

  Light.begin(false);
  Matter.begin();
  Serial.println("FullAPI MatterOnOffLight ready; '?' for menu");
}

void printHelp() {
  Serial.println("1=on 0=off t=toggle s=getOnOff u=updateAccessory ?=help");
}

void loop() {
  /* Every iteration, unconditionally: URC dispatch, and therefore every
   * onChange* callback in this sketch, only runs from inside poll(). */
  Hearth.poll();

  if (Serial.available()) {
    switch (Serial.read()) {
      case '1': Serial.println(Light.setOnOff(true)  ? "on: OK"  : "on: failed");  break;
      case '0': Serial.println(Light.setOnOff(false) ? "off: OK" : "off: failed"); break;
      case 't': Serial.println(Light.toggle() ? "toggled" : "toggle failed");      break;
      case 's': Serial.print("state: "); Serial.println(Light.getOnOff());         break;
      case 'u': Light.updateAccessory(); Serial.println("updateAccessory called"); break;
      case '?': printHelp();                                                       break;
    }
  }
  delay(10);
}
```

Adaptation rules the worked instance cannot show:
- Callback signatures vary (bool/uint8_t/double/void returns): match
  the header exactly; bool-returning callbacks return true in demos.
- Mutually exclusive begin() variants (cabinet TN/TL): a
  `#define DEMO_MODE_TN 1` selects; both paths present under #if.
- Read-only telemetry setters (pump Max*) go in the menu; getters that
  are URC-fed print with a note saying a controller change feeds them.
- Adjudicated classes (DoorLock): verdict callbacks controlled by a
  menu-set policy flag, plus the HEARTH_CMD_TIMEOUT link event handler
  and the poll-latency warning in the banner.
- Where a class's constants matter (AirQuality_t levels, DoorLock
  sources/states), the menu demonstrates at least two distinct values
  and the banner lists the constant set.

The compile sweep (used by every task; run from the repo root):

```sh
for d in examples/FullAPI/*/; do
  n=$(basename "$d")
  arduino-cli compile --fqbn pico:rp2040:challenger_2350_wifi6_ble5 "$d" 2>&1 \
    | grep -E "bytes|error" | sed "s/^/$n: /"
done
pkill -f serial-discovery; pkill -f mdns-discovery; pkill -f dfu-discovery
ps -eo pid,comm | grep -i discovery || true
```

---

### Task E1: Lights, plugs, mounted (9 sketches)

**Files:**
- Create: `examples/FullAPI/<C>/<C>.ino` for MatterOnOffLight,
  MatterDimmableLight, MatterColorTemperatureLight, MatterColorLight,
  MatterEnhancedColorLight, MatterOnOffPlugin, MatterDimmablePlugin,
  MatterMountedOnOffControl, MatterMountedDimmableLoadControl

**Interfaces:**
- Consumes: the template above; each class's header under
  src/MatterEndpoints/ (the banner is built from its public section).
- Produces: nothing later tasks depend on beyond the shared layout.

- [ ] **Step 1**: create branch fullapi-examples from main (1fe482f).
- [ ] **Step 2**: for each class, read the header, write the sketch
per the template (MatterOnOffLight IS the worked instance above,
verbatim start). Color classes demonstrate every color entry point
(HSV/RGB/mireds setters and getters as each header offers) with at
least two values each; brightness classes demonstrate the
brightness+onChangeBrightness pair.
- [ ] **Step 3**: run the compile sweep over the nine; all green;
record sizes.
- [ ] **Step 4**: commit.

### Task E2: Sensors (11 sketches)

**Files:**
- Create: FullAPI sketches for MatterTemperatureSensor,
  MatterHumiditySensor, MatterPressureSensor, MatterLightSensor,
  MatterFlowSensor, MatterAirQualitySensor, MatterOccupancySensor,
  MatterContactSensor, MatterRainSensor, MatterWaterFreezeDetector,
  MatterWaterLeakDetector

- [ ] **Step 1**: per class per the template. Sensor menus drive the
measurement setters up/down ('+'/'-' plus one absolute-set key) and
read back; classes with onChange demonstrate it, classes without
(Light/Flow/AirQuality) carry the banner note WHY (kView-only
attributes, controllers cannot write them; see README). AirQuality
cycles all seven enum values on 'q' and carries the no-echo note.
- [ ] **Step 2**: compile sweep green; record sizes.
- [ ] **Step 3**: commit.

### Task E3: Appliances and climate (9 sketches)

**Files:**
- Create: FullAPI sketches for MatterFan, MatterAirPurifier,
  MatterExtractorHood, MatterCooktop, MatterThermostat,
  MatterRoomAirConditioner, MatterPump, MatterWindowCovering,
  MatterTemperatureControlledCabinet

- [ ] **Step 1**: per class per the template. Cooktop's menu has no
"on" key and the banner carries the OffOnly sentence. RoomAC
demonstrates all four onChange callbacks and the dead-front sentence.
Pump drives OperationMode + the three telemetry setters and prints
the Effective* getters with the URC-fed note. WindowCovering drives
the percent100ths pairs and documents the absent absolute-position
API (1.5.1 fact, per the class header). Cabinet: #define DEMO_MODE_TN
switch, both begin() paths, the numeric-levels setter AND
setSupportedTemperatureLevelLabels demo with a comma-containing label,
per-mode menus.
- [ ] **Step 2**: compile sweep green (BOTH cabinet modes compiled:
once as-is, once with the #define flipped via a temporary edit,
restored after; record both sizes).
- [ ] **Step 3**: commit.

### Task E4: Adjudicated + event classes, README, 0.4.1, integration

**Files:**
- Create: FullAPI sketches for MatterDoorLock, MatterGenericSwitch
- Modify: `README.md` (an "Examples" section documenting the three
  tiers: parity proofs at the root, FullAPI references, scenario
  showcases), `library.properties` (version=0.4.1)

- [ ] **Step 1**: MatterDoorLock sketch: onLock/onUnlock verdicts from
a menu-set policy flag, setLockState demonstrating at least two
distinct sources (manual and keypad), lock()/unlock() conveniences,
getLockState, HEARTH_CMD_TIMEOUT via Hearth.onLinkEvent, poll-latency
warning in the banner. MatterGenericSwitch sketch: click() on a menu
key, the no-writable-attribute banner note, onChange if the header
offers it.
- [ ] **Step 2**: README Examples section (three tiers, one paragraph
each, the FullAPI banner-as-checklist convention explained);
library.properties 0.4.1.
- [ ] **Step 3**: run the compile sweep over ALL of examples/FullAPI/
(31 sketches) as the integration gate; all green; record the full
size table in the report.
- [ ] **Step 4**: commit.

### Task E5: Bench spot-run (no unplugs; scripted power cuts allowed)

- [ ] Flash the standard bench firmware state (0.4.0 resident is
fine; factory-fresh; espnow bridge for the AT phase is NOT needed:
this is sketch-side only).
- [ ] Flash the FullAPI MatterDoorLock sketch; commission; drive the
menu over CDC: policy allow -> controller lock SUCCESS with the
verdict print; policy deny -> FAILURE; setLockState keypad source
read back controller-side.
- [ ] Flash the FullAPI MatterPressureSensor (or another sensor)
sketch; drive '+'/'-' and verify controller-side reads follow.
- [ ] Restore bench (factory-fresh, 0x0100 composition, espnow
bridge), verbatim evidence in the report, PSK hygiene.
