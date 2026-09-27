/*
 * mt_devtypes_sl_tables_b5.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 5a (nRF 671-698, 722-743, 1101-1123,
 * 2008-2281): the three registry rows that reuse an endpoint (their type
 * arrays only) and the generic switch, pump and room air conditioner; and
 * catalogue batch 5b (nRF 2283-2435): the robotic vacuum cleaner.
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, after mt_devtypes_sl_tables_b4.inc
 * and before mt_devtypes_sl_registry.inc, so the whole is still one
 * translation unit. Do not add it to hearth.slcp.
 */

/* ---- mounted on/off control (0x010F) ----------------------------------
 *
 * Catalogue batch 5, a registry row reusing &onOffPlugInUnitEndpoint. The
 * C6's mounted_on_off_control::add() (esp_matter_endpoint.cpp:1794-1811)
 * creates Identify with TriggerEffect, Groups, OnOff with the Lighting
 * feature plus On/Toggle (Off from on_off::create() itself), and
 * ScenesManagement with CopyScene: the on/off plug-in unit's OnOff shape
 * exactly (the device library's classification row even marks 0x010F a
 * superset of 0x010A). On this port that collapses to the same three
 * clusters the plug declares, and every OnOff seed (FeatureMap 0x01
 * Lighting, StartUpOnOff null, revision 6) is already in s_seeds.
 *
 * Groups and ScenesManagement are mandatoryConform for this device type in
 * the XML, and are deliberately NOT added: the standing convention argued
 * on onOffPlugInUnitClusters above (this catalogue serves attribute-only
 * clusters this build declares, and Groups/Scenes were never added for the
 * lights or plugs either) covers this type identically.
 *
 * One row, one device type, per the port's one-devtype-per-row convention:
 * the superset relation could justify appending { 0x010F } to the plug's
 * own EmberAfDeviceType span instead, but no row in this registry declares
 * two device types and this batch does not start.
 *
 * Revision 2 per data_model/1.5/device_types/MountedOnOffControl.xml (the
 * device-revision authority the temperature sensor fix pinned; the
 * zap-templates matter-devices.xml still says 1 for this type and is stale
 * against the 1.5 snapshot). */
constexpr EmberAfDeviceType kMountedOnOffControlTypes[] = { { 0x010F, 2 } };

/* ---- mounted dimmable load control (0x0110) ---------------------------
 *
 * Catalogue batch 5, a registry row reusing &dimmablePlugInUnitEndpoint:
 * the C6's mounted_dimmable_load_control::add() (esp_matter_endpoint.cpp:
 * 1831-1851) composes the dimmable plug's exact OnOff (Lighting) plus
 * LevelControl (OnOff|Lighting) set, with the same Groups/ScenesManagement
 * omission argued on the mounted on/off control above. The device
 * library's element requirements (Device-Library-Specification.md:
 * 2080-2087) mandate OnOff/LT, LevelControl/OO, LevelControl/LT and the
 * CurrentLevel 1..254 / MinLevel 1 / MaxLevel 254 bounds, all of which the
 * dimmable light's existing seeds already provide.
 *
 * B388 check, verified rather than assumed: the LevelControl ServerInit
 * call site in mt_devtype_create() below keys on the CLUSTER list (it
 * walks type->ep_type->cluster[] for LevelControl::Id), not on the device
 * type id, so this row's endpoint gets its Min/MaxLevel cache initialised
 * exactly as 0x0101/0x010B do and nothing new joins the call site.
 *
 * Revision 2 per data_model/1.5/device_types/MountedDimmableLoadControl.xml
 * (matter-devices.xml's 1 is stale against the snapshot, same note as the
 * mounted on/off control). */
constexpr EmberAfDeviceType kMountedDimmableLoadControlTypes[] = { { 0x0110, 2 } };

/* ---- air purifier (0x002D) --------------------------------------------
 *
 * Catalogue batch 5. A registry row, not a new endpoint type: the C6's
 * air_purifier::add() creates Identify and FanControl and nothing else
 * (esp_matter_endpoint.cpp:745-754), which is byte-identical to the fan
 * 0x002B's composition above, so &fanEndpoint is reused verbatim and only
 * the EmberAfDeviceType differs. The device library lists Groups, OnOff
 * and the two filter-monitoring clusters (HepaFilterMonitoring 0x0071,
 * ActivatedCarbonFilterMonitoring 0x0072) as optional on this device type
 * (Device-Library-Specification.md:3936-3939) and the C6 takes none of
 * them, so resource-monitoring is deliberately NOT composed here either.
 *
 * The fan's known FanMode <-> PercentSetting coupling gap (the audit note
 * on fanControlAttrs above: MatterFanControlClusterServerAttributeChanged
 * Callback is functions-array-bound and never runs on a dynamic endpoint)
 * now ships under a SECOND device-type name. Restated deliberately, not
 * silently inherited: an air purifier host owns that coupling exactly the
 * way a fan host does, and nothing about this row re-solves it.
 *
 * Revision 2 per data_model/1.5/device_types/AirPurifier.xml (the same
 * source the temperature sensor's fix-round correction pinned as this
 * file's authority for device revisions). */
constexpr EmberAfDeviceType kAirPurifierTypes[] = { { 0x002D, 2 } };

/* ---- generic switch (0x000F) ------------------------------------------
 *
 * Catalogue batch 5 audit, Switch (0x003B). The port's first EVENT
 * EMITTER: this device type exists to raise Switch events at controllers,
 * not to serve writable state.
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; regenerating hearth.zap with the cluster on ep240 left
 *   CodeDrivenInitShutdown.cpp byte-identical, the batch 3 method). Not
 *   CommandHandlerInterface-only either (config-data.yaml:21-88), which is
 *   harmless: switch-cluster.xml declares NO commands at all (:83-151 is
 *   attributes and events only), so no generated dispatch case exists or
 *   is needed, and the same regeneration left IMClusterCommandHandler.cpp
 *   byte-identical too.
 *
 *   Delegate or plain ember? Plain ember plus a STATELESS singleton.
 *   SwitchServer is `static SwitchServer instance` (switch-server.h:82,
 *   defined switch-server.cpp:41) with no data members and no per-endpoint
 *   state: all seven On* methods take an EndpointId and do nothing but
 *   build the event struct and LogEvent() it. No delegate, no AAI, no
 *   Init(), no pool, nothing to hand out at create time. The declared
 *   value attributes are ordinary ember external storage against this
 *   arena.
 *
 *   ServerInit? None. MatterSwitchPluginServerInitCallback and its
 *   Shutdown twin are empty bodies (switch-server.cpp:154-155); on this
 *   tree the regeneration supplies an empty weak
 *   emberAfSwitchClusterInitCallback (callback-stub.cpp), so there is
 *   still nothing to hook, and the XML says
 *   <server init="false" tick="false"> (switch-cluster.xml:31). Does NOT
 *   join the B388 call site.
 *
 *   Event emission, the load-bearing part. The server never self-emits:
 *   mt_matter_switch_click() (the bridge, in mt_matter_sl_b5.inc on this
 *   port) calls SwitchServer::Instance().OnInitialPress(ep, 1), whose
 *   whole body is
 *   LogEvent(event, endpoint, eventNumber) (switch-server.cpp:66-78). On a
 *   dynamic endpoint DECLARE_DYNAMIC_CLUSTER leaves eventList/eventCount
 *   zero (attribute-storage.h:48-51 supplies only eight initializers), so
 *   the EventList global attribute reads EMPTY on this endpoint; that does
 *   NOT block emission, which never consults it. The event's read
 *   privilege resolves through CodegenDataModelProvider::EventInfo(),
 *   which falls back to RequiredPrivilege::ForReadEvent(path) when no
 *   registered cluster object claims the path
 *   (CodegenDataModelProvider.cpp:249-257); this build's generated
 *   access.h carries event privilege overrides only for Access Control
 *   (access.h:284-300, byte-identical through this batch's regeneration),
 *   so InitialPress defaults to kView and no access.h change is needed.
 *   InitialPress is priority="info" (switch-cluster.xml:98) and lands in
 *   the info event buffer the image already allocates. Argued on the
 *   source end to end; the bench proof (a subscribed controller receiving
 *   InitialPress from a dynamic endpoint) is the controller's, recorded in
 *   the batch report.
 *
 * FeatureMap 0x02 is MomentarySwitch (Switch/Enums.h:35): the composed
 * appendix mandates MS for this device type
 * (Device-Library-Specification.md:8075), it matches the C6 thunk's
 * explicit choice (mk_generic_switch(), mt_devtypes.cpp:257-270, where a
 * feature_flags of 0 would assert in esp-matter's VALIDATE_FEATURES_
 * EXACT_ONE), and it is what makes the InitialPress event conformant.
 * MultiPressMax is MSM-gated (switch-cluster.xml:85-89) and not declared;
 * the richer momentary features (MSR, MSL, MSM) are not advertised and
 * their five events are never emitted on either platform.
 *
 * The writability split, stated precisely (batch 5 fix round, review
 * I2, which caught this note over-claiming "read-only over AT"):
 * NumberOfPositions and CurrentPosition carry no WRITABLE mask below, so
 * a CONTROLLER'S IM write is refused; that is the read-only the device
 * library means and what AT_MT_SPEC.md 462-467 describes. A LOCAL
 * AT+MTATTR write, however, reaches ember and lands in the arena: this
 * port's generic write bridge deliberately has no IsWritable() gate (the
 * argued delta at mt_matter_zephyr.cpp:779 and :805-822, where a gate
 * would break the bench-mandated MeasuredValue write), and that is C6
 * parity, not a gap: esp-matter's set_val() skips its WRITABLE-less
 * refusal for a plain, not-managed-internally attribute, which both
 * create_number_of_positions() and create_current_position() produce. So
 * "no writable attribute" here means no attribute a host has any REASON
 * to write (the type is an event emitter; a host that pokes
 * CurrentPosition is talking to itself) and none a controller CAN write.
 * CurrentPosition is NOT updated by the event path on either platform
 * (SwitchServer::OnInitialPress only calls LogEvent). The command that
 * drives the type is AT+MTSWITCH. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(switchAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Switch::Attributes::NumberOfPositions::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Switch::Attributes::CurrentPosition::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Switch::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(genericSwitchClusters)
DECLARE_DYNAMIC_CLUSTER(Switch::Id, switchAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(genericSwitchEndpoint, genericSwitchClusters);

/* Revision 3 per data_model/1.5/device_types/GenericSwitch.xml. */
constexpr EmberAfDeviceType kGenericSwitchTypes[] = { { 0x000F, 3 } };

/* ---- the featureless OnOff list (pump 0x0303, room air conditioner
 * 0x0072) ---------------------------------------------------------------
 *
 * Catalogue batch 5. A THREE-slot OnOff list for device types whose OnOff
 * cluster carries no Lighting feature: OnOff, FeatureMap and the implicit
 * ClusterRevision, nothing else. onOffAttrs above must NEVER be reused for
 * these types, because it declares GlobalSceneControl, OnTime, OffWaitTime
 * and StartUpOnOff, and all four are mandatoryConform ON FEATURE LT in the
 * cluster XML (onoff-cluster.xml:92-112): declaring them on an endpoint
 * whose FeatureMap is 0 (pump) or 0x02 (RAC's DeadFrontBehavior) is a
 * conformance break no build check would catch. The C6 composes the same
 * three-attribute shape for both types (pump::add() and
 * room_air_conditioner::add() call on_off::create() with no lighting::add;
 * the RAC adds dead_front_behavior::add(), which sets the feature bit and
 * creates nothing, esp_matter_feature.cpp:491-505).
 *
 * The two types need DIFFERENT FeatureMap seeds on the same cluster (pump
 * 0x00, RAC 0x02 DeadFrontBehavior, the latter mandatory per the RAC's
 * element requirements, Device-Library-Specification.md:5137), which is
 * exactly what s_seeds' per-device-type qualifier exists for: two rows
 * naming 0x0303 and 0x0072 win over the wildcard OnOff FeatureMap row
 * (0x01, the Lighting types'). Named for the RAC because that type is the
 * reason the list exists; the pump lands first in the build order and
 * shares it. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(racOnOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* ---- pump (0x0303) ----------------------------------------------------
 *
 * Catalogue batch 5 audit, PumpConfigurationAndControl (0x0200).
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; regenerating hearth.zap with the cluster on ep240 left
 *   CodeDrivenInitShutdown.cpp byte-identical). Not CHI-only either
 *   (config-data.yaml:21-88), harmlessly: the cluster declares no commands
 *   at all (pump-configuration-and-control-cluster.xml is attributes and
 *   events only), and the regeneration left IMClusterCommandHandler.cpp
 *   byte-identical too. One generated file did move, against the batch
 *   audit's prediction: access.h gained the OperationMode write-privilege
 *   row (the XML gives it access op="write" privilege="manage"), joining
 *   the DoorLock/WindowCovering/Thermostat manage-write entries already
 *   there. That is the generated access module doing its job for a
 *   controller write; nothing on this port consults it locally.
 *
 *   Delegate or plain ember? Plain ember, no server object of any kind:
 *   pump-configuration-and-control-server.cpp registers no AAI and defines
 *   no class; every declared attribute is external storage against this
 *   arena.
 *
 *   ServerInit? The cluster sits in ClustersWithInitFunctions,
 *   ClustersWithAttributeChangedFunctions AND
 *   ClustersWithPreAttributeChangeFunctions (config-data.yaml:100, :111,
 *   :136), all three reached only through the per-cluster functions array
 *   DECLARE_DYNAMIC_CLUSTER nulls, so none of them runs here. Its
 *   emberAf...ServerInitCallback is a real strong body whose entire
 *   content is one ChipLogProgress line (pump-configuration-and-control-
 *   server.cpp:248-251): it does NOT join the B388 call site, whose bar is
 *   "per-endpoint boot state the endpoint would otherwise never get", and
 *   running a log line buys none. Do not add it later either: the
 *   attribute-changed callback's setEffectiveModes() reads
 *   emberAfLocateAttributeMetadata(...)->defaultValue unconditionally when
 *   ControlMode is absent (:82-86), which on a dynamic endpoint is
 *   ZAP_EMPTY_DEFAULT garbage; unreachable today only because the
 *   functions array is null.
 *
 *   Two never-run callbacks, documented as HOST responsibilities rather
 *   than rediscovered on a bench:
 *     - MatterPumpConfigurationAndControlClusterServerAttributeChanged
 *       Callback (:355-369) derives EffectiveOperationMode/
 *       EffectiveControlMode from OperationMode/ControlMode. It never runs
 *       on a dynamic endpoint, so a write of OperationMode (host AT+MTATTR
 *       or controller IM) does NOT update EffectiveOperationMode by
 *       itself; the host owns that coupling, the FanControl
 *       FanMode/PercentSetting precedent above.
 *     - The PreAttributeChanged guard (:253-353) answers ConstraintError
 *       for a ControlMode value whose feature is absent. ControlMode is
 *       not composed here (optional, C6 parity), so a controller cannot
 *       change the control mode at all and the guard is moot twice over.
 *
 * Feature ConstantSpeed (0x8, PumpConfigurationAndControl/Enums.h:67), the
 * C6 thunk's documented choice (mk_pump(), mt_devtypes.cpp:467-478:
 * esp-matter VALIDATEs at least one operation-mode feature and constant
 * speed is the least constrained), which is what makes MinConstSpeed and
 * MaxConstSpeed mandatory and everything else feature-gated-absent.
 * ControlModeEnum::kConstantSpeed and OperationModeEnum::kNormal are both
 * 0x00 (Enums.h:34, :50), so the three zero-fill mode seeds are legal
 * under this feature map. Eleven slots; the optional PumpStatus and
 * ControlMode are not created, matching the C6. Eight events are declared
 * in the XML, all optional, none emitted by either platform. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(pumpAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxPressure::Id, INT16S, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxSpeed::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxFlow::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MinConstSpeed::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxConstSpeed::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::EffectiveOperationMode::Id,
                              ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::EffectiveControlMode::Id,
                              ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::Capacity::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::OperationMode::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(pumpClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(PumpConfigurationAndControl::Id, pumpAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(pumpEndpoint, pumpClusters);

/* Revision 3 per data_model/1.5/device_types/Pump.xml. */
constexpr EmberAfDeviceType kPumpTypes[] = { { 0x0303, 3 } };

/* ---- room air conditioner (0x0072) ------------------------------------
 *
 * Catalogue batch 5. No new cluster at all: a new CLUSTER LIST combining
 * the featureless OnOff (racOnOffAttrs above, FeatureMap seeded 0x02
 * DeadFrontBehavior per the element requirements,
 * Device-Library-Specification.md:5137) with the existing Thermostat,
 * Identify and Descriptor. The C6's room_air_conditioner::add()
 * (esp_matter_endpoint.cpp:1274-1288) creates Identify, OnOff with
 * dead_front_behavior plus On/Toggle, and Thermostat after an
 * unconditional `|= cooling` on the feature flags; its thunk
 * (mk_room_air_conditioner(), mt_devtypes.cpp:429-465) ORs Heating in as
 * well, because the device library does not restrict the Thermostat to
 * Cool-only and the host library writes OccupiedHeatingSetpoint
 * unconditionally. The optional Groups/ScenesManagement/HEPA/
 * ActivatedCarbon/FanControl are composed on neither platform
 * (Device-Library-Specification.md:5093-5104; the C6 takes no optionals,
 * so resource-monitoring stays out of this batch entirely).
 *
 * Every Thermostat seed row above is already correct for this device
 * type, verified rather than assumed: FeatureMap 0x03 Heating|Cooling,
 * ControlSequenceOfOperation 4 kCoolingAndHeating, SystemMode 0 Off (the
 * first-write-swallowed cache reasoning on thermostatAttrs), setpoints
 * 1600/2400 (the C6's cross-layer I1 values) and the 700/3000/1600/3200
 * limits. So this type adds NO Thermostat rows and no thermostat code;
 * only the OnOff FeatureMap row keyed 0x0072 (beside the pump's in
 * s_seeds) is its own.
 *
 * Reusing onOffAttrs here instead of racOnOffAttrs would declare the four
 * Lighting-gated attributes on a FeatureMap 0x02 cluster, the conformance
 * break the racOnOffAttrs comment names; that is the one trap on this
 * type. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(roomAirConditionerClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(roomAirConditionerEndpoint, roomAirConditionerClusters);

/* Revision 3 per data_model/1.5/device_types/RoomAirConditioner.xml. */
constexpr EmberAfDeviceType kRoomAirConditionerTypes[] = { { 0x0072, 3 } };


/* ---- robotic vacuum cleaner (0x0074) ----------------------------------
 *
 * Catalogue batch 5's weight: RvcRunMode 0x0054 + RvcCleanMode 0x0055
 * (one server, mode-base-server) and RvcOperationalState 0x0061, plus
 * Identify and Descriptor. Five clusters, fourteen slots, and the two
 * host-fed ModeBase stores that make this the widest block in the
 * catalogue (see the sizing table below).
 *
 * The C6 composes the same five: robotic_vacuum_cleaner::add()
 * (esp_matter_endpoint.cpp:1389-1400) creates Identify, RvcRunMode and
 * RvcOperationalState (plus its OperationCompletion event), and the thunk
 * hand-adds RvcCleanMode after create() (mk_rvc(), mt_devtypes.cpp:
 * 846-852). RvcCleanMode is optional in the device library
 * (Device-Library-Specification.md:4780) and STAYS composed here, ruled
 * DE404: parity with the C6's deliberate hand-add beats the one design
 * lever that would have bought capacity (dropping it would cost a real
 * cross-platform data-model divergence to fit 16 endpoints instead of 9).
 *
 * ---- the two ModeBase clusters (RvcRunMode, RvcCleanMode) -------------
 *
 *   Code-driven? No for both (config-data.yaml:144-168; this batch's
 *   regeneration left CodeDrivenInitShutdown.cpp byte-identical). Both
 *   ARE CommandHandlerInterface-only ("RVC Clean Mode" config-data.yaml:
 *   67, "RVC Run Mode" :69): ChangeToMode reaches the per-(endpoint,
 *   cluster) ModeBase::Instance's own handler and IMClusterCommandHandler
 *   stayed byte-identical too.
 *
 *   Delegate or plain ember? ModeBase::Instance PLUS ModeBase::Delegate,
 *   one PAIR per (endpoint, cluster): both interfaces are scoped
 *   Optional<EndpointId>(aEndpointId) with the cluster id
 *   (mode-base-server.cpp:44-53), so the one RVC endpoint carries TWO
 *   pairs. The pools and the Init() ordering rules live in
 *   mt_matter_zephyr.cpp beside the delegate; the create-path handout is
 *   below in mt_devtype_create(). The ordering is this batch's sharpest
 *   constraint and is documented at both ends: Instance::Init()
 *   VerifyOrDies on emberAfContainsServer (mode-base-server.cpp:77), a
 *   PANIC where the opstate trio's same check soft-bails, so construction
 *   and Init() run strictly below a successful emberAfSetDynamicEndpoint().
 *
 *   Which attributes the Instance's AAI answers: CurrentMode, StartUpMode,
 *   OnMode, FeatureMap and SupportedModes (Instance::Read(),
 *   mode-base-server.cpp:325-347), and the switch has NO default arm, so
 *   ClusterRevision falls through to ember and its seeds are LIVE:
 *   RvcRunMode 4 and RvcCleanMode 5 per this tree's Metadata.h:20, per
 *   cluster in s_seeds. The CurrentMode and FeatureMap slots are inert
 *   shadows (the opstate split), carved out for AT+MTATTR by
 *   k_instance_served in mt_matter_zephyr.cpp. StartUpMode and OnMode are
 *   optional and deliberately not declared, the ModeSelect precedent.
 *
 *   Neither joins B388: MatterRvcRunMode/RvcCleanModePluginServerInit
 *   Callback are empty bodies (src/app/util/util.cpp:126-127), no
 *   emberAf...InitCallback exists, and the real init is Instance::Init(),
 *   the create path's job.
 *
 *   The one metadata list serves BOTH clusters: the ModeBase aliases share
 *   attribute ids (SupportedModes 0x0000, CurrentMode 0x0001) and command
 *   ids (ChangeToMode 0x00 / ChangeToModeResponse 0x01, verified against
 *   both clusters' generated CommandIds.h), so modeBaseAttrs and the two
 *   command lists below are declared once, the shared-metadata principle
 *   at the top of this file. The response command is declared in the
 *   outgoing list for the same two reasons the opstate trio's is
 *   (DE399 truthfulness AND, here, C6 parity: esp-matter's
 *   rvc_run_mode/rvc_clean_mode::create() add ChangeToModeResponse
 *   unconditionally).
 *
 * ---- RvcOperationalState ----------------------------------------------
 *
 *   Costs no new translation unit: zap_cluster_list.json maps
 *   OPERATIONAL_STATE_RVC_CLUSTER to operational-state-server, already in
 *   this build, and RvcOperationalState::Instance lives in the same
 *   operational-state-server.cpp (:520-570). CHI-only (config-data.yaml:
 *   68), no generated dispatch. The Instance derives from
 *   OperationalState::Instance, constructed Instance(Delegate*, ep)
 *   forwarding Id as the cluster id (operational-state-server.h:421-423);
 *   the delegate is a NEW subclass (HearthRvcOpStateDelegate,
 *   mt_matter_zephyr.cpp) because the base HearthOpStateDelegate has no
 *   GoHome hook, and it takes its own pool: the base
 *   Delegate::SetInstance() VerifyOrDies on sharing
 *   (operational-state-server.h:349-353), so handing an RVC Instance a
 *   pooled trio delegate would abort the device.
 *
 *   The incoming command list is EXACTLY {Pause 0x00, Resume 0x03, GoHome
 *   0x80}, and declaring Start/Stop would be a real defect, not just
 *   noise: Instance::InvokeCommand() switches on Pause/Resume/Start/Stop
 *   unconditionally before InvokeDerivedClusterCommand()
 *   (operational-state-server.cpp:296-334), so a declared Start would
 *   route a controller invoke into the base handler and out to the
 *   derived delegate's kUnknownEnumValue stub, where the RVC cluster XML
 *   declares no Start/Stop at all. The outgoing list carries
 *   OperationalCommandResponse 0x04, the DE399 discipline (kOpStateOutgoing
 *   reused: same numeric id on the derived cluster's CommandIds.h).
 *
 *   Server-side guards that run BEFORE the delegate, so the +MTCMD
 *   forwards only ever carry legitimate adjudication requests: Pause is
 *   additionally compatible with kSeekingCharger (:522-525), Resume with
 *   kCharging/kDocked (:527-531); GoHome from kCharging/kDocked answers
 *   kCommandInvalidInState WITHOUT calling the delegate (:556-559) and
 *   from kSeekingCharger answers success without calling it either
 *   (:561).
 *
 *   Attribute split: Instance::Read() (operational-state-server.cpp:
 *   335-408) serves everything except FeatureMap, which falls through to
 *   ember (live seed 0); ClusterRevision is answered by the AAI as the
 *   BASE cluster's kRevision constant (:408-409), so unlike the ModeBase pair
 *   the revision seed here is INERT; both constants are 1 in this tree
 *   (OperationalState/Metadata.h:20, RvcOperationalState/Metadata.h:20)
 *   and the seed is written 1 anyway for truthful arena metadata. The
 *   opStateAttrs list above is reused verbatim: the derived cluster
 *   shares the base's attribute ids and this composition declares the
 *   same subset (CountdownTime stays optional-absent on both platforms).
 *
 *   Events: OperationalError and OperationCompletion are declared in the
 *   XML (the former optional="false"); this firmware emits neither, the
 *   C6 emits neither, and the dynamic endpoint's empty eventList makes
 *   EventList read empty without blocking emission (the generic switch
 *   note above). The C6's rvc create() has its own conformance gap here
 *   (no OperationalError event metadata where the base cluster creates
 *   one); nothing for this port to mirror since dynamic eventList is
 *   empty regardless.
 *
 * Revision 4 per data_model/1.5/device_types/RoboticVacuumCleaner.xml. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(modeBaseAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(RvcRunMode::Attributes::SupportedModes::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RvcRunMode::Attributes::CurrentMode::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RvcRunMode::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kModeBaseIncoming[] = { RvcRunMode::Commands::ChangeToMode::Id,
                                            kInvalidCommandId };
constexpr CommandId kModeBaseOutgoing[] = { RvcRunMode::Commands::ChangeToModeResponse::Id,
                                            kInvalidCommandId };

constexpr CommandId kRvcOpStateIncoming[] = { RvcOperationalState::Commands::Pause::Id,
                                              RvcOperationalState::Commands::Resume::Id,
                                              RvcOperationalState::Commands::GoHome::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(rvcClusters)
DECLARE_DYNAMIC_CLUSTER(RvcRunMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming,
                        kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(RvcCleanMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(RvcOperationalState::Id, opStateAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kRvcOpStateIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(rvcEndpoint, rvcClusters);

constexpr EmberAfDeviceType kRvcTypes[] = { { 0x0074, 4 } };
