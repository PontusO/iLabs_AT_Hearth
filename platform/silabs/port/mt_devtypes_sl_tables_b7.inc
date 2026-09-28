/*
 * mt_devtypes_sl_tables_b7.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 7a-1 (nRF 2437-2723): the
 * measurement family, the electrical sensor (0x0510) and electrical meter
 * (0x0514) with their power-only variants, and the heat pump (0x0309) and
 * solar power (0x0017) on the wired electrical sensor lists; and
 * catalogue batch 7a-2 (nRF 2725-2954): the electrical utility meter
 * (0x0511) and device energy management (0x050D) with its report-only
 * variant; and catalogue batch 7b (nRF 2956-3320): the water heater
 * (0x050F) and battery storage (0x0018), each with its variant; and the
 * EVSE round (nRF 4112-4400): the energy EVSE (0x050C) with its no-SOC
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
 * step-0 build), pool MT_METER_MAX (2, core/include/mt_matter.h:1303, the
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

/* ---- water heater (0x050F) --------------------------------------------
 *
 * Catalogue batch 7b audit (batch7-audit.md section 3.3), every citation
 * re-verified against the pinned NCS tree while writing. One new cluster,
 * WaterHeaterManagement (0x0094), plus the fourth ModeBase alias
 * (WaterHeaterMode 0x009E, mode-base-server per zap_cluster_list.json:367,
 * no new translation unit), on the C6's mk_water_heater()
 * (mt_devtypes.cpp:1628-1736, class comment :1531-1627).
 *
 * WaterHeaterManagement's three answers: code-driven no, CHI-only YES
 * (config-data.yaml:81; the batch's zap regeneration (on the nRF build) left
 * IMClusterCommandHandler.cpp and CodeDrivenInitShutdown.cpp
 * byte-identical, sha256-checked; access.h did NOT stay byte-identical,
 * against the audit 5.5 prediction: it gained Boost and CancelBoost with
 * the manage invoke privilege the cluster XML really declares,
 * water-heater-management-cluster.xml:94 and :100, a correct delta the
 * batch 3 empirical check exists to catch). AAI YES per endpoint: the
 * Instance derives from both AttributeAccessInterface and
 * CommandHandlerInterface (water-heater-management-server.h:139-149, ctor
 * Instance(EndpointId, Delegate &, Feature) whose body sets the delegate's
 * endpoint); Init() registers CHI then AAI, SOFT on both
 * (water-heater-management-server.cpp:94-100, no emberAfContainsServer
 * check, no VerifyOrDie), constructed by the port's own
 * mt_matter_whm_register() below a successful emberAfSetDynamicEndpoint(),
 * the DEM handout's shape, pool MT_WHM_MAX (4, core/include/
 * mt_matter.h:952, the C6 depth per DE407).
 *
 * THE ONE INERT REVISION SEED IN THE ENERGY FAMILY: Instance::Read()
 * serves ClusterRevision ITSELF (water-heater-management-server.cpp:
 * 146-147, encoding kClusterRevision = 2, :40), the only energy cluster
 * that does; every other case falls through for NOTHING (the switch covers
 * all six attributes plus FeatureMap). So the WHM ClusterRevision seed
 * below is an inert shadow kept equal to that served 2
 * (WaterHeaterManagement/Metadata.h:20 agrees), the Thermostat
 * seed-agrees-with-AAI discipline, while EPM/EEM/PTOP/DEM revision seeds
 * stay LIVE.
 *
 * Slots, v0: HeaterTypes, HeatDemand, TankVolume, TankPercentage,
 * BoostState, FeatureMap, ClusterRevision (7). EstimatedHeatRequired is
 * energy_mwh (water-heater-management-cluster.xml:78, min 0), declared
 * true-width 8 B for the AAI gate (DE407 option C), no slot, its own
 * kQuietNoSlot row, k_instance_served live-read row. v1 drops the
 * feature-gated pair whole per the cluster XML's gates (TankVolume and
 * EstimatedHeatRequired mandatory under EM only, :73-82; TankPercentage
 * under TP only, :83-86; HeaterTypes/HeatDemand/BoostState mandatory
 * unconditionally, :67-71 and :88-90): 5 slots. All six values are
 * Instance-served from the HearthWhmDelegate cache (AT+MTMEAS 0x94 is the
 * write path, AT_MT_SPEC.md 3.25); the enum/bitmap/u16 slots are inert
 * shadows zero-filled to the delegate defaults (everything 0, BoostState
 * kInactive = 0, WaterHeaterManagement/Enums.h:32-36).
 *
 * THE FEATUREMAP IS THE CATALOGUE'S SECOND VARIANT-DEPENDENT BOOT VALUE
 * (like the DEM's: both variants share devtype 0x050F, so a devtype-keyed
 * seed row alone cannot split them; the seed table's variant column does). Variant 0 is
 * EnergyManagement|TankPercent = 0x3 (Enums.h:44-48; the value the C6
 * hand-sets because BOTH esp-matter feature helpers are broken,
 * mt_devtypes.cpp:1594-1607: energy_management::add() as declared does
 * not link and tank_percent::get_id() returns the kEnergyManagement bit;
 * neither helper exists on this port, so the obligation reduces to the
 * bits themselves, read from CHIP's own enum). Variant 1 is 0 (HeaterTypes
 * /HeatDemand/BoostState only, which the cluster's own feature conformance
 * permits: EM and TP are both optionalConform, cluster XML :57-64).
 * On this port the seed table's variant column carries it (the two
 * variant-qualified 0x050F FeatureMap rows in mt_devtypes_sl_seeds.inc),
 * as on the nRF (whose seed_slots() keeps a special case only for
 * AirQuality); the create path hands
 * the same variant predicate to mt_matter_whm_register(),
 * whose Instance mask is what Read() actually answers for FeatureMap
 * (server.cpp:144-145 encodes mFeature, snapshotted at construction):
 * seed and Instance agree by construction, single-sourced on the variant.
 *
 * Thermostat: REUSES thermostatAttrs whole with a HEATING-ONLY FeatureMap
 * seed (0x01) keyed to 0x050F below (the seed table's per-devtype keying;
 * the standalone thermostat and the room air conditioner keep the wildcard
 * 0x03), per WaterHeater.xml's Thermostat(HEAT) mandate (:75-81; HEAT
 * alone makes only OccupiedHeatingSetpoint mandatory, the C6 class
 * comment's own conformance trace). ControlSequenceOfOperation gets a
 * keyed 0x02 (HeatingOnly) seed: the wildcard 0x04 (CoolingAndHeating)
 * would advertise cooling this endpoint does not have. DELIBERATE
 * DIVERGENCE from the C6, which serves esp-matter's inherited config
 * default 4 on its water heater (esp_matter_cluster.h:415, the thunk does
 * not override it); the FanModeSequence precedent, noted rather than
 * copied. DISCLOSED OVER-DECLARATION riding on the reuse (the batch
 * brief's ruled trade): thermostatAttrs also declares
 * OccupiedCoolingSetpoint and the four setpoint-limit attributes, three of
 * which Thermostat.xml gates on COOL; the C6's water heater creates no
 * cooling attributes at all, so this endpoint's AttributeList is wider
 * than the C6's and than the letter of HEAT-only conformance. Disclosed
 * here and in the batch report, not silently inherited. No panic-trap
 * analogue exists on this port (the C6's VALIDATE_FEATURES_AT_LEAST_ONE
 * pre-seed, mt_devtypes.cpp:1638-1641, is esp-matter machinery); what
 * carries over is the conformance conclusion, the 0x01 seed.
 *
 * WaterHeaterMode reuses modeBaseAttrs and kModeBaseIncoming/
 * kModeBaseOutgoing verbatim (every ModeBase alias numbers its elements
 * identically, the DEMMode note above), one more kStoreWalk row and one
 * more pool consumer; the batch 5 ModeBase notes apply unchanged,
 * including the Init() ordering panic and the placeholder-mode-0 policy.
 * ChangeToMode forwards on cluster 158, already in AT_MT_SPEC.md 3.17's
 * registration list (EnergyEvseMode 0x9D, once the list's known gap under
 * ruling DE408, has since been admitted too).
 * ClusterRevision 1 LIVE (WaterHeaterMode/Metadata.h:20; the ModeBase AAI
 * has no revision case). Tag-0 default kManual on every mode
 * (AT_MT_SPEC.md 3.20's table row; WaterHeaterMode/Enums.h:45).
 *
 * Commands: Boost (0x00) and CancelBoost (0x01) in the incoming list; the
 * server's InvokeCommand dispatches both to the delegate
 * (water-heater-management-server.cpp:154-169), and access.h carries their
 * manage privilege (above). NOTHING outgoing: neither command has a
 * response (the cluster XML), the DE399 truthfulness discipline. The
 * five-field Boost payload and the CancelBoost in-state guard live in the
 * delegate (mt_matter_sl_b7.inc, where the guard's SDK-versus-C6 placement
 * is traced).
 *
 * Variant 0 additionally grafts the Electrical Sensor composition on the
 * SAME endpoint, which WaterHeater.xml:84-95 mandates (composedDeviceTypes:
 * an Electrical Sensor with EPM and EEM): PTOP + EPM + EEM with the batch
 * 7a lists verbatim, drawing one EPM and one PTOP slot from the
 * MT_MEAS_MAX pools, and the span advertises 0x0510 alongside 0x050F, the
 * C6's exact span (electrical_sensor::add() calls add_device_type(0x0510),
 * esp_matter_endpoint.cpp:1505-1508, and mk_water_heater() grafts it only
 * on variant 0, so the C6's variant-1 span is 0x050F ALONE: this port's
 * first variant-dependent device-type span, carried by the registry's new
 * device_types_v1 member below). Variant 1 is the SDK-bare build, no
 * graft, DISCLOSED SUB-CONFORMANT against the mandated composed sensor
 * (the variant-1 electrical meter precedent; AT_MT_SPEC.md 3.9's 0x050F
 * row and ARCHITECTURE.md 8.11 carry it on the C6's side).
 *
 * No Identify on either variant: WaterHeater.xml marks it optionalConform
 * (:66-68) and the C6's water_heater::add() creates none.
 *
 * Events: BoostStarted and BoostEnded, derived FIRMWARE-SIDE from
 * BoostState transitions pushed over AT+MTMEAS, with the cached-parameters
 * lifecycle (core/include/mt_matter.h:874-883, binding); the derivation
 * state machine and its exhaustive commentary live beside the delegate in
 * mt_matter_sl_b7.inc. Plus CumulativeEnergyMeasured on v0 (the EEM push
 * path, batch 7a).
 *
 * Block: v0 7 clusters, 29 slots, one 306 B mt_mb_store_t = 798 B payload
 * (800 of this port's arena; the nRF's heap charges 808); v1 4 clusters,
 * 19 slots, one store = 626 B payload, 632 of arena. Both wider than the
 * 608 B the other types are sixteen-of, so an admission candidate like
 * the RVC's (its 936 over-count, 856 exact), compile-time-derived and
 * asserted in mt_devtypes_sl_arena.inc.
 *
 * Device revision 1 per data_model/1.5/device_types/WaterHeater.xml:60. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(whmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeaterTypes::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeatDemand::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::TankVolume::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::EstimatedHeatRequired::Id,
                              ENERGY_MWH, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::TankPercentage::Id, PERCENT, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::BoostState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Variant 1, the bare cluster: the three unconditionally mandatory values
 * only (the feature-gate trace in the section comment; FeatureMap 0 may
 * not advertise what is not declared, the DEM report-only list's rule). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(whmBareAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeaterTypes::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeatDemand::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::BoostState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kWhmIncoming[] = { WaterHeaterManagement::Commands::Boost::Id,
                                       WaterHeaterManagement::Commands::CancelBoost::Id,
                                       kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(waterHeaterClusters)
DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterManagement::Id, whmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kWhmIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1, the SDK-bare build: no sensor graft (the disclosure in the
 * section comment). */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(waterHeaterBareClusters)
DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterManagement::Id, whmBareAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kWhmIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(waterHeaterEndpoint, waterHeaterClusters);
HEARTH_DECLARE_CONST_ENDPOINT(waterHeaterBareEndpoint, waterHeaterBareClusters);

/* The variant-0 span grafts 0x0510 on the same endpoint (revision 1,
 * matching the standalone sensor row); variant 1 has no graft and no
 * second id, the C6's exact behaviour (the section comment). */
constexpr EmberAfDeviceType kWaterHeaterTypes[] = { { 0x050F, 1 }, { 0x0510, 1 } };
constexpr EmberAfDeviceType kWaterHeaterBareTypes[] = { { 0x050F, 1 } };

/* ---- battery storage (0x0018) -----------------------------------------
 *
 * Catalogue batch 7b audit (batch7-audit.md section 3.6). NO new cluster:
 * the whole composition is batch 7a machinery (the sensor graft, the DEM
 * pair, the pools, AT+MTDEMCAP, the PA event protocol) under a THIRD
 * PowerSource attribute list, on the C6's mk_battery_storage()
 * (mt_devtypes.cpp:2182-2264, class comment :2137-2181). Only the
 * registry rows, the spans and this note are new; every create-path arm,
 * seed row and carve-out row is keyed on cluster ids these lists
 * already carry.
 *
 * THE RECHG CONFORMANCE FIX, the reason the battery-shaped
 * powerSourceAttrs above cannot be reused (the audit's risk line; the C6
 * comment at :2146-2159 records the SDK defect this corrects):
 * battery_storage::add() sets kBattery alone and then hand-creates four
 * attributes PowerSourceCluster.xml gates on the RECHG feature, while the
 * RECHG-MANDATORY BatChargeState is missing entirely. The fix is
 * Battery|Rechargeable together (Feature 0x2 | 0x4 = 0x6,
 * PowerSource/Enums.h:280-286, the keyed FeatureMap seed below) AND
 * declaring the full attribute population that makes it true: the four
 * RECHG-gated attributes (BatTimeToFullCharge, BatChargingCurrent,
 * ActiveBatChargeFaults, BatCapacity) plus BatChargeState plus
 * BatFunctionalWhileCharging, alongside the battery family the existing
 * list already carries. Declaring the gated four without BatChargeState,
 * or without the Rechargeable bit, reproduces the exact SDK defect the C6
 * thunk was written to correct, with no build check to catch it.
 *
 * B263 BINDS THE DECLARED WIDTHS (the C6's own note at :2221-2246): the
 * five uint32 battery attributes are XML-typed int32u with NO <constraint>
 * element (power-source-cluster.xml:99 BatVoltage, :110 BatTimeRemaining,
 * :165 BatCapacity, :183 BatTimeToFullCharge, :193 BatChargingCurrent),
 * so the TYPE IS THE BOUND: all five are declared INT32U size 4, never an
 * invented narrower size, and ember serves the full uint32 domain the C6
 * serves. On this port the obligation is only the declared width (there
 * is no created min/max to widen); a 4 B slot holds all five, so they are
 * ordinary AT+MTATTR territory, plain slots, NO carve-out rows, no quiet
 * rows, the audit's "no new AT surface" line. Nullable per the XML:
 * BatVoltage, BatTimeRemaining, BatTimeToFullCharge and BatChargingCurrent
 * are isNullable (seeded to the 4-byte unsigned sentinel below);
 * BatCapacity and BatChargeState are NOT nullable (zero-fill: 0 and
 * kUnknown = 0, PowerSource/Enums.h:109-115), BatFunctionalWhileCharging
 * boots false, all matching the C6 config defaults. The two fault lists
 * (ActiveBatFaults :135, ActiveBatChargeFaults :199) are arrays, declared
 * metadata-only, no slot, quiet by type; AT+MTATTR answers +MTERR:5 on
 * either, and they ship empty and host-untouched (AT_MT_SPEC.md 3.9's
 * 0x0018 row). BatPercentRemaining keeps its real 0..200 bounds through
 * the same hand-rolled MIN_MAX entry as the battery list.
 *
 * Variants: v0 carries the DEM pair (demAttrs + kDemIncoming, the
 * WITH-PowerAdjustment list: the create path's PowerAdjustment predicate
 * (type_has_attr) feeds mt_matter_dem_register(), the seed table's
 * variant-qualified DEM FeatureMap rows agree with it, and variant 0 IS the PA variant here exactly
 * as on the standalone 0x050D row, so every DEM obligation of the
 * inverted iron rule holds unchanged). The triple is OVER-DELIVERY
 * matching the SDK build (BatteryStorage.xml never requests DEM, the C6
 * comment :2170-2172); v1 omits it and stays conformant. Both variants
 * carry the full sensor graft WITH EEM (unlike solar, which spends its
 * variant on the EEM: the two types' variant bytes mean different things,
 * AT_MT_SPEC.md 3.9's shared paragraph).
 *
 * Spans: v0 advertises 0x0018 (revision 2, BatteryStorage.xml:60) +
 * 0x0011 + 0x0510 + 0x050D, the C6's exact sequence; v1 drops 0x050D with
 * the DEM pair (the C6 adds that id inside the variant-0 branch only), the
 * water heater's device_types_v1 mechanism.
 *
 * DISCLOSED XML INCONSISTENCY, flagged not chased (the C6 comment
 * :2173-2181): BatteryStorage.xml revision 2's summary says "two Power
 * Source and Electrical Sensor composed devices types" while the body
 * encodes ONE composed 0x0510 block with duplicated cluster rows and no
 * Power Source device type at all; this endpoint delivers one sensor and
 * one power source and discloses the departure on both platforms' terms.
 *
 * No Identify (BatteryStorage.xml lists it optional, nothing else); no
 * tag_list, the port-wide rule.
 *
 * Block: v0 7 clusters, 32 slots, one 306 B mt_mb_store_t = 846 B
 * payload (848 of this port's arena; the nRF's heap charges 856); v1 5
 * clusters, 23 slots, no store = 388 B payload, 392 of arena. The nRF audit's 3.6 table said 31 slots / 830 / 832 for v0: it
 * under-counted this list by one slot (its "about 14" PowerSource figure
 * against the real 15 below) and mis-rounded the heap cost; re-derived
 * here per the batch brief; wider than the 608 B the other types are
 * sixteen-of, so an admission candidate like the RVC's, pinned with the
 * corrected arithmetic in mt_devtypes_sl_arena.inc. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(rechargeableBatteryPowerSourceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Status::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Order::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Description::Id, CHAR_STRING, 61, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatVoltage::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    /* Hand-rolled MIN_MAX entry, shared with the battery list; see the
     * powerSourceAttrs audit note. */
    { &kBatPercentRemainingBounds, PowerSource::Attributes::BatPercentRemaining::Id, 1,
      ZAP_TYPE(INT8U),
      ZAP_ATTRIBUTE_MASK(MIN_MAX) | ZAP_ATTRIBUTE_MASK(NULLABLE) |
          ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE) | ZAP_ATTRIBUTE_MASK(READABLE) },
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatTimeRemaining::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargeLevel::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplacementNeeded::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplaceability::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::ActiveBatFaults::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatCapacity::Id, INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargeState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatTimeToFullCharge::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatFunctionalWhileCharging::Id, BOOLEAN, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargingCurrent::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::ActiveBatChargeFaults::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::EndpointList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(batteryStorageClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, rechargeableBatteryPowerSourceAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kDemIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1: the same stack without the over-delivered DEM pair. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(batteryStorageNoDemClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, rechargeableBatteryPowerSourceAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(batteryStorageEndpoint, batteryStorageClusters);
HEARTH_DECLARE_CONST_ENDPOINT(batteryStorageNoDemEndpoint, batteryStorageNoDemClusters);

/* The v0 span carries the DEM id; v1 drops it with the cluster pair (the
 * section comment; revisions match the standalone rows: 0x0011 rev 1,
 * 0x0510 rev 1, 0x050D rev 3). */
constexpr EmberAfDeviceType kBatteryStorageTypes[] = { { 0x0018, 2 },
                                                       { 0x0011, 1 },
                                                       { 0x0510, 1 },
                                                       { 0x050D, 3 } };
constexpr EmberAfDeviceType kBatteryStorageNoDemTypes[] = { { 0x0018, 2 },
                                                            { 0x0011, 1 },
                                                            { 0x0510, 1 } };

/* ---- energy EVSE (0x050C) ----------------------------------------------
 *
 * The last device type, and the one ruling DE408 excluded when the nRF sat
 * at 96.67 percent of RAM. The nRF's memory reclaim rounds left 46.5 KB free
 * and made the row staging buffers pay per use, so the exclusion is lifted
 * WITH A CAP: MT_EVSE_MAX (2, core/include/mt_matter.h:1292), the C6's own
 * constant, and
 * a third declared EVSE fails its create with the established loud line while
 * the composition serves the prefix before it, exactly as the measurement
 * pools do. On this port the same cap holds, and DE608 grew the
 * cluster-object arena to 8,464 B so two fit beside the other energy
 * families at their caps.
 *
 * On the nRF, batch 7 audit section 3.8 is the lead this was written
 * from, and it was written before the const catalogue macros, the object
 * heap, store_walk and
 * batch 8's shape mechanism existed. Everything below is re-verified against
 * the pinned NCS tree and against current main; where the audit and the code
 * disagree the code wins and the disagreement is named at the line. This
 * port's Silicon Labs SDK carries energy-evse-server sources identical to
 * the pinned NCS tree's, so the line citations below hold here too.
 *
 * ---- THE COMPOSITION, verified against the C6's thunk ------------------
 *
 * mk_energy_evse() (platform/esp32c6/main/mt_devtypes.cpp) calls the SDK's
 * energy_evse::create() and then grafts, so the endpoint's real cluster set
 * is the union of four things:
 *
 *   energy_evse::add()      EnergyEvse 0x0099, EnergyEvseMode 0x009D and a
 *                           bare DeviceEnergyManagement 0x0098
 *   feature::charging_preferences::add(), called UNCONDITIONALLY at
 *                           esp_matter_cluster.cpp:3395, OUTSIDE the
 *                           CLUSTER_FLAG_SERVER guard that closes one line
 *                           above it: the four NextCharge* attributes and
 *                           SetTargets/GetTargets/ClearTargets
 *   mt_graft_electrical_sensor(ep, true, ...)   PowerTopology 0x009C,
 *                           ElectricalPowerMeasurement 0x0090 and
 *                           ElectricalEnergyMeasurement 0x0091, with_eem TRUE
 *                           on BOTH variants (EVSE.xml revision 2 mandates a
 *                           composed Electrical Sensor carrying both)
 *   mt_add_dem_triple(ep, FALSE, ...)   the over-delivered DEM rewired with
 *                           its delegate and DeviceEnergyManagementMode
 *                           0x009F, and the 0x050D device type
 *
 * EIGHT clusters, the largest list in the catalogue by one (the water
 * heater's and battery storage's are seven), on both variants. THE AUDIT
 * AND THE CODE DISAGREE ON NOTHING HERE, but one of its remarks does not
 * survive the port: it says the DEM cluster is "kept, not
 * stripped ... relying on esp-matter's idempotent duplicate cluster create".
 * On this port there is no duplicate to reconcile, because nothing creates a
 * bare DEM first: the list below simply declares DEM once, with the
 * report-only attribute set.
 *
 * ---- THE TWO VARIANTS --------------------------------------------------
 *
 * Variant 0 adds SOC reporting, variant 1 does not, and it is a genuine
 * wire-visible difference rather than decoration: with the SOC feature
 * Instance::ValidateTargets() makes targetSoC MANDATORY in every charging
 * target and answers InvalidCommand when it is missing, while without it
 * targetSoC must be absent or exactly 100. evse_targets_apply_locked()
 * enforces the identical rule on the AT path so a host cannot install a
 * schedule a controller's SetTargets would have been refused. Both variants
 * are conformant; the difference is two attributes and one validation rule.
 *
 * ---- WHAT IS NEVER BUILT, AND WHY EACH ONE IS A DELIBERATE OMISSION ----
 *
 * PREF (charging preferences) is UNCONDITIONALLY ON, both variants, because
 * the SDK's own Instance derives the advertised AcceptedCommandList from its
 * feature mask (RetrieveAcceptedCommands, energy-evse-server.cpp:186-211) and
 * this port hands it kChargingPreferences. So the four NextCharge*
 * attributes and SetTargets/GetTargets/ClearTargets exist on every EVSE.
 *
 * V2X IS NEVER BUILT, and this is the iron rule (ARCHITECTURE.md 8.12) in its
 * clearest form yet. Setting the V2X bit would put EnableDischarging in that
 * derived AcceptedCommandList, and the four V2X attributes
 * (DischargingEnabledUntil, MaximumDischargeCurrent, SessionEnergyDischarged
 * and the discharge half of the session) are NOT declared below, so a
 * hand-set bit would advertise a command with no attribute behind it and a
 * capability this firmware has no host story for. On the C6 the rule is
 * "never write FeatureMap, always feature::add()"; here there is no
 * feature::add(), so it inverts into "the mask handed to the Instance and the
 * declared list are one decision", which is what mt_matter_evse_register()'s
 * with_soc argument and this list are.
 *
 * RFID IS NEVER BUILT either. On the C6, feature::rfid::add() sets the bit
 * and creates nothing at all, not even the Rfid event metadata the XML
 * declares, so an advertising endpoint would answer an empty shell to any
 * controller that subscribed. Here it would be one bit with no consequence,
 * which is worse rather than better: a truthful FeatureMap is the whole
 * point.
 *
 * PLUG-AND-CHARGE IS NEVER BUILT: VehicleID is a char_string with no host
 * story, and the delegate's getter answers a null CharSpan for exactly that
 * reason.
 *
 * STARTDIAGNOSTICS IS NEVER ADVERTISED: the Instance is constructed with an
 * EMPTY OptionalCommands mask, so RetrieveAcceptedCommands never appends it
 * and an invoke answers UnsupportedCommand. The delegate implements the pure
 * virtual and can never be reached, which is coherent and needs no fix. The
 * C6 reaches the same wire by a different route (esp-matter declares
 * command::create_start_diagnostics and has zero callers for it).
 *
 * THE THREE OPTIONAL WRITABLES are not declared: UserMaximumChargeCurrent,
 * RandomizationDelayWindow and ApproximateEVEfficiency. esp-matter creates
 * none of them, the Instance gates each one on an OptionalAttributes mask
 * this port constructs empty, and AT+MTMEAS fields 7, 8 and 13 therefore
 * answer +MTERR:4 on every variant, exactly as AT_MT_SPEC.md 3.25 says.
 *
 * ---- THE ATTRIBUTE LISTS ------------------------------------------------
 *
 * EnergyEvse's three answers: code-driven no, CHI-only YES (its seven
 * commands have no generated dispatch; the batch's zap regeneration on
 * the nRF build left
 * IMClusterCommandHandler.cpp and CodeDrivenInitShutdown.cpp byte-identical,
 * as batch 7's audit 5.5 predicted), AAI YES per endpoint (Instance derives
 * from both AttributeAccessInterface and CommandHandlerInterface,
 * energy-evse-server.h:176-186, and its ctor calls both SetEndpointId() and
 * SetInstance() on the delegate). Init() registers the CHI then the AAI, SOFT
 * on both (energy-evse-server.cpp:40-46): no emberAfContainsServer check, no
 * VerifyOrDie, so an ordering mistake here cannot panic the device, unlike
 * the ModeBase alias riding beside it.
 *
 * Instance::Read() serves ALL 23 attributes from the delegate and falls
 * through to ember only for ClusterRevision (:68-133), so every slot below is
 * an INERT SHADOW except ClusterRevision, whose seed is live, and the DE397
 * carve-out in mt_matter_sl.cpp is what makes an AT read answer the same
 * value a subscribed controller sees.
 *
 * SIX of the declarations are metadata-only, the DE407 option-C shape: the
 * three amperage_ma currents, NextChargeRequiredEnergy, BatteryCapacity and
 * SessionEnergyCharged are 8-byte ZCL types that attr_gets_slot() refuses a
 * 4-byte slot, and they are Instance-served, so they take quiet-table rows
 * below rather than shouting at boot.
 *
 * Slots: v0 = State, SupplyState, FaultState, ChargingEnabledUntil,
 * SessionID, SessionDuration, FeatureMap, NextChargeStartTime,
 * NextChargeTargetTime, NextChargeTargetSoC, StateOfCharge and
 * ClusterRevision = 12; v1 drops StateOfCharge = 11. That is the audit's
 * 12/11 exactly.
 *
 * ---- THE BLOCK ---------------------------------------------------------
 *
 * 8 clusters, 31 slots on v0 (12 EVSE + 3 EvseMode + 5 DEM + 3 DEMMode + 2
 * PTOP + 4 EPM + 2 EEM) and TWO mt_mb_store_t, the second type after the RVC
 * to carry two: 32 + 496 + 612 = 1,140 B payload, 1,144 of this port's
 * arena. The widest block in the catalogue (the RVC's is 936 by this
 * port's over-count, 856 exact; battery storage's 848), and an admission
 * candidate like the RVC's (kEvseBlockBytes in mt_devtypes_sl_arena.inc).
 * Variant 1 is 1,124 B payload, 1,128 of arena.
 *
 * ---- EVENTS -------------------------------------------------------------
 *
 * CumulativeEnergyMeasured, from the EEM push path (batch 7a), and nothing
 * else: EVSEs, EVNotDetected, EnergyTransferStarted/Stopped and Fault are all
 * declared by the cluster XML and emitted by nobody, on either platform,
 * because this firmware has no host verb that would say when one happened.
 *
 * Device revision 2 per data_model/1.5/device_types/EVSE.xml:60. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(evseAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::State::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SupplyState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FaultState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::ChargingEnabledUntil::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::CircuitCapacity::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MinimumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MaximumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeStartTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeRequiredEnergy::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetSoC::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::StateOfCharge::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::BatteryCapacity::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionID::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionDuration::Id, ELAPSED_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionEnergyCharged::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Variant 1, without SOC reporting: the same list minus StateOfCharge and
 * BatteryCapacity, the two attributes soc_reporting::add() creates. The
 * FeatureMap seed drops the bit with them, and mt_matter_evse_register()
 * builds the Instance's mask by asking THIS list for StateOfCharge (the
 * create path's second half), so the declared list and the Instance cannot
 * disagree: one is derived from the other. The SEED is the one of the three
 * that is not, being a table row keyed on the variant, and it agrees because
 * the registry selects the list by that same variant; if a future row ever
 * breaks that, the seed is the copy that goes wrong, and it is an inert
 * shadow (the Instance serves FeatureMap from mFeature), so it would go
 * wrong quietly. That is the DEM report-only list's rule with its one real
 * weak point named rather than glossed. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(evseNoSocAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::State::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SupplyState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FaultState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::ChargingEnabledUntil::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::CircuitCapacity::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MinimumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MaximumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeStartTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeRequiredEnergy::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetSoC::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionID::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionDuration::Id, ELAPSED_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionEnergyCharged::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* The five commands the Instance really advertises under
 * kChargingPreferences, and no more: Disable and EnableCharging are
 * unconditional, SetTargets/GetTargets/ClearTargets ride PREF, and
 * EnableDischarging (V2X) and StartDiagnostics (an optional command with an
 * empty mask) are absent from BOTH this list and the Instance's own derived
 * one. The two must agree: RetrieveAcceptedCommands is what a controller
 * reads, and a declared-but-unadvertised command is the quiet incoherence the
 * DE399 truthfulness discipline exists to prevent. */
constexpr CommandId kEvseIncoming[] = { EnergyEvse::Commands::Disable::Id,
                                        EnergyEvse::Commands::EnableCharging::Id,
                                        EnergyEvse::Commands::SetTargets::Id,
                                        EnergyEvse::Commands::GetTargets::Id,
                                        EnergyEvse::Commands::ClearTargets::Id,
                                        kInvalidCommandId };

/* GetTargetsResponse is the port's second outgoing command list, after the
 * OperationalState trio's, and unlike that one it is not a DE399 disclosure:
 * GetTargets really does answer with it (HandleGetTargets calls
 * AddResponse()), so declaring it is the truthful thing to do. */
constexpr CommandId kEvseOutgoing[] = { EnergyEvse::Commands::GetTargetsResponse::Id,
                                        kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(energyEvseClusters)
DECLARE_DYNAMIC_CLUSTER(EnergyEvse::Id, evseAttrs, ZAP_CLUSTER_MASK(SERVER), kEvseIncoming,
                        kEvseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(EnergyEvseMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demReportOnlyAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1: the identical eight clusters with the no-SOC EnergyEvse list.
 * Nothing else moves, which is why the two variants draw the same six
 * per-endpoint objects and differ by 16 block bytes. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(energyEvseNoSocClusters)
DECLARE_DYNAMIC_CLUSTER(EnergyEvse::Id, evseNoSocAttrs, ZAP_CLUSTER_MASK(SERVER), kEvseIncoming,
                        kEvseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(EnergyEvseMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demReportOnlyAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(energyEvseEndpoint, energyEvseClusters);
HEARTH_DECLARE_CONST_ENDPOINT(energyEvseNoSocEndpoint, energyEvseNoSocClusters);

/* Three device types on one endpoint, the same span on both variants: the
 * sensor graft and the DEM triple are unconditional here, unlike the water
 * heater's (whose variant 1 loses the graft and its 0x0510 id together), so
 * this row needs no device_types_v1. 0x050C revision 2 per EVSE.xml:60,
 * 0x0510 revision 1 and 0x050D revision 3 matching their standalone rows. */
constexpr EmberAfDeviceType kEnergyEvseTypes[] = { { 0x050C, 2 }, { 0x0510, 1 }, { 0x050D, 3 } };

/*
 * The DE407 quiet table (nRF mt_devtypes_zephyr.cpp 4924-4988): the
 * specific (cluster, attribute) pairs whose over-wide scalar declarations
 * are DELIBERATE metadata-only 64-bit declarations, each proven
 * Instance-served, admitted one pair at a time and NEVER by type: a future
 * ember-served 64-bit attribute must keep failing loudly in seed_slots()
 * (declared but slotless, it would answer a bare ERROR over AT). The
 * declarations are refused a slot by attr_gets_slot()'s size clause; this
 * table only silences the boot shout for the proven pairs. Catalogue batch
 * 7a-1 brought the seven ElectricalPowerMeasurement rows, 7a-2 the two
 * DeviceEnergyManagement rows, 7b the WaterHeaterManagement row and the
 * EVSE round the six EnergyEvse rows.
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
    /* WaterHeaterManagement (batch 7b): the one energy_mwh scalar, served
     * by Instance::Read()'s own case from the HearthWhmDelegate cache
     * (water-heater-management-server.cpp:128-133; the carve-out serves
     * the AT side). Declared 8 B in whmAttrs, variant 0 only. */
    { WaterHeaterManagement::Id, WaterHeaterManagement::Attributes::EstimatedHeatRequired::Id },
    /* EnergyEvse (the EVSE round): the six 8-byte scalars, served by
     * Instance::Read()'s own cases from the HearthEvseDelegate cache
     * (energy-evse-server.cpp:73-127; the k_instance_served carve-out serves
     * the AT side). Three amperage_ma currents that are declared on both
     * variants, NextChargeRequiredEnergy which rides unconditional PREF,
     * SessionEnergyCharged which is mandatory, and BatteryCapacity which is
     * variant 0 only; a variant-1 endpoint never declares that last one, so
     * its row is simply never consulted there. Admitted one pair at a time
     * per the DE407 ruling, never by type. */
    { EnergyEvse::Id, EnergyEvse::Attributes::CircuitCapacity::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::MinimumChargeCurrent::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::MaximumChargeCurrent::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeRequiredEnergy::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::BatteryCapacity::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::SessionEnergyCharged::Id },
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
