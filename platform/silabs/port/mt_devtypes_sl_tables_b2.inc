/*
 * mt_devtypes_sl_tables_b2.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 2 (nRF 745-1246).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, so the whole is still one translation
 * unit: file-local names stay file-local and the generated code is the
 * same as when this text lived in the .cpp. Split out 2026-09-25 so each
 * file fits a small model's context (graph T549, DE550). Do not add it
 * to hearth.slcp.
 */

/* ---- color temperature light (0x010C) and extended color light (0x010D)
 * (nRF 745-946)
 *
 * Catalogue batch 2 audit, ColorControl (0x0300). Three questions, three
 * answers from this tree:
 *
 *   Code-driven? No. There is no CodegenIntegration.cpp under
 *   src/app/clusters/color-control-server/, and adding the cluster to
 *   hearth.zap emitted no case in zap-generated/CodeDrivenInitShutdown.cpp
 *   (that file still lists exactly the clusters it did before this
 *   batch, BooleanState last). So no registered ServerCluster object wins
 *   ahead of ember, unlike BooleanState/Descriptor: reads and writes land in
 *   the arena below.
 *
 *   AAI? No. MatterColorControlPluginServerInitCallback() is empty
 *   (color-control-server.cpp:3345); nothing registers an
 *   AttributeAccessInterface for this cluster.
 *
 *   ServerInit? YES, and it does observable work, so this cluster joins the
 *   init-hook section below.
 *   emberAfColorControlClusterServerInitCallback() (:3291) calls
 *   startUpColorTempCommand(), which applies a non-null
 *   StartUpColorTemperatureMireds to ColorTemperatureMireds and forces
 *   ColorMode/EnhancedColorMode to kColorTemperatureMireds (:2577-2624).
 *   With this batch's seeds the value it writes equals the seed already
 *   there, so today it is a no-op; it is called anyway, because the moment
 *   the StartUp seed changes (or a persisted value differs) a dynamic
 *   endpoint that never ran it boots in the wrong color mode, and that is
 *   exactly the class of bug B388 was.
 *
 *   Per-endpoint state arrays: safe. ColorControlServer's transition and
 *   quiet-reporting arrays are sized kColorControlClusterServerMaxEndpointCount
 *   = MATTER_DM_COLOR_CONTROL_CLUSTER_SERVER_ENDPOINT_COUNT +
 *   CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT (color-control-server.h:293),
 *   so dynamic endpoints are accounted for, and
 *   emberAfGetClusterServerEndpointIndex() returns fixedCount + (epIndex -
 *   FIXED_ENDPOINT_COUNT) for one (attribute-storage.cpp:957-962).
 *
 * The two device types differ only in which ColorControl attributes they
 * declare and in three seeds (FeatureMap, ColorCapabilities), which is why
 * s_seeds below grew an optional devtype qualifier rather than a second
 * table. Everything else - OnOff, LevelControl, Identify, Descriptor - is
 * the dimmable light's set verbatim.
 *
 * ColorTemperatureLight.xml mandates ColorControl feature CT only;
 * ExtendedColorLight.xml mandates XY and CT and lists HS as
 * optionalConform. HS is therefore inside the device type, not an extension
 * of it: taking it is the C6's deliberate step beyond the MANDATORY set
 * (platform/esp32c6/main/mt_devtypes.cpp mk_extended_color_light() bolts
 * hue_saturation onto the cluster after create() so the host library's
 * HSV-driven class has CurrentHue/CurrentSaturation to write); mirrored
 * here. EHUE and CL are set on neither: the cluster XML makes HS mandatory
 * only when EHUE is set and EHUE mandatory only when CL is, so HS|XY|CT
 * conforms with neither of them present.
 *
 * NumberOfPrimaries is here because it is mandatoryConform in
 * ColorControl.xml with no feature gate (the ZAP conformance checker flagged
 * its absence outright on the first regeneration); it is nullable and seeded
 * null, since a co-processor has no idea how many physical primaries the
 * host's lamp has. Options and CoupleColorTempToLevelMinMireds are likewise
 * mandatory (the latter under CT) and not in the round's scope list; they
 * are declared because the cluster XML binds them, the same OccupancySensing
 * lesson as batch 1. RemainingTime is optional and stays out. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(colorTempAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, INT16U, 2,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::StartUpColorTemperatureMireds::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id, BITMAP16, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::NumberOfPrimaries::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* The extended light's list is colorTempAttrs plus the HS and XY quartet.
 * Spelled out rather than composed: DECLARE_DYNAMIC_ATTRIBUTE_LIST_* builds a
 * plain array and there is no concatenation macro. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(extendedColorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentHue::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentSaturation::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentX::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentY::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, INT16U, 2,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::StartUpColorTemperatureMireds::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id, BITMAP16, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::NumberOfPrimaries::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/*
 * These two lists are the FULL mandatory command set for the feature map
 * each device type advertises, not a chosen subset. Every command in
 * ColorControl.xml is `mandatoryConform` on a feature, so advertising CT or
 * HS or XY and then omitting one of that feature's commands is a
 * conformance gap rather than a scope decision:
 *
 *   CT              -> 0x0A MoveToColorTemperature, 0x4B MoveColorTemperature,
 *                      0x4C StepColorTemperature
 *   HS              -> 0x00 MoveToHue, 0x01 MoveHue, 0x02 StepHue,
 *                      0x03 MoveToSaturation, 0x04 MoveSaturation,
 *                      0x05 StepSaturation, 0x06 MoveToHueAndSaturation
 *   XY              -> 0x07 MoveToColor, 0x08 MoveColor, 0x09 StepColor
 *   HS or XY or CT  -> 0x47 StopMoveStep (an orTerm over the three)
 *
 * So 0x010C (CT) owes four commands and 0x010D (HS|XY|CT) owes fourteen.
 *
 * All fourteen were audited the way the CT trio was, and all pass. Each
 * handler validates its parameters, fetches its per-endpoint transition
 * state through the bounds-checked getEndpointIndex() ->
 * get*TransitionStateByIndex() pair (color-control-server.cpp:820-828,
 * :848-856, :2075-2083, :2103-2111, :2451-2459 each return nullptr for an
 * out-of-range index) and answers Status::UnsupportedEndpoint if that comes
 * back null, then FULLY initialises the state it is about to run
 * (initialValue, currentValue, finalValue, stepsRemaining, stepsTotal,
 * timeRemaining, transitionTime, endpoint and the low/high limits) BEFORE
 * scheduleTimerCallbackMs() is reached. No path calls a delegate; the ember
 * callbacks (:3099-3290) are plain thunks into ColorControlServer plus
 * AddStatus. The handlers themselves: moveHueCommand :1414, stepHueCommand
 * :1664, moveSaturationCommand :1751, stepSaturationCommand :1837,
 * moveColorCommand :2260, stepColorCommand :2344, stopMoveStepCommand :473.
 *
 * StopMoveStep is compiled unconditionally: its definition at :3283 sits
 * outside all three MATTER_DM_PLUGIN_COLOR_CONTROL_SERVER_{HSV,XY,TEMP}
 * guards (:3097-3281), and the HSV-specific half of its body is separately
 * guarded, so it serves the CT-only light too.
 *
 * EHUE and CL commands (0x40-0x44) stay out: neither feature is advertised,
 * so the XML does not mandate them.
 */
constexpr CommandId kColorTempIncoming[] = { ColorControl::Commands::MoveToColorTemperature::Id,
                                             ColorControl::Commands::MoveColorTemperature::Id,
                                             ColorControl::Commands::StepColorTemperature::Id,
                                             ColorControl::Commands::StopMoveStep::Id,
                                             kInvalidCommandId };

constexpr CommandId kExtendedColorIncoming[] = {
    ColorControl::Commands::MoveToHue::Id,
    ColorControl::Commands::MoveHue::Id,
    ColorControl::Commands::StepHue::Id,
    ColorControl::Commands::MoveToSaturation::Id,
    ColorControl::Commands::MoveSaturation::Id,
    ColorControl::Commands::StepSaturation::Id,
    ColorControl::Commands::MoveToHueAndSaturation::Id,
    ColorControl::Commands::MoveToColor::Id,
    ColorControl::Commands::MoveColor::Id,
    ColorControl::Commands::StepColor::Id,
    ColorControl::Commands::MoveToColorTemperature::Id,
    ColorControl::Commands::MoveColorTemperature::Id,
    ColorControl::Commands::StepColorTemperature::Id,
    ColorControl::Commands::StopMoveStep::Id,
    kInvalidCommandId
};

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(colorTemperatureLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, colorTempAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kColorTempIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(colorTemperatureLightEndpoint, colorTemperatureLightClusters);

constexpr EmberAfDeviceType kColorTemperatureLightTypes[] = { { 0x010C, 4 } };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(extendedColorLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, extendedColorAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kExtendedColorIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(extendedColorLightEndpoint, extendedColorLightClusters);

constexpr EmberAfDeviceType kExtendedColorLightTypes[] = { { 0x010D, 4 } };

/* ---- thermostat (0x0301) (nRF 947-1040) -------------------------------
 *
 * Catalogue batch 2 audit, Thermostat (0x0201). This is the one server in
 * the batch that registers a wildcard AttributeAccessInterface, so it was
 * audited attribute by attribute rather than by presence alone.
 *
 *   Code-driven? No: no CodegenIntegration.cpp under thermostat-server/, and
 *   no case emitted in zap-generated/CodeDrivenInitShutdown.cpp.
 *
 *   AAI? Yes, and for EVERY endpoint: gThermostatAttrAccess is constructed
 *   with Optional<EndpointId>::Missing() (thermostat-server.h:54) and
 *   registered from MatterThermostatPluginServerInitCallback()
 *   (thermostat-server.cpp:1415), which the generated MATTER_PLUGINS_INIT
 *   now calls. It is nonetheless harmless here, because its Read() and
 *   Write() switches fall through to "just read/write the attribute store"
 *   for everything this file declares (:702-706 and :800-804 respectively):
 *     - LocalTemperature is intercepted ONLY when the
 *       LocalTemperatureNotExposed feature is set (:539-544). It is not.
 *     - RemoteSensing, likewise gated on LTNE, is not declared here.
 *     - Every delegate-backed case (PresetTypes, NumberOfPresets, Presets,
 *       ActivePresetHandle, ScheduleTypes, Schedules, MaxThermostatSuggestions,
 *       ThermostatSuggestions, CurrentThermostatSuggestion,
 *       ThermostatSuggestionNotFollowingReason) belongs to the Presets /
 *       MatterScheduleConfiguration / ThermostatSuggestions features, none of
 *       which is in FeatureMap and none of whose attributes is declared, so
 *       no read can reach a GetDelegate() call that would return nullptr.
 *     - ClusterRevision is the one attribute the AAI answers itself, from
 *       Thermostat::kRevision (:701). The seed below is that same 9, so the
 *       arena and the fabric agree; a stale seed here would show up as
 *       AT+MTATTR and a controller disagreeing about the revision.
 *
 *   ServerInit? emberAfThermostatClusterServerInitCallback()
 *   (thermostat-server.cpp:866) is an empty TODO body. It caches nothing, so
 *   this cluster does NOT join the init-hook section.
 *
 *   Delegate for the command? None. emberAfThermostatClusterSetpointRaiseLower
 *   Callback() (:1176) works entirely off FeatureMap and the setpoint
 *   attributes and never touches GetDelegate(); EnforceHeating/Cooling
 *   SetpointLimits() fall back to spec defaults when the optional Abs*
 *   limits are absent (:85-107), which is why they are not declared.
 *
 *   Deadband: MatterThermostatClusterServerAttributeChangedCallback() ->
 *   EnsureDeadband() returns immediately unless the AutoMode feature is set
 *   (:485-488), and it is function-array-bound so it never runs on a dynamic
 *   endpoint anyway.
 *
 * Thermostat.xml makes HEAT and COOL a choice="a" min="1" group (at least
 * one), so Heating|Cooling = 0x03 conforms and matches the C6, whose
 * mk_thermostat() ORs exactly those two. Thermostat.xml (device type) marks
 * SCH disallowConform; not set. SystemMode is seeded Off (0) rather than
 * esp-matter's constructor default of Auto (1): Auto is only meaningful with
 * the AutoMode feature, which this endpoint does not advertise. The setpoint
 * seeds are 1600/2400 hundredths, the C6's own deliberate departure from
 * esp-matter's 2000/2600 (cross-layer finding I1: the host library caches
 * upstream's boot values, and a first write matching the cache is swallowed
 * before it reaches the wire). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(thermostatAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::LocalTemperature::Id, TEMPERATURE, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::OccupiedCoolingSetpoint::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::OccupiedHeatingSetpoint::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MinHeatSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MaxHeatSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MinCoolSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MaxCoolSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::ControlSequenceOfOperation::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::SystemMode::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kThermostatIncoming[] = { Thermostat::Commands::SetpointRaiseLower::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(thermostatClusters)
DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(thermostatEndpoint, thermostatClusters);

constexpr EmberAfDeviceType kThermostatTypes[] = { { 0x0301, 4 } };

/* ---- fan (0x002B) (nRF 1041-1100) -------------------------------------
 *
 * Catalogue batch 2 audit, FanControl (0x0202). The simplest of the batch:
 * no CodegenIntegration.cpp and no CodeDrivenInitShutdown case, no
 * AttributeAccessInterface at all, and MatterFanControlPluginServerInit
 * Callback() is an empty definition in CHIP's own src/app/util/util.cpp:109.
 * There is no emberAfFanControlClusterServerInitCallback either, so nothing
 * to join the init-hook section with. Plain ember external storage.
 *
 * FanControl.xml declares all six features optionalConform with no choice
 * group, so FeatureMap 0 conforms - unlike OccupancySensing in batch 1,
 * whose min-1 group was the lesson that sent us to the cluster XML in the
 * first place. Every attribute declared below is mandatoryConform with no
 * feature gate; SpeedMax/SpeedSetting/SpeedCurrent (MultiSpeed),
 * RockSupport/RockSetting (Rocking), WindSupport/WindSetting (Wind) and
 * AirflowDirection are each behind a feature this endpoint does not
 * advertise, and Rocking/Wind/AirflowDirection/Step are delegate territory
 * (emberAfFanControlClusterStepCallback, fan-control-server.cpp:453, calls
 * GetDelegate() at :473 and answers Status::Failure at :478-482 when there
 * is none), which is why the Step feature is deliberately absent and no
 * incoming command is advertised.
 *
 * FanModeSequence is seeded OffLowMedHigh (0), NOT esp-matter's constructor
 * default of OffLowMedHighAuto (2). FanControl.xml gates enum values 2, 3
 * and 4 behind the AUT feature (choice "b") and values 0, 1 and 5 behind
 * !AUT (choice "a"), so with FeatureMap 0 the esp-matter default is not a
 * conformant value. Deliberate divergence from the C6, whose mk_fan() takes
 * the default; noted rather than copied.
 *
 * Known limitation, documented rather than bridged: the server's own
 * FanMode <-> PercentSetting coupling lives in MatterFanControlCluster
 * ServerAttributeChangedCallback (:327-451), which is reached through the
 * per-cluster functions array and therefore never runs on a dynamic
 * endpoint (the same mechanism as the OccupancySensing init in batch 1).
 * Both attributes read and write correctly over AT+MTATTR and over a
 * controller's IM; what does not happen is FanMode=Off zeroing PercentSetting
 * by itself. The host owns that coupling, which is the co-processor model
 * everywhere else in this file. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(fanControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanMode::Id, ENUM8, 1,
                          ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanModeSequence::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentSetting::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentCurrent::Id, PERCENT, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(fanClusters)
DECLARE_DYNAMIC_CLUSTER(FanControl::Id, fanControlAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(fanEndpoint, fanClusters);

constexpr EmberAfDeviceType kFanTypes[] = { { 0x002B, 4 } };

/* ---- window covering (0x0202) (nRF 1125-1199) -------------------------
 *
 * Catalogue batch 2 audit, WindowCovering (0x0102).
 *
 *   Code-driven? No: no CodegenIntegration.cpp, no CodeDrivenInitShutdown
 *   case.
 *
 *   AAI? Yes, wildcard-endpoint (WindowCoverAttrAccess is constructed with
 *   Optional<EndpointId>::Missing(), window-covering-server.h:79, registered
 *   from MatterWindowCoveringPluginServerInitCallback(), :994). Its Read()
 *   is four lines long (:118-128): it answers ClusterRevision from
 *   WindowCovering::kRevision and falls through to the attribute store for
 *   everything else. There is no Write() override at all. So the seed below
 *   must carry that same revision (5), and every other attribute is plain
 *   ember external storage.
 *
 *   ServerInit? There is no emberAfWindowCoveringClusterServerInitCallback
 *   in this tree - the answer to the round's "the WC server caches
 *   per-endpoint state?" question is no, it does not, and this cluster does
 *   NOT join the init-hook section. The only per-endpoint state is the delegate
 *   table, sized MATTER_DM_WINDOW_COVERING_CLUSTER_SERVER_ENDPOINT_COUNT +
 *   CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT (:47-48), so a dynamic
 *   endpoint index cannot run off it; GetDelegate() returns nullptr and
 *   every command handler explicitly tolerates that.
 *
 *   Commands with no delegate: correct, not merely tolerated. UpOrOpen
 *   (:637), DownOrClose (:687), StopMotion (:736) and GoToLiftPercentage
 *   (:837) each set TargetPositionLiftPercent100ths in the arena first, then
 *   log "WindowCovering has no delegate set" and still answer Success. That
 *   is exactly the co-processor split we want: the fabric's target lands in
 *   the arena, MatterPostAttributeChangeCallback turns it into a +MTATTR
 *   URC, and the host moves the motor and writes CurrentPosition back.
 *
 * WindowCovering.xml makes LF and TL a choice="a" min="1" group; Lift plus
 * PositionAwareLift (0x05) is the pair the percent100ths surface needs.
 * Tilt is left out this round (the C6 enables all four bits; this is a
 * narrower, honest subset rather than a parity bug - the tilt attributes and
 * their two commands are simply not declared). ConfigStatus is seeded
 * Operational|LiftPositionAware (0x09) rather than the C6's 0: the attribute
 * has default="desc" in the XML, GetMotionLockStatus() (:585) reads it, and
 * describing a position-aware, operational covering is the truthful answer
 * for what this endpoint presents. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(windowCoveringAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::Type::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::ConfigStatus::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::OperationalStatus::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id,
                              PERCENT100THS, 2, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::EndProductType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id,
                              PERCENT100THS, 2, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::Mode::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kWindowCoveringIncoming[] = { WindowCovering::Commands::UpOrOpen::Id,
                                                  WindowCovering::Commands::DownOrClose::Id,
                                                  WindowCovering::Commands::StopMotion::Id,
                                                  WindowCovering::Commands::GoToLiftPercentage::Id,
                                                  kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(windowCoveringClusters)
DECLARE_DYNAMIC_CLUSTER(WindowCovering::Id, windowCoveringAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kWindowCoveringIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(windowCoveringEndpoint, windowCoveringClusters);

constexpr EmberAfDeviceType kWindowCoveringTypes[] = { { 0x0202, 5 } };

/* ---- air quality sensor (0x002C) (nRF 1200-1246) ----------------------
 *
 * Catalogue batch 2 audit, AirQuality (0x005B). This is the "Instance that
 * requires app construction" case from the round's lesson list, and it comes
 * out clean:
 *
 *   Code-driven? No: no CodegenIntegration.cpp under air-quality-server/, no
 *   CodeDrivenInitShutdown case.
 *
 *   Instance? air-quality-server.cpp defines an Instance whose Init()
 *   registers it as an AttributeAccessInterface (:45-50), and if one existed
 *   it WOULD answer AirQuality and FeatureMap ahead of ember. Nothing in
 *   this firmware constructs one: the object file is linked, but
 *   MatterAirQualityPluginServerInitCallback() is CHIP's own empty
 *   definition in src/app/util/util.cpp:112, not something the cluster
 *   provides, and this port has no equivalent of the C6's
 *   mt_air_quality_register_all(). With no Instance registered, reads and
 *   writes are plain ember external storage against the arena below - the
 *   same shape as OccupancySensing in batch 1.
 *
 *   ServerInit? None exists; nothing joins the init-hook section.
 *
 * The four optional features (Fair, Moderate, VeryPoor, ExtremelyPoor) are
 * all advertised, so the host library's seven-value AirQuality_t enum can
 * never report a value this endpoint's feature map does not admit. The mask
 * is NOT a literal in s_seeds: seed_slots() reads it from
 * mt_air_quality_feature_mask() (mt_matter.h), the single accessor whose
 * whole point is that the ember feature map and any future Instance's
 * BitMask<Feature> cannot drift apart. On this platform that function was a
 * stub returning 0 until this batch; it now lives in mt_matter_sl.cpp
 * with the real bits. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(airQualityAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(AirQuality::Attributes::AirQuality::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(AirQuality::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(airQualitySensorClusters)
DECLARE_DYNAMIC_CLUSTER(AirQuality::Id, airQualityAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(airQualitySensorEndpoint, airQualitySensorClusters);

constexpr EmberAfDeviceType kAirQualitySensorTypes[] = { { 0x002C, 1 } };

