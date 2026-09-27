/*
 * mt_devtypes_sl_tables_b7.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 7a-1 (nRF 2437-2723): the
 * measurement family, the electrical sensor (0x0510) and electrical meter
 * (0x0514) with their power-only variants, and the heat pump (0x0309) and
 * solar power (0x0017) on the wired electrical sensor lists; and
 * catalogue batch 7a-2 (nRF 2725-2954): the electrical utility meter
 * (0x0511) and device energy management (0x050D) with its report-only
 * variant.
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, after mt_devtypes_sl_tables_b5.inc
 * and before mt_devtypes_sl_registry.inc, so the whole is still one
 * translation unit. Do not add it to hearth.slcp.
 */

/* ---- electrical sensor (0x0510) and electrical meter (0x0514) ---------
 *
 * Catalogue batch 7a audit (the nRF's batch7-audit.md sections 2.2 and
 * 3.1/3.2; the SDK citations below are the pinned NCS tree's).
 * The two measurement-push device types behind AT+MTMEAS, and the port's
 * first variant-carrying registry rows: variant 0 is power + energy,
 * variant 1 power only (the current-clamp case). On the C6 the variant-1
 * electrical meter DESTROYS the EEM cluster its SDK add() forced
 * (mt_devtypes.cpp:1512-1526, the B216 destroy-after-create precedent);
 * this port has no cluster::destroy() analogue, so each variant is its own
 * declared cluster list and the registry row carries both (ep_type_v1).
 *
 * ElectricalPowerMeasurement (0x0090), the batch's three-answers shape:
 *
 *   Code-driven? No (absent from config-data.yaml's CodeDrivenClusters
 *   :144-168; confirmed empirically, the batch 3 method: regenerating
 *   hearth.zap with all six batch 7a clusters left CodeDrivenInitShutdown
 *   .cpp byte-identical, sha256-checked).
 *
 *   CHI-only? Listed yes (config-data.yaml:46), harmlessly: the cluster
 *   declares no commands, so IMClusterCommandHandler.cpp stayed
 *   byte-identical too.
 *
 *   AAI? YES, per endpoint: ElectricalPowerMeasurement::Instance's Read()
 *   has a case for EVERY attribute the cluster defines including all the
 *   int64 measurement values, and no default arm (electrical-power-
 *   measurement-server.cpp:65-221), so of the declared set only
 *   ClusterRevision ever falls through to ember. The Instance is
 *   constructed by the port itself in
 *   mt_matter_meas_delegate_set_endpoint() (mt_matter_sl_b7.inc; no SDK
 *   init callback exists on this platform); its Init() is SOFT (AAI
 *   registration only, :43-47), no VerifyOrDie anywhere in this batch's
 *   energy clusters.
 *
 * THE DE407 OPTION-C DECLARATIONS: the seven push fields (Voltage,
 * ActiveCurrent, ActivePower, RMSVoltage, RMSCurrent, Frequency,
 * PowerFactor) are declared with their TRUE 64-bit ZCL types
 * (voltage_mv/amperage_ma/power_mw/int64s, electrical-power-measurement-
 * cluster.xml:64-138) and size 8, because the AAI is only consulted for
 * attributes that have ember metadata (CodegenDataModelProvider_Read.cpp:
 * 108-109, "we only allow AAI on ember-registered clusters" and the
 * metadata gate it explains) and because
 * AttributeList must be truthful. They get NO arena slot
 * (attr_gets_slot()'s existing md.size <= kSlotDataBytes refusal) and NO
 * seed, and they are in seed_slots()'s narrow per-(cluster, attribute)
 * quiet table below, so the boot log stays silent for exactly these and
 * keeps shouting for any other over-wide scalar. AT+MTATTR reads reach
 * them through the k_instance_served carve-out (mt_matter_sl.cpp),
 * answering the live delegate cache; writes answer +MTERR:11.
 *
 * PowerMode and NumberOfMeasurementTypes ARE slot-served over AT: their
 * seeds (kAc = 2, 1) equal the constants the delegate serves the fabric
 * (HearthEpmDelegate::GetPowerMode/GetNumberOfMeasurementTypes), the
 * FanControl seed-agreement discipline. Accuracy is ARRAY (AAI-served
 * list, no slot, quiet by type). The FeatureMap slot is an inert shadow
 * (Instance::Read() serves mFeature) seeded 0x2 AlternatingCurrent to
 * agree with the BitMask the Instance is constructed with; the
 * ClusterRevision seed is LIVE (no Read() case) and is this tree's
 * ElectricalPowerMeasurement/Metadata.h:20 kRevision = 3.
 *
 * ElectricalEnergyMeasurement (0x0091): no Instance and no delegate at
 * all; ONE wildcard AttributeAccessInterface serves every EEM endpoint
 * (ElectricalEnergyMeasurementCluster.h:36-57), registered once by
 * mt_matter_eem_register() (mt_matter_sl_b7.inc) because nothing in the
 * SDK ever calls its Init(). Its fixed Imported|Exported|Cumulative mask
 * is what every FeatureMap read on every EEM endpoint answers, so the
 * seed here is 0x7 and may never diverge from that construction site
 * (both sides cross-reference). The three value attributes are STRUCTs
 * (Accuracy MeasurementAccuracyStruct, the two cumulative counters
 * EnergyMeasurementStruct, electrical-energy-measurement-cluster.xml:
 * 61-79), declared size-0 metadata-only like batch 4's OperationalError,
 * refused by attr_gets_slot() and quiet by type; an AT+MTATTR read of any
 * of them answers +MTERR:5 from attr_type_info()'s refusal, the same code
 * the C6 answers for all of them (AT_MT_SPEC.md 3.9's 0x0510 row). The
 * ClusterRevision seed is LIVE (the wildcard AAI's Read() has no revision
 * case, ElectricalEnergyMeasurementCluster.cpp:57-130) and is
 * ElectricalEnergyMeasurement/Metadata.h:20 kRevision = 2.
 *
 * PowerTopology (0x009C, the sensor only): NodeTopology alone, the C6's
 * binding choice (mt_devtypes.cpp:1432-1447): the two endpoint-list
 * attributes exist only under the SET/TREE features and are NOT declared,
 * so the cluster contributes FeatureMap (inert shadow, seeded 0x1) and
 * ClusterRevision (LIVE: Instance::Read() serves FeatureMap and the two
 * absent lists only, power-topology-server.cpp:64-77; PowerTopology/
 * Metadata.h:20 kRevision = 1).
 *
 * No Identify on either type: neither ElectricalSensor.xml nor
 * ElectricalMeter.xml (data_model/1.5) requires it, and the C6 adds none
 * (mt_devtypes.cpp:1342-1346).
 *
 * Conformance disclosures, restated from AT_MT_SPEC.md 3.9:652-673 at the
 * declaration per the batch brief: the SENSOR's device type XML lists its
 * two measurement clusters as a pick-at-least-one choice, so the power-only
 * variant-1 sensor is fully conformant; the METER's XML marks both
 * clusters mandatory, so variant 1 on 0x0514 is a DISCLOSED departure that
 * exists for variant-scheme symmetry, and the strictly conformant
 * current-clamp declaration is the variant-1 SENSOR.
 *
 * Device revisions: both 1 per data_model/1.5/device_types/
 * ElectricalSensor.xml / ElectricalMeter.xml (the pinned authority). */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(epmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::PowerMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::NumberOfMeasurementTypes::Id,
                              INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::Accuracy::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::Voltage::Id, VOLTAGE_MV, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id,
                              AMPERAGE_MA, 8, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::ActivePower::Id, POWER_MW, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::RMSVoltage::Id, VOLTAGE_MV,
                              8, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::RMSCurrent::Id, AMPERAGE_MA,
                              8, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::Frequency::Id, INT64S, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::PowerFactor::Id, INT64S, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(eemAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::Accuracy::Id, STRUCT, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id,
                              STRUCT, 0, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::CumulativeEnergyExported::Id,
                              STRUCT, 0, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(ptopAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerTopology::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalSensorClusters)
DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1, power only: the same list minus the energy cluster. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalSensorPowerOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalMeterClusters)
DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1, the disclosed sub-conformant power-only meter (the section
 * comment above): EPM and Descriptor alone. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalMeterPowerOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(electricalSensorEndpoint, electricalSensorClusters);
HEARTH_DECLARE_CONST_ENDPOINT(electricalSensorPowerOnlyEndpoint, electricalSensorPowerOnlyClusters);
HEARTH_DECLARE_CONST_ENDPOINT(electricalMeterEndpoint, electricalMeterClusters);
HEARTH_DECLARE_CONST_ENDPOINT(electricalMeterPowerOnlyEndpoint, electricalMeterPowerOnlyClusters);

constexpr EmberAfDeviceType kElectricalSensorTypes[] = { { 0x0510, 1 } };
constexpr EmberAfDeviceType kElectricalMeterTypes[] = { { 0x0514, 1 } };

/* ---- heat pump (0x0309) and solar power (0x0017) ----------------------
 *
 * Catalogue batch 7a audit sections 3.4/3.5. Zero new clusters: both types
 * are the Electrical Sensor composition grafted onto the SAME endpoint as
 * a Wired power source, the C6's hand-built stacks (mk_heat_pump(),
 * mt_devtypes.cpp:1790-1867, and mk_solar_power(), :2101-2136) rendered as
 * declared lists. The endpoint advertises THREE device types at once
 * (0x0309 or 0x0017, plus 0x0011 Power Source and 0x0510 Electrical
 * Sensor, same endpoint, NOT children), the C6's exact span through
 * add_device_type().
 *
 * A NEW Wired PowerSource attribute list, never the battery-shaped
 * powerSourceAttrs above: reusing that list here would advertise
 * BatChargeLevel/BatReplacementNeeded/BatReplaceability on a mains device,
 * a conformance break no build check catches (the audit's heat pump risk
 * line). The wired list is Status, Order, Description (CHAR_STRING,
 * metadata-only, no slot), WiredCurrentType (mandatory under the WIRED
 * feature, PowerSource.xml), EndpointList (ARRAY, served by the cluster's
 * wildcard AAI as an empty list, the batch 4 note on powerSourceAttrs)
 * and FeatureMap 0x1 Feature::kWired, seeded per device type below since
 * the battery wildcard seed row must keep winning for 0x0011.
 * WiredCurrentType boots kAc, which is 0 (PowerSource/Enums.h
 * WiredCurrentTypeEnum, kAc = 0x00), the zero-fill, so it carries no seed
 * row; the C6's wired feature::add() creates it 0 identically.
 *
 * THE C6'S WORKAROUNDS DO NOT TRANSFER, THE POSITIVE OBLIGATIONS DO (the
 * audit's solar risk line, restated here as the batch brief directs).
 * mk_heat_pump() exists because heat_pump::add() boot-panics on EEM
 * feature validation with a default config and would bolt on a DEM pair
 * (esp_matter_endpoint.cpp:2015-2020); mk_solar_power() exists because
 * solar_power::add() silently ASSIGNS feature flags over any pre-seed,
 * fixes an EEM mask of exported|cumulative that would contradict the
 * wildcard AAI's Imported|Exported|Cumulative and lose
 * CumulativeEnergyImported entirely, and creates Voltage/ActiveCurrent as
 * non-null zeros, irreversibly breaking the null-until-pushed contract
 * (mt_devtypes.cpp:1890-1903). None of those SDK code paths exist on this
 * port; what carries over is what they were protecting: (1) the seven EPM
 * fields are declared 64-bit, slotless and null until pushed (epmAttrs);
 * (2) the EEM mask is exactly Imported|Exported|Cumulative
 * (mt_matter_eem_register and the eemAttrs seed); (3) the EEM cluster is
 * present only where declared below.
 *
 * Variants: heat pump has ONE variant (the C6 row's max_variant 0). Solar
 * spends its variant on the graft's EEM: variant 0 with, variant 1
 * without, and variant 1 is DISCLOSED SUB-CONFORMANT against
 * SolarPower.xml's composedDeviceTypes block, which mandates EPM and EEM
 * both on the composed sensor (AT_MT_SPEC.md 3.9:735-739; the conformant
 * declaration is variant 0).
 *
 * DISCLOSED GAPS, matching the C6 and the SDK build's own omissions
 * (mt_devtypes.cpp:1781-1789, :2096-2099): HeatPump.xml's
 * composedDeviceTypes block also mandates a composed Thermostat device
 * type with User Label, which neither platform builds; both types' User
 * Label describedConform rows likewise. Disclosed, not wired.
 *
 * Device revisions: both 1 per data_model/1.5/device_types/HeatPump.xml /
 * SolarPower.xml. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(wiredPowerSourceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Status::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Order::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Description::Id, CHAR_STRING, 61, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::WiredCurrentType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::EndpointList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* One cluster list serves the heat pump and the variant-0 solar (the
 * booleanStateSensorClusters sharing principle: the whole composition is
 * identical, only the EmberAfDeviceType span differs per row). */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(wiredElectricalSensorClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, wiredPowerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Solar variant 1: the same stack without the energy cluster. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(wiredElectricalSensorPowerOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, wiredPowerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(wiredElectricalSensorEndpoint, wiredElectricalSensorClusters);
HEARTH_DECLARE_CONST_ENDPOINT(wiredElectricalSensorPowerOnlyEndpoint,
                         wiredElectricalSensorPowerOnlyClusters);

/* The same-endpoint grafts: three device types per span, the C6's
 * add_device_type() sequence (0x0011 revision 1 matching the standalone
 * power source row, 0x0510 revision 1 matching the sensor row above). */
constexpr EmberAfDeviceType kHeatPumpTypes[] = { { 0x0309, 1 }, { 0x0011, 1 }, { 0x0510, 1 } };
constexpr EmberAfDeviceType kSolarPowerTypes[] = { { 0x0017, 1 }, { 0x0011, 1 }, { 0x0510, 1 } };

/* ---- electrical utility meter (0x0511) --------------------------------
 *
 * Catalogue batch 7a audit section 3.9. One new cluster,
 * MeterIdentification (0x0B06), the cheapest type in the batch: Instance
 * only, NO delegate of any kind (the Instance owns its own attribute
 * storage, three 64-byte string buffers included: 304 B measured in the
 * step-0 build), pool MT_METER_MAX (2, core/include/mt_matter.h:1302, the
 * C6 depth).
 *
 *   Code-driven? No; CHI-only? Not listed, harmless (no commands). Both
 *   confirmed empirically by the batch's one zap regeneration
 *   (byte-identical trio, the epmAttrs note).
 *
 *   AAI? YES, per endpoint, and NOTHING IN THE SDK EVER CONSTRUCTS IT:
 *   Instance::Init() (meter-identification-server.cpp:59-68, soft: nulls
 *   the five attributes and registers the AAI) has no SDK caller anywhere
 *   in the pinned tree, the chime/EEM disease in the organ the C6's
 *   mt_meter.cpp documents at length. The port's fix is the same shape:
 *   mt_meter_register_all() (mt_matter_sl_b7.inc), a registration scan
 *   over the live composition run once from main.cpp after
 *   rebuild_composition() and before mt_at_start(), so every meter
 *   endpoint answers before +MTREADY lets the host ask. Forget the scan
 *   and the endpoint advertises five attributes and answers none of them,
 *   the exact failure mode the C6 round found on the bench (the audit's
 *   risk line).
 *
 *   Capacity is claimed at CREATE time, not registration time:
 *   mt_meter_reserve() below runs with the other pre-create pool claims,
 *   because only a create failure can abort the composition rebuild; a
 *   shortfall discovered in the later scan could not un-create the
 *   cluster already on the wire (the C6's fix-round-2 lesson,
 *   mt_meter.cpp's s_meter_reserved comment).
 *
 * Slots: MeterType (ENUM8, nullable, k_instance_served live-read; its
 * inert shadow seeds the null sentinel), FeatureMap (inert shadow of the
 * Instance's construction mask, mt_meter_feature_mask() = kPowerThreshold
 * 0x1) and ClusterRevision (LIVE: Instance::Read() serves the five
 * attributes and FeatureMap only, meter-identification-server.cpp:
 * 215-245; MeterIdentification/Metadata.h:20 kRevision = 1). The three
 * char_strings (64 bytes each, so 65 ember bytes, under the 66-byte IO
 * buffer) and the PowerThresholdStruct are metadata-only, no slot, quiet
 * by type; an AT+MTATTR read of any of the four answers +MTERR:5. On the
 * C6 those four +MTERR:5 answers come from two different mechanisms (the
 * strings reach the Instance and fail conversion, the struct is refused
 * before dispatch by an esp-matter ARRAY refusal, AT_MT_SPEC.md 3.9:
 * 808-822); on this port all four come from attr_type_info()'s one
 * refusal, same wire, different plumbing, the spec's platform-note gap
 * the batch report flags.
 *
 * Identify IS composed, a conformance requirement, not decoration:
 * ElectricalUtilityMeter.xml declares superset="Meter Reference Point"
 * and MeterReferencePoint.xml's own body mandates Identify; a superset
 * conformance is transitive (the C6's mk_electrical_utility_meter()
 * reasoning, mt_devtypes.cpp:2547-2557).
 *
 * TimeSyncCond is DISCLOSED, NOT FIXED, on both platforms: this
 * firmware's root endpoint carries no Time Synchronization cluster on any
 * image, so no meter endpoint is conformant against that condition
 * (AT_MT_SPEC.md 3.9:800-806; adding the cluster to endpoint 0 is a
 * whole-composition decision out of this batch's scope).
 *
 * The five attributes are set in ONE call, AT+MTMETERID
 * (mt_matter_meter_set_identity, mt_matter_sl_b7.inc), all-or-nothing;
 * there is no other write path and deliberately no read-back verb
 * (AT_MT_SPEC.md 3.29). One variant. Device revision 1 per
 * data_model/1.5/device_types/ElectricalUtilityMeter.xml. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(meterIdAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::MeterType::Id, ENUM8, 1,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::PointOfDelivery::Id, CHAR_STRING,
                              65, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::MeterSerialNumber::Id, CHAR_STRING,
                              65, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::ProtocolVersion::Id, CHAR_STRING,
                              65, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::PowerThreshold::Id, STRUCT, 0,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(utilityMeterClusters)
DECLARE_DYNAMIC_CLUSTER(MeterIdentification::Id, meterIdAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(utilityMeterEndpoint, utilityMeterClusters);

constexpr EmberAfDeviceType kElectricalUtilityMeterTypes[] = { { 0x0511, 1 } };

/* ---- device energy management (0x050D) --------------------------------
 *
 * Catalogue batch 7a audit section 3.7. DeviceEnergyManagement (0x0098)
 * plus DeviceEnergyManagementMode (0x009F, a ModeBase alias riding the
 * batch 5 mode-base-server frame: no new translation unit, one more
 * kStoreWalk row, one more pool consumer) plus Descriptor. No Identify and
 * no tag_list, SDK parity (the C6's mk_dem(), mt_devtypes.cpp:2287-2304).
 * The XML classifies it a utility device type stackable on any endpoint;
 * this row is the standalone declaration.
 *
 * DeviceEnergyManagement's three answers: code-driven no, CHI-only YES
 * (config-data.yaml:41: its eight commands have no generated dispatch;
 * the byte-identical IMClusterCommandHandler check covered it), AAI YES
 * per endpoint (Instance derives from both AttributeAccessInterface and
 * CommandHandlerInterface, device-energy-management-server.h:207-217;
 * Init() registers CHI then AAI, SOFT on both,
 * device-energy-management-server.cpp:40-47). Instance::Read() serves the
 * six delegate scalars, the two structs and FeatureMap and falls through
 * to ember for the rest (:60-105), so only ClusterRevision's seed is
 * live.
 *
 * THE IRON RULE, INVERTED (the C6's mt_add_dem_triple() comment,
 * mt_devtypes.cpp:1974-2025, and ARCHITECTURE.md 8.12, restated verbatim
 * in reasoning as the batch brief directs). On the C6, DEM feature bits
 * are set ONLY via feature::power_adjustment::add(), never by writing
 * FeatureMap, because that one call is what creates
 * PowerAdjustmentCapability and OptOutState, both PA commands and both PA
 * events alongside the bit; a hand-set bit would advertise conformance
 * with no ember entry behind it, and a missing entry means an invoke
 * returns NO status at all on that platform. On this port there is no
 * feature::add(): FeatureMap seeds and command lists are written by hand,
 * so the rule inverts into a declaration obligation. SEEDING THE
 * POWERADJUSTMENT BIT ON VARIANT 0 OBLIGES, all four together:
 *   - declaring PowerAdjustmentCapability (STRUCT, metadata-only,
 *     Instance-served, feature-gated in Read());
 *   - declaring OptOutState (enum8; its slot is an inert shadow, the
 *     Instance serves it, the carve-out reads it live);
 *   - both PA commands in the INCOMING list (kDemIncoming below; the
 *     Instance's CHI handles the invokes and its
 *     RetrieveAcceptedCommands derives the advertised list from its own
 *     feature mask, device-energy-management-server.cpp:110-127, so the
 *     declared list and the mask must agree);
 *   - NOTHING in the outgoing list: neither PA command has a response
 *     (the cluster XML), the DE399 truthfulness discipline.
 * Getting it half right advertises commands with no capability behind
 * them and turns every AT+MTDEMCAP into +MTERR:4, with no build check to
 * catch it: the variant-0 list below is the four together, the variant-1
 * list is NONE of them (report-only ESA, FeatureMap 0, conformant: the
 * XML's five-feature min-1 choice applies only under the ControllableESA
 * condition, which variant 1 does not declare).
 *
 * The FeatureMap seed itself is the one variant-dependent boot value in
 * the catalogue; on this port the seed table's variant column carries it
 * (the two variant-qualified 0x050D FeatureMap rows in
 * mt_devtypes_sl_seeds.inc) where the nRF has a seed_slots() special
 * case; the create path's PowerAdjustment predicate, which answers the
 * same split for the two variants, feeds mt_matter_dem_register()'s
 * Instance mask, so shadow and Instance agree by construction.
 *
 * DEMMode reuses modeBaseAttrs and kModeBaseIncoming/kModeBaseOutgoing
 * verbatim: every ModeBase alias numbers SupportedModes 0, CurrentMode 1,
 * ChangeToMode 0 and ChangeToModeResponse 1 identically (the generated
 * per-cluster headers), and the batch 5 audit notes on modeBaseAttrs
 * apply unchanged, the sharp one included: ModeBase Instance::Init()
 * VerifyOrDies on ordering (mode-base-server.cpp:77) and reads the
 * delegate's index 0 first (:74), so the DEMMode store construction below
 * and the strictly-after-create set_endpoint are both load-bearing.
 * ClusterRevision 2 (DeviceEnergyManagementMode/Metadata.h:20, LIVE, the
 * ModeBase no-default-arm rule); tag-0 default kNoOptimization
 * (AT_MT_SPEC.md 3.20's table row).
 *
 * Slots: v0 = ESAType, ESACanGenerate, ESAState, OptOutState, FeatureMap,
 * ClusterRevision (6) + DEMMode 3; v1 drops OptOutState (5 + 3). The two
 * power_mw attributes are DE407 metadata-only declarations (quiet-table
 * rows), Instance-served, carve-out read. Block: v0 3 clusters, 9 slots,
 * 306 B store = 462 B payload (the nRF's heap charges 472 B,
 * roundup(462 + 4, 8)); v1 446 B payload (456 B there). This port's
 * floor prices the block with its over-count (kDemBlockBytes, 528 B of
 * arena), under the mode select's 608 B widest.
 *
 * ESAState's shadow seeds kOnline (1), the delegate's default and the
 * spec's pre-first-push answer (3.25); ESAType/ESACanGenerate/OptOutState
 * zero-fill to match the delegate defaults. ClusterRevision 4 is
 * DeviceEnergyManagement/Metadata.h:20 in THIS tree.
 *
 * Device revision 3 per data_model/1.5/device_types/
 * DeviceEnergyManagement.xml. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(demAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESACanGenerate::Id, BOOLEAN, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMinPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMaxPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::PowerAdjustmentCapability::Id,
                              STRUCT, 0, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::OptOutState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Variant 1, the report-only ESA: no PA-gated attributes (the iron rule's
 * other half: FeatureMap 0 may not advertise what is not declared). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(demReportOnlyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESACanGenerate::Id, BOOLEAN, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMinPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMaxPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kDemIncoming[] = { DeviceEnergyManagement::Commands::PowerAdjustRequest::Id,
                                       DeviceEnergyManagement::Commands::CancelPowerAdjustRequest::Id,
                                       kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(demClusters)
DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kDemIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(demReportOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demReportOnlyAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(demEndpoint, demClusters);
HEARTH_DECLARE_CONST_ENDPOINT(demReportOnlyEndpoint, demReportOnlyClusters);

constexpr EmberAfDeviceType kDemTypes[] = { { 0x050D, 3 } };

/*
 * The DE407 quiet table (nRF mt_devtypes_zephyr.cpp 4924-4988): the
 * specific (cluster, attribute) pairs whose over-wide scalar declarations
 * are DELIBERATE metadata-only 64-bit declarations, each proven
 * Instance-served, admitted one pair at a time and NEVER by type: a future
 * ember-served 64-bit attribute must keep failing loudly in seed_slots()
 * (declared but slotless, it would answer a bare ERROR over AT). The
 * declarations are refused a slot by attr_gets_slot()'s size clause; this
 * table only silences the boot shout for the proven pairs. Catalogue batch
 * 7a-1 brought the seven ElectricalPowerMeasurement rows and 7a-2 the two
 * DeviceEnergyManagement rows; the nRF's WaterHeaterManagement (7b) and
 * EnergyEvse (the EVSE round) rows arrive with their batches.
 */
struct quiet_no_slot_attr {
    ClusterId cluster;
    AttributeId attr;
};

constexpr quiet_no_slot_attr kQuietNoSlot[] = {
    /* ElectricalPowerMeasurement: the seven AT+MTMEAS push fields, served
     * by Instance::Read()'s own cases from the HearthEpmDelegate cache
     * (mt_matter_sl_b7.inc; the k_instance_served carve-out in
     * mt_matter_sl.cpp serves the AT side). */
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Voltage::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActivePower::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::RMSVoltage::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::RMSCurrent::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Frequency::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::PowerFactor::Id },
    /* DeviceEnergyManagement: the two power_mw scalars, served by
     * Instance::Read()'s own cases from the HearthDemDelegate cache
     * (device-energy-management-server.cpp:70-73; the carve-out serves
     * the AT side). */
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::AbsMinPower::Id },
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::AbsMaxPower::Id },
};

bool attr_quiet_no_slot(ClusterId cluster, AttributeId attr)
{
    for (auto &q : kQuietNoSlot) {
        if (q.cluster == cluster && q.attr == attr) {
            return true;
        }
    }
    return false;
}
