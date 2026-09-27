/*
 * mt_devtypes_sl_tables_b7.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 7a-1 (nRF 2437-2723): the
 * measurement family, the electrical sensor (0x0510) and electrical meter
 * (0x0514) with their power-only variants, and the heat pump (0x0309) and
 * solar power (0x0017) on the wired electrical sensor lists.
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

/*
 * The DE407 quiet table (nRF mt_devtypes_zephyr.cpp 4924-4988): the
 * specific (cluster, attribute) pairs whose over-wide scalar declarations
 * are DELIBERATE metadata-only 64-bit declarations, each proven
 * Instance-served, admitted one pair at a time and NEVER by type: a future
 * ember-served 64-bit attribute must keep failing loudly in seed_slots()
 * (declared but slotless, it would answer a bare ERROR over AT). The
 * declarations are refused a slot by attr_gets_slot()'s size clause; this
 * table only silences the boot shout for the proven pairs. Catalogue batch
 * 7a-1 brings the seven ElectricalPowerMeasurement rows; the nRF's
 * DeviceEnergyManagement (7a-2), WaterHeaterManagement (7b) and EnergyEvse
 * (the EVSE round) rows arrive with their batches.
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
