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
 * Catalogue batch 8. Until this batch mt_devtype_parent_ok() was a
 * `return true` stub, because no device type in s_registry required a parent
 * or restricted which device type could parent it. Two now do
 * (AT_MT_SPEC.md 3.9's policy), and this constexpr predicate is where the rule
 * lives; the extern "C" entry point the AT layer calls is a one-line
 * forwarder further down.
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
 * WHY NOT A WIDENED ROW FOR ALL 44 EXISTING ROWS. Ruled in the batch brief
 * (DE416). A Span<const shape> replacing ep_type/ep_type_v1 on every row is
 * the more general mechanism and remains the right refactor for the day a
 * SECOND parent-conditional device type appears; landing it in the same
 * commit as seven new device types, a heap resize, three new SDK translation
 * units and a zap regeneration would make the whole batch unreviewable. This
 * member is additive by construction: it is the LAST member, so aggregate
 * initialization zero-fills it to an empty span for every row that does not
 * name it, and all 44 pre-batch-8 rows keep their exact original text and
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
 * batch brief: compose WITHOUT Identify. The cost is four slots and 64 arena
 * bytes cheaper per endpoint, which is incidental.
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
 *   2356-2362 says reading all three and writing Mask or Supported works
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
