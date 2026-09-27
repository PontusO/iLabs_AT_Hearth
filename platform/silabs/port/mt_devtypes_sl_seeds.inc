/*
 * mt_devtypes_sl_seeds.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds the seed table and seed_slots (nRF 5905-7005).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, so the whole is still one translation
 * unit: file-local names stay file-local and the generated code is the
 * same as when this text lived in the .cpp. Split out 2026-09-25 so each
 * file fits a small model's context (graph T549, DE550). Do not add it
 * to hearth.slcp.
 */

/*
 * Boot values for attributes where zero is the wrong answer, little-endian
 * as the attribute store holds them (nRF 5979-6889). Two kinds of entry live
 * here:
 *
 *   Functional. LevelControl reads MinLevel and MaxLevel out of the store
 *   when the endpoint is enabled and keeps them as the movement bounds for
 *   the life of the endpoint. Left at zero, a dimmable light clamps every
 *   MoveToLevel to 0 and never lights.
 *
 *   Conformance. FeatureMap and ClusterRevision must describe the cluster
 *   the endpoint actually presents. Revisions are the tree's own
 *   zzz_generated/app-common/clusters/<Cluster>/Metadata.h kRevision; feature
 *   bits are from the matching Enums.h.
 *
 * Nullable attributes whose correct boot value is null carry the type's
 * null sentinel rather than a zero: NumericAttributeTraits::GetNullValue()
 * is the type maximum for unsigned and enum types (0xFF for a 1-byte one)
 * and the type minimum for signed ones (0x8000 for INT16S, stored
 * little-endian as 00 80).
 *
 * THE WHOLE TABLE IS TRANSFERRED, not just the rows the twenty ported
 * device types consult, and that is a deliberate departure from this file's own
 * "only what is ported" rule. Three reasons. It is const data keyed by
 * (cluster, attribute), so a row for a cluster no list declares is never
 * looked up and costs flash only. It compiles against the app-common id
 * headers, which the SDK generates for all 145 clusters whether or not their
 * servers are in the image, so nothing here depends on a cluster component.
 * And the seeds are the part of a device type most easily got wrong and
 * least visible when wrong: a batch that ports a device type and forgets its
 * FeatureMap row ships an endpoint that misreports its own capabilities, so
 * having the rows already here, already reviewed on the other arm, is worth
 * more than the flash.
 *
 * The `devtype` qualifier (nRF batch 2) exists because one (cluster,
 * attribute) pair does not always have one boot value across every device
 * type carrying it: 0x010C advertises ColorControl's CT alone and 0x010D
 * advertises HS|XY|CT. devtype 0 means "any device type". The `variant`
 * qualifier (nRF batch 8) is the same trick one member further out, for a
 * FeatureMap that is variant-dependent AND shared by two device types.
 * Both are LAST members, so a row that names neither keeps its exact
 * original text and gets the wildcard for free.
 *
 * THE ENCODING, because the zero-fill has to keep meaning "any variant":
 * 0 is the wildcard and a row naming a variant stores variant + 1.
 * seed_variant() is the only place that arithmetic appears.
 *
 * PRECEDENCE: the most specific matching row wins, scored devtype-qualified
 * 2, variant-qualified 1, both 3, neither 0. A row whose qualifier is
 * present but does not match is skipped outright.
 */
constexpr uint8_t kSeedAnyVariant = 0;
constexpr uint8_t seed_variant(uint8_t v) { return (uint8_t)(v + 1); }

struct attr_seed {
    ClusterId cluster;
    AttributeId attr;
    uint8_t size;
    uint8_t bytes[4];
    uint32_t devtype;
    uint8_t variant;
};

const attr_seed s_seeds[] = {
    /* OnOff */
    { OnOff::Id, OnOff::Attributes::GlobalSceneControl::Id, 1, { 0x01 } },     /* TRUE, cluster spec default */
    { OnOff::Id, OnOff::Attributes::StartUpOnOff::Id, 1, { 0xFF } },           /* null */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 } },
    { OnOff::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x06, 0x00 } },

    /* LevelControl */
    { LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, 1, { 0xFE } },
    { LevelControl::Id, LevelControl::Attributes::MinLevel::Id, 1, { 0x01 } },
    { LevelControl::Id, LevelControl::Attributes::MaxLevel::Id, 1, { 0xFE } },
    { LevelControl::Id, LevelControl::Attributes::OnLevel::Id, 1, { 0xFF } },  /* null */
    { LevelControl::Id, LevelControl::Attributes::StartUpCurrentLevel::Id, 1, { 0xFF } }, /* null */
    { LevelControl::Id, LevelControl::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 } },
    { LevelControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x06, 0x00 } },

    /* TemperatureMeasurement: no features, and no reading until the host
     * writes one, so all three values start null. */
    { TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MinMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MaxMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { TemperatureMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x04, 0x00 } },

    /* Identify: no features; IdentifyType 0 (None) is the zero-fill. */
    { Identify::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x06, 0x00 } },

    /* BooleanState: no features; StateValue false is the zero-fill (spelled
     * out anyway, since it is the attribute this cluster exists for). */
    { BooleanState::Id, BooleanState::Attributes::StateValue::Id, 1, { 0x00 } },
    { BooleanState::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* OccupancySensing: fix round 2, C2. The device-type XML alone was the
     * wrong source for FeatureMap: it names no <features> block, but the
     * CLUSTER XML (data_model/1.5/clusters/OccupancySensing.xml) declares
     * all eight Feature bits as `optionalConform choice="a" min="1"` --
     * choice group "a" requires AT LEAST ONE of them set, so FeatureMap 0
     * does not conform. Seeding a PIR sensor means Feature::
     * kPassiveInfrared (0x2) must be set.
     *
     * Trap worth naming explicitly: kPassiveInfrared is BIT 1 of Feature
     * (Enums.h:46-56, value 0x2), but OccupancySensorTypeBitmap::kPir is
     * BIT 0 of a DIFFERENT bitmap (Enums.h:64-70 -> Bitmap for
     * OccupancySensorTypeBitmap, value 0x1). The two "PIR" bits live in
     * unrelated attributes at different bit positions by design -- do not
     * copy one value into the other.
     *
     * Sensor-type fields seed a PIR default (type 0 = kPir, bitmap 0x01 =
     * kPir); see the comment above occupancyAttrs for why the seed rows
     * below are the only writer of these fields on this (dynamic)
     * endpoint. */
    { OccupancySensing::Id, OccupancySensing::Attributes::OccupancySensorType::Id, 1, { 0x00 } },
    { OccupancySensing::Id, OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id, 1, { 0x01 } },
    { OccupancySensing::Id, OccupancySensing::Attributes::FeatureMap::Id, 4,
      { 0x02, 0x00, 0x00, 0x00 } },
    { OccupancySensing::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* RelativeHumidityMeasurement: no features, and no reading until the
     * host writes one, so all three values start null (uint16, sentinel is
     * the type maximum 0xFFFF). */
    { RelativeHumidityMeasurement::Id, RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { RelativeHumidityMeasurement::Id, RelativeHumidityMeasurement::Attributes::MinMeasuredValue::Id,
      2, { 0xFF, 0xFF } },
    { RelativeHumidityMeasurement::Id, RelativeHumidityMeasurement::Attributes::MaxMeasuredValue::Id,
      2, { 0xFF, 0xFF } },
    { RelativeHumidityMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* PressureMeasurement: no features mandated, and no reading until the
     * host writes one, so all three values start null (int16s, sentinel is
     * the type minimum 0x8000, same convention as TemperatureMeasurement
     * above). */
    { PressureMeasurement::Id, PressureMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { PressureMeasurement::Id, PressureMeasurement::Attributes::MinMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { PressureMeasurement::Id, PressureMeasurement::Attributes::MaxMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { PressureMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* IlluminanceMeasurement: no features, and no reading until the host
     * writes one, so all three values start null. uint16, NOT int16s like
     * temperature/pressure above: the sentinel is the type maximum 0xFFFF,
     * not the type minimum -- see the comment on illuminanceAttrs. */
    { IlluminanceMeasurement::Id, IlluminanceMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { IlluminanceMeasurement::Id, IlluminanceMeasurement::Attributes::MinMeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { IlluminanceMeasurement::Id, IlluminanceMeasurement::Attributes::MaxMeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { IlluminanceMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* FlowMeasurement: no features, and no reading until the host writes
     * one, so all three values start null (uint16, sentinel 0xFFFF). */
    { FlowMeasurement::Id, FlowMeasurement::Attributes::MeasuredValue::Id, 2, { 0xFF, 0xFF } },
    { FlowMeasurement::Id, FlowMeasurement::Attributes::MinMeasuredValue::Id, 2, { 0xFF, 0xFF } },
    { FlowMeasurement::Id, FlowMeasurement::Attributes::MaxMeasuredValue::Id, 2, { 0xFF, 0xFF } },
    { FlowMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* On/Off and dimmable plug-in units reuse OnOff/LevelControl/Identify
     * verbatim (same FeatureMap and ClusterRevision seeds as the lights
     * above), so they need no seed rows of their own. */

    /* ---- catalogue batch 2 ------------------------------------------- */

    /* ColorControl, shared by both color lights. Values are esp-matter's own
     * feature-config defaults (esp_matter_feature.h color_temperature:291-293,
     * xy:306, hue_saturation:275), so a host library written against the C6
     * sees the same boot state here.
     *
     * ColorMode and EnhancedColorMode are seeded kColorTemperatureMireds (2)
     * rather than esp-matter's constructor default of 1. Note this is NOT a
     * conformance requirement: all three ColorModeEnum values are
     * mandatoryConform and ungated in ColorControl.xml, so 0, 1 and 2 are
     * each a legal value of the attribute in the abstract. The reason is the
     * power-up rule. StartUpColorTemperatureMireds is seeded non-null (250,
     * C6 parity), and the cluster spec says a lamp with a non-null startup
     * color temperature powers up in color-temperature mode with ColorMode
     * and EnhancedColorMode reflecting that; startUpColorTempCommand()
     * (color-control-server.cpp:2577-2624) writes exactly that pair at
     * endpoint create. Seeding the same value means the arena is right
     * whether or not that init runs. For 0x010C there is a second reason:
     * it declares neither CurrentHue/CurrentSaturation nor CurrentX/CurrentY,
     * so ColorMode 0 or 1 would name a mode whose defining attributes the
     * endpoint does not present.
     *
     * NumberOfPrimaries is null: this firmware drives no primaries of its
     * own and has no way to know what the host's lamp has. */
    { ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id, 2, { 0xFA, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, 2,
      { 0x01, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, 2,
      { 0xFF, 0xFE } },
    { ColorControl::Id, ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, 2,
      { 0x01, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::StartUpColorTemperatureMireds::Id, 2,
      { 0xFA, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::ColorMode::Id, 1, { 0x02 } },
    { ColorControl::Id, ColorControl::Attributes::EnhancedColorMode::Id, 1, { 0x02 } },
    { ColorControl::Id, ColorControl::Attributes::NumberOfPrimaries::Id, 1, { 0xFF } }, /* null */
    { ColorControl::Id, ColorControl::Attributes::CurrentX::Id, 2, { 0x6B, 0x61 } },
    { ColorControl::Id, ColorControl::Attributes::CurrentY::Id, 2, { 0x7D, 0x60 } },
    { ColorControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x07, 0x00 } },
    /* CurrentHue, CurrentSaturation and Options are all zero at boot, which
     * is the zero-fill, so they carry no row. */

    /* The two per-device-type ColorControl seeds. ColorCapabilities must
     * mirror FeatureMap bits 0..4 (ColorControl.xml constrains it to
     * 0x001F), so the pairs move together: 0x010C is CT alone
     * (Feature::kColorTemperature 0x10), 0x010D is HS|XY|CT (0x1|0x8|0x10 =
     * 0x19). Bit values from ColorControl/Enums.h:148-155 and the matching
     * ColorCapabilitiesBitmap. */
    { ColorControl::Id, ColorControl::Attributes::FeatureMap::Id, 4, { 0x10, 0x00, 0x00, 0x00 },
      0x010C },
    { ColorControl::Id, ColorControl::Attributes::ColorCapabilities::Id, 2, { 0x10, 0x00 }, 0x010C },
    { ColorControl::Id, ColorControl::Attributes::FeatureMap::Id, 4, { 0x19, 0x00, 0x00, 0x00 },
      0x010D },
    { ColorControl::Id, ColorControl::Attributes::ColorCapabilities::Id, 2, { 0x19, 0x00 }, 0x010D },

    /* Thermostat. LocalTemperature is a `temperature` (int16s), so its null
     * sentinel is the signed-type minimum, the same 00 80 the temperature
     * and pressure sensors use. Setpoints are hundredths of a degree:
     * 1600 = 16 C, 2400 = 24 C (the C6's cross-layer I1 values, not
     * esp-matter's 2000/2600), limits are the spec defaults 700/3000 heating
     * and 1600/3200 cooling. ControlSequenceOfOperation 4 is
     * CoolingAndHeating, matching FeatureMap Heating|Cooling; SystemMode 0
     * is Off, spelled out because esp-matter's default is 1 (Auto) and Auto
     * is not a mode this endpoint can honour without the AutoMode feature.
     * Revision 9 is Thermostat/Metadata.h kRevision, which is also what the
     * cluster's own AttributeAccessInterface answers for ClusterRevision. */
    { Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, 2, { 0x00, 0x80 } }, /* null */
    { Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, 2, { 0x40, 0x06 } },
    { Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, 2, { 0x60, 0x09 } },
    { Thermostat::Id, Thermostat::Attributes::MinHeatSetpointLimit::Id, 2, { 0xBC, 0x02 } },
    { Thermostat::Id, Thermostat::Attributes::MaxHeatSetpointLimit::Id, 2, { 0xB8, 0x0B } },
    { Thermostat::Id, Thermostat::Attributes::MinCoolSetpointLimit::Id, 2, { 0x40, 0x06 } },
    { Thermostat::Id, Thermostat::Attributes::MaxCoolSetpointLimit::Id, 2, { 0x80, 0x0C } },
    { Thermostat::Id, Thermostat::Attributes::ControlSequenceOfOperation::Id, 1, { 0x04 } },
    { Thermostat::Id, Thermostat::Attributes::SystemMode::Id, 1, { 0x00 } },
    { Thermostat::Id, Thermostat::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 } },
    { Thermostat::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x09, 0x00 } },
    /* Catalogue batch 7b: the water heater's HEATING-ONLY thermostat,
     * keyed per device type so the Heating|Cooling wildcard rows above
     * keep winning for the standalone thermostat (0x0301) and the room
     * air conditioner (0x0072), the ColorControl 0x010C/0x010D mechanism.
     * 0x1 is Feature::kHeating alone, WaterHeater.xml's Thermostat(HEAT)
     * mandate (the waterHeaterClusters audit note traces it);
     * ControlSequenceOfOperation 0x02 is HeatingOnly, because the wildcard
     * 0x04 (CoolingAndHeating) would advertise cooling this endpoint does
     * not have (a DELIBERATE divergence from the C6, which serves
     * esp-matter's inherited config default 4 here; the FanModeSequence
     * precedent). The setpoint seeds, SystemMode 0 and the revision row
     * stay shared: same cluster, same AAI answer. */
    { Thermostat::Id, Thermostat::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 },
      0x050F },
    { Thermostat::Id, Thermostat::Attributes::ControlSequenceOfOperation::Id, 1, { 0x02 },
      0x050F },

    /* FanControl. FeatureMap 0 conforms (FanControl.xml declares every
     * feature optionalConform with no choice group), and FanModeSequence
     * must therefore be one of the !AUT values 0, 1 or 5:
     * kOffLowMedHigh (0) is the widest of them. FanMode Off, PercentSetting
     * and PercentCurrent 0 are the zero-fill, spelled out because they are
     * the attributes this cluster exists for. Revision 5 is
     * FanControl/Metadata.h kRevision.
     *
     * Fix round 2 re-examined PercentSetting's 0, since it is a NULLABLE
     * attribute seeded with a real value rather than the null sentinel and
     * a bench reader saw that 0 come back from chip-tool before any write.
     * The 0 is deliberate and stays, for two independent reasons.
     *
     * First, it IS the cluster's declared default. Mind which XML: the one
     * this build's zap actually consumes is
     * src/app/zap-templates/zcl/data-model/chip/fan-control-cluster.xml,
     * reached through zcl.json's xmlRoot (['.', './data-model/chip']) and
     * its xmlFile list, NOT the data_model/1.x spec snapshots this file
     * cites for conformance and feature questions. Line 102 of that XML
     * declares PercentSetting `default="0" isNullable="true"`, and that is
     * where hearth.zap's generated defaultValue of 0x00 comes from. (The
     * 1.5 snapshot carries no default for this attribute, which an earlier
     * version of this comment wrongly read as "no XML default to defer to".
     * Right seed, wrong reason; the build consumes the zap-templates copy.)
     *
     * Second, the spec binds its pairing with FanMode and the server
     * implements it: setting FanMode to Off SHALL set PercentSetting,
     * PercentCurrent, SpeedSetting and SpeedCurrent to 0
     * (fan-control-server.cpp:337-361). FanMode is seeded Off one row
     * below, so 0 is the only value consistent with it, and a null seed
     * would contradict that pairing at boot. Null is the
     * Auto-mode answer instead (:363-380 sets PercentSetting null when
     * FanMode goes to Auto), and Auto sits behind the AUT feature this
     * endpoint does not advertise. It also matches esp-matter's own
     * fan_control config defaults (esp_matter_cluster.h:377-392, fan_mode 0
     * and percent_setting 0), so the C6 boots the same pair. The bench's
     * chip-tool 0 was this seed read back correctly: FanControl registers
     * no AttributeAccessInterface at all, so nothing intercepts the read. */
    { FanControl::Id, FanControl::Attributes::FanMode::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::FanModeSequence::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::PercentSetting::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::PercentCurrent::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { FanControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* WindowCovering. Target and current lift positions are percent100ths
     * (uint16), so their null sentinel is the type maximum 0xFFFF, not the
     * signed minimum: nothing is known about the covering's position until
     * the host says so. ConfigStatus 0x09 is Operational|LiftPositionAware
     * (ConfigStatus bitmap, WindowCovering/Enums.h:88-97). FeatureMap 0x05 is
     * Lift|PositionAwareLift. Revision 5 is WindowCovering/Metadata.h
     * kRevision, which is also what the cluster's AttributeAccessInterface
     * answers for ClusterRevision (window-covering-server.cpp:122-123).
     * Type, OperationalStatus, EndProductType and Mode are all zero at boot
     * (roller shade, idle, no mode bits), which is the zero-fill. */
    { WindowCovering::Id, WindowCovering::Attributes::ConfigStatus::Id, 1, { 0x09 } },
    { WindowCovering::Id, WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id, 2,
      { 0xFF, 0xFF } },
    { WindowCovering::Id, WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, 2,
      { 0xFF, 0xFF } },
    { WindowCovering::Id, WindowCovering::Attributes::FeatureMap::Id, 4,
      { 0x05, 0x00, 0x00, 0x00 } },
    { WindowCovering::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* AirQuality. The AirQuality attribute is kUnknown (0) at boot, the
     * zero-fill. FeatureMap deliberately has NO row here: seed_slots() fills
     * it from mt_air_quality_feature_mask() instead, so this file and any
     * future server Instance read the enabled feature set from one accessor
     * rather than two transcribed literals (mt_matter.h's contract for that
     * function). Revision 1 is AirQuality/Metadata.h kRevision. */
    { AirQuality::Id, AirQuality::Attributes::AirQuality::Id, 1, { 0x00 } },
    { AirQuality::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* DoorLock. LockState is seeded to its ENUM8 null sentinel (0xFF)
     * rather than to a state, because that is what the endpoint genuinely
     * knows at boot: the firmware never actuates a lock itself, and only
     * the host's AT+MTLOCK can report a real one (AT_MT_SPEC.md 3.18). It
     * is also what the SDK writes over the seed a moment later regardless,
     * from DoorLockServer::InitEndpoint() (door-lock-server.cpp:101), which
     * calls Attributes::LockState::SetNull() and SetActuatorEnabled(true).
     * The C6 reaches the same pair by the same route, through esp-matter's
     * own emberAfDoorLockClusterInitCallback registration, even though its
     * door_lock::config_t nominally defaults lock_state to 0: the init runs
     * after the attribute is created and wins. So these two rows are seeded
     * to agree with what InitEndpoint will write, the same discipline the
     * ColorControl rows follow: called so the agreement stays true if the
     * seeds change.
     *
     * LockType 0 is kDeadBolt, OperatingMode 0 is kNormal, both zero-fill
     * and both spelled out because they are attributes a controller reads.
     * SupportedOperatingModes 0xFFF6 is the XML's own declared default
     * (door-lock-cluster.xml:272) and esp-matter's, so it is what both
     * platforms present. Its bits are inverted-sense per the cluster spec
     * (a CLEAR bit means the mode IS supported), which makes 0xFFF6 read
     * as Normal (0x01) and NoRemoteLockUnlock (0x08) supported. Nothing in
     * the SDK reads this attribute, so that reading is the spec's, not
     * something the server enforces; the value is taken from the XML rather
     * than computed. AutoRelockTime 0 disables auto-relock and, more
     * importantly, exists at all: see the B129 note on doorLockAttrs.
     * FeatureMap 0, no PIN/USER/schedule/Aliro features this round.
     * Revision 9 is DoorLock/Metadata.h kRevision in THIS tree
     * (third_party/matter_sdk/zzz_generated/app-common/clusters/DoorLock/
     * Metadata.h, Silabs 2.8.1); the nRF port seeds 7, which is its own
     * NCS tree's value, and the C6's esp-matter pins 10, so the three
     * ports differ on purpose (graph F540): the rule is the tree the
     * build consumes. */
    { DoorLock::Id, DoorLock::Attributes::LockState::Id, 1, { 0xFF } }, /* null */
    { DoorLock::Id, DoorLock::Attributes::LockType::Id, 1, { 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::ActuatorEnabled::Id, 1, { 0x01 } },
    { DoorLock::Id, DoorLock::Attributes::AutoRelockTime::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::OperatingMode::Id, 1, { 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::SupportedOperatingModes::Id, 2, { 0xF6, 0xFF } },
    { DoorLock::Id, DoorLock::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { DoorLock::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x09, 0x00 } },

    /* ValveConfigurationAndControl. All five state attributes boot null,
     * which is both the XML's declared default and the truthful answer for
     * a valve this firmware has never actuated: the host owns actuation and
     * reports it back with AT+MTVALVE (AT_MT_SPEC.md 3.19). ELAPSED_S is an
     * alias for INT32U, so its null sentinel is the unsigned type maximum
     * (four 0xFF bytes); CurrentState and TargetState are ENUM8, so 0xFF.
     *
     * RemainingDuration is seeded here for completeness but a controller
     * never reads this slot: the cluster's wildcard AttributeAccessInterface
     * answers that one attribute from its own gRemainingDuration[] shadow
     * ahead of ember (see the audit note on valveAttrs). The split is the
     * BooleanState shape, and unlike BooleanState it is deliberately NOT
     * bridged: RemainingDuration is a countdown the server itself owns and
     * ticks, with no host write in the AT contract to push into it. The
     * consequence to know is that an AT+MTATTR read of it answers from this
     * arena, i.e. +MTERR:5 for the null seeded here, while a subscribed
     * controller sees the server's live countdown. The C6 has the same
     * split for the same reason.
     *
     * FeatureMap 0, no TS and no LVL: see the valveAttrs comment for why TS
     * in particular would be actively harmful. Revision 2 is
     * ValveConfigurationAndControl/Metadata.h kRevision and the XML's
     * globalAttribute value in this tree; the C6's esp-matter pins 1, and
     * the SDK's own chef reference also says 1, which is stale against its
     * own XML. This tree's 2 is what this build serves. */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::OpenDuration::Id,
      4, { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id,
      ValveConfigurationAndControl::Attributes::DefaultOpenDuration::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id,
      ValveConfigurationAndControl::Attributes::RemainingDuration::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::CurrentState::Id,
      1, { 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::TargetState::Id, 1,
      { 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 } },
    { ValveConfigurationAndControl::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x02, 0x00 } },

    /* ---- catalogue batch 4 ------------------------------------------- */

    /* PowerSource. Status 0 (Unspecified), Order 0, BatChargeLevel 0 (OK),
     * BatReplacementNeeded false and BatReplaceability 0 (Unspecified) are
     * all the zero-fill and all match the C6's config defaults
     * (esp_matter_cluster.h power_source config, all-zero; the battery
     * sub-config likewise), so they carry no rows. BatPercentRemaining
     * boots null: this firmware measures no battery, and only a host
     * AT+MTATTR write can report a real reading (INT8U, so the unsigned
     * 1-byte sentinel 0xFF; ember's MIN_MAX check admits it past the
     * 0..200 bounds because null is always in-range for a nullable
     * attribute, attribute-table.cpp:401-403). FeatureMap 0x02 is
     * Feature::kBattery (PowerSource/Enums.h:282-283), the exact-one
     * Wired/Battery choice matching the C6. Revision 3 is
     * PowerSource/Metadata.h:20 kRevision, which is also what the
     * cluster's wildcard AttributeAccessInterface answers for
     * ClusterRevision (power-source-server.cpp:131), the same
     * seed-agrees-with-AAI discipline as the Thermostat and
     * WindowCovering rows above. Description gets no slot and no row. */
    { PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id, 1, { 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x02, 0x00, 0x00, 0x00 } },
    { PowerSource::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },
    /* Catalogue batch 7a: the WIRED PowerSource FeatureMap for the heat
     * pump and solar endpoints, keyed per device type so the battery
     * wildcard row above keeps winning for the standalone power source
     * (0x0011), the ColorControl 0x010C/0x010D mechanism. 0x1 is
     * Feature::kWired (PowerSource/Enums.h:282). Status 0, Order 0 and
     * WiredCurrentType 0 (kAc, WiredCurrentTypeEnum) are the zero-fill;
     * the four battery-list seeds above target attributes
     * wiredPowerSourceAttrs does not declare and never match here. The
     * revision row (3) is shared: same cluster, same AAI answer. */
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 },
      0x0309 },
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 },
      0x0017 },
    /* Catalogue batch 7b: battery storage's Battery|Rechargeable
     * FeatureMap (0x2 | 0x4 = 0x6, PowerSource/Enums.h:280-286), the
     * RECHG conformance fix's feature half, keyed per device type like
     * the wired rows above. The four nullable uint32 battery attributes
     * boot null (4-byte unsigned sentinel; this firmware measures no
     * battery and only a host AT+MTATTR write can report a reading, the
     * BatPercentRemaining rule); the rows are wildcard because only the
     * rechargeable list declares these attributes, so no other type can
     * match them. BatCapacity 0, BatChargeState 0 (kUnknown) and
     * BatFunctionalWhileCharging false are the zero-fill matching the C6
     * config defaults (the rechargeableBatteryPowerSourceAttrs audit
     * note). */
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x06, 0x00, 0x00, 0x00 },
      0x0018 },
    { PowerSource::Id, PowerSource::Attributes::BatVoltage::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::BatTimeRemaining::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::BatTimeToFullCharge::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::BatChargingCurrent::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */

    /* SmokeCoAlarm. Every state boots at its enum's zero: ExpressedState 0
     * (kNormal), SmokeState/COState/BatteryAlert 0 (kNormal),
     * TestInProgress and HardwareFaultAlert false, EndOfServiceAlert 0
     * (kNormal): the truthful boot state for an alarm this firmware has
     * never been told anything about, and all the zero-fill, so only
     * ExpressedState is spelled out (it is the attribute the cluster
     * derives everything toward; see mt_matter_alarm_set()'s B165
     * recompute in mt_matter_zephyr.cpp). FeatureMap 3 is
     * SmokeAlarm|CoAlarm (SmokeCoAlarm/Enums.h:115-119), both features
     * because AT+MTALARM's field table has no single-feature variant.
     * Revision 1 is SmokeCoAlarm/Metadata.h:20 kRevision and the XML's
     * globalAttribute value (smoke-co-alarm-cluster.xml:35). */
    { SmokeCoAlarm::Id, SmokeCoAlarm::Attributes::ExpressedState::Id, 1, { 0x00 } },
    { SmokeCoAlarm::Id, SmokeCoAlarm::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 } },
    { SmokeCoAlarm::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* OperationalState (the washer/dishwasher/dryer trio). Every cluster
     * attribute is served by the per-endpoint Instance's AAI, so these
     * slots are INERT on the fabric and exist for AT+MTATTR arena
     * consistency and AttributeList truthfulness (see the opStateAttrs
     * audit note). Seeded to agree with the Instance's own boot state so
     * the two stores at least start in agreement: OperationalState 0
     * (kStopped, the Instance's member default,
     * operational-state-server.h:224) is the zero-fill and carries no
     * row; CurrentPhase boots null (nullable INT8U, sentinel 0xFF).
     * FeatureMap 0 (the cluster defines no features). Revision 1 is
     * OperationalState/Metadata.h:20 kRevision, which is also what the
     * Instance's AAI answers for ClusterRevision
     * (operational-state-server.cpp:408). */
    { OperationalState::Id, OperationalState::Attributes::CurrentPhase::Id, 1, { 0xFF } }, /* null */
    { OperationalState::Id, OperationalState::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 } },
    { OperationalState::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ModeSelect. StandardNamespace boots null (nullable ENUM16, unsigned
     * 2-byte sentinel 0xFFFF): this co-processor cannot know which
     * standard namespace the host's modes belong to, and the C6's config
     * default is the same null. CurrentMode 0 is the zero-fill and
     * matches the C6's config default; the host's AT+MTMODES list
     * conventionally starts at mode 0. FeatureMap 0 (no DEPONOFF; the
     * cluster's only feature needs an OnOff server on the endpoint).
     * Revision 2 is ModeSelect/Metadata.h:20 kRevision and the XML's
     * globalAttribute value (mode-select-cluster.xml:45). Description
     * gets no slot and no row. */
    { ModeSelect::Id, ModeSelect::Attributes::StandardNamespace::Id, 2, { 0xFF, 0xFF } }, /* null */
    { ModeSelect::Id, ModeSelect::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { ModeSelect::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* Chime. SelectedChime 0 is the zero-fill and the XML default;
     * Enabled seeds TRUE, the XML default (chime-cluster.xml:42) and the
     * ChimeServer's own constructed state (mEnabled true,
     * chime-server.cpp:44). Both slots are fully inert since fix round 2
     * (the server's AAI shadows them on the fabric, and DE397's carve-out
     * routes both the AT+MTATTR read AND write legs to the live server,
     * mt_matter_zephyr.cpp), so these seeds are never observable
     * anywhere; kept truthful to a fresh server anyway, the arena
     * discipline every other row follows. FeatureMap 0
     * (the cluster declares no features). Revision 1 is
     * Chime/Metadata.h:20 kRevision and the XML's globalAttribute value
     * (chime-cluster.xml:39). */
    { Chime::Id, Chime::Attributes::Enabled::Id, 1, { 0x01 } },
    { Chime::Id, Chime::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { Chime::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- catalogue batch 5 ------------------------------------------- */

    /* Switch. NumberOfPositions 2 is the cluster's own declared default
     * and minimum (switch-cluster.xml:83, min="2") and esp-matter's config
     * default, so the C6 boots the same pair; CurrentPosition 0 is the
     * zero-fill and carries no row. FeatureMap 0x02 is MomentarySwitch
     * (Switch/Enums.h:35; see the switchAttrs audit note for why exactly
     * that bit). Revision 2 is Switch/Metadata.h:20 kRevision in THIS
     * tree, the batch 3 rule (the tree the build consumes, not the
     * sibling platform's pins). */
    { Switch::Id, Switch::Attributes::NumberOfPositions::Id, 1, { 0x02 } },
    { Switch::Id, Switch::Attributes::FeatureMap::Id, 4, { 0x02, 0x00, 0x00, 0x00 } },
    { Switch::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* The featureless-OnOff pair (racOnOffAttrs): the wildcard OnOff
     * FeatureMap row above says 0x01 (Lighting) for the light and plug
     * family, so the pump and the RAC each need a per-device-type row
     * that wins over it (the ColorControl 0x010C/0x010D mechanism). The
     * pump's OnOff has no features at all; the RAC's carries exactly
     * DeadFrontBehavior (0x02, OnOff/Enums.h:84), mandatory per the RAC's
     * element requirements. The wildcard OnOff ClusterRevision row (6)
     * stays correct for both, and the four LT-gated seeds above target
     * attributes racOnOffAttrs does not declare, so they simply never
     * match on these endpoints. */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 }, 0x0303 },
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x02, 0x00, 0x00, 0x00 }, 0x0072 },

    /* PumpConfigurationAndControl. The five capability attributes boot
     * null, the truthful answer for hardware this co-processor has never
     * seen: MaxPressure and Capacity are int16s (sentinel 0x8000, stored
     * 00 80, the temperature/pressure convention), MaxSpeed, MaxFlow,
     * MinConstSpeed and MaxConstSpeed are int16u (sentinel 0xFFFF). All
     * six match the C6's config defaults (esp_matter_cluster.h pump
     * config, max_* and capacity nullable-null; constant_speed::config_t
     * both null). EffectiveOperationMode, EffectiveControlMode and
     * OperationMode are all 0, the zero-fill, and all three are LEGAL
     * zeros under a ConstantSpeed-only feature map because
     * OperationModeEnum::kNormal and ControlModeEnum::kConstantSpeed are
     * both 0x00 (PumpConfigurationAndControl/Enums.h:50, :34), so they
     * carry no rows. FeatureMap 0x8 is Feature::kConstantSpeed
     * (Enums.h:67). Revision 4 is PumpConfigurationAndControl/
     * Metadata.h:20 kRevision in THIS tree. */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxPressure::Id, 2,
      { 0x00, 0x80 } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxSpeed::Id, 2,
      { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxFlow::Id, 2,
      { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MinConstSpeed::Id,
      2, { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxConstSpeed::Id,
      2, { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::Capacity::Id, 2,
      { 0x00, 0x80 } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::FeatureMap::Id, 4,
      { 0x08, 0x00, 0x00, 0x00 } },
    { PumpConfigurationAndControl::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x04, 0x00 } },

    /* The RVC's ModeBase pair. CurrentMode 0 and FeatureMap 0 are the
     * zero-fill (both slots are inert AAI shadows anyway; DirectModeChange
     * is the aliases' only feature and is optional, so 0 is passed to the
     * Instance constructor too and the two stay in agreement). The
     * ClusterRevision seeds are LIVE, unlike every other Instance-served
     * cluster in this file: ModeBase's Instance::Read() has no default
     * arm, so revision reads fall through to ember (mode-base-server.cpp:
     * 325-347, and CodegenDataModelProvider_Read.cpp treats a no-encode
     * AAI return as "continue to ember"). RvcRunMode/Metadata.h:20 says 4,
     * RvcCleanMode/Metadata.h:20 says 5, per cluster in THIS tree. */
    { RvcRunMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x04, 0x00 } },
    { RvcCleanMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* RvcOperationalState. Same split as the base trio's rows above
     * (CurrentPhase boots null, sentinel 0xFF; OperationalState 0 kStopped
     * and FeatureMap 0 are the zero-fill), keyed to the DERIVED cluster id
     * since seed matching is per cluster. The revision seed is INERT here,
     * unlike the ModeBase pair's: the Instance's AAI answers
     * ClusterRevision itself, hardcoded to the BASE cluster's
     * OperationalState::kRevision constant even on the derived cluster
     * (operational-state-server.cpp:408-409); both constants are 1 in this
     * tree (OperationalState/Metadata.h:20, RvcOperationalState/
     * Metadata.h:20), so there is no observable divergence, and the seed
     * is written 1 anyway so the arena's metadata stays truthful. */
    { RvcOperationalState::Id, OperationalState::Attributes::CurrentPhase::Id, 1,
      { 0xFF } }, /* null */
    { RvcOperationalState::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- catalogue batch 7a: the energy foundation ------------------- */

    /* ElectricalPowerMeasurement. PowerMode and NumberOfMeasurementTypes
     * ARE the AT-served truth (slot-served; the fabric side reads the
     * identical constants from HearthEpmDelegate::GetPowerMode()/
     * GetNumberOfMeasurementTypes(), so the two agree by the FanControl
     * discipline): kAc = 2 (ElectricalPowerMeasurement/Enums.h PowerMode
     * Enum), one measurement type (ActivePower, the mandatory accuracy
     * entry). FeatureMap 0x2 is Feature::kAlternatingCurrent, the inert
     * shadow of the BitMask every EPM Instance is constructed with
     * (mt_matter_meas_delegate_set_endpoint, cross-referenced there).
     * ClusterRevision 3 is LIVE (Instance::Read() has no revision case)
     * and is ElectricalPowerMeasurement/Metadata.h:20 in THIS tree. The
     * seven 64-bit fields carry no rows: slotless (DE407 option C), so a
     * seed would have nothing to land in. */
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::PowerMode::Id, 1,
      { 0x02 } },
    { ElectricalPowerMeasurement::Id,
      ElectricalPowerMeasurement::Attributes::NumberOfMeasurementTypes::Id, 1, { 0x01 } },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::FeatureMap::Id, 4,
      { 0x02, 0x00, 0x00, 0x00 } },
    { ElectricalPowerMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x03, 0x00 } },

    /* ElectricalEnergyMeasurement. FeatureMap 0x7 is the inert shadow of
     * the ONE wildcard AttrAccess's fixed Imported|Exported|Cumulative
     * mask (mt_matter_eem_register, mt_matter_sl_b7.inc: that object
     * answers FeatureMap for every EEM endpoint, so this seed may never
     * diverge from its construction). ClusterRevision 2 is LIVE (the
     * wildcard AAI serves no revision case) and is
     * ElectricalEnergyMeasurement/Metadata.h:20 in THIS tree. The three
     * struct attributes are metadata-only and carry no rows. */
    { ElectricalEnergyMeasurement::Id, ElectricalEnergyMeasurement::Attributes::FeatureMap::Id, 4,
      { 0x07, 0x00, 0x00, 0x00 } },
    { ElectricalEnergyMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x02, 0x00 } },

    /* PowerTopology. FeatureMap 0x1 is Feature::kNodeTopology, the inert
     * shadow of the Instance's construction mask; ClusterRevision 1 is
     * LIVE (Instance::Read() serves FeatureMap and the two absent
     * endpoint lists only, power-topology-server.cpp:64-77) and is
     * PowerTopology/Metadata.h:20 in THIS tree. */
    { PowerTopology::Id, PowerTopology::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { PowerTopology::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* MeterIdentification. MeterType boots null (Instance::Init() nulls
     * all five attributes; the inert shadow carries the 1-byte unsigned
     * sentinel so the arena agrees at boot; reads answer the live
     * Instance through the carve-out either way). FeatureMap 0x1 is
     * Feature::kPowerThreshold, the inert shadow of
     * mt_meter_feature_mask(), the one-accessor-two-callers discipline.
     * ClusterRevision 1 is LIVE (no Read() case) and is
     * MeterIdentification/Metadata.h:20 in THIS tree. The strings and
     * the struct are metadata-only and carry no rows. */
    { MeterIdentification::Id, MeterIdentification::Attributes::MeterType::Id, 1,
      { 0xFF } }, /* null */
    { MeterIdentification::Id, MeterIdentification::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { MeterIdentification::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* DeviceEnergyManagement. ESAState's shadow seeds kOnline (1), the
     * delegate default and the spec's pre-first-push answer
     * (AT_MT_SPEC.md 3.25); ESAType 0 (kEvse), ESACanGenerate false and
     * OptOutState 0 (kNoOptOut) are the zero-fill matching the delegate
     * defaults, all Instance-served with carve-out reads either way.
     * ClusterRevision 4 is LIVE (Instance::Read() falls through for it) and
     * is DeviceEnergyManagement/Metadata.h:20 in THIS tree. The DEMMode
     * revision is LIVE too (the ModeBase no-default-arm rule the RVC rows
     * above document): DeviceEnergyManagementMode/Metadata.h:20 says 2.
     *
     * FeatureMap is the catalogue's FIRST variant-dependent boot value.
     * Until batch 8 it had no row here at all and was written by a special
     * case in seed_slots(); the variant column retired that (the s_seeds
     * header comment). Variant 0 is Feature::kPowerAdjustment (0x1) and
     * variant 1 is 0, the same two values the create path's PowerAdjustment
     * predicate (type_has_attr on PowerAdjustmentCapability) hands
     * mt_matter_dem_register(), so the seeded shadow and the Instance's own
     * mask agree by construction.
     * Both variants share device type 0x050D, which is why the devtype
     * column could never have split them. */
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::ESAState::Id, 1, { 0x01 } },
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 }, 0, seed_variant(0) },
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 }, 0, seed_variant(1) },
    { DeviceEnergyManagement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x04, 0x00 } },
    { DeviceEnergyManagementMode::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x02, 0x00 } },

    /* ---- catalogue batch 7b: the delegate-served energy pair --------- */

    /* WaterHeaterManagement. THE ONE INERT REVISION SEED IN THE ENERGY
     * FAMILY: Instance::Read() serves ClusterRevision itself
     * (water-heater-management-server.cpp:146-147, kClusterRevision = 2 at
     * :40), so this row is a shadow kept equal to that served value, the
     * Thermostat discipline; every OTHER energy revision seed is live.
     * HeaterTypes 0, HeatDemand 0, TankVolume 0, TankPercentage 0 and
     * BoostState 0 (kInactive) are the zero-fill matching the
     * HearthWhmDelegate defaults (all Instance-served with carve-out reads
     * either way).
     *
     * FeatureMap is the catalogue's SECOND variant-dependent boot value and
     * had no row here either until batch 8's variant column retired its
     * special case. Variant 0 is Feature::kEnergyManagement |
     * Feature::kTankPercent (0x3, the value the C6 hand-sets around its two
     * broken feature helpers) and variant 1 is 0; the create path still
     * hands the identical `variant == 0` predicate to
     * mt_matter_whm_register(), whose Instance snapshots mFeature at
     * construction and answers the fabric-side read from it, so seed and
     * Instance agree by construction. Both variants share device type
     * 0x050F. */
    { WaterHeaterManagement::Id, WaterHeaterManagement::Attributes::FeatureMap::Id, 4,
      { 0x03, 0x00, 0x00, 0x00 }, 0, seed_variant(0) },
    { WaterHeaterManagement::Id, WaterHeaterManagement::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 }, 0, seed_variant(1) },
    { WaterHeaterManagement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },
    /* WaterHeaterMode. ClusterRevision 1 is LIVE (the ModeBase AAI has no
     * revision case, the RVC rows' rule on the fourth alias) and is
     * WaterHeaterMode/Metadata.h:20 in THIS tree. CurrentMode 0 and
     * FeatureMap 0 are the zero-fill, the DEMMode shape. */
    { WaterHeaterMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- catalogue batch 8: the composed appliances ------------------- */

    /* Cooktop's OnOff FeatureMap: Feature::kOffOnly (0x4, OnOff/Enums.h:85),
     * device-type-qualified against the lighting types' wildcard 0x01, the
     * pump and room air conditioner rows' exact mechanism. Mandatory rather
     * than optional here: Cooktop.xml:69-74 makes OFFONLY mandatory inside a
     * mandatory OnOff, which is also why cooktopClusters declares its own
     * incoming command list (the cooktop audit note). ClusterRevision rides
     * the shared OnOff row (6) and OnOff itself is the zero-fill false. */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x04, 0x00, 0x00, 0x00 }, kDtCooktop },

    /* RefrigeratorAlarm. Mask 1 and Supported 1 are the C6's config
     * defaults (esp_matter_cluster.h:926-930) and the values AT_MT_SPEC.md
     * 2364-2366 documents to hosts: bit 0 (DoorOpen) supported and unmasked,
     * every other bit neither. State 0 and FeatureMap 0 are the zero-fill,
     * spelled out anyway for State because it is the attribute this cluster
     * exists for. All three are LIVE arena values, not shadows: the server
     * is a singleton that reads and writes them through the generated
     * Accessors, so nothing intercepts them. ClusterRevision 1 is
     * RefrigeratorAlarm/Metadata.h:20 in THIS tree. */
    { RefrigeratorAlarm::Id, RefrigeratorAlarm::Attributes::Mask::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { RefrigeratorAlarm::Id, RefrigeratorAlarm::Attributes::State::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 } },
    { RefrigeratorAlarm::Id, RefrigeratorAlarm::Attributes::Supported::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { RefrigeratorAlarm::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* RefrigeratorAndTemperatureControlledCabinetMode. ClusterRevision 2 is
     * LIVE (the ModeBase AAI has no revision case, the RVC rows' standing
     * rule) and is this tree's own Metadata.h:20. It DIVERGES from the C6,
     * which says 3 (esp_matter_cluster_revisions.h:270-272); the port's
     * convention is this tree's own kRevision and has been since batch 2, so
     * 2 is right here and the divergence is disclosable rather than a bug.
     * CurrentMode 0 and FeatureMap 0 are the zero-fill, the DEMMode
     * shape. */
    { RefrigeratorAndTemperatureControlledCabinetMode::Id,
      Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* Cook surface's OnOff FeatureMap: kOffOnly again, the cooktop's row one
     * endpoint down, and the second consumer of that device-type
     * qualifier. */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x04, 0x00, 0x00, 0x00 },
      kDtCookSurface },

    /* TemperatureControl, and the reason attr_seed grew a variant column.
     * The FeatureMap rows are variant-qualified and device-type-WILDCARD,
     * because 0x0071 and 0x0077 need the identical pair of values: variant 0
     * is kTemperatureNumber | kTemperatureStep (0x05) and variant 1 is
     * kTemperatureLevel (0x02), from TemperatureControl::Feature
     * (Enums.h:34-36). Getting one of these wrong does not merely misreport
     * a capability, it makes SetTemperature answer Failure or InvalidCommand
     * on a healthy endpoint (the temperatureControl*Attrs audit note).
     *
     * The four number-variant values are esp-matter's own feature-config
     * defaults (esp_matter_feature.h:970-975, :997-1001) so the two
     * platforms boot a cabinet identically: setpoint 1, min 0, max 10, step
     * 1, all INT16S little-endian. SelectedTemperatureLevel 1 is the
     * level-variant default (esp_matter_feature.h:985-988). ClusterRevision
     * 1 is TemperatureControl/Metadata.h:20 in THIS tree and esp-matter
     * agrees for once (esp_matter_cluster_revisions.h:262-264). Every one of
     * these is a LIVE arena value: this cluster has no Instance, and its one
     * AAI serves SupportedTemperatureLevels alone. */
    { TemperatureControl::Id, TemperatureControl::Attributes::FeatureMap::Id, 4,
      { 0x05, 0x00, 0x00, 0x00 }, 0, seed_variant(0) },
    { TemperatureControl::Id, TemperatureControl::Attributes::FeatureMap::Id, 4,
      { 0x02, 0x00, 0x00, 0x00 }, 0, seed_variant(1) },
    { TemperatureControl::Id, TemperatureControl::Attributes::TemperatureSetpoint::Id, 2,
      { 0x01, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::MinTemperature::Id, 2,
      { 0x00, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::MaxTemperature::Id, 2,
      { 0x0A, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::Step::Id, 2, { 0x01, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::SelectedTemperatureLevel::Id, 1,
      { 0x01 } },
    { TemperatureControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* OvenMode. ClusterRevision 1 is LIVE (the ModeBase AAI has no revision
     * case, the standing rule on the sixth alias) and is
     * OvenMode/Metadata.h:20 in THIS tree; the C6 hand-set 2 from the 1.5.1
     * XML, a disclosable divergence. CurrentMode 0 and FeatureMap 0 are the
     * zero-fill.
     *
     * OvenCavityOperationalState. It reuses opStateAttrs, so it also reuses
     * that list's seed values, but NOT through the same rows: every
     * OperationalState seed here is keyed on cluster id, and the cavity is a
     * different cluster id, so it needs its own three. CurrentPhase 0xFF is
     * null (these cavities publish no phases, so PhaseList reads null and
     * CurrentPhase with it), OperationalState 0 is kStopped, FeatureMap 0 is
     * the zero-fill. ClusterRevision 1 is this tree's Metadata.h and is
     * INERT: Instance::Read() answers ClusterRevision itself
     * (operational-state-server.cpp:408-409), so this row is a shadow kept
     * equal to the served value, the Thermostat and WHM discipline. */
    { OvenMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },
    { OvenCavityOperationalState::Id, OperationalState::Attributes::CurrentPhase::Id, 1,
      { 0xFF } }, /* null */
    { OvenCavityOperationalState::Id, OperationalState::Attributes::OperationalState::Id, 1,
      { 0x00 } },
    { OvenCavityOperationalState::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x01, 0x00 } },

    /* MicrowaveOvenMode. ClusterRevision 1 is LIVE (the ModeBase AAI has no
     * revision case, the standing rule on the seventh alias) and is
     * MicrowaveOvenMode/Metadata.h:20 in THIS tree; the C6 says 2, a
     * disclosable divergence. CurrentMode 0 and FeatureMap 0 are the
     * zero-fill.
     *
     * MicrowaveOvenControl. All four declared values are Instance-served
     * shadows kept equal to what Instance::Read() answers at boot, the
     * FanControl agreement discipline (the mwocAttrs audit note explains why
     * there is no k_instance_served carve-out and why the shadow is expected
     * to go stale after the first cooking command):
     *   CookTime 30, kDefaultCookTimeSec: the constant at
     *     microwave-oven-control-server.h:34 and the Instance's own
     *     mCookTimeSec initialiser at :88, which Instance::Read() answers
     *     CookTime from. The audit's estimate said 0; 30 is what the server
     *     actually serves from the first read onward, and a shadow that
     *     disagrees with its source on boot would be wrong for no gain.
     *     (Fix round M6: an earlier version of this note also claimed the
     *     cluster XML gives CookTime a default of 30. It does not:
     *     data_model/1.5/clusters/MicrowaveOvenControl.xml declares CookTime
     *     with a constraint and no default. The SDK evidence alone carries
     *     the decision and the XML claim is withdrawn.)
     *   MaxCookTime 86400, HearthMwocDelegate::GetMaxCookTimeSec(), the C6's
     *     same constant and the XML's own maximum.
     *   PowerSetting 100, kDefaultMaxPowerNum and the delegate's
     *     m_power_setting initialiser.
     *   FeatureMap 0x01, Feature::kPowerAsNumber, MANDATORY rather than
     *     chosen (the mwocAttrs note), and the value Init() will refuse to
     *     start without.
     * ClusterRevision 1 is MicrowaveOvenControl/Metadata.h:20.
     *
     * The microwave's OperationalState seeds ride the existing 0x0060 rows
     * except CountdownTime, which no earlier device type declares: 0xFFFFFFFF
     * is the uint32 null sentinel, matching the NullNullable the delegate's
     * GetCountdownTime() answers, so the inert shadow and the served value
     * agree on null. */
    { MicrowaveOvenMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },
    { OperationalState::Id, OperationalState::Attributes::CountdownTime::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::CookTime::Id, 4,
      { 0x1E, 0x00, 0x00, 0x00 } },
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::MaxCookTime::Id, 4,
      { 0x80, 0x51, 0x01, 0x00 } },
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::PowerSetting::Id, 1, { 0x64 } },
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { MicrowaveOvenControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- the EVSE round: the last device type ------------------------- */

    /* EnergyEvse. Every one of these slots is an INERT SHADOW: Instance::Read()
     * serves all 23 attributes from the delegate (energy-evse-server.cpp:68-133)
     * and the DE397 carve-out serves the AT side, so nothing reads these values
     * after boot. They are seeded anyway, and correctly, for the reason the
     * MeterIdentification MeterType row is: the arena and the served value must
     * agree at boot, or a future reader comparing them has to work out which
     * one lied.
     *
     * The three enums (State kNotPluggedIn, SupplyState kDisabled, FaultState
     * kNoError) are all zero and ride the zero-fill; the seven nullables carry
     * their type's null sentinel, which is what the delegate's default-
     * constructed Nullable<> members answer, so shadow and cache agree on
     * "nothing pushed yet". The six 8-byte declarations have no slot at all
     * (the quiet table) and so no row here.
     *
     * FeatureMap is the catalogue's FOURTH variant-dependent boot value, after
     * DEM's, WHM's and TemperatureControl's, and it uses the same variant
     * column: variant 0 is kChargingPreferences | kSoCReporting (0x3) and
     * variant 1 is kChargingPreferences alone (0x1). PREF is on in BOTH, which
     * is the whole point of the pair: the C6's esp-matter adds charging
     * preferences unconditionally, and mt_matter_evse_register() hands the
     * Instance the identical predicate, so the seeded shadow and the
     * Instance's own mFeature snapshot agree by construction. Both variants
     * share device type 0x050C, which is why the devtype column could not have
     * split them.
     *
     * ClusterRevision 3 is LIVE (Read() has no case for it and falls through
     * to ember, energy-evse-server.cpp:131-133) and is
     * EnergyEvse/Metadata.h:20 in THIS tree. */
    { EnergyEvse::Id, EnergyEvse::Attributes::ChargingEnabledUntil::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeStartTime::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeTargetTime::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeTargetSoC::Id, 1, { 0xFF } },  /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::StateOfCharge::Id, 1, { 0xFF } },        /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::SessionID::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::SessionDuration::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 }, 0,
      seed_variant(0) },
    { EnergyEvse::Id, EnergyEvse::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 }, 0,
      seed_variant(1) },
    { EnergyEvse::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* EnergyEvseMode. ClusterRevision 2 is LIVE (the ModeBase AAI has no
     * revision case, the standing rule on the eighth and last alias) and is
     * EnergyEvseMode/Metadata.h:20 in THIS tree. CurrentMode 0 and FeatureMap 0
     * are the zero-fill, the DEMMode shape. */
    { EnergyEvseMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* THE EVSE'S OWN DEM FEATUREMAP, and the one row in this table whose
     * whole job is to override two others. The DEM rows above are
     * variant-qualified (variant 0 is kPowerAdjustment, variant 1 is 0)
     * because the DEM and battery-storage device types put PA on their
     * variant 0. The EVSE's variant axis is SOC, not PA: it carries the
     * over-delivered DEM cluster with NO PowerAdjustment on EITHER variant
     * (mt_add_dem_triple(ep, false, ...) on the C6, and demReportOnlyAttrs in
     * both cluster lists here), so a variant-0 EVSE inheriting the wildcard
     * variant-0 row would advertise PA with none of its four obligations met:
     * the iron rule (ARCHITECTURE.md 8.12) broken by a table lookup.
     *
     * One devtype-qualified row settles it for both variants, because the
     * precedence rule scores a devtype match (2) above a variant match (1).
     * That is the first time in this table those two qualifiers compete, which
     * the batch-8 precedence comment said would need arguing when it happened:
     * the argument is that "which device type is this" is a stronger statement
     * about an endpoint than "which variant is this", since a variant only
     * means anything relative to a device type in the first place. */
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 }, 0x050C },
};

/* Fills this endpoint's block (nRF 6891-7005). Walks the same two
 * predicates count_slots() used to size it, so slot_count lands on
 * slot_capacity exactly; the bound below is a backstop against those two
 * drifting, not a normal path. */
void seed_slots(dyn_endpoint *d)
{
    attr_slot *slots = block_slots(*d);
    d->slot_count = 0;
    for (uint8_t c = 0; c < d->ep_type->clusterCount; c++) {
        const EmberAfCluster &cl = d->ep_type->cluster[c];
        if (!cluster_gets_slots(cl)) {
            continue;
        }
        for (uint16_t a = 0; a < cl.attributeCount; a++) {
            const EmberAfAttributeMetadata &md = cl.attributes[a];
            if (!attr_gets_slot(md)) {
                /* Named here rather than in the predicate so an attribute
                 * too wide for a slot is visible in the log instead of
                 * silently unserved; the ARRAY globals are expected and
                 * stay quiet, as are CHAR_STRING and STRUCT, which are
                 * deliberate metadata-only declarations made so
                 * AttributeList stays truthful for attributes this arena
                 * cannot hold. Anything ELSE too wide is still a mistake
                 * worth shouting about.
                 *
                 * attr_quiet_no_slot() (DE407 option C, catalogue batch
                 * 7a-1, mt_devtypes_sl_tables_b7.inc) is the narrow
                 * per-(cluster, attribute) table that silences the shout
                 * for over-wide scalars PROVEN to be served by a cluster
                 * Instance instead, one pair at a time and never a whole
                 * type. */
                if (md.attributeType != ZAP_TYPE(ARRAY) &&
                    md.attributeType != ZAP_TYPE(CHAR_STRING) &&
                    md.attributeType != ZAP_TYPE(STRUCT) &&
                    !attr_quiet_no_slot(cl.clusterId, md.attributeId)) {
                    HEARTH_LOGE("devtypes", "attr 0x%08X on cluster 0x%08X is %u bytes, a slot "
                                            "holds %u",
                                (unsigned)md.attributeId, (unsigned)cl.clusterId,
                                (unsigned)md.size, (unsigned)kSlotDataBytes);
                }
                continue;
            }
            if (d->slot_count >= d->slot_capacity) {
                HEARTH_LOGE("devtypes", "devtype 0x%04X block holds %u slots, seeding wanted more",
                            (unsigned)d->type->id, (unsigned)d->slot_capacity);
                return;
            }
            attr_slot &s = slots[d->slot_count++];
            s.cluster = cl.clusterId;
            s.attr = md.attributeId;
            s.size = (uint8_t)md.size;
            memset(s.data, 0, sizeof(s.data));

            /* AirQuality's FeatureMap is the one boot value that is not a
             * literal in s_seeds. mt_matter.h makes
             * mt_air_quality_feature_mask() the single source of truth for
             * which of Fair/Moderate/VeryPoor/ExtremelyPoor are enabled,
             * precisely so an ember feature map and a server Instance's
             * BitMask<Feature> cannot be edited apart; honour that here
             * rather than transcribing the bits a second time. Written
             * little-endian, the same convention as every seed row.
             * Catalogue batch 2's air quality sensor (0x002C) is the first
             * device type in this build to reach this arm. */
            if (cl.clusterId == AirQuality::Id &&
                md.attributeId == Globals::Attributes::FeatureMap::Id) {
                uint32_t mask = mt_air_quality_feature_mask();
                for (uint8_t b = 0; b < s.size && b < sizeof(s.data); b++) {
                    s.data[b] = (uint8_t)(mask >> (8 * b));
                }
                continue;
            }

            /* The most specific matching seed row wins: a row naming this
             * device type AND this variant beats one naming only the device
             * type, which beats one naming only the variant, which beats the
             * bare wildcard. A row whose qualifier is present but does not
             * match is skipped. Scored rather than short-circuited so the
             * table's row order stays irrelevant. */
            const attr_seed *chosen = nullptr;
            int chosen_score = -1;
            for (auto &seed : s_seeds) {
                if (seed.cluster != cl.clusterId || seed.attr != md.attributeId) {
                    continue;
                }
                if (seed.devtype != 0 && seed.devtype != d->type->id) {
                    continue;
                }
                if (seed.variant != kSeedAnyVariant && seed.variant != seed_variant(d->variant)) {
                    continue;
                }
                const int score = (seed.devtype != 0 ? 2 : 0) +
                                  (seed.variant != kSeedAnyVariant ? 1 : 0);
                if (score > chosen_score) {
                    chosen = &seed;
                    chosen_score = score;
                }
            }
            if (chosen != nullptr) {
                memcpy(s.data, chosen->bytes, (chosen->size < s.size) ? chosen->size : s.size);
            }
        }
    }
}

