# Generic switch and color light design: the cheap two of the parked trio

Date: 2026-08-04. Adds MatterGenericSwitch and MatterColorLight support
across firmware and library, taking the supported set to 18 of
arduino-esp32's 20 endpoint classes. The third parked class
(TemperatureControlledCabinet, gated on AT+MTATTRX) and the two
sub-deferrals (occupancy HoldTime, window-covering absolute position)
stay parked; scope decided with the user 2026-08-04.

## 1. What planning established (evidence in D106's exploration)

- Upstream's MatterGenericSwitch is minimal: begin/end/click plus the
  inherited callback; click() emits ONE Matter EVENT (Switch cluster
  InitialPress, position 1) via a scheduled lambda; the endpoint is
  configured with the Momentary Switch feature only. Device type 0x000F.
- esp-matter 21aa3d1 provides one-line event senders
  (cluster::switch_cluster::event::send_initial_press(endpoint, position)
  forwarding to CHIP's SwitchServer), and the event METADATA is created
  at endpoint build time by the momentary_switch feature add(). CHIP's
  EventLogging requires the Matter stack lock or CHIP-thread marshalling;
  our bridge functions' ChipStackLock pattern satisfies it.
- The switch cluster's create() runs VALIDATE_FEATURES_EXACT_ONE on
  latching vs momentary: a default config (feature_flags 0) ASSERTS.
  This is abort trap number four, same family as window covering,
  thermostat and occupancy.
- Upstream's MatterColorLight reports device type 0x010D (the Extended
  Color Light ID) with a hand-built hue/saturation-only ColorControl.
  Our firmware's existing 0x010D row already serves HS + XY + CT (the
  0.3.0 bolt-on), a strict superset: ColorLight therefore needs NO
  firmware change at all. Stock esp-matter's extended_color_light has no
  HS in this revision, which is why upstream rolled its own namespace
  and why our bolt-on was necessary in the first place.

## 2. Firmware

- **Table row**: generic_switch (0x000F), thunk setting
  switch_cluster.feature_flags to the momentary-switch feature id ONLY
  (upstream parity; satisfies the exact-one validation). The abort-trap
  comment convention from the other three trapped types applies, and the
  ARCHITECTURE 8.2 trap list grows to four.
- **New command, AT+MTSWITCH** (combined-grammar rules as everywhere):
  - `AT+MTSWITCH=<ep>[,<action>]`: action 0 (default when omitted)
    emits InitialPress with position 1 on endpoint <ep>. Reply OK after
    the event is queued; +MTERR:1 for any action other than 0 (values
    reserved for future switch features); the established lookup codes
    for a bad endpoint (+MTERR:2 family) and for an endpoint whose
    device type is not generic_switch (the cluster lookup fails
    naturally: same code the data-model lookups already use).
  - Query and exec forms are wrong-form: bare ERROR.
  - This is the AT surface's first EVENT-emission command. The spec
    section says so explicitly: events are fire-and-forget toward
    subscribed controllers, do not echo as +MTATTR URCs, and are not
    readable back over AT.
- **Bridge**: mt_matter_switch_click(uint16_t ep) in main.cpp, holding
  ChipStackLock, resolving the endpoint and calling
  send_initial_press(ep, 1). Declared in mt_matter.h, called from the
  mt_at.c handler (mt_at.c stays C).
- **Docs**: AT_MT_SPEC.md gains the AT+MTSWITCH section and the 0x000F
  table row (18 rows); ARCHITECTURE.md 8.2 gains the fourth trap and a
  sentence on the event surface. README table updated. MT_FW_VERSION
  0.3.1 (additive contract).

## 3. Library

- **MatterGenericSwitch**: mirror of upstream's API exactly: begin()
  (declares 0x000F, no AT traffic), end(), click() sending
  `AT+MTSWITCH=<ep>` and returning true on OK, attributeChangeCB
  override present but a no-op body (the switch has no
  library-consumed attributes; document that events do not arrive as
  URCs). No cache. hearthAttrTypeFor falls through to the base.
- **MatterColorLight**: mirror of upstream's API (begin with initial
  state + HSV, on/off setters/operators, setColorRGB/setColorHSV,
  getColorRGB/getColorHSV, onChangeOnOff/onChangeColorHSV/onChange,
  updateAccessory): drives OnOff 0x0006/0x0000, LevelControl
  0x0008/0x0000 (the HSV value component, as upstream does) and
  ColorControl 0x0300 CurrentHue/CurrentSaturation. Declares 0x010D
  over the existing firmware row; deviation note states the wire
  carries XY and CT attributes this class never touches (superset,
  harmless). Reuses HearthColorUtil conversions.
- Tests per the established recipe (the switch's minimum list adapts:
  begin/declare/adopt, click sends the exact command and returns
  OK-gated, click on ERROR returns false, re-begin refused; no
  URC/cache cases since there is no cache). Examples: both upstream
  sketches byte-identical, compile-checked for the Challenger.
  keywords.txt, README (18 supported / 2 parked + sub-deferrals),
  library.properties 0.3.1.

## 4. Verification

- Host suites in both repos at every boundary.
- Bench (one short operator session, no unplugs): compose 0x000F +
  0x010D; commission; `AT+MTSWITCH=<switch-ep>` observed controller-side
  (chip-tool subscribe-event switch initial-press, or read-event after
  the fact) proving the event reaches the fabric; ColorLight HSV
  round-trip via the library sketch (same wire path the 0.3.0 smoke
  proved, re-pinned through the new class); unknown action answers
  +MTERR:1; AT+MTSWITCH against a non-switch endpoint answers the
  documented lookup error.

## 5. Out of scope, recorded

- Richer switch events (short/long release, long press, multi-press:
  the MSR/MSL/MSM features) and latching switches: the action field is
  reserved for them; firmware feature_flags stay momentary-only until a
  consumer exists.
- AT+MTATTRX and TemperatureControlledCabinet (parked, unchanged).
- Occupancy HoldTime, window-covering absolute position (parked,
  unchanged).
