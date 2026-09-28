/*
 * mt_devtypes_sl_tables_b8.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds catalogue batch 8 (nRF 3322-4110): the composed appliances' banner
 * and device-type constants, the parenting policy and the shape struct
 * (moved here from mt_devtypes_sl_registry.inc, the nRF's order), and the
 * tables of the cooktop (0x0078), oven (0x007B), extractor hood (0x007A),
 * refrigerator (0x0070), temperature controlled cabinet (0x0071, six
 * shapes), cook surface (0x0077, two shapes) and microwave oven (0x0079).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, after mt_devtypes_sl_tables_b7.inc and
 * before mt_devtypes_sl_registry.inc, so the whole is still one
 * translation unit. Do not add it to hearth.slcp.
 */

/* ======================================================================
 * Catalogue batch 8: the composed appliances
 * ======================================================================
 *
 * The five device type ids the PARENTING POLICY names, given names here
 * because they appear in three places that must agree: mt_devtype_parent_ok()
 * below, the shape maps that render the same policy as cluster sets, and the
 * registry rows themselves. Every other row in this file keeps its bare hex
 * literal, because no other row's id is read by anything but the lookup.
 */
constexpr uint32_t kDtRefrigerator          = 0x0070;
constexpr uint32_t kDtTempControlledCabinet = 0x0071;
constexpr uint32_t kDtCookSurface           = 0x0077;
constexpr uint32_t kDtCooktop               = 0x0078;
constexpr uint32_t kDtOven                  = 0x007B;

/*
 * ---- the parenting policy, encoding one of two ------------------------
 *
 * Catalogue batch 8. On this port the predicate and its extern "C"
 * forwarder (mt_devtype_parent_ok() in mt_devtypes_sl.cpp) were live before
 * this batch, ported ahead of the cluster sets so AT+MTEP= answered the
 * policy early; batch 8 moved the predicate here, the nRF's order. Two
 * device types restrict their parent (AT_MT_SPEC.md 3.9's policy), and this
 * constexpr predicate is where the rule lives.
 *
 * It is constexpr for one reason: the SAME policy is encoded a second time,
 * as the per-row shape maps below, and the two can disagree. That hazard is
 * exactly the one ARCHITECTURE.md records the C6 rejecting when it
 * declined to give the cabinet a variant bit ("a second encoding of a fact
 * the tree already carries, able to disagree with it"). Here the second
 * encoding is unavoidable (the cluster set has to come from somewhere) so it
 * is made CHECKABLE instead: shape_domain_matches_policy() below evaluates
 * this function over the whole registry at compile time and fails the build
 * on any disagreement in either direction.
 *
 * variant is a parameter of the contract (core/include/mt_devtypes.h) and is
 * deliberately unused: neither rule depends on it today. It stays in the
 * signature, and in the compile-time check's enumeration, so a future rule
 * that DOES depend on it needs no plumbing.
 */
constexpr bool parent_policy_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    (void)variant;
    /* Cook Surface: legal ONLY under a Cooktop. The unparented form is
     * rejected by the same expression, since no device type id is 0 and
     * parent_devtype 0 is how the AT layer spells "no parent"
     * (core/mt/mt_at.c:1903-1905). This is the catalogue's only device type
     * that REQUIRES a parent; the C6's rule is mt_devtypes.cpp:2767-2772 and
     * carries the identical one-expression note. */
    if (devtype_id == kDtCookSurface) {
        return parent_devtype == kDtCooktop;
    }
    /* Temperature Controlled Cabinet: legal unparented (the standalone rows
     * earlier compositions declared keep working), or under a Refrigerator
     * or an Oven and nothing else. Those two are the appliances that give a
     * child cabinet a coherent conditional cluster set (Cooler, Heater); a
     * third parent would be a cabinet with no defined set at all, which is
     * why the policy refuses rather than falling back to the bare shape.
     * The C6's rule is mt_devtypes.cpp:2773-2783. */
    if (devtype_id == kDtTempControlledCabinet) {
        return parent_devtype == 0 || parent_devtype == kDtRefrigerator ||
               parent_devtype == kDtOven;
    }
    /* Everything else is permissive, including the unparented form: PartsList
     * simply reflects whatever the host declares and this firmware holds no
     * opinion about whether the pairing makes sense (AT_MT_SPEC.md 3.9). */
    return true;
}

/*
 * ---- shape selection, the second encoding ----------------------------
 *
 * A shape is one realised cluster set for one (variant, parent devtype)
 * pair. Rows whose cluster set does not depend on the parent carry no
 * shapes at all and keep the ep_type / ep_type_v1 pair the struct comment
 * below describes; only a row the policy RESTRICTS needs a shape map, and
 * the compile-time check makes that an if-and-only-if rather than a
 * convention.
 *
 * parent_devtype 0 is the unparented shape, the same spelling
 * mt_devtype_create()'s parent_devtype argument uses (main.cpp passes 0 when
 * comp.parent[i] is MT_COMP_NO_PARENT).
 *
 * WHY NOT A WIDENED ROW FOR EVERY ROW. The nRF's ruling (DE416), kept here
 * as its reasoning; on this port the member and all 52 rows were carried
 * before batch 8, so this batch added tables only. A Span<const shape>
 * replacing ep_type/ep_type_v1 on every row is
 * the more general mechanism and remains the right refactor for the day a
 * SECOND parent-conditional device type appears; landing it in the same
 * commit as seven new device types, an arena resize, three new SDK translation
 * units and a zap regeneration would make the whole batch unreviewable. This
 * member is additive by construction: it is the LAST member, so aggregate
 * initialization zero-fills it to an empty span for every row that does not
 * name it, and every shapeless row keeps its exact original text and
 * their exact original bytes. That is the same trick batches 7a and 7b each
 * relied on once (ep_type_v1, device_types_v1) and this is its third use.
 *
 * WHY NOT DUPLICATE REGISTRY ROWS keyed by parent: mt_devtype_is_known() and
 * mt_devtype_variant_ok() are called from core/mt/mt_at.c:1875 and :1883
 * BEFORE any parent has been parsed, so the registry's primary key has to
 * stay the bare device type id. Changing it to (id, parent) would have made
 * AT+MTEP=0x0071 answer +MTERR:6.
 */
struct hearth_shape {
    uint8_t variant;
    uint32_t parent_devtype;
    const EmberAfEndpointType *ep_type;
};

/* ---- cooktop (0x0078) --------------------------------------------------
 *
 * Catalogue batch 8 audit, OnOff with the OffOnly feature.
 *
 *   Nothing new CHIP-side: OnOff is already compiled, already dispatched by
 *   the generated IMClusterCommandHandler, already in hearth.zap, already
 *   seeded. The regeneration for this batch leaves it untouched.
 *
 *   The C6's mk_cooktop() (platform/esp32c6/main/mt_devtypes.cpp:422-427) is
 *   a bare thunk: cooktop::add() calls add_device_type(), on_off::create()
 *   and on_off::feature::off_only::add() (esp_matter_endpoint.cpp:1562-1573),
 *   and cooktop::config_t is {descriptor, on_off} with NO identify field, so
 *   there is no Identify on a cooktop on either platform. Cooktop.xml:66-67
 *   lists Identify as optional only, so the omission is conformant and is
 *   the C6-parity choice, not an oversight.
 *
 * THE ONE TRAP ON THIS DEVICE TYPE, and it is the whole reason for the
 * command list below. Cooktop.xml:69-74 makes OnOff mandatory WITH the
 * OFFONLY feature mandatory inside it: a controller may switch a cooktop
 * off, never on. Reusing kOnOffIncoming (Off/On/Toggle, declared with the
 * lighting types above) would advertise On and Toggle in
 * AcceptedCommandList, and the generated OnOff dispatch would happily
 * execute them, so a controller could switch a cooktop ON against the
 * device type's own conformance. Turning a cooktop on is the host's act, an
 * AT+MTATTR write of 0x0006/0x0000 (AT_MT_SPEC.md 3.9 says so for the
 * cook surface, and the same rule governs its parent).
 *
 * The C6 arrives at the same AcceptedCommandList by a different road, worth
 * recording because it reads like an accident there and is a declaration
 * here: cluster::on_off::create() adds ONLY command::create_off()
 * (esp_matter_cluster.cpp:1233-1260); On and Toggle are added per device
 * type by esp_matter_endpoint.cpp, and cooktop::add() does not add them. So
 * the C6's cooktop also accepts Off alone, and feature::off_only::add() sets
 * the feature-map bit and nothing else (esp_matter_feature.cpp:508-523).
 *
 * racOnOffAttrs is reused rather than onOffAttrs for the reason its own note
 * gives: the four Lighting-gated attributes (GlobalSceneControl, OnTime,
 * OffWaitTime, StartUpOnOff) have no place on a featureless-but-for-OffOnly
 * OnOff. The FeatureMap seed is device-type-qualified (0x04,
 * OnOff::Feature::kOffOnly, OnOff/Enums.h:85) against the wildcard 0x01 the
 * lighting types take.
 *
 * Revision 1 per data_model/1.5/device_types/Cooktop.xml. */
constexpr CommandId kOnOffOffOnlyIncoming[] = { OnOff::Commands::Off::Id, kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cooktopClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffOffOnlyIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cooktopEndpoint, cooktopClusters);

constexpr EmberAfDeviceType kCooktopTypes[] = { { 0x0078, 1 } };

/* ---- oven (0x007B) -----------------------------------------------------
 *
 * Catalogue batch 8 audit. Identify plus Descriptor, four slots, the
 * second cheapest block in the catalogue (the cooktop's 56 B is the
 * cheapest).
 *
 *   Nothing new CHIP-side: Identify is already compiled and already seeded.
 *
 *   The C6's mk_oven() (mt_devtypes.cpp:1037-1049) calls oven::create() and
 *   then hand-adds Identify, because oven::add() calls add_device_type() and
 *   nothing else (esp_matter_endpoint.cpp:1360-1367) and oven::config_t
 *   carries no identify field. This list is the result, declared.
 *
 * THE OVEN IS A BARE PARENT ENDPOINT BY DESIGN (AT_MT_SPEC.md 3.9): all
 * of its function lives in its Temperature Controlled Cabinet children, so a
 * useful oven is always at least two AT+MTEP rows. That is also a disclosed
 * conformance hole shared with the refrigerator: Oven.xml:67-71 puts a
 * mandatory conditionRequirement on a 0x0071 child, so a lone AT+MTEP=0x007B
 * row is sub-conformant. The firmware does not and should not refuse it: a
 * composition-level requirement is not an append-level one, the host can
 * only satisfy it by declaring a second row, and the C6 has the identical
 * hole. AT_MT_SPEC.md carries the sentence instead of the code carrying a
 * check.
 *
 * Revision 2 per data_model/1.5/device_types/Oven.xml. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(ovenClusters)
DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(ovenEndpoint, ovenClusters);

constexpr EmberAfDeviceType kOvenTypes[] = { { 0x007B, 2 } };

/* ---- extractor hood (0x007A) -------------------------------------------
 *
 * Catalogue batch 8 audit. FanControl plus Descriptor, and NO Identify.
 *
 *   Nothing new CHIP-side: FanControl is already compiled, already in
 *   hearth.zap, already seeded, and its ClustersWithAttributeChangedFunctions
 *   entry (config-data.yaml:106-112) already works for 0x002B.
 *
 * WHY THIS IS A NEW TWO-CLUSTER LIST AND NOT A SECOND fanEndpoint ROW. The
 * air purifier (0x002D) above reuses &fanEndpoint verbatim, and its note is
 * explicit about why that is legitimate: air_purifier::add() really does
 * create Identify AND FanControl (esp_matter_endpoint.cpp:745-754), so the
 * reused list mirrors what the C6 actually builds. extractor_hood::add()
 * does NOT: it calls add_device_type() and fan_control::create() only
 * (esp_matter_endpoint.cpp:1650-1657), and extractor_hood::config_t is
 * {descriptor, fan_control} with no identify field. Reusing fanEndpoint here
 * would carry an Identify cluster the C6 does not, breaking the stated
 * invariant that this file's lists mirror what the C6 actually builds, for a
 * cluster ExtractorHood.xml:66-67 lists as optional anyway. Ruled in the
 * batch brief: compose WITHOUT Identify. The cost is four slots and 72 arena
 * bytes cheaper per endpoint (104 against the fan's 176), which is
 * incidental.
 *
 * Conformance note: ExtractorHood.xml:79-85 marks the FanControl Wind,
 * AirflowDirection and Rocking features disallowConform. This port seeds
 * FanControl's FeatureMap 0 for every fan-bearing type, so nothing is
 * advertised and nothing disallowed is present. Clean.
 *
 * The fan's known FanMode/PercentSetting coupling gap (the audit note on
 * fanControlAttrs above: MatterFanControlClusterServerAttributeChangedCallback
 * is functions-array-bound and never runs on a dynamic endpoint) now ships
 * under a THIRD device-type name. Restated deliberately, the air purifier's
 * rule: an extractor hood host owns that coupling exactly the way a fan host
 * does, and nothing about this row re-solves it.
 *
 * Revision 1 per data_model/1.5/device_types/ExtractorHood.xml. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(extractorHoodClusters)
DECLARE_DYNAMIC_CLUSTER(FanControl::Id, fanControlAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(extractorHoodEndpoint, extractorHoodClusters);

constexpr EmberAfDeviceType kExtractorHoodTypes[] = { { 0x007A, 1 } };

/* ---- refrigerator (0x0070) ---------------------------------------------
 *
 * Catalogue batch 8 audit, RefrigeratorAlarm (0x0057) and
 * RefrigeratorAndTemperatureControlledCabinetMode (0x0052).
 *
 * RefrigeratorAlarm 0x0057:
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; the regeneration left CodeDrivenInitShutdown.cpp
 *   byte-identical). NOT CommandHandlerInterface-only either
 *   (config-data.yaml:21-88), and harmlessly so: the cluster declares no
 *   commands at all, matching RefrigeratorAlarm.xml's disallowConform on
 *   ModifyEnabledAlarms and this tree's own CommandIds.h, which declares
 *   none. The regeneration left IMClusterCommandHandler.cpp byte-identical.
 *
 *   NO Instance, NO Delegate, NO per-endpoint object of any kind. This is
 *   the batch's one genuinely free cluster: RefrigeratorAlarmServer is a
 *   process-global singleton (`static RefrigeratorAlarmServer instance`,
 *   refrigerator-alarm-server.h:53) reached through Instance(), and it reads
 *   and writes the three attributes through the generated Accessors, which
 *   on a dynamic endpoint resolve straight into this file's arena through
 *   the external-storage callbacks. 1 byte of .bss for the whole device.
 *
 *   ServerInit? None: MatterRefrigeratorAlarmPluginServerInitCallback() is
 *   an empty body (refrigerator-alarm-server.cpp:198). No ordering hazard of
 *   any kind, which is why this device type has nothing at all in
 *   mt_devtype_create()'s claim block except its ModeBase slot.
 *
 *   Events: Notify (0x0000). Emitted by SetStateValue() itself, which writes
 *   the State attribute and then LogEvent()s the transition in one call
 *   (refrigerator-alarm-server.cpp:115-149) - the "one call, both effects"
 *   shape AT_MT_SPEC.md §3.22 describes, and the reason AT+MTALARM
 *   rather than AT+MTATTR is the write path for State. The dynamic
 *   endpoint's empty eventList makes EventList read empty without blocking
 *   emission, the generic switch precedent.
 *
 *   Note there is no attribute 0x0001: the ids are Mask 0x0000, State
 *   0x0002, Supported 0x0003 (RefrigeratorAlarm/AttributeIds.h:19-29). Every
 *   one is a plain BITMAP32 and every one is ordinary AT+MTATTR-reachable;
 *   only State's WRITE path is diverted to AT+MTALARM (AT_MT_SPEC.md
 *   3.22 says reading all three and writing Mask or Supported works
 *   over AT+MTATTR like any other attribute, and that only writing State
 *   goes through AT+MTALARM so the Notify event actually fires; nothing in
 *   the code enforces that, and a raw write would land in the arena and
 *   skip the event).
 *
 * RefrigeratorAndTemperatureControlledCabinetMode 0x0052 is a plain
 * mode-base-server alias (zap_cluster_list.json:311-313), already compiled
 * since batch 5, so modeBaseAttrs and kModeBaseIncoming/kModeBaseOutgoing
 * are reused verbatim: every ModeBase alias numbers ChangeToMode 0x00 and
 * ChangeToModeResponse 0x01 in its own generated CommandIds.h. Its
 * VerifyOrDie-on-ordering Init() is batch 5's already-domesticated hazard.
 *
 * The C6's own quirk check on this device type came back CLEAN and is worth
 * carrying, because so few in this catalogue did: refrigerator_and_tcc_mode::
 * create() wires create_change_to_mode() and create_change_to_mode_response()
 * unconditionally (esp_matter_cluster.cpp:2934-2937), unlike the
 * OperationalState and SmokeCoAlarm creates that famously did not, and
 * refrigerator_alarm::create() has no command section at all (:2869-2898),
 * which is what conformance wants.
 *
 * Identify is hand-added on the C6 (mk_refrigerator(), mt_devtypes.cpp:
 * 1005-1008) because refrigerator::config_t carries no identify field;
 * declared here, same result.
 *
 * Revision 2 per data_model/1.5/device_types/Refrigerator.xml. Composing a
 * Temperature Controlled Cabinet under this endpoint is legal and gives that
 * cabinet the Cooler cluster set (the parenting policy above); a lone
 * refrigerator row is sub-conformant against Refrigerator.xml:66-72's
 * mandatory 0x0071 child, the oven's disclosed hole exactly. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(refrigeratorAlarmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::Mask::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::State::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::Supported::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(refrigeratorClusters)
DECLARE_DYNAMIC_CLUSTER(RefrigeratorAndTemperatureControlledCabinetMode::Id, modeBaseAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(RefrigeratorAlarm::Id, refrigeratorAlarmAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(refrigeratorEndpoint, refrigeratorClusters);

constexpr EmberAfDeviceType kRefrigeratorTypes[] = { { 0x0070, 2 } };

/* ---- TemperatureControl (0x0056), and its two mutually exclusive shapes -
 *
 * Catalogue batch 8 audit. One cluster, two declared attribute lists, and
 * the variant picks which. Shared verbatim by the Temperature Controlled
 * Cabinet (0x0071) and the Cook Surface (0x0077), which is what forced the
 * seed table's variant column: the two device types need the SAME
 * variant-dependent FeatureMap, so no device-type-qualified row could split
 * them.
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; the regeneration left CodeDrivenInitShutdown.cpp
 *   byte-identical). NOT CommandHandlerInterface-only either
 *   (config-data.yaml:21-88), and this time that MATTERS: the cluster
 *   declares SetTemperature, so regenerating hearth.zap added a
 *   TemperatureControl case to IMClusterCommandHandler.cpp, the first new
 *   generated dispatch case since catalogue batch 4. Before it, this build
 *   dispatched Groups, OnOff, LevelControl, ColorControl, Thermostat,
 *   WindowCovering, DoorLock, ValveConfigurationAndControl,
 *   ThreadNetworkDiagnostics, SmokeCoAlarm and ModeSelect.
 *
 *   No Instance and no per-endpoint Delegate. What the SDK keeps is one
 *   file-scope TemperatureControlAttrAccess constructed
 *   Optional<EndpointId>::Missing() (temperature-control-server.cpp:40-49),
 *   i.e. a wildcard AttributeAccessInterface serving
 *   SupportedTemperatureLevels on every endpoint, and one process-global
 *   SupportedTemperatureLevelsIteratorDelegate * behind free-function
 *   GetInstance()/SetInstance() (:37, :56-66). The AAI is registered by
 *   MatterTemperatureControlPluginServerInitCallback() (:232-235), which is
 *   emitted into MATTER_PLUGINS_INIT only because the cluster is now in the
 *   zap; miss that and SupportedTemperatureLevels answers
 *   UnsupportedAttribute on every cabinet, silently. The iterator delegate
 *   is the SDK's usual "the app must supply it" hole: SetInstance() has no
 *   caller anywhere in the SDK, so the port calls it itself, exactly as the
 *   C6 does (main.cpp:1829).
 *
 *   Init()? None by that name, and no VerifyOrDie anywhere in this cluster.
 *   emberAfTemperatureControlClusterServerInitCallback() is an empty body
 *   (:230). No ordering hazard at all.
 *
 *   Events: none.
 *
 * SetTemperature IS NOT A +MTCMD CONSUMER, on either platform, and that is
 * worth stating loudly because every other adjudicated cluster in this
 * firmware is. The whole command is handled by the ember callback
 * (temperature-control-server.cpp:112-224): it range-checks the target
 * against MinTemperature/MaxTemperature, enforces the Step modulus
 * (:150-168), then writes TemperatureSetpoint (:169) or
 * SelectedTemperatureLevel (:186). Those writes land in this port's arena
 * and DO raise a +MTATTR URC through MatterPostAttributeChangeCallback(). So
 * a controller changing a cabinet's setpoint reaches the host as an
 * attribute URC, never as a command forward, and the host has no veto. The
 * C6 overrides nothing here either.
 *
 * THE FEATUREMAP IS FUNCTIONALLY LOAD-BEARING, not descriptive. The callback
 * reads it back through TemperatureControlHasFeature() to choose its branch
 * (:117-124, :183), so the seeded value and the declared list must agree or
 * a healthy endpoint rejects valid commands: with BOTH bits set it answers
 * Failure outright (:118-123), and on the TemperatureLevel branch a null
 * iterator delegate answers NotFound (:188-192). The C6 enforces
 * exactly-one at cluster-create time through VALIDATE_FEATURES_EXACT_ONE;
 * here the two separate declared lists plus the two variant-qualified seed
 * rows are what enforce it.
 *
 * The Step bit rides variant 0 with TemperatureNumber, and that is the B119
 * fix carried over rather than rediscovered: Step is its own feature
 * (esp_matter_feature.cpp:1943-1966), and a TemperatureNumber cabinet
 * without it answered +MTERR:4 to every host-library begin(). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(temperatureControlNumberAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::TemperatureSetpoint::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::MinTemperature::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::MaxTemperature::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::Step::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* SupportedTemperatureLevels is declared ARRAY size 0: metadata-only, so
 * AttributeList stays truthful while attr_gets_slot() refuses it a slot and
 * seed_slots() skips it quietly. It is served by the SDK's wildcard AAI out
 * of this endpoint's block-resident label store, and it doubles as the
 * kStoreWalk discriminator that tells this list from the number list above
 * (they share a cluster id). AT+MTATTR has no path to it at all
 * (AT_MT_SPEC.md 3.16); AT+MTTEMPLEVELS is how a host fills it. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(temperatureControlLevelAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::SelectedTemperatureLevel::Id, INT8U, 1,
                          0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::SupportedTemperatureLevels::Id,
                              ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* SetTemperature is mandatory on both variants (temperature-control-cluster.xml
 * :83 declares it with no optional attribute) and, unlike every other
 * incoming list in this file, it is answered entirely inside the SDK: the
 * generated dispatch case the zap regeneration added calls
 * emberAfTemperatureControlClusterSetTemperatureCallback(), which validates
 * and writes an arena attribute. No outgoing list: the command's response is
 * a plain status, not a response command. */
constexpr CommandId kTemperatureControlIncoming[] = {
    TemperatureControl::Commands::SetTemperature::Id, kInvalidCommandId
};

/* ---- temperature controlled cabinet (0x0071), unparented -----------------
 *
 * Catalogue batch 8. The bare cabinet: TemperatureControl and Descriptor and
 * nothing else, which is exactly what the C6 builds, because
 * temperature_controlled_cabinet::add() calls add_device_type() then
 * temperature_control::create() and stops (esp_matter_endpoint.cpp:
 * 1308-1316), and its config_t is {descriptor, temperature_control} with no
 * identify field. TemperatureControlledCabinet.xml lists no Identify either,
 * so there is NO Identify on a cabinet on either platform.
 *
 * These are two of the cabinet's six realised shapes, the two the parent
 * does not touch; the Cooler and Heater halves are below. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cabinetNumberEndpoint, cabinetNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetLevelEndpoint, cabinetLevelClusters);

/* Revision 5 per data_model/1.5/device_types/TemperatureControlledCabinet.xml.
 * The advertised span does NOT change with the parent, unlike the water
 * heater's and battery storage's variant spans: a Cooler cabinet and a
 * Heater cabinet are both 0x0071 revision 5, and the conformance difference
 * lives in the cluster set alone. */
constexpr EmberAfDeviceType kCabinetTypes[] = { { 0x0071, 5 } };

/* ---- the cabinet's parent-conditional halves --------------------------
 *
 * Catalogue batch 8. Four more shapes, and the batch's one genuinely good
 * surprise: where the C6 had to HAND-ROLL two entire cluster shells because
 * esp-matter ships no cluster::oven_mode and no
 * cluster::oven_cavity_operational_state namespace at all
 * (ARCHITECTURE.md records that as the section 8.6 disease at its
 * worst grade), CHIP ships both server halves. OvenMode is a plain
 * mode-base-server alias (zap_cluster_list.json:279), so modeBaseAttrs and
 * the two shared ModeBase command lists carry it verbatim, and
 * OvenCavityOperationalState::Instance is a purpose-built public subclass
 * that forwards to the protected three-argument OperationalState::Instance
 * ctor with the id baked in (operational-state-server.h:459-478). It adds no
 * data members, so opStateAttrs is reused verbatim too and the existing
 * OperationalState Instance storage is exactly the right size (asserted at
 * the pool in mt_matter_sl_b4.inc).
 *
 * THE CAVITY'S COMMAND LIST IS NARROWER THAN THE BASE CLUSTER'S, and that is
 * not an omission. operational-state-oven-cluster.xml declares Stop (0x01)
 * and Start (0x02) only; Pause (0x00) and Resume (0x03) are disallowConform
 * on this derived cluster, so a controller invoking either gets
 * UNSUPPORTED_COMMAND from the data model with nothing reaching the host at
 * all (AT_MT_SPEC.md 3.17). The outgoing list is the shared
 * kOpStateOutgoing: OperationalCommandResponse is 0x04 in the cavity's own
 * generated CommandIds.h as well, so one constant serves both.
 *
 * Revisions are this tree's own Metadata.h, as ever, and BOTH diverge
 * downward from the C6, which hand-set 2 for each from the 1.5.1 XMLs
 * (mt_devtypes.cpp:1200, :1218) rather than copying the 4 and 3 its mirrored
 * rvc_run_mode / rvc_operational_state bodies carried. This tree says
 * OvenMode 1 and OvenCavityOperationalState 1. Disclosable, not a bug: the
 * port's convention has been this tree's kRevision since batch 2, and the
 * cavity's is inert anyway because Instance::Read() answers ClusterRevision
 * itself. */
constexpr CommandId kOvenCavityIncoming[] = { OvenCavityOperationalState::Commands::Stop::Id,
                                              OvenCavityOperationalState::Commands::Start::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetCoolerNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(RefrigeratorAndTemperatureControlledCabinetMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetCoolerLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(RefrigeratorAndTemperatureControlledCabinetMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetHeaterNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OvenMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(OvenCavityOperationalState::Id, opStateAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kOvenCavityIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetHeaterLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OvenMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(OvenCavityOperationalState::Id, opStateAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kOvenCavityIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cabinetCoolerNumberEndpoint, cabinetCoolerNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetCoolerLevelEndpoint, cabinetCoolerLevelClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetHeaterNumberEndpoint, cabinetHeaterNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetHeaterLevelEndpoint, cabinetHeaterLevelClusters);

/* All six realised shapes, and the whole point of the mechanism: the
 * composition blob stores only device type, variant and parent index, and
 * this table is what turns the pair into a cluster set at rebuild time. A
 * host cannot ask for a different set, and changing a cabinet's parent
 * changes the cabinet (AT_MT_SPEC.md 3.9). */
constexpr hearth_shape kCabinetShapes[] = {
    { 0, 0, &cabinetNumberEndpoint },
    { 1, 0, &cabinetLevelEndpoint },
    { 0, kDtRefrigerator, &cabinetCoolerNumberEndpoint },
    { 1, kDtRefrigerator, &cabinetCoolerLevelEndpoint },
    { 0, kDtOven, &cabinetHeaterNumberEndpoint },
    { 1, kDtOven, &cabinetHeaterLevelEndpoint },
};

/* ---- cook surface (0x0077) ---------------------------------------------
 *
 * Catalogue batch 8. TemperatureControl (either variant) plus an OffOnly
 * OnOff plus Descriptor, and no Identify: cook_surface::config_t is
 * {descriptor, temperature_control} (esp_matter_endpoint.h:832-841) and
 * CookSurface.xml lists only OnOff, TemperatureControl and
 * TemperatureMeasurement.
 *
 * THE ONLY DEVICE TYPE IN THE CATALOGUE THAT REQUIRES A PARENT. Both shapes
 * name a Cooktop; the unparented form has no shape at all, which is the
 * cluster-set half of what mt_devtype_parent_ok() says on the AT+MTEP= line
 * itself (+MTERR:1, AT_MT_SPEC.md 3.9). It is also the only append this
 * platform can reject for a reason no earlier nRF test exercises.
 *
 * THE C6'S B216 SCRUB IS NOT MIRRORED, AND THAT IS DELIBERATE.
 * cook_surface::add() OVERWRITES config->temperature_control.feature_flags
 * with temperature_level before creating the cluster
 * (esp_matter_endpoint.cpp:1540), so pre-set flags cannot survive; the C6's
 * thunk therefore destroys the forced TemperatureLevel cluster after
 * create() and rebuilds it as TemperatureNumber plus Step, for variant 0
 * only (mt_devtypes.cpp:1305-1321). None of that transfers: this port
 * declares its lists, so there is nothing to overwrite and nothing to
 * destroy. What DOES transfer is the reason B216 existed, that the two
 * variants must be distinguishable on the wire, and the two declared lists
 * deliver it directly. The regression net that caught B216 (AT+MTATTR on
 * 0x0056/0x0000 and 0x0003 answering on v0 and +MTERR:4 on v1, and
 * AT+MTTEMPLEVELS accepted on v1 only) is the right check here too.
 *
 * The OffOnly OnOff is the cooktop's list one endpoint down: racOnOffAttrs
 * with kOnOffOffOnlyIncoming, and the same device-type-qualified 0x04
 * FeatureMap seed. CookSurface.xml:67-73 makes OnOff optional with OFFONLY
 * mandatory inside it, so composing it at all is the C6's superset choice
 * mirrored, not a requirement; AT_MT_SPEC.md 3.9 already tells hosts
 * that turning a surface on is an AT+MTATTR write of 0x0006/0x0000.
 *
 * Revision 2 per data_model/1.5/device_types/CookSurface.xml. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cookSurfaceNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kOnOffOffOnlyIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cookSurfaceLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kOnOffOffOnlyIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cookSurfaceNumberEndpoint, cookSurfaceNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cookSurfaceLevelEndpoint, cookSurfaceLevelClusters);

constexpr EmberAfDeviceType kCookSurfaceTypes[] = { { 0x0077, 2 } };

constexpr hearth_shape kCookSurfaceShapes[] = {
    { 0, kDtCooktop, &cookSurfaceNumberEndpoint },
    { 1, kDtCooktop, &cookSurfaceLevelEndpoint },
};

/* ---- microwave oven (0x0079) -------------------------------------------
 *
 * Catalogue batch 8 audit, MicrowaveOvenMode (0x005E) and
 * MicrowaveOvenControl (0x005F). The heaviest device type in the batch by
 * every measure: three per-endpoint object pairs on one endpoint, and the
 * heaviest object draw of the batch (416 B), which the 8,464 B
 * cluster-object arena holds sixteen of.
 *
 * MicrowaveOvenMode 0x005E:
 *
 *   Code-driven? No. CHI-only? No either (absent from config-data.yaml:
 *   21-88), and harmlessly, because IT DECLARES NO COMMANDS AT ALL. The
 *   regeneration left IMClusterCommandHandler.cpp byte-identical.
 *
 *   THE TRAP, and it is a copy-paste trap rather than a reasoning one:
 *   MicrowaveOvenMode inherits ChangeToMode from ModeBase and then marks it
 *   disallowConform, along with ChangeToModeResponse
 *   (data_model/1.5/clusters/Mode_MicrowaveOven.xml, the commands block),
 *   which is exactly the treatment the oven cavity gives Pause and Resume
 *   in the cabinet section above; the generated MicrowaveOvenMode/CommandIds.h
 *   consequently carries kAcceptedCommandsCount 0 and kGeneratedCommandsCount
 *   0. Every other ModeBase alias in this file reuses kModeBaseIncoming and
 *   kModeBaseOutgoing, and this one MUST NOT: its incoming and outgoing lists
 *   are both nullptr, so both lists come out empty. The mode is selected
 *   through MicrowaveOvenControl's SetCookingParameters cookMode field
 *   instead (AT_MT_SPEC.md 3.17 and 3.20.1; batch 5's audit found the
 *   same). Advertising ChangeToMode here would let a controller invoke a
 *   command the cluster disallows, and the ModeBase Instance's own
 *   CommandHandlerInterface would execute it.
 *
 *   Everything else about it is the standard alias: modeBaseAttrs verbatim,
 *   a ModeBase pool slot, a block-resident mt_mb_store_t, and Init()'s
 *   VerifyOrDie ordering hazard.
 *
 * MicrowaveOvenControl 0x005F:
 *
 *   Code-driven? No. CHI-only? YES (config-data.yaml:61), so no generated
 *   dispatch for its two commands; the Instance's own
 *   CommandHandlerInterface takes them. Events: none.
 *
 *   Its Instance is the batch's sharpest hazard, and the whole ordering
 *   contract lives at mt_matter_mwoc_register() in mt_matter_sl_b8.inc.
 *
 *   PowerAsNumber is MANDATORY and PowerNumberLimits is dead code, and that
 *   pair of facts is why mwocAttrs is four attributes and not nine.
 *   Instance::Init() enforces exactly one of PowerAsNumber/PowerInWatts and
 *   that PowerNumberLimits implies PowerAsNumber
 *   (microwave-oven-control-server.cpp:66-96); esp-matter's own create() runs
 *   VALIDATE_FEATURES_EXACT_ONE against PowerAsNumber alone
 *   (esp_matter_cluster.cpp:3083-3084), and its power_number_limits::add()
 *   only applies when PowerAsNumber is ABSENT, the inverse of conformance
 *   and therefore dead in the pinned tree. Consequence, on both platforms:
 *   MinPower, MaxPower and PowerStep are NOT declared and read the SDK's own
 *   compiled-in 10/100/10 (microwave-oven-control-server.cpp:163-179,
 *   constants at the header's :34-38). SupportedWatts, SelectedWattIndex and
 *   WattRating are PowerInWatts-side and absent for the same reason.
 *
 *   CookTime, MaxCookTime and PowerSetting ARE declared, and all three are
 *   Instance-served: Instance::Read() answers CookTime from its own member
 *   and the other two from the delegate (:142-157). The slots exist for
 *   AttributeList truthfulness and are seeded to exactly what those sources
 *   serve at boot, the FanControl agreement discipline, so an AT+MTATTR read
 *   answers the same value a controller sees until the first cooking command
 *   moves the live value. NO k_instance_served carve-out, deliberately and
 *   against the audit's suggestion: AT_MT_SPEC.md 3.17 says plainly
 *   that neither attribute raises a +MTATTR URC and that a host reads them
 *   back only through a commissioned controller, because SetCookTimeSec()
 *   and the delegate's own bookkeeping bypass emberAfWriteAttribute
 *   entirely. A carve-out would have made the AT read live and contradicted
 *   the published contract; the shadow going stale is what the spec
 *   describes.
 *
 * AddMoreTime is where this port is SIMPLER than the C6. esp-matter ships
 * create_add_more_time() (esp_matter_command.cpp:2590) with zero callers, so
 * the C6's thunk hand-adds it after create() (mt_devtypes.cpp:946-949). Here
 * the accepted-command list is declared metadata and AddMoreTime is simply a
 * second entry in it.
 *
 * OperationalState needs its OWN attribute list, and this is the one place
 * opStateAttrs could not be reused: MicrowaveOven.xml:76-88 makes
 * CountdownTime (0x0002) MANDATORY and opStateAttrs does not declare it.
 * Instance::Read() serves it from the delegate's GetCountdownTime()
 * (operational-state-server.cpp:402-406), which this port's delegate answers
 * NullNullable for, so the ember slot is an inert shadow seeded to the uint32
 * null sentinel: the arena read and the served value agree on null, both
 * answer +MTERR:5, and no carve-out row is needed. That agreement is exactly
 * why CountdownTime is NOT in k_instance_served even though its two
 * neighbours on the same cluster are.
 *
 * NO Identify: microwave_oven::config_t derives from
 * app_with_operational_state_config, whose base has no identify field at all
 * (mt_devtypes.cpp:885-889), and MicrowaveOven.xml agrees.
 *
 * ONE DISCLOSED CONFORMANCE GAP, and it is worth naming because it is the
 * thing revision 2 exists for. MicrowaveOven.xml's own revision history
 * reads "2: Mandate OperationCompletion Event", and this firmware emits
 * neither OperationCompletion nor OperationalError, on either platform: the
 * host owns appliance state transitions and reports them with AT+MTOPSTATE,
 * which writes the attribute and reports it without generating the cluster's
 * events. The dynamic endpoint's eventList is empty regardless, so EventList
 * reads empty rather than advertising an event that never fires. The
 * advertised device-type revision stays 2 because it matches the XML this
 * file has taken as its authority since batch 1 AND what the C6 advertises
 * (ESP_MATTER_MICROWAVE_OVEN_DEVICE_TYPE_VERSION 2,
 * esp_matter_endpoint.h:90), so dropping to 1 would invent a divergence
 * rather than fix one. The same gap exists unremarked on the washer trio and
 * the RVC; it is named here because here the revision number points straight
 * at it.
 *
 * Revision 2 per data_model/1.5/device_types/MicrowaveOven.xml. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(microwaveOpStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::PhaseList::Id, ARRAY, 0,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::CurrentPhase::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::CountdownTime::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalStateList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalError::Id, STRUCT, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(mwocAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::CookTime::Id, INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::MaxCookTime::Id, INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::PowerSetting::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kMwocIncoming[] = {
    MicrowaveOvenControl::Commands::SetCookingParameters::Id,
    MicrowaveOvenControl::Commands::AddMoreTime::Id, kInvalidCommandId
};

/* kOpStateIncoming keeps all four commands here, and Start is load-bearing
 * beyond its own invoke: SetCookingParameters with a startAfterSetting field
 * asks the data model provider whether OperationalState's Start command is
 * accepted on this endpoint, and answers InvalidCommand if it is not
 * (microwave-oven-control-server.cpp:249-273). Dropping Start from this list
 * as "the host starts it anyway" would break a legal cooking command. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(microwaveOvenClusters)
DECLARE_DYNAMIC_CLUSTER(OperationalState::Id, microwaveOpStateAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kOpStateIncoming, kOpStateOutgoing),
    /* nullptr, NOT kModeBaseIncoming: this alias has no ChangeToMode at all
     * (the section comment's trap). */
    DECLARE_DYNAMIC_CLUSTER(MicrowaveOvenMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(MicrowaveOvenControl::Id, mwocAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kMwocIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(microwaveOvenEndpoint, microwaveOvenClusters);

constexpr EmberAfDeviceType kMicrowaveOvenTypes[] = { { 0x0079, 2 } };
