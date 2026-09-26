/*
 * mt_devtypes_sl_tables_b4.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 4 (nRF 1485-2006).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, after mt_devtypes_sl_tables_b3.inc
 * and before mt_devtypes_sl_registry.inc, so the whole is still one
 * translation unit: file-local names stay file-local and the generated
 * code is the same as when this text lived in the .cpp. Split out
 * 2026-09-26 so each file fits a small model's context. Do not add it
 * to hearth.slcp.
 */

/* ---- power source (0x0011) --------------------------------------------
 *
 * Catalogue batch 4 audit, PowerSource (0x002F). Registry row and seeds
 * only: there is no mt_matter_power* port function in core/include/
 * mt_matter.h at all, so the generic AT+MTATTR bridge
 * (mt_matter_attr_read/write, mt_matter_zephyr.cpp) is the whole host
 * surface for this type.
 *
 *   Code-driven? No. PowerSource is absent from CodeDrivenClusters
 *   (src/app/common/templates/config-data.yaml:144-168). Confirmed
 *   empirically, the batch 3 method: regenerating hearth.zap with the
 *   cluster on ep240 left zap-generated/CodeDrivenInitShutdown.cpp
 *   byte-identical.
 *
 *   AAI? Yes, wildcard-endpoint, for three attributes. PowerSourceAttrAccess
 *   (power-source-server.h:48, ctor at :52 with
 *   Optional<EndpointId>::Missing(), registered from
 *   MatterPowerSourcePluginServerInitCallback, power-source-server.cpp:
 *   88-91) answers ActiveBatFaults (:108, not declared here), EndpointList
 *   (:112, from PowerSourceServer's own table; empty list until something
 *   calls SetEndpointList, which nothing on this firmware does) and
 *   ClusterRevision (:131, from PowerSource::kRevision), and falls through
 *   default: for everything else. So Status, Order, Description and the
 *   three Bat* scalars are plain ember external storage against the arena,
 *   and the ClusterRevision seed below must be that same kRevision (3,
 *   PowerSource/Metadata.h:20), the Thermostat/WindowCovering discipline.
 *   The server's per-endpoint table is already dynamic-endpoint aware:
 *   sPowerSourceClusterInfo is sized MATTER_DM_POWER_SOURCE_CLUSTER_SERVER_
 *   ENDPOINT_COUNT + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT
 *   (power-source-server.cpp:75-81), which this file's static assert ties
 *   to kServiceableEndpoints.
 *
 *   ServerInit? None per endpoint. MatterPowerSourcePluginServerInitCallback
 *   only registers the AAI; no emberAfPowerSourceCluster*InitCallback
 *   exists. Does NOT join the B388 call site.
 *
 * NO Identify, a documented exception to this port's "Identify rides on
 * every device type" convention: the device type mandates Descriptor plus
 * PowerSource and nothing else (matter-devices.xml:98-115, MA-powersource,
 * lockOthers with exactly those two includes), and the C6
 * composes exactly PowerSource plus Descriptor (esp_matter_endpoint.cpp
 * power_source::add(), which creates no Identify). Adding Identify here
 * would be a gratuitous divergence from the C6's data model and would cost
 * four extra slots per endpoint.
 *
 * Feature set: the cluster VALIDATEs exactly one of Wired/Battery on the
 * C6 (esp-matter's VALIDATE_FEATURES_EXACT_ONE); Battery (0x2,
 * PowerSource/Enums.h:282-283) is taken, matching the C6's choice for
 * upstream's battery-powered smoke/CO alarm devices, and the three
 * BAT-gated attributes it makes mandatory (BatChargeLevel,
 * BatReplacementNeeded, BatReplaceability) are declared.
 *
 * Description (char_string, length 60, so 61 ember bytes) and EndpointList
 * (array) get no attribute slot: attr_gets_slot() refuses both, and
 * seed_slots() skips them quietly as deliberate metadata-only declarations.
 * They are declared anyway so AttributeList is truthful. EndpointList is
 * served by the AAI (empty list); Description is served by nothing on this
 * platform and reads UNSUPPORTED_ATTRIBUTE, pending a string-store
 * decision. Both sizes pass emberAfSetDynamicEndpoint's IO-buffer check
 * (ATTRIBUTE_LARGEST is 66, endpoint_config.h).
 *
 * BatPercentRemaining is NULLABLE with real 0..200 bounds, matching the C6
 * thunk's hand-add (platform/esp32c6/main/mt_devtypes.cpp mk_power_source(),
 * bounds copied there from battery_storage::add()). DECLARE_DYNAMIC_ATTRIBUTE
 * cannot express MIN_MAX, so the entry below is hand-rolled to the exact
 * shape ZAP generates for a bounded attribute: a MIN_MAX-flagged metadata
 * row whose default union points at an EmberAfAttributeMinMaxValue. ember's
 * bounds check runs on every write regardless of storage kind
 * (attribute-table.cpp:368-406), so an out-of-range AT+MTATTR write answers
 * +MTERR:1 here exactly as it does on the C6; the null sentinel stays
 * writable because "null value is always in-range for a nullable attribute"
 * (attribute-table.cpp:401-403). Seeded null. */

constexpr EmberAfAttributeMinMaxValue kBatPercentRemainingBounds = {
    (uint16_t)0xFF, /* default: null */
    (uint16_t)0x00, (uint16_t)0xC8 /* 0..200, half-percent units */
};

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(powerSourceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Status::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Order::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Description::Id, CHAR_STRING, 61, 0),
    /* Hand-rolled MIN_MAX entry; see the audit note above. The mask spells
     * out the two flags DECLARE_DYNAMIC_ATTRIBUTE always adds. */
    { &kBatPercentRemainingBounds, PowerSource::Attributes::BatPercentRemaining::Id, 1,
      ZAP_TYPE(INT8U),
      ZAP_ATTRIBUTE_MASK(MIN_MAX) | ZAP_ATTRIBUTE_MASK(NULLABLE) |
          ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE) | ZAP_ATTRIBUTE_MASK(READABLE) },
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargeLevel::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplacementNeeded::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplaceability::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::EndpointList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(powerSourceClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, powerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(powerSourceEndpoint, powerSourceClusters);

constexpr EmberAfDeviceType kPowerSourceTypes[] = { { 0x0011, 1 } };

/* ---- smoke/co alarm (0x0076) ------------------------------------------
 *
 * Catalogue batch 4 audit, SmokeCoAlarm (0x005C).
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; confirmed empirically, regenerating hearth.zap with the
 *   cluster on ep240 left CodeDrivenInitShutdown.cpp byte-identical). Also
 *   NOT CommandHandlerInterface-only (absent from
 *   CommandHandlerInterfaceOnlyClusters, config-data.yaml:21-88), so
 *   SelfTestRequest is dispatched by the generated IMClusterCommandHandler:
 *   the regeneration added exactly the SmokeCoAlarm dispatch case, which is
 *   why this cluster had to enter hearth.zap at all.
 *
 *   Delegate or plain ember? Plain ember with a SINGLETON server.
 *   SmokeCoAlarmServer is `static SmokeCoAlarmServer sInstance`
 *   (smoke-co-alarm-server.h:163), reached through Instance() (:36); every
 *   setter takes an EndpointId and reads or writes ember storage through
 *   the generated Accessors, so all nine declared attributes land in this
 *   arena. No per-endpoint object, no pool, no placement-new: the simplest
 *   server shape in the batch.
 *
 *   ServerInit? None. MatterSmokeCoAlarmPluginServerInitCallback and its
 *   Shutdown twin are empty bodies (smoke-co-alarm-server.cpp:514-515), the
 *   XML declares <server tick="false" init="false">
 *   (smoke-co-alarm-cluster.xml:32), and no emberAfSmokeCoAlarmCluster*
 *   InitCallback exists beyond the generated weak stub. Does NOT join the
 *   B388 call site.
 *
 *   The one LINK-MANDATORY symbol in the batch:
 *   emberAfPluginSmokeCoAlarmSelfTestRequestCommand(), declared at
 *   smoke-co-alarm-server.h:178 and called from smoke-co-alarm-server.cpp
 *   :112 (RequestSelfTest) and :450 (HandleRemoteSelfTestRequest), has NO
 *   weak default anywhere under src/; the only definitions in the tree are
 *   example apps'. Omitting it is a link error. Defined in
 *   mt_matter_zephyr.cpp with the other command hooks, notify-only.
 *
 * Both features, SmokeAlarm 0x1 and CoAlarm 0x2 (SmokeCoAlarm/Enums.h:
 * 115-119): AT+MTALARM's field table has no single-feature variant, the
 * same reason the C6 enables both, so both feature-gated state attributes
 * exist and FeatureMap is 3, which is also the XML's own declared default
 * (smoke-co-alarm-cluster.xml:37). Identify is mandatory on this device
 * type (matter-devices.xml:2522-2545, MA-smokecoalarm), so the port
 * convention holds here, in contrast to the power source above. The
 * device type's Power Source mandate is satisfied the C6's way: a flat
 * sibling 0x0011 endpoint the host declares alongside, no composition
 * tree.
 *
 * Nine attributes, every one a 1-byte enum or boolean plus the two
 * globals (smoke-co-alarm-cluster.xml:49-76), so all of them slot
 * cleanly. The optional DeviceMuted, InterconnectSmokeAlarm,
 * InterconnectCOAlarm, ContaminationState and SmokeSensitivityLevel are
 * NOT declared, matching the C6's composition exactly; an AT+MTALARM
 * write to one of those fields passes its range check and then fails
 * with a bare ERROR because the setter's underlying attribute write
 * fails, the identical behaviour on both platforms. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(smokeCoAlarmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::ExpressedState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::SmokeState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::COState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::BatteryAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::TestInProgress::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::HardwareFaultAlert::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::EndOfServiceAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kSmokeCoAlarmIncoming[] = { SmokeCoAlarm::Commands::SelfTestRequest::Id,
                                                kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(smokeCoAlarmClusters)
DECLARE_DYNAMIC_CLUSTER(SmokeCoAlarm::Id, smokeCoAlarmAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kSmokeCoAlarmIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(smokeCoAlarmEndpoint, smokeCoAlarmClusters);

constexpr EmberAfDeviceType kSmokeCoAlarmTypes[] = { { 0x0076, 1 } };
/* ---- laundry washer (0x0073), dishwasher (0x0075), laundry dryer (0x007C)
 *
 * Catalogue batch 4 audit, OperationalState (0x0060). The three appliance
 * types compose identically (OperationalState + Identify + Descriptor;
 * the C6's three config_t are literally one aliased type and its three
 * add() bodies differ only in the device type id), so one cluster list
 * and one EmberAfEndpointType serve all three, the
 * booleanStateSensorClusters principle; only the EmberAfDeviceType (id,
 * revision) differs per registry row. No mode cluster, no controls
 * cluster, no Temperature Control: the device library makes Operational
 * State the only mandatory application cluster on all three, and the C6
 * composes the same.
 *
 * Identify IS composed here although the device XML lists it server="false"
 * on all three types (matter-devices.xml:2718 washer, :2609 dishwasher,
 * :2749 dryer) and the C6 composes only OperationalState plus Descriptor.
 * Optional-not-forbidden: server="false" under lockOthers means "not
 * required as a server", not "locked out" (serverLocked="false" on the same
 * lines), so adding it is legal, and the port's standing convention
 * (Identify rides on every device type; the batch 4 brief directed it for
 * this trio) wins over C6 parity here. The chime below makes the OPPOSITE
 * call on the same XML shape because there the C6 also omits it and no
 * brief direction says otherwise; the divergence is priced in the heap
 * table and recorded in the batch report.
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; regenerating hearth.zap with the cluster on ep240 left
 *   CodeDrivenInitShutdown.cpp byte-identical). It IS
 *   CommandHandlerInterface-only (config-data.yaml:63), so no generated
 *   IMClusterCommandHandler dispatch exists or is needed; the same
 *   regeneration left IMClusterCommandHandler.cpp byte-identical too, and
 *   invokes reach the Instance's own CommandHandlerInterface.
 *
 *   Delegate or plain ember? Instance PLUS Delegate, one PAIR per
 *   endpoint, the batch's genuinely new machinery. Instance derives from
 *   both CommandHandlerInterface and AttributeAccessInterface, each scoped
 *   MakeOptional(aEndpointId), and its constructor calls
 *   mDelegate->SetInstance(this) (operational-state-server.cpp:43-52);
 *   Delegate::SetInstance() VerifyOrDies if a second Instance tries to
 *   share one delegate object (operational-state-server.h:349-355), so the
 *   pools in mt_matter_zephyr.cpp are strictly one delegate and one
 *   Instance per endpoint, kServiceableEndpoints deep. All six cluster
 *   attributes are AAI-served, never ember (the Read() switch from
 *   operational-state-server.cpp:336: OperationalStateList :341,
 *   OperationalState :360, OperationalError :365, PhaseList :370,
 *   CurrentPhase :398, ClusterRevision :408 from Metadata kRevision).
 *   They are declared below anyway so AttributeList is truthful; the two
 *   scalars get inert slots, the arrays and the struct get none
 *   (attr_gets_slot refuses ARRAY and STRUCT; the declared ARRAY/STRUCT
 *   sizes are 0, mirroring what ZAP generates for External-storage list
 *   attributes on the static endpoint).
 *
 *   Split to know on the bench: an AT+MTATTR read goes through the classic
 *   ember path (emberAfReadAttribute) and answers this ARENA, not the
 *   Instance, so OperationalState over AT reads the inert seed (0,
 *   Stopped) no matter what the Instance last published; the fabric sees
 *   the Instance. The host's state authority is its own AT+MTOPSTATE round
 *   trip, and Instance-owned attributes never raise +MTATTR URCs, the
 *   same rule the C6 documents for every Instance-served cluster.
 *
 *   ServerInit? None by name: no emberAfOperationalStateClusterServerInit
 *   Callback exists, and MatterOperationalStatePluginServerInitCallback is
 *   an empty definition in src/app/util/util.cpp:131. The REAL init is
 *   Instance::Init(), the application's job, and its first act is
 *   `if (!emberAfContainsServer(mEndpointId, mClusterId)) return
 *   CHIP_ERROR_INVALID_ARGUMENT` (operational-state-server.cpp:65-70),
 *   which forces construct-and-Init() to run only AFTER
 *   emberAfSetDynamicEndpoint() succeeds: the same forced ordering as the
 *   valve's SetDefaultDelegate(), one mechanism further in. It does NOT
 *   join the B388 call site (that site substitutes for the ember
 *   INIT_FUNCTION slot; this is not that mechanism); construction happens
 *   in mt_matter_opstate_delegate_set_endpoint(), the delegate handout's
 *   second half in mt_devtype_create() below.
 *
 *   Never destroyed, matching the valve pool and the allocate-only block
 *   heap: ~Instance() would unregister both interfaces
 *   (operational-state-server.cpp:55-63), but no teardown path exists on
 *   this platform (the composition is edited by AT+MTEP and applied by a
 *   reboot, which resets every pool wholesale, the same policy the valve
 *   delegate pool set), so no explicit destructor call is ever needed or
 *   made.
 *
 * Commands are hand-declared: the Instance registers NO
 * EnumerateAcceptedCommands override, so AcceptedCommandList comes from
 * this ember metadata, and operational_state::create() on the C6 famously
 * created no command entries at all (finding F-C10-1, every invoke
 * answered 0x81 until the thunk hand-added them). Pause 0x00, Stop 0x01,
 * Start 0x02, Resume 0x03; the OperationalCommandResponse is the
 * server-to-client response, so it lives in the OUTGOING list (fix round
 * I1/DE399, see kOpStateOutgoing below), never the incoming one.
 *
 * The delegate publishes exactly Stopped/Running/Paused/Error in
 * OperationalStateList and answers CHIP_ERROR_NOT_FOUND at phase index 0
 * so PhaseList reads null; the verdict mapping (allow kNoError, deny
 * kUnableToCompleteOperation, and why the delegate never calls
 * SetOperationalState on allow) is on HearthOpStateDelegate in
 * mt_matter_zephyr.cpp. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(opStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::PhaseList::Id, ARRAY, 0,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::CurrentPhase::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalStateList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalError::Id, STRUCT, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kOpStateIncoming[] = { OperationalState::Commands::Pause::Id,
                                           OperationalState::Commands::Stop::Id,
                                           OperationalState::Commands::Start::Id,
                                           OperationalState::Commands::Resume::Id,
                                           kInvalidCommandId };

/* Fix round, review I1, RULED (graph DE399): the outgoing list is declared,
 * NOT left nullptr like every other cluster in this file, because this is
 * the port's first cluster that answers its commands with a RESPONSE
 * COMMAND rather than a plain status: every one of the four handlers
 * builds an OperationalCommandResponse and AddResponse()s it
 * (operational-state-server.cpp:443-446 for Pause, and identically in the
 * Stop/Start/Resume handlers), and the cluster spec requires that command
 * in GeneratedCommandList when its triggers are supported. Introduced as a
 * knowing divergence from the C6, whose identical gap was tracked as bug
 * B400 (fabric metadata truthfulness beats parity of an untruth, and
 * GeneratedCommandList is not part of the AT contract, so no host-visible
 * behaviour moves). The C6 closed B400 in the parity round of 2026-09-01
 * by adding create_operational_command_response() to its own
 * mt_opstate_add_commands(), so the two ports agree again. */
constexpr CommandId kOpStateOutgoing[] = {
    OperationalState::Commands::OperationalCommandResponse::Id, kInvalidCommandId
};

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(applianceOpStateClusters)
DECLARE_DYNAMIC_CLUSTER(OperationalState::Id, opStateAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kOpStateIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(applianceOpStateEndpoint, applianceOpStateClusters);

constexpr EmberAfDeviceType kLaundryWasherTypes[] = { { 0x0073, 2 } };
constexpr EmberAfDeviceType kDishwasherTypes[] = { { 0x0075, 2 } };
constexpr EmberAfDeviceType kLaundryDryerTypes[] = { { 0x007C, 2 } };
/* ---- mode select (0x0027) ---------------------------------------------
 *
 * Catalogue batch 4 audit, ModeSelect (0x0050).
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; the regeneration left CodeDrivenInitShutdown.cpp
 *   byte-identical). NOT CommandHandlerInterface-only either, so
 *   ChangeToMode needs the generated IMClusterCommandHandler dispatch,
 *   which the regeneration added, exactly one case; that dispatch is why
 *   the cluster had to enter hearth.zap.
 *
 *   Delegate or plain ember? Neither exactly. Description,
 *   StandardNamespace and CurrentMode are plain ember against this arena;
 *   SupportedModes is served by gModeSelectAttrAccess, a wildcard AAI
 *   registered once from MatterModeSelectPluginServerInitCallback
 *   (mode-select-server.cpp:416-419), which reads a single PROCESS-GLOBAL
 *   SupportedModesManager fetched fresh on every access
 *   (getSupportedModesManager(), :57; the setter at :62 stores a bare
 *   global pointer). One manager for the whole device, not per endpoint:
 *   the manager itself dispatches on endpoint id, so a second mode select
 *   endpoint re-registering it is harmless, and the registration lives in
 *   mt_matter_mode_select_manager() (mt_matter_zephyr.cpp), called from
 *   mt_devtype_create() before any resource is spent.
 *
 *   ServerInit? YES, a real strong body, and this cluster JOINS the B388
 *   by-hand call site. emberAfModeSelectClusterServerInitCallback
 *   (mode-select-server.cpp:318-398) is reachable only through the
 *   per-cluster functions array (Mode Select sits in
 *   ClustersWithInitFunctions, config-data.yaml:97; the XML itself says
 *   init="false", mode-select-cluster.xml:44), and DECLARE_DYNAMIC_CLUSTER
 *   nulls that array, the exact B388 mechanism. Called by hand below next
 *   to LevelControl/ColorControl. What it does HERE, traced in this tree
 *   rather than assumed: its volatility gate passes on this endpoint
 *   (emberAfIsKnownVolatileAttribute, util.cpp:156-166, answers false both
 *   for our EXTERNAL-storage CurrentMode, IsExternal, and for the
 *   undeclared StartUpMode, metadata null), and the body then skips at
 *   Attributes::StartUpMode::Get() answering UnsupportedAttribute
 *   (:334), StartUpMode not being declared. So the init is a no-op on
 *   this composition today; it is called anyway because the moment a
 *   later round declares StartUpMode, a dynamic endpoint that never ran
 *   it boots with the wrong CurrentMode, which is exactly bug B388's
 *   class. Bench-verify item: confirm no CurrentMode write and no error
 *   log at rebuild time.
 *
 * Identify is composed per convention AND per mandate: unlike the trio
 * above, this device type requires the Identify server outright
 * (matter-devices.xml:2470, MA-modeselect).
 *
 * Description (char_string 64, so 65 ember bytes) gets no slot and reads
 * UNSUPPORTED_ATTRIBUTE on this platform, pending a string-store
 * decision; declared so AttributeList is truthful, same note as the
 * power source's Description. StartUpMode and OnMode are optional and
 * deliberately not declared (no DEPONOFF feature, no OnOff cluster on
 * this endpoint), which also parks the cluster's
 * PreAttributeChanged mode-membership guard: it only checks those two
 * attributes (mode-select-server.cpp:434-454, dispatching to
 * verifyModeValue at :462), is function-array-bound
 * anyway, and CurrentMode is not writable over the fabric. A
 * controller's ChangeToMode validates membership against the manager and
 * writes CurrentMode itself (emberAfModeSelectClusterChangeToModeCallback,
 * :284-311), which lands in this arena and reaches the host as an
 * ordinary +MTATTR URC, the C6's documented contract for this type. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(modeSelectAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::Description::Id, CHAR_STRING, 65, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::StandardNamespace::Id, ENUM16, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::SupportedModes::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::CurrentMode::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kModeSelectIncoming[] = { ModeSelect::Commands::ChangeToMode::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(modeSelectClusters)
DECLARE_DYNAMIC_CLUSTER(ModeSelect::Id, modeSelectAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kModeSelectIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(modeSelectEndpoint, modeSelectClusters);

constexpr EmberAfDeviceType kModeSelectTypes[] = { { 0x0027, 1 } };
