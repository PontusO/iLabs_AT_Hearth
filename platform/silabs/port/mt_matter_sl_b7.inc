/*
 * mt_matter_sl_b7.inc: a fragment of mt_matter_sl.cpp, NOT a compile unit.
 *
 * Holds bridge code: catalogue batch 7a-1 (nRF mt_matter_zephyr.cpp
 * 4581-5104 and 5106-5346: the measurement family's ElectricalPowerMeasurement
 * and PowerTopology delegates and pools, the ElectricalEnergyMeasurement
 * reserve and register, the EPM live read and the AT+MTMEAS bridge), and
 * catalogue batch 7a-2 (5347-5639, 5641-6049 and 6051-6324: the utility
 * meter's MeterIdentification pool, HearthDemDelegate, the DEM pool and
 * register, its AT+MTMEAS and AT+MTDEMCAP bridges and its live read), and
 * catalogue batch 7b (6326-6936: HearthWhmDelegate, the WHM pool and
 * register, its AT+MTMEAS bridge and its live read),
 * included at the end of mt_matter_sl.cpp after mt_matter_sl_b5.inc, so the
 * whole is still one translation unit. Do not add it to hearth.slcp.
 */

/*
 * ============ catalogue batch 7a: the energy foundation =================
 *
 * ElectricalPowerMeasurement (0x0090), PowerTopology (0x009C) and
 * ElectricalEnergyMeasurement (0x0091), the three clusters behind the
 * Electrical Sensor (0x0510) and Electrical Meter (0x0514) device types and
 * every later energy type's sensor graft. The AT surface is AT+MTMEAS
 * (mt_matter_meas_set() below, AT_MT_SPEC.md 3.25); the storage ruling is
 * DE407 option C (no arena slot widening anywhere: the 64-bit attributes are
 * metadata-only declarations served from the delegate caches here, and
 * AT+MTATTR reads reach them through the k_instance_served carve-out above).
 *
 * Three clusters, two models, one bridge, the C6's shape
 * (platform/esp32c6/main/main.cpp, the energy round A block) with one
 * structural delta: on the C6, esp-matter's own delegate-init callbacks
 * construct the EPM and PowerTopology Instances at endpoint enable
 * (ElectricalPowerMeasurementDelegateInitCB and PowerTopologyDelegateInitCB,
 * esp_matter_delegate_callbacks.cpp); this platform has no such layer, so
 * mt_matter_meas_delegate_set_endpoint() below placement-constructs and
 * Init()s both Instances itself, the ModeBase handout's shape. Both Init()s
 * are SOFT (AttributeAccessInterfaceRegistry registration only, no
 * emberAfContainsServer check and no VerifyOrDie anywhere in this batch's
 * energy clusters: electrical-power-measurement-server.cpp:43-47,
 * power-topology-server.cpp:42-46), so a misordered call cannot panic; it
 * would only leave the endpoint unserved, which is why the setter checks and
 * logs the return like every other handout in this file.
 *
 *   ElectricalPowerMeasurement is PULL-model: the Instance is an
 *   AttributeAccessInterface answering every read from the Delegate's
 *   Get*() methods (its Read() has a case for every attribute the cluster
 *   defines and no default arm, electrical-power-measurement-server.cpp:
 *   65-221, so of the declared set only ClusterRevision ever reaches
 *   ember). The host's pushed values live in HearthEpmDelegate members and
 *   one MatterReportingAttributeChangeCallback per applied field makes
 *   subscriptions fire.
 *
 *   PowerTopology with the NodeTopology feature needs only a constructible
 *   Delegate: its two pure virtuals are the endpoint-list iterators backing
 *   AvailableEndpoints/ActiveEndpoints, which exist only under the SET/TREE
 *   features, so HearthPtopDelegate answers both PROVIDER_LIST_EXHAUSTED
 *   and is never actually consulted.
 *
 *   ElectricalEnergyMeasurement is PUSH-model free functions:
 *   NotifyCumulativeEnergyMeasured() stores the timestamped structs into
 *   the server's own per-endpoint MeasurementData AND emits the
 *   CumulativeEnergyMeasured event in one call (ElectricalEnergyMeasurement
 *   Cluster.cpp:191-218, the LogEvent at :207), this port's second
 *   event emitter after batch 5's InitialPress. Its per-endpoint storage
 *   is the Hearth SDK patch's pool (sdk-patches, HEARTH_SDK_PATCH_REV 3):
 *   CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES (8,
 *   src/CHIPProjectConfig.h) entries of 496 B, 3,968 B of .bss measured in
 *   the catalogue batch 7a-1 build (the stock table would be 17 x 496 =
 *   8,432 B), charged the moment this translation unit is referenced,
 *   whether or not an EEM endpoint is ever declared. A lookup for an
 *   endpoint that holds no pool entry returns nullptr, which is why
 *   mt_matter_eem_reserve() gates the pool before create.
 *
 * Accuracy: both electrical clusters must serve a MeasurementAccuracyStruct.
 * This firmware is a bridge and cannot know the host's real metering
 * hardware; the figures served (0.1% to 1%, 0.5% typical, one range over the
 * full XML value span) are the middle band of the SDK reference
 * implementation's table, the C6's identical documented assumption
 * (AT_MT_SPEC.md 3.25, "Accuracy is a fixed firmware constant").
 */

/* The XML value bounds mt_matter_meas_set() validates against
 * (electrical-power-measurement-cluster.xml /
 * electrical-energy-measurement-cluster.xml, pinned tree). Not available
 * from any generated header, so cited literals rather than transcribed
 * enum values, the C6's identical constants. */
static constexpr int64_t kMeasValueAbsMax = 4611686018427387904LL; /* +-2^62 */
static constexpr int64_t kMeasFreqMax     = 1000000;   /* Frequency, mHz  */
static constexpr int64_t kMeasPfAbsMax    = 10000;     /* PowerFactor, 1/100 % */

static const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type
    s_meas_power_accuracy_ranges[] = {
    {
        .rangeMin       = -kMeasValueAbsMax,
        .rangeMax       = kMeasValueAbsMax,
        .percentMax     = chip::MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = chip::MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = chip::MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
};

/* The one mandatory EPM accuracy entry: ActivePower, the cluster's one
 * mandatory measured value. */
static const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type
    s_meas_epm_accuracy = {
    .measurementType  = chip::app::Clusters::ElectricalPowerMeasurement::MeasurementTypeEnum::kActivePower,
    .measured         = true,
    .minMeasuredValue = -kMeasValueAbsMax,
    .maxMeasuredValue = kMeasValueAbsMax,
    .accuracyRanges   = chip::app::DataModel::List<
        const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(
        s_meas_power_accuracy_ranges),
};

static const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type
    s_meas_energy_accuracy_ranges[] = {
    {
        .rangeMin       = 0,
        .rangeMax       = kMeasValueAbsMax,
        .percentMax     = chip::MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = chip::MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = chip::MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
};

/* Same tolerance figures as the EPM entry above, but typed kElectricalEnergy
 * over the energy value span: EEM's Accuracy attribute describes the energy
 * measurement itself, so serving the ActivePower-typed struct verbatim would
 * satisfy the letter of "the same accuracy" while violating the attribute's
 * meaning (the C6's own note, kept). */
static const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyStruct::Type
    s_meas_eem_accuracy = {
    .measurementType  = chip::app::Clusters::ElectricalEnergyMeasurement::MeasurementTypeEnum::kElectricalEnergy,
    .measured         = true,
    .minMeasuredValue = 0,
    .maxMeasuredValue = kMeasValueAbsMax,
    .accuracyRanges   = chip::app::DataModel::List<
        const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(
        s_meas_energy_accuracy_ranges),
};

/*
 * The EPM delegate: a store for host-pushed values, all null until the host
 * first pushes them (AT_MT_SPEC.md 3.25's null-until-pushed contract), plus
 * the fixed answers the pull-model server needs. Mirrored from the C6's
 * HearthEpmDelegate (main.cpp) with no semantic change; base-class
 * SetEndpointId()/mEndpointId (electrical-power-measurement-server.h:36)
 * carry the endpoint, set ONCE, by the Instance constructor that
 * mt_matter_meas_delegate_set_endpoint() runs (the ctor's
 * mDelegate.SetEndpointId(aEndpointId), server.h:107): the setter itself
 * makes no SetEndpointId() call of its own for this pool, so endpoint()
 * and meas_epm_for() resolve only from the construction onward, which is
 * always before the first push can name the endpoint. The PTOP branch
 * differs: its set_endpoint() writes the port delegate's OWN member (the
 * base PowerTopology::Delegate has none), and the Instance ctor there
 * sets nothing back.
 */
class HearthEpmDelegate : public chip::app::Clusters::ElectricalPowerMeasurement::Delegate
{
public:
    /* Host-pushed values, written only by mt_matter_meas_set() below. */
    chip::app::DataModel::Nullable<int64_t> m_voltage, m_active_current, m_active_power,
        m_frequency, m_power_factor, m_rms_voltage, m_rms_current;

    chip::EndpointId endpoint() const { return mEndpointId; }

    chip::app::Clusters::ElectricalPowerMeasurement::PowerModeEnum GetPowerMode() override
    {
        /* kAc, agreeing with the AlternatingCurrent feature every EPM
         * endpoint this port builds advertises AND with the PowerMode
         * arena seed (mt_devtypes_sl.cpp), which the AT read answers. */
        return chip::app::Clusters::ElectricalPowerMeasurement::PowerModeEnum::kAc;
    }
    uint8_t GetNumberOfMeasurementTypes() override { return 1; }

    /* Accuracy: exactly one entry (ActivePower, the mandatory one), backed
     * by a static const table, so the Start/End read brackets have nothing
     * to lock (the SDK reference delegate's own reasoning). */
    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(
        uint8_t index,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type &accuracy) override
    {
        if (index > 0) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        accuracy = s_meas_epm_accuracy;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    /* Ranges and harmonics: empty lists (the attributes are not declared
     * on any endpoint this port builds; these satisfy the pure-virtual
     * contract). */
    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementRangeStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    /* The seven field-table attributes, served from the pushed store. */
    chip::app::DataModel::Nullable<int64_t> GetVoltage() override { return m_voltage; }
    chip::app::DataModel::Nullable<int64_t> GetActiveCurrent() override { return m_active_current; }
    chip::app::DataModel::Nullable<int64_t> GetActivePower() override { return m_active_power; }
    chip::app::DataModel::Nullable<int64_t> GetFrequency() override { return m_frequency; }
    chip::app::DataModel::Nullable<int64_t> GetPowerFactor() override { return m_power_factor; }
    chip::app::DataModel::Nullable<int64_t> GetRMSVoltage() override { return m_rms_voltage; }
    chip::app::DataModel::Nullable<int64_t> GetRMSCurrent() override { return m_rms_current; }

    /* Everything the field table does not carry: permanently null. These
     * attributes are never declared on any endpoint this port builds, so
     * these answers exist only to satisfy the pure-virtual contract. */
    chip::app::DataModel::Nullable<int64_t> GetReactiveCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetReactivePower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetNeutralCurrent() override { return {}; }
};

/*
 * The PowerTopology delegate: NodeTopology only, so both pure virtuals
 * answer "empty" and the object exists purely so the Instance has something
 * to hold a reference to. The endpoint member mirrors the other pools'
 * shape for the by-endpoint lookup; nothing else reads it.
 */
class HearthPtopDelegate : public chip::app::Clusters::PowerTopology::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    CHIP_ERROR GetAvailableEndpointAtIndex(size_t, chip::EndpointId &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR GetActiveEndpointAtIndex(size_t, chip::EndpointId &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/*
 * The two pools, MT_MEAS_MAX (8, core/include/mt_matter.h:828, the C6's
 * depth: cite-checked, "measurement-capable endpoints per composition")
 * each, deliberately NOT kServiceableEndpoints: the DE407 ruling pins the
 * C6 pool constants, and eight measurement-capable endpoints already
 * exceeds any host this firmware targets. Exhaustion aborts the create
 * before anything is spent (mt_devtype_create()'s two-halves rule).
 *
 * Each pool is a table of POINTERS into the cluster-object arena (the
 * nRF's heap since its memory reclaim round A), one block per delegate with the raw storage for
 * its Instance behind it: the four-argument Instance ctor needs create-time
 * values, so the Instances are placement-constructed exactly once per block
 * in mt_matter_meas_delegate_set_endpoint() and never destroyed (no
 * teardown path on this platform; AT+MTEP edits apply by reboot), the
 * standing allocate-only pool policy. MT_MEAS_MAX is unchanged and is still
 * checked first.
 */
static HearthEpmDelegate  *s_meas_epm_delegates[MT_MEAS_MAX];
static size_t             s_meas_epm_next;
static HearthPtopDelegate *s_meas_ptop_delegates[MT_MEAS_MAX];
static size_t             s_meas_ptop_next;

/* The measurement pairs, 148 B (EPM) and 36 B (PowerTopology) on the nRF's
 * measurement; these pin the MG24's arena costs. One of each per
 * measurement endpoint, admitted up to MT_MEAS_MAX (graph DE584). */
static_assert(hearth_arena_cost(obj_pair_bytes<HearthEpmDelegate,
                                                chip::app::Clusters::ElectricalPowerMeasurement::Instance>()) == 152,
              "the EPM pair changed size; redo the measurement admission arithmetic");
static_assert(hearth_arena_cost(obj_pair_bytes<HearthPtopDelegate,
                                                chip::app::Clusters::PowerTopology::Instance>()) == 40,
              "the PowerTopology pair changed size; redo the measurement admission arithmetic");
static_assert(MT_MEAS_MAX * (152 + 40) <= HEARTH_OBJ_ARENA_BYTES,
              "MT_MEAS_MAX measurement endpoints' objects no longer fit the cluster-object arena (DE584)");

extern "C" void *mt_matter_epm_delegate_alloc(void)
{
    if (s_meas_epm_next >= MT_MEAS_MAX) {
        return nullptr;
    }
    HearthEpmDelegate *d =
        obj_pair_new<HearthEpmDelegate,
                     chip::app::Clusters::ElectricalPowerMeasurement::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    s_meas_epm_delegates[s_meas_epm_next++] = d;
    return d;
}

extern "C" void *mt_matter_ptop_delegate_alloc(void)
{
    if (s_meas_ptop_next >= MT_MEAS_MAX) {
        return nullptr;
    }
    HearthPtopDelegate *d =
        obj_pair_new<HearthPtopDelegate, chip::app::Clusters::PowerTopology::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    s_meas_ptop_delegates[s_meas_ptop_next++] = d;
    return d;
}

/*
 * The second half of the measurement handout, and where both Instances are
 * BORN on this platform (the structural delta from the C6 in the section
 * comment above). One setter serves both pools, mt_matter.h's documented
 * contract: the void* is matched against each pool's own objects, so
 * mt_devtypes_sl.cpp needs neither a second name nor any knowledge of
 * which class it is holding.
 *
 * Must run below a successful emberAfSetDynamicEndpoint(): not for safety
 * (both Init()s are soft, the section comment), but because registering the
 * AAI for an endpoint that then fails to enable would strand a registration
 * nothing serves. Instance feature masks are fixed here and must agree with
 * the FeatureMap arena seeds in mt_devtypes_sl.cpp, which the audit
 * notes on epmAttrs/ptopAttrs cross-reference: EPM AlternatingCurrent
 * (0x2) with the six optional attributes the declared metadata carries
 * (Voltage, ActiveCurrent, RMSVoltage, RMSCurrent, Frequency, PowerFactor;
 * ActivePower is mandatory and carries no optional-attribute bit), and
 * PowerTopology NodeTopology (0x1) with no optional attributes, so the
 * endpoint-list reads genuinely do not exist.
 */
extern "C" void mt_matter_meas_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    using namespace chip::app::Clusters;
    for (size_t i = 0; i < s_meas_epm_next; i++) {
        HearthEpmDelegate *d = s_meas_epm_delegates[i];
        if (d == delegate) {
            auto *inst = new (obj_inst_storage<HearthEpmDelegate,
                                               ElectricalPowerMeasurement::Instance>(d))
                ElectricalPowerMeasurement::Instance(
                ep, *d,
                chip::BitMask<ElectricalPowerMeasurement::Feature>(
                    ElectricalPowerMeasurement::Feature::kAlternatingCurrent),
                chip::BitMask<ElectricalPowerMeasurement::OptionalAttributes>(
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeVoltage,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeActiveCurrent,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSVoltage,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSCurrent,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeFrequency,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributePowerFactor));
            CHIP_ERROR err = inst->Init();
            if (err != CHIP_NO_ERROR) {
                HEARTH_LOGE("matter", "EPM Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                        "; every ElectricalPowerMeasurement read on it will fail",
                        (unsigned)ep, err.Format());
            }
            return;
        }
    }
    for (size_t i = 0; i < s_meas_ptop_next; i++) {
        HearthPtopDelegate *d = s_meas_ptop_delegates[i];
        if (d == delegate) {
            d->set_endpoint(ep);
            auto *inst =
                new (obj_inst_storage<HearthPtopDelegate, PowerTopology::Instance>(d))
                    PowerTopology::Instance(
                ep, *d, chip::BitMask<PowerTopology::Feature>(PowerTopology::Feature::kNodeTopology),
                chip::BitMask<PowerTopology::OptionalAttributes>());
            CHIP_ERROR err = inst->Init();
            if (err != CHIP_NO_ERROR) {
                HEARTH_LOGE("matter", "PowerTopology Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                        "; every PowerTopology read on it will fail",
                        (unsigned)ep, err.Format());
            }
            return;
        }
    }
    HEARTH_LOGE("matter", "meas_delegate_set_endpoint: pointer belongs to neither pool (endpoint %u)",
            (unsigned)ep);
}

/* The pool lookup the AT+MTMEAS EPM branch and the DE397 live reader use. */
static HearthEpmDelegate *meas_epm_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_meas_epm_next; i++) {
        if (s_meas_epm_delegates[i]->endpoint() == ep) {
            return s_meas_epm_delegates[i];
        }
    }
    return nullptr;
}

/*
 * Per-EEM-endpoint registration, called by mt_devtype_create() below its
 * successful emberAfSetDynamicEndpoint() for every endpoint whose declared
 * cluster list carries ElectricalEnergyMeasurement. The port's
 * HearthEemInitCB equivalent (C6 main.cpp:4066-4088), two jobs:
 *
 *   1. Once, on the first EEM endpoint of the boot: construct and Init()
 *      the server's ONE wildcard ElectricalEnergyMeasurementAttrAccess.
 *      Nothing in the SDK ever registers it (Init() has no caller in the
 *      pinned tree), so without this every EEM attribute read answers
 *      Failure while the ember metadata advertises the attributes, the
 *      ARCHITECTURE.md 8.6 disease in its serving-path flavour. THE MASK IS
 *      LOAD-BEARING: this one object answers FeatureMap for EVERY EEM
 *      endpoint (its Read() encodes mFeature,
 *      ElectricalEnergyMeasurementCluster.cpp:66-67), so it is exactly
 *      Imported|Exported|Cumulative, the three bits every EEM cluster list
 *      in mt_devtypes_sl.cpp seeds and AT_MT_SPEC.md 3.25 promises; a
 *      different mask here would make FeatureMap lie on every EEM endpoint
 *      and CumulativeEnergyImported vanish behind its feature gate
 *      (Read()'s VerifyOrReturnError, :75-80). Static storage plus
 *      placement-new, never destroyed, the standing pool policy (the C6
 *      uses a one-time heap new; this platform avoids the heap).
 *   2. Per endpoint: serve the Accuracy attribute via
 *      SetMeasurementAccuracy(), which resolves the endpoint through
 *      emberAfGetClusterServerEndpointIndex() and therefore needs the
 *      endpoint configured and enabled first, which the call site's
 *      below-the-successful-create placement guarantees.
 */

/*
 * DE397 live read for the seven EPM push fields, dispatched from
 * mt_matter_attr_read() under its StackLock: the delegate cache is the
 * served truth, the metadata-only 64-bit declarations have no arena slot
 * to answer from. All seven are signed (the electrical-measurement alias
 * family maps to INT64S, attr_type_info()'s doc note above); a null value
 * (never pushed) answers MT_ATTR_ERR_TYPE with the flag already set, the
 * no-null-literal contract every nullable read in this file follows.
 */
static int mt_epm_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    namespace EPM = chip::app::Clusters::ElectricalPowerMeasurement;
    if (is_unsigned) {
        *is_unsigned = false;
    }
    HearthEpmDelegate *d = meas_epm_for(ep);
    if (d == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    const chip::app::DataModel::Nullable<int64_t> *v = nullptr;
    switch (attr) {
    case EPM::Attributes::Voltage::Id:       v = &d->m_voltage; break;
    case EPM::Attributes::ActiveCurrent::Id: v = &d->m_active_current; break;
    case EPM::Attributes::ActivePower::Id:   v = &d->m_active_power; break;
    case EPM::Attributes::RMSVoltage::Id:    v = &d->m_rms_voltage; break;
    case EPM::Attributes::RMSCurrent::Id:    v = &d->m_rms_current; break;
    case EPM::Attributes::Frequency::Id:     v = &d->m_frequency; break;
    case EPM::Attributes::PowerFactor::Id:   v = &d->m_power_factor; break;
    default:
        /* Unreachable: only the seven k_instance_served rows route here. */
        return MT_ATTR_ERR_FAILED;
    }
    if (v->IsNull()) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = v->Value();
    return MT_ATTR_OK;
}


/*
 * The EEM measurement-table capacity gate, the split-reserve shape every
 * other pool in this port uses (the EPM and PowerTopology blocks in
 * mt_devtypes_sl.cpp, and the C6's mt_matter_evse_reserve()).
 *
 * It exists because the SDK patch in ../sdk-patches replaced
 * ElectricalEnergyMeasurement's gMeasurements, which used to be as long as
 * the whole dynamic endpoint space and so had a slot for every endpoint
 * this port could possibly create, with a pool of
 * CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES. A pool has a
 * bottom, and reaching it inside the SDK is SILENT on the AT wire:
 * SetMeasurementAccuracy() answers CHIP_ERROR_INVALID_ARGUMENT,
 * mt_matter_eem_register() logs and continues, the composition applies OK,
 * and a controller then gets a Failure reading the MANDATORY Accuracy
 * attribute of a cluster the endpoint declares. A device that says yes and
 * then serves a broken endpoint is exactly what this port refuses to do
 * everywhere else, so the count is checked HERE, before anything is built,
 * and the rebuild stops with the constant to raise named in the log.
 *
 * Unreachable today, and deliberately kept anyway. Every one of the six
 * device types whose cluster list carries ElectricalEnergyMeasurement also
 * carries ElectricalPowerMeasurement (the nRF's mt_devtypes_zephyr.cpp:2564/2566,
 * 2582/2584, 2682/2684, 3120/3122, 3269/3271, 3287/3289), so the
 * MT_MEAS_MAX EPM delegate pool is exhausted first and the static_assert
 * below keeps the EEM pool at least as deep. A seventh EEM-bearing type
 * declared WITHOUT EPM would break that coupling silently; this gate is
 * what makes it break loudly instead.
 */
static_assert(CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES >= MT_MEAS_MAX,
              "the EEM measurement pool must be at least as deep as the EPM delegate pool, "
              "or an endpoint this port accepts cannot serve its Accuracy attribute; raise "
              "CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES in "
              "src/CHIPProjectConfig.h");

static uint8_t s_eem_reserved;

extern "C" bool mt_matter_eem_reserve(void)
{
    if (s_eem_reserved >= CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES) {
        return false;
    }
    s_eem_reserved++;
    return true;
}

extern "C" void mt_matter_eem_register(uint16_t ep)
{
    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;

    alignas(ElectricalEnergyMeasurementAttrAccess) static uint8_t
        s_eem_attr_access_storage[sizeof(ElectricalEnergyMeasurementAttrAccess)];
    static bool s_eem_registered;

    if (!s_eem_registered) {
        auto *aai = new (s_eem_attr_access_storage) ElectricalEnergyMeasurementAttrAccess(
            chip::BitMask<Feature, uint32_t>(Feature::kImportedEnergy, Feature::kExportedEnergy,
                                             Feature::kCumulativeEnergy),
            chip::BitMask<OptionalAttributes, uint32_t>());
        CHIP_ERROR err = aai->Init();
        if (err != CHIP_NO_ERROR) {
            HEARTH_LOGE("matter", "EEM AttrAccess registration failed: %" CHIP_ERROR_FORMAT
                    "; every ElectricalEnergyMeasurement read will fail",
                    err.Format());
        }
        s_eem_registered = true;
    }

    CHIP_ERROR err = SetMeasurementAccuracy(ep, s_meas_eem_accuracy);
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "EEM SetMeasurementAccuracy failed on endpoint %u: %" CHIP_ERROR_FORMAT,
                (unsigned)ep, err.Format());
    }
}


/*
 * The AT+MTMEAS bridge (AT_MT_SPEC.md 3.25). Grammar (pair count bounds,
 * per-field signedness at parse) is core's business (cmd_mtmeas and
 * mt_meas_field_signed(), mt_at.c); this bridge owns everything that needs
 * the data model: endpoint and cluster lookup, per-field range validation
 * against the cluster XML bounds, and the atomic apply. Two passes, the
 * family's hard contract: every pair is validated before any pair is
 * applied, so a bad third pair leaves the first two unapplied.
 *
 * Cluster admission WAS a batch boundary and is not one any more: all five
 * push-served ids the spec names are served here, 0x0090, 0x0091 and 0x0098
 * (batch 7a), 0x0094 (batch 7b) and 0x0099 (the EVSE round), so the list
 * below is now the spec's own set rather than a subset of it. An id outside
 * it still answers MT_ATTR_ERR_CLUSTER.
 */
/* The DeviceEnergyManagement (0x0098) and WaterHeaterManagement (0x0094)
 * branch bodies: defined in their sections below, after HearthDemDelegate
 * and HearthWhmDelegate and their pools exist to look into. Both run
 * under the StackLock mt_matter_meas_set() already holds. */
static int mt_meas_dem_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count);
static int mt_meas_whm_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count);
/* The EnergyEvse (0x0099) branch body, defined beside HearthEvseDelegate
 * and its pool in mt_matter_sl_evse.inc; it runs under the StackLock this
 * function holds. */
static int mt_meas_evse_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                              uint8_t count);
extern "C" int mt_matter_meas_set(uint16_t ep, uint32_t cluster, const uint8_t *fields,
                                  const int64_t *values, uint8_t count)
{
    using namespace chip::app::Clusters;
    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (cluster != ElectricalPowerMeasurement::Id && cluster != ElectricalEnergyMeasurement::Id &&
        cluster != DeviceEnergyManagement::Id && cluster != WaterHeaterManagement::Id &&
        cluster != EnergyEvse::Id) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (!emberAfContainsServer(ep, cluster)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1) {
        /* Defensive; mt_at.c never passes an empty list. */
        return MT_ATTR_ERR_FAILED;
    }

    if (cluster == DeviceEnergyManagement::Id) {
        return mt_meas_dem_apply(ep, fields, values, count);
    }
    if (cluster == WaterHeaterManagement::Id) {
        return mt_meas_whm_apply(ep, fields, values, count);
    }
    if (cluster == EnergyEvse::Id) {
        return mt_meas_evse_apply(ep, fields, values, count);
    }
    if (cluster == ElectricalPowerMeasurement::Id) {
        HearthEpmDelegate *d = meas_epm_for(ep);
        if (d == nullptr) {
            /* Cluster present but no pool slot serves this endpoint: cannot
             * happen once the boot rebuild has run; defensive, the pool
             * bridges' standing answer. */
            return MT_ATTR_ERR_FAILED;
        }

        /* Pass 1: validate everything. */
        for (uint8_t i = 0; i < count; i++) {
            switch (fields[i]) {
            case MT_MEAS_F_VOLTAGE:
            case MT_MEAS_F_ACTIVE_CURRENT:
            case MT_MEAS_F_ACTIVE_POWER:
            case MT_MEAS_F_RMS_VOLTAGE:
            case MT_MEAS_F_RMS_CURRENT:
                if (values[i] < -kMeasValueAbsMax || values[i] > kMeasValueAbsMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            case MT_MEAS_F_FREQUENCY:
                if (values[i] < 0 || values[i] > kMeasFreqMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            case MT_MEAS_F_POWER_FACTOR:
                if (values[i] < -kMeasPfAbsMax || values[i] > kMeasPfAbsMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            default:
                return MT_ATTR_ERR_VALUE;
            }
        }

        /* Pass 2: apply, one subscription report per applied field. */
        for (uint8_t i = 0; i < count; i++) {
            chip::app::DataModel::Nullable<int64_t> v =
                chip::app::DataModel::MakeNullable(values[i]);
            uint32_t attr_id;
            switch (fields[i]) {
            case MT_MEAS_F_VOLTAGE:
                d->m_voltage = v;
                attr_id = ElectricalPowerMeasurement::Attributes::Voltage::Id;
                break;
            case MT_MEAS_F_ACTIVE_CURRENT:
                d->m_active_current = v;
                attr_id = ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id;
                break;
            case MT_MEAS_F_ACTIVE_POWER:
                d->m_active_power = v;
                attr_id = ElectricalPowerMeasurement::Attributes::ActivePower::Id;
                break;
            case MT_MEAS_F_FREQUENCY:
                d->m_frequency = v;
                attr_id = ElectricalPowerMeasurement::Attributes::Frequency::Id;
                break;
            case MT_MEAS_F_POWER_FACTOR:
                d->m_power_factor = v;
                attr_id = ElectricalPowerMeasurement::Attributes::PowerFactor::Id;
                break;
            case MT_MEAS_F_RMS_VOLTAGE:
                d->m_rms_voltage = v;
                attr_id = ElectricalPowerMeasurement::Attributes::RMSVoltage::Id;
                break;
            default: /* MT_MEAS_F_RMS_CURRENT, pass 1 admits nothing else */
                d->m_rms_current = v;
                attr_id = ElectricalPowerMeasurement::Attributes::RMSCurrent::Id;
                break;
            }
            MatterReportingAttributeChangeCallback(ep, ElectricalPowerMeasurement::Id, attr_id);
        }
        return MT_ATTR_OK;
    }

    /* ---- ElectricalEnergyMeasurement ---- */

    /* Pass 1: validate everything. Values are cumulative mWh counters,
     * unsigned on the wire; anything negative here is either a negative
     * input or a u64 pattern above INT64_MAX, both outside the XML's
     * 0..2^62. */
    for (uint8_t i = 0; i < count; i++) {
        if (fields[i] != MT_ENERGY_F_IMPORTED && fields[i] != MT_ENERGY_F_EXPORTED) {
            return MT_ATTR_ERR_VALUE;
        }
        if (values[i] < 0 || values[i] > kMeasValueAbsMax) {
            return MT_ATTR_ERR_VALUE;
        }
    }

    ElectricalEnergyMeasurement::MeasurementData *data =
        ElectricalEnergyMeasurement::MeasurementDataForEndpoint(ep);
    if (data == nullptr) {
        /* Reachable only for an endpoint that never took a pool entry;
         * mt_matter_eem_reserve() gates the pool before create (the
         * section comment above), so this is defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    bool    have_imp = false, have_exp = false;
    int64_t imp_val = 0, exp_val = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (fields[i] == MT_ENERGY_F_IMPORTED) {
            have_imp = true;
            imp_val  = values[i]; /* duplicate fields: last one wins */
        } else {
            have_exp = true;
            exp_val  = values[i];
        }
    }

    /* Timestamp policy (AT_MT_SPEC.md 3.25:2713-2730, binding): end is
     * taken by the firmware at push time, Matter epoch seconds when wall
     * time is synced, milliseconds since boot otherwise; each push's start
     * is the previous push's end, carried per endpoint by the server's own
     * stored structs, so consecutive pushes chain into contiguous
     * measurement periods without the host supplying any time at all. */
    uint32_t now_ts = 0;
    bool     wall   = (chip::System::Clock::GetClock_MatterEpochS(now_ts) == CHIP_NO_ERROR);
    uint64_t now_ms = static_cast<uint64_t>(
        chip::System::SystemClock().GetMonotonicMilliseconds64().count());

    auto build = [&](int64_t energy,
                     const chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> &prev) {
        ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type s;
        s.energy = energy;
        if (prev.HasValue()) {
            s.startTimestamp = prev.Value().endTimestamp;
            s.startSystime   = prev.Value().endSystime;
        }
        if (wall) {
            s.endTimestamp.SetValue(now_ts);
        } else {
            s.endSystime.SetValue(now_ms);
        }
        return s;
    };

    /* A side this call does not mention is carried forward unchanged:
     * NotifyCumulativeEnergyMeasured() REPLACES both stored sides
     * (ElectricalEnergyMeasurementCluster.cpp:197-198), so passing Missing
     * for the un-pushed one would null its attribute out from under any
     * subscriber. The cost is that the event restates the carried side's
     * previous reading, which the spec's "imported, exported, or both"
     * event wording tolerates (the C6's identical note). */
    chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> imported =
        data->cumulativeImported;
    chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> exported =
        data->cumulativeExported;
    if (have_imp) {
        imported.SetValue(build(imp_val, data->cumulativeImported));
    }
    if (have_exp) {
        exported.SetValue(build(exp_val, data->cumulativeExported));
    }

    if (!ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(ep, imported, exported)) {
        /* The one narrow post-validation failure the spec discloses: the
         * store may already hold the new values when the event emission
         * fails, so "error answered" does not imply "attributes unchanged"
         * on this path (3.25's event-buffer-exhaustion note). */
        return MT_ATTR_ERR_FAILED;
    }

    /* Notify writes the store and emits the event but reports no attribute
     * change (ElectricalEnergyMeasurementCluster.cpp:191-218 has no
     * MatterReportingAttributeChangeCallback), so subscriptions on the
     * attributes themselves are fired here, one per pushed side, the same
     * one-report-per-applied-field contract as the EPM branch. */
    if (have_imp) {
        MatterReportingAttributeChangeCallback(
            ep, ElectricalEnergyMeasurement::Id,
            ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id);
    }
    if (have_exp) {
        MatterReportingAttributeChangeCallback(
            ep, ElectricalEnergyMeasurement::Id,
            ElectricalEnergyMeasurement::Attributes::CumulativeEnergyExported::Id);
    }
    return MT_ATTR_OK;
}

/*
 * ---- electrical utility meter: the MeterIdentification pool (batch 7a) ----
 *
 * The gap this section fills, the audit's third organ of the
 * declared-but-never-called disease: nothing in the SDK ever constructs a
 * MeterIdentification::Instance (Init() has no caller anywhere in the
 * pinned tree), so without this scan the cluster's five attributes would
 * advertise in the metadata and answer nothing. The C6 fixed it with its
 * own mt_meter.cpp; this is that file's pool rendered in this port's
 * idiom.
 *
 * The Instance is AttributeAccessInterface only, NO delegate, and OWNS its
 * attribute storage (three 64-byte string buffers plus five Nullables,
 * meter-identification-server.h:60-78; 304 B, pinned for this port
 * below the pool), so the pool is raw aligned storage plus placement-new, the
 * ModeBase Instance shape, MT_METER_MAX (2, core/include/mt_matter.h:1303,
 * the C6 depth per DE407) deep.
 *
 * Reservation versus construction, two different times on purpose (the
 * C6's fix-round-2 lesson, mt_meter.cpp): capacity is claimed at CREATE
 * time by mt_meter_reserve() from mt_devtype_create()'s pre-create claim
 * block, because only a thunk failure can abort the composition rebuild;
 * construction waits for mt_meter_register_all()'s one scan, run by
 * main.cpp after rebuild_composition() and before mt_at_start(). On this
 * platform the CHIP server is already running during the rebuild, so
 * "pre-start" means before +MTREADY lets the host ask, and the scan takes
 * the StackLock; Instance::Init() is SOFT (nulls the five attributes,
 * registers the AAI, meter-identification-server.cpp:59-68).
 *
 * s_meter_reserved counts attempts ADMITTED, not completions: a claim
 * stranded by a later create failure expires with the boot (any create
 * failure aborts the whole rebuild; nothing retries within one boot), the
 * same monotonic-claim reasoning as every pool in this file.
 */
namespace {

/* The raw Instance storage is not a .bss array (the nRF's memory reclaim
 * round A). mt_meter_reserve() takes it from the cluster-object arena at
 * CLAIM time and parks the pointer here, so an arena shortfall fails the
 * claim and aborts the create like an exhausted pool, rather than surfacing in
 * mt_meter_register_all()'s post-rebuild scan where nothing could abort
 * anything. */
struct mt_meter_entry {
    bool used;
    uint16_t ep;
    chip::app::Clusters::MeterIdentification::Instance *instance;
    uint8_t *storage;
};

mt_meter_entry s_meter[MT_METER_MAX];
uint16_t s_meter_reserved;

chip::app::Clusters::MeterIdentification::Instance *mt_meter_find(uint16_t ep)
{
    for (uint16_t i = 0; i < MT_METER_MAX; i++) {
        if (s_meter[i].used && s_meter[i].ep == ep) {
            return s_meter[i].instance;
        }
    }
    return nullptr;
}

} /* namespace */

/* Pinned (catalogue batch 7a-2, DE584): the Instance's arena cost and the
 * meter admission maximum, so a grown Instance stops the build instead
 * of a meter composition at runtime. */
static_assert(hearth_arena_cost(sizeof(chip::app::Clusters::MeterIdentification::Instance)) == 304,
              "the MeterIdentification Instance changed size; redo the meter admission arithmetic");
static_assert(MT_METER_MAX * 304 <= HEARTH_OBJ_ARENA_BYTES,
              "MT_METER_MAX meters' Instances no longer fit the cluster-object arena (DE584)");

/* The one value both the thunk's FeatureMap seed and this pool's Instance
 * construction read (mt_matter.h's one-accessor-two-callers contract), so
 * the ember shadow and the Instance's own BitMask cannot drift. Neither
 * caller snapshots the other's output, so their order is immaterial:
 * Instance::Read() answers FeatureMap from the BitMask given at
 * construction, never from ember. */
extern "C" uint32_t mt_meter_feature_mask(void)
{
    return chip::to_underlying(chip::app::Clusters::MeterIdentification::Feature::kPowerThreshold);
}

extern "C" bool mt_meter_reserve(void)
{
    if (s_meter_reserved >= MT_METER_MAX) {
        return false;
    }
    uint8_t *raw = obj_inst_new<chip::app::Clusters::MeterIdentification::Instance>();
    if (raw == nullptr) {
        return false;
    }
    s_meter[s_meter_reserved].storage = raw;
    s_meter_reserved++;
    return true;
}

extern "C" void mt_meter_register_all(void)
{
    using namespace chip::app::Clusters::MeterIdentification;
    chip::DeviceLayer::StackLock lock;

    chip::BitMask<Feature> features(mt_meter_feature_mask());
    uint16_t slot = 0;

    /* slot < MT_METER_MAX is a defensive backstop, not the primary gate:
     * mt_meter_reserve() already bounded how many meter endpoints the
     * rebuild could create, and a composition that exceeded it aborted
     * before this function runs. The walk is the live endpoint table (the
     * C6's own scan shape), filtered by the real ember composition. */
    for (uint16_t i = 0; i < mt_matter_endpoint_count() && slot < MT_METER_MAX; i++) {
        uint32_t devtype;
        uint16_t ep;
        uint8_t variant, parent_idx;
        if (mt_matter_endpoint_info(i, &devtype, &ep, &variant, &parent_idx) != 0) {
            continue;
        }
        if (!emberAfContainsServer(ep, chip::app::Clusters::MeterIdentification::Id)) {
            continue;
        }
        /* Memory reclaim round A: every meter-bearing endpoint in this walk
         * passed mt_meter_reserve(), which allocated this slot's storage,
         * so a null here is an internal invariant violation rather than a
         * capacity outcome. Stop rather than fault. */
        if (s_meter[slot].storage == nullptr) {
            HEARTH_LOGE("matter", "MeterIdentification slot %u has no storage: reserve/scan disagree; "
                    "endpoint %u will serve nothing",
                    (unsigned)slot, (unsigned)ep);
            break;
        }
        auto *inst = new (s_meter[slot].storage) Instance(ep, features);
        CHIP_ERROR err = inst->Init();
        if (err != CHIP_NO_ERROR) {
            /* Logged, not aborted: Init() refusing an (endpoint, cluster)
             * pair this same boot's rebuild just created is an internal
             * invariant violation no host action can trigger, the
             * mt_meter.cpp reasoning. The slot is not consumed. */
            HEARTH_LOGE("matter", "MeterIdentification Instance::Init failed for endpoint %u: "
                    "%" CHIP_ERROR_FORMAT "; its five attributes will answer nothing",
                    (unsigned)ep, err.Format());
            continue;
        }
        s_meter[slot].used = true;
        s_meter[slot].ep = ep;
        s_meter[slot].instance = inst;
        slot++;
    }
}

/*
 * DE397 live read for MeterType, dispatched from mt_matter_attr_read()
 * under its StackLock: unsigned enum8, null until the host's first
 * AT+MTMETERID (+MTERR:5, the no-null-literal rule).
 */
static int mt_meter_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    using namespace chip::app::Clusters::MeterIdentification;
    if (is_unsigned) {
        *is_unsigned = true;
    }
    if (attr != Attributes::MeterType::Id) {
        /* Unreachable: MeterType is the cluster's only carve-out row. */
        return MT_ATTR_ERR_FAILED;
    }
    Instance *inst = mt_meter_find(ep);
    if (inst == nullptr) {
        /* Cannot happen once mt_meter_register_all() has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    if (inst->GetMeterType().IsNull()) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = chip::to_underlying(inst->GetMeterType().Value());
    return MT_ATTR_OK;
}

/*
 * AT+MTMETERID (AT_MT_SPEC.md 3.29): push the full MeterIdentification
 * identity in one call, the only write path any of the five attributes
 * has, all-or-nothing (every field validated before any SetXxx() runs).
 * Grammar (the hand-parse, quoting, printability, the 64-byte scan bound)
 * is cmd_mtmeterid's business in mt_at.c; this bridge owns the lookups and
 * the semantic checks. The C6 splits this across main.cpp (lock, lookups)
 * and mt_meter.cpp (validate, apply) only because ChipStackLock lives in
 * its main.cpp; here one function owns the whole path.
 *
 * The cluster lookup answers MT_ATTR_ERR_ATTRIBUTE, not the generic
 * MT_ATTR_ERR_CLUSTER: this command's own error table maps "endpoint
 * exists but carries no MeterIdentification" to +MTERR:4, the AT+MTROW
 * family's no-payload-of-this-kind code (3.29's lookup-errors paragraph),
 * and attr_err_to_mterr() renders MT_ATTR_ERR_ATTRIBUTE as exactly that.
 *
 * There is deliberately no read-back verb: the identity is
 * host-originated and re-pushed on reconcile, and a full identity line
 * (worst case 265 B) exceeds the host library's 255-byte usable receive
 * line, so a read-back would be silently discarded by the host's own line
 * reader rather than fail loudly (the C6's measured arithmetic,
 * mt_meter.cpp, restated in 3.29).
 */
extern "C" int mt_matter_meter_set_identity(uint16_t ep, const mt_meter_identity_t *id)
{
    using namespace chip::app::Clusters::MeterIdentification;
    using chip::app::Clusters::Globals::PowerThresholdSourceEnum;
    /* PowerThresholdStruct is a namespace (the payload type is
     * PowerThresholdStruct::Type), so a namespace alias rather than a
     * using-declaration. */
    namespace PowerThresholdStruct = chip::app::Clusters::Globals::Structs::PowerThresholdStruct;
    using chip::app::DataModel::MakeNullable;

    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::MeterIdentification::Id)) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    Instance *inst = mt_meter_find(ep);
    if (inst == nullptr) {
        /* Cluster present but no pool slot: cannot happen once
         * mt_meter_register_all() has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    /* ---- validate everything before applying anything ---- */

    if (id->meter_type > static_cast<uint8_t>(MeterTypeEnum::kGeneric)) {
        return MT_ATTR_ERR_VALUE;
    }
    if (!id->pwr_present && !id->apparent_present) {
        /* PowerThresholdStruct's "choice b": at least one of the two power
         * optionals. cmd_mtmeterid already rejects this at parse; the
         * double gate is the demcap precedent. */
        return MT_ATTR_ERR_VALUE;
    }
    if (id->src_present && id->src > static_cast<uint8_t>(PowerThresholdSourceEnum::kEquipment)) {
        return MT_ATTR_ERR_VALUE;
    }
    size_t pod_len      = strlen(id->pod);
    size_t serial_len   = strlen(id->serial);
    size_t protocol_len = strlen(id->protocol);
    if (pod_len > MT_METERID_MAX_STR || serial_len > MT_METERID_MAX_STR ||
        protocol_len > MT_METERID_MAX_STR) {
        /* Defensive: cmd_mtmeterid enforces the length while scanning the
         * quoted string; re-checked as the last point before any SetXxx()
         * call, the all-or-nothing contract's whole purpose. */
        return MT_ATTR_ERR_VALUE;
    }

    /* ---- apply ---- */

    PowerThresholdStruct::Type pt;
    if (id->pwr_present) {
        pt.powerThreshold.SetValue(id->pwr);
    }
    if (id->apparent_present) {
        pt.apparentPowerThreshold.SetValue(id->apparent);
    }
    if (id->src_present) {
        pt.powerThresholdSource.SetNonNull(static_cast<PowerThresholdSourceEnum>(id->src));
    } else {
        pt.powerThresholdSource.SetNull();
    }

    /* Every call below is expected to return CHIP_NO_ERROR: the block
     * above already checked everything each SetXxx() would reject. A
     * failure here is an internal SDK invariant violation, logged rather
     * than propagated so one unexpected failure does not also abandon the
     * fields that would have applied cleanly after it (the C6's identical
     * disposition; the Instance copies every span into its own fixed
     * buffers before returning, so the caller's stack strings are not
     * held). */
    CHIP_ERROR err;
    err = inst->SetMeterType(MakeNullable(static_cast<MeterTypeEnum>(id->meter_type)));
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "SetMeterType failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetPointOfDelivery(MakeNullable(chip::CharSpan(id->pod, pod_len)));
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "SetPointOfDelivery failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetMeterSerialNumber(MakeNullable(chip::CharSpan(id->serial, serial_len)));
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "SetMeterSerialNumber failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetProtocolVersion(MakeNullable(chip::CharSpan(id->protocol, protocol_len)));
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "SetProtocolVersion failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetPowerThreshold(MakeNullable(pt));
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "SetPowerThreshold failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }

    return MT_ATTR_OK;
}

/*
 * ---- device energy management: HearthDemDelegate and its pool (batch 7a) ----
 *
 * The C6's HearthDemDelegate (main.cpp:4865-5266) ported whole: the cached
 * attribute store the Instance's AAI reads, the owned
 * PowerAdjustmentCapability list AT+MTDEMCAP replaces, the two PA command
 * forwards, and the firmware-owned event policy. The policy, binding per
 * AT_MT_SPEC.md 3.17:1487-1533 and 3.25:2755-2768:
 *
 *   - An ALLOWED PowerAdjustRequest sets ESAState kPowerAdjustActive
 *     itself, arms the duration clock and emits the fieldless
 *     PowerAdjustStart (the host does NOT push the entry transition, the
 *     contrast to the water heater's Boost where the host pushes
 *     BoostState).
 *   - Re-adjust rule: a second accept while already PowerAdjustActive
 *     emits NO second Start and does not re-arm the clock (certification
 *     behaviour: TC_DEM_2_2 step 14 requires "SUCCESS and no event sent"
 *     and asserts the eventual End's duration against the FIRST accept).
 *     The forward still reaches the host either way.
 *   - A host ESAState push LEAVING kPowerAdjustActive emits
 *     PowerAdjustEnd(NormalCompletion, measured duration, cached field-6
 *     energyUse, cache consumed); a same-state push emits nothing and
 *     reports nothing (ESAState reports on change only, unlike the WHM's
 *     per-sample BoostState).
 *   - An accepted CancelPowerAdjustRequest emits
 *     PowerAdjustEnd(Cancelled) and resets to Online through the RAW
 *     setter, never SetESAState(): the derivation there would emit a
 *     second End with the wrong cause for a session already ended.
 *
 * The server pre-validates EVERY PowerAdjustRequest against the pushed
 * capability and the OptOutState x cause matrix BEFORE the delegate runs
 * (device-energy-management-server.cpp:254-359: a null capability answers
 * ConstraintError, an out-of-range request likewise, and
 * CancelPowerAdjustRequest has a server-side in-state guard answering
 * InvalidInState, :371-390), so a null capability never wakes the host and
 * no +MTCMD is ever raised for a cancel in the wrong state: AT+MTDEMCAP's
 * whole reason for existing.
 *
 * Cause tracking (the C6's task-review F2 contract): the capability
 * struct's LIVE cause is firmware-owned while an adjustment runs (stamped
 * on accept with the request's adjustment reason, reported dirty); the
 * BASELINE member is the host-pushed resting value every PowerAdjustEnd
 * restores.
 *
 * Lock discipline, the file's standing rule: the two command forwards run
 * on the CHIP task from the Instance's CHI invoke path, NO StackLock
 * taken; the push-path derivation (SetESAState) runs from
 * mt_matter_meas_set()/mt_matter_demcap_set(), which already hold it.
 */
class HearthDemDelegate : public chip::app::Clusters::DeviceEnergyManagement::Delegate
{
public:
    /* Host-pushed cached state, written only by mt_meas_dem_apply(). The
     * defaults are the spec's pre-first-push answers (3.25): ESAType 0
     * (kEvse), canGenerate false, ESAState Online, powers 0, OptOutState
     * NoOptOut. */
    uint8_t m_esa_type     = 0;
    bool    m_can_generate = false;
    chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum m_esa_state =
        chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum::kOnline;
    int64_t m_abs_min_power = 0;
    int64_t m_abs_max_power = 0;
    chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum m_opt_out =
        chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum::kNoOptOut;

    /* The session's approximate energy use (mWh), the PowerAdjustEnd
     * event's energyUse field: MT_DEM_F_ADJ_ENERGY_USE, an event carrier
     * and not an attribute (mt_matter.h). Host-pushed while an adjustment
     * runs, consumed and reset by every PowerAdjustEnd emission. */
    int64_t m_energy_use = 0;

    /* The owned PowerAdjustmentCapability store, AT+MTDEMCAP's surface:
     * fixed backing array, entry count, the Nullable the getter hands
     * out (null until the host installs entries), and the baseline cause
     * (the F2 contract in the section comment). */
    chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustStruct::Type
        m_pa_entries[MT_DEM_CAP_MAX_ENTRIES];
    uint8_t m_pa_count = 0;
    chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustCapabilityStruct::Type>
        m_pa_capability;
    chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum m_pa_cause_baseline =
        chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum::kNoAdjustment;

    chip::EndpointId endpoint() const { return mEndpointId; }

    /* Whether this endpoint's variant seeded the PowerAdjustment bit:
     * stamped by mt_matter_dem_register() from the create path's variant
     * predicate, the single source the FeatureMap seed shares. The
     * AT+MTDEMCAP feature gate reads it here (the C6 reads its
     * esp-matter FeatureMap attribute back for the same answer; this
     * port's ember shadow is a seed, so the port record is the honest
     * source). */
    void set_with_pa(bool pa) { m_with_pa = pa; }
    bool with_pa() const { return m_with_pa; }

    void SetAdjEnergyUse(int64_t mwh) { m_energy_use = mwh; }

    chip::Protocols::InteractionModel::Status PowerAdjustRequest(
        const int64_t power, const uint32_t duration,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        using chip::Protocols::InteractionModel::Status;

        char fields[48];
        snprintf(fields, sizeof(fields), "%lld,%lu,%u", (long long)power, (unsigned long)duration,
                 (unsigned)chip::to_underlying(cause));

        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::PowerAdjustRequest::Id,
                                           fields);
        if (!allow) {
            return Status::Failure;
        }

        /* Whether this accept STARTS an adjustment or RE-ADJUSTS a
         * running one, decided before the state write (the section
         * comment's re-adjust rule). */
        bool in_progress = (m_esa_state == ESAStateEnum::kPowerAdjustActive);

        /* Stamp the capability's live cause with the accepted request's
         * adjustment reason (F2). The server has already rejected
         * kUnknownEnumValue; the default arm is defensive. */
        switch (cause) {
        case AdjustmentCauseEnum::kLocalOptimization:
            SetCapabilityCause(PowerAdjustReasonEnum::kLocalOptimizationAdjustment);
            break;
        case AdjustmentCauseEnum::kGridOptimization:
            SetCapabilityCause(PowerAdjustReasonEnum::kGridOptimizationAdjustment);
            break;
        default:
            break;
        }

        SetStateRaw(ESAStateEnum::kPowerAdjustActive);
        if (!in_progress) {
            m_pa_start_ms = static_cast<uint64_t>(
                chip::System::SystemClock().GetMonotonicMilliseconds64().count());

            /* Emission discipline: COMMAND PATH, CHIP task, no StackLock
             * (the section comment). A LogEvent failure is logged and
             * deliberately does not fail the command: the host has
             * already accepted and the state is applied and served. */
            Events::PowerAdjustStart::Type event;
            chip::EventNumber n;
            CHIP_ERROR err = chip::app::LogEvent(event, mEndpointId, n);
            if (err != CHIP_NO_ERROR) {
                HEARTH_LOGE("matter", "DEM ep %u: PowerAdjustStart event failed: %" CHIP_ERROR_FORMAT,
                        (unsigned)mEndpointId, err.Format());
            }
        }
        /* else: re-adjust while active, no second Start, the clock keeps
         * measuring from the first accept. */
        return Status::Success;
    }

    chip::Protocols::InteractionModel::Status CancelPowerAdjustRequest() override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        using chip::Protocols::InteractionModel::Status;

        /* Payload-less forward (NULL fields reproduces mt_cmd_forward()'s
         * exact four-field +MTCMD line). No firmware in-state guard: the
         * server refused the command with InvalidInState unless ESAState
         * was kPowerAdjustActive before this ran (the section comment). */
        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::CancelPowerAdjustRequest::Id,
                                           NULL);
        if (!allow) {
            return Status::Failure;
        }

        EmitPowerAdjustEnd(CauseEnum::kCancelled);
        SetStateRaw(ESAStateEnum::kOnline);
        return Status::Success;
    }

    /* The six non-PowerAdjustment command handlers. Unreachable while
     * this firmware is PA-only: the server's InvokeCommand dispatch
     * answers UnsupportedCommand for every one of them unless the
     * matching feature bit is in the Instance's mask
     * (device-energy-management-server.cpp:181-251), and this port only
     * ever constructs with kPowerAdjustment or nothing. Failure rather
     * than Success so a future feature bit without a handler is a
     * visible command failure, not a silent lie (the C6's reasoning,
     * kept). */
    chip::Protocols::InteractionModel::Status StartTimeAdjustRequest(
        const uint32_t requestedStartTime,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)requestedStartTime;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status PauseRequest(
        const uint32_t duration,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)duration;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status ResumeRequest() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status ModifyForecastRequest(
        const uint32_t forecastID,
        const chip::app::DataModel::DecodableList<
            chip::app::Clusters::DeviceEnergyManagement::Structs::SlotAdjustmentStruct::Type>
            &slotAdjustments,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)forecastID;
        (void)slotAdjustments;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status RequestConstraintBasedForecast(
        const chip::app::DataModel::DecodableList<
            chip::app::Clusters::DeviceEnergyManagement::Structs::ConstraintsStruct::Type>
            &constraints,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)constraints;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status CancelRequest() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }

    /* The eight getters, serving the host-pushed cache. */
    chip::app::Clusters::DeviceEnergyManagement::ESATypeEnum GetESAType() override
    {
        return static_cast<chip::app::Clusters::DeviceEnergyManagement::ESATypeEnum>(m_esa_type);
    }
    bool GetESACanGenerate() override { return m_can_generate; }
    chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum GetESAState() override
    {
        return m_esa_state;
    }
    int64_t GetAbsMinPower() override { return m_abs_min_power; }
    int64_t GetAbsMaxPower() override { return m_abs_max_power; }
    chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum GetOptOutState() override
    {
        return m_opt_out;
    }
    const chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustCapabilityStruct::Type> &
    GetPowerAdjustmentCapability() override
    {
        return m_pa_capability;
    }
    /* Permanently null: PFR/SFR out of this batch's scope. */
    const chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::ForecastStruct::Type> &
    GetForecast() override
    {
        return m_forecast;
    }

    /*
     * SetESAState: the push-path transition entry. mt_meas_dem_apply()
     * calls this for every MT_DEM_F_ESA_STATE pair; the derivation lives
     * here (the section comment's push rules). Rejects kUnknownEnumValue
     * and up, reports the attribute dirty ON CHANGE ONLY, so callers do
     * not report ESAState again themselves. The one SDK caller of this
     * virtual is the server's Pause path, unreachable while PA-only, so
     * in practice every call site is a bridge holding the StackLock.
     */
    CHIP_ERROR SetESAState(chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum next) override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        if (next >= ESAStateEnum::kUnknownEnumValue) {
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        if (next == m_esa_state) {
            return CHIP_NO_ERROR;
        }
        ESAStateEnum prev = m_esa_state;
        if (prev == ESAStateEnum::kPowerAdjustActive) {
            /* Emission discipline: PUSH PATH, the bridge already holds
             * the StackLock (the section comment). */
            EmitPowerAdjustEnd(CauseEnum::kNormalCompletion);
        }
        if (next == ESAStateEnum::kPowerAdjustActive) {
            /* Entering via a push is off-design (the firmware owns the
             * entry transition on an accepted PowerAdjustRequest, which
             * arms this clock itself), but nothing stops a host doing
             * it; arm the clock here too so a later End measures from
             * this moment instead of from boot (3.25's rule). */
            m_pa_start_ms = static_cast<uint64_t>(
                chip::System::SystemClock().GetMonotonicMilliseconds64().count());
        }
        SetStateRaw(next);
        return CHIP_NO_ERROR;
    }

    /* AT+MTDEMCAP's apply half needs the private helpers; the bridge is a
     * free function, so it goes through these two rather than friending. */
    void demcap_apply(chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum baseline,
                      uint8_t n, const int64_t *quads)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        /* The live cause carried into the rebuilt struct: the pushed
         * baseline normally, the firmware-stamped value while an
         * adjustment is running (the F2 contract: replacing the
         * capability mid-adjustment must not overwrite the stamped cause;
         * the eventual PowerAdjustEnd restores the NEW baseline). */
        PowerAdjustReasonEnum live = baseline;
        if (m_esa_state == ESAStateEnum::kPowerAdjustActive && !m_pa_capability.IsNull()) {
            live = m_pa_capability.Value().cause;
        }
        m_pa_cause_baseline = baseline;

        if (n == 0) {
            m_pa_count = 0;
            m_pa_capability.SetNull();
        } else {
            for (uint8_t i = 0; i < n; i++) {
                m_pa_entries[i].minPower    = quads[4 * i];
                m_pa_entries[i].maxPower    = quads[4 * i + 1];
                m_pa_entries[i].minDuration = static_cast<uint32_t>(quads[4 * i + 2]);
                m_pa_entries[i].maxDuration = static_cast<uint32_t>(quads[4 * i + 3]);
            }
            m_pa_count = n;
            Structs::PowerAdjustCapabilityStruct::Type cap;
            cap.powerAdjustCapability.SetNonNull(
                chip::app::DataModel::List<const Structs::PowerAdjustStruct::Type>(m_pa_entries,
                                                                                  n));
            cap.cause = live;
            m_pa_capability.SetNonNull(cap);
        }
        MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                               Attributes::PowerAdjustmentCapability::Id);
    }

private:
    bool m_with_pa = false;

    /* Forecast: permanently null, never written. */
    chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::ForecastStruct::Type>
        m_forecast;

    /* Monotonic ms at the last accepted PowerAdjustRequest, the duration
     * clock. */
    uint64_t m_pa_start_ms = 0;

    /* Stamp the owned capability struct's cause and report the attribute
     * dirty. A null capability (AT+MTDEMCAP never sent) has no cause to
     * stamp; that state cannot carry a running adjustment anyway (the
     * server refuses PowerAdjustRequest on a null capability), so
     * skipping is right on the restore path too. */
    void SetCapabilityCause(chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum r)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        if (m_pa_capability.IsNull() || m_pa_capability.Value().cause == r) {
            return;
        }
        m_pa_capability.Value().cause = r;
        MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                               Attributes::PowerAdjustmentCapability::Id);
    }

    /* Cache write + dirty report, no event derivation: the command paths
     * use this directly because they emit their own event (or none)
     * before transitioning; SetESAState() is the derived-emission entry.
     * Reports on change only. */
    void SetStateRaw(chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum next)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        if (m_esa_state != next) {
            m_esa_state = next;
            MatterReportingAttributeChangeCallback(mEndpointId, Id, Attributes::ESAState::Id);
        }
    }

    /* Fill and log PowerAdjustEnd: cause per caller, duration measured
     * against the accept timestamp, energyUse from the host-pushed
     * field-6 cache, consumed (reset to 0) by the emission; the
     * capability cause restored to the host-pushed baseline (F2). Lock
     * discipline is the CALLER's, commented at each call site; a
     * LogEvent failure is logged and does not propagate (the transition
     * the event describes has already been decided). */
    void EmitPowerAdjustEnd(chip::app::Clusters::DeviceEnergyManagement::CauseEnum cause)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        uint64_t now_ms = static_cast<uint64_t>(
            chip::System::SystemClock().GetMonotonicMilliseconds64().count());
        Events::PowerAdjustEnd::Type event;
        event.cause     = cause;
        event.duration  = static_cast<uint32_t>((now_ms - m_pa_start_ms) / 1000);
        event.energyUse = m_energy_use;

        chip::EventNumber n;
        CHIP_ERROR err = chip::app::LogEvent(event, mEndpointId, n);
        if (err != CHIP_NO_ERROR) {
            HEARTH_LOGE("matter", "DEM ep %u: PowerAdjustEnd event failed: %" CHIP_ERROR_FORMAT,
                    (unsigned)mEndpointId, err.Format());
        }
        m_energy_use = 0;
        SetCapabilityCause(m_pa_cause_baseline);
    }
};

/*
 * The pool, MT_DEM_MAX (4, core/include/mt_matter.h:1034, the C6 depth per
 * DE407) delegates plus raw aligned Instance storage: unlike the C6, where
 * esp-matter's DeviceEnergyManagementDelegateInitCB news the Instance at
 * endpoint enable from a FeatureMap snapshot, this port constructs it in
 * mt_matter_dem_register() with the variant's own mask (the pair pinned
 * below). Endpoint id stamped in the SUCCESS-ONLY second half by the
 * Instance constructor, never at handout (the nRF's batch 7b
 * fix round M1, the reasoning at the alloc below). Exhaustion aborts the
 * create before anything is spent.
 */
static HearthDemDelegate *s_dem_delegates[MT_DEM_MAX];
static size_t            s_dem_next;

/* Pinned (catalogue batch 7a-2, DE584): the DEM pair's arena cost, the
 * DEM admission maximum with its DEMMode ModeBase pair (80 B, pinned in
 * mt_matter_sl_b5.inc), and the energy families at their caps together
 * (five since the EVSE round and DE608's 8,464 B arena, which they fill
 * exactly: the water heater's 168 B and the energy EVSE's 2,104 B outside
 * the measurement and DEM caps), so any composition of those families
 * inside their caps fits the object arena. */
static_assert(hearth_arena_cost(obj_pair_bytes<HearthDemDelegate,
                                                chip::app::Clusters::DeviceEnergyManagement::Instance>()) == 280,
              "the DEM pair changed size; redo the DEM admission arithmetic");
static_assert(MT_DEM_MAX * (280 + 80) <= HEARTH_OBJ_ARENA_BYTES,
              "MT_DEM_MAX DEM endpoints' objects no longer fit the cluster-object arena (DE584)");
static_assert(MT_MEAS_MAX * (152 + 40) + MT_DEM_MAX * (280 + 80) + MT_METER_MAX * 304 +
                      MT_WHM_MAX * (88 + 80) + MT_EVSE_MAX * (2024 + 80) ==
                  HEARTH_OBJ_ARENA_BYTES,
              "the five energy families at their caps no longer fill the cluster-object arena "
              "exactly (DE608)");

/* Fix round M1 (the nRF's batch 7b): the id is NOT stamped at handout. The
 * WHM alloc's comment below carries the full reasoning, one discipline for
 * both id-at-alloc pools: a claim stranded by a create failure after
 * this point used to keep the s_next_ep_id stamp the NEXT successful
 * create then took, and dem_for()'s first-match walk would bind the
 * 0x98 pushes, AT+MTDEMCAP and the carve-out reads to the dead delegate
 * (inherited 7a pattern, fixed with its double). The sentinel is
 * unmatchable; the real id lands in mt_matter_dem_register()'s Instance
 * construction, whose ctor runs mDelegate.SetEndpointId(aEndpointId)
 * (device-energy-management-server.h:210-216), success-only. */
extern "C" void *mt_matter_dem_delegate_alloc(uint16_t ep)
{
    (void)ep;
    if (s_dem_next >= MT_DEM_MAX) {
        return nullptr;
    }
    HearthDemDelegate *d =
        obj_pair_new<HearthDemDelegate,
                     chip::app::Clusters::DeviceEnergyManagement::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->SetEndpointId(chip::kInvalidEndpointId);
    s_dem_delegates[s_dem_next++] = d;
    return d;
}

/* The pool lookup the 0x98 push branch, the AT+MTDEMCAP bridge and the
 * DE397 live reader use. */
static HearthDemDelegate *dem_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_dem_next; i++) {
        if (s_dem_delegates[i]->endpoint() == ep) {
            return s_dem_delegates[i];
        }
    }
    return nullptr;
}

/*
 * The DEM second half: construct the Instance with the variant's feature
 * mask and Init() it (CHI registration then AAI, SOFT on both,
 * device-energy-management-server.cpp:40-47: no emberAfContainsServer
 * check anywhere, so ordering mistakes cannot panic; below the successful
 * create by the same strand-nothing reasoning as the measurement
 * halves). with_pa is the create path's variant predicate, the single
 * source the variant-qualified FeatureMap seed rows mirror
 * (mt_devtypes_sl_seeds.inc), and is recorded on the delegate for
 * mt_matter_demcap_set()'s feature gate.
 */
extern "C" void mt_matter_dem_register(void *delegate, uint16_t ep, bool with_pa)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;
    auto *d = static_cast<HearthDemDelegate *>(delegate);
    d->set_with_pa(with_pa);
    auto *inst = new (obj_inst_storage<HearthDemDelegate, Instance>(d))
        Instance(ep, *d, with_pa ? Feature::kPowerAdjustment : static_cast<Feature>(0));
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "DEM Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; the cluster will serve nothing and its commands will not dispatch",
                (unsigned)ep, err.Format());
    }
}

/*
 * AT+MTMEAS's DeviceEnergyManagement (0x0098) branch. Called only from
 * mt_matter_meas_set(), which already holds the StackLock and resolved
 * the endpoint and cluster lookups; this function owns the field table
 * (AT_MT_SPEC.md 3.25's 0x0098 rows). Value bounds from the cluster XML
 * and the generated enums: ESAType is 0..0x0D plus 0xFF (kOther);
 * ESACanGenerate bool; ESAState ESAStateEnum 0..4; OptOutState 0..3; the
 * two powers carry the full int64 width (the XML constrains neither
 * beyond its type; the AbsMaxPower>=AbsMinPower relation is a
 * cross-field data-model property between independently pushed fields,
 * deliberately not enforced per push). Field 6 is the event carrier:
 * caches only, never dirty. Two passes, the family's hard contract.
 */
static int mt_meas_dem_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;

    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Pass 1: validate everything. */
    for (uint8_t i = 0; i < count; i++) {
        switch (fields[i]) {
        case MT_DEM_F_ESA_TYPE:
            if (!((values[i] >= 0 && values[i] <= 0x0D) || values[i] == 0xFF)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ESA_CAN_GEN:
            if (values[i] < 0 || values[i] > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ESA_STATE:
            if (values[i] < 0 ||
                values[i] >= chip::to_underlying(ESAStateEnum::kUnknownEnumValue)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ABS_MIN_POWER:
        case MT_DEM_F_ABS_MAX_POWER:
        case MT_DEM_F_ADJ_ENERGY_USE:
            /* int64 full width, no XML bound beyond the type. */
            break;
        case MT_DEM_F_OPT_OUT_STATE:
            if (values[i] < 0 ||
                values[i] >= chip::to_underlying(OptOutStateEnum::kUnknownEnumValue)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Pass 2: apply, in order (sequential last-writer semantics).
     * ESAState transitions derive events as they are applied. */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t attr_id;
        switch (fields[i]) {
        case MT_DEM_F_ESA_TYPE:
            d->m_esa_type = (uint8_t)values[i];
            attr_id = Attributes::ESAType::Id;
            break;
        case MT_DEM_F_ESA_CAN_GEN:
            d->m_can_generate = (values[i] != 0);
            attr_id = Attributes::ESACanGenerate::Id;
            break;
        case MT_DEM_F_ESA_STATE:
            /* SetESAState owns derivation AND the on-change dirty report;
             * pass 1 already cut the enum range, so its ConstraintError
             * arm is unreachable here. */
            (void)d->SetESAState(static_cast<ESAStateEnum>(values[i]));
            continue;
        case MT_DEM_F_ABS_MIN_POWER:
            d->m_abs_min_power = values[i];
            attr_id = Attributes::AbsMinPower::Id;
            break;
        case MT_DEM_F_ABS_MAX_POWER:
            d->m_abs_max_power = values[i];
            attr_id = Attributes::AbsMaxPower::Id;
            break;
        case MT_DEM_F_OPT_OUT_STATE:
            d->m_opt_out = static_cast<OptOutStateEnum>(values[i]);
            attr_id = Attributes::OptOutState::Id;
            break;
        default: /* MT_DEM_F_ADJ_ENERGY_USE, pass 1 admits nothing else */
            /* Event carrier: cache only, never dirty (3.25's field-6
             * row). */
            d->SetAdjEnergyUse(values[i]);
            continue;
        }
        MatterReportingAttributeChangeCallback(ep, Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * The AT+MTDEMCAP bridge (AT_MT_SPEC.md 3.26). Grammar (the 3+4n arity,
 * integer parses, the u32 duration bound) is cmd_mtdemcap's business;
 * this bridge owns everything semantic, in the established
 * lookup-then-value order: endpoint and cluster lookups, the
 * PowerAdjustment feature gate (the delegate's with_pa record, stamped
 * from the same variant predicate the FeatureMap seed derives from; a
 * variant-1 endpoint has no PowerAdjustmentCapability attribute to set
 * and answers MT_ATTR_ERR_ATTRIBUTE, the spec's +MTERR:4 row), the cause
 * enum range, and per-entry minPower<=maxPower / minDuration<=maxDuration
 * ordering (the server's request walk takes each entry as an interval; a
 * mis-ordered entry would be an empty one). Full replacement per call,
 * not persisted, reported dirty on success; the store lives in the
 * delegate and starts null every boot.
 */
extern "C" int mt_matter_demcap_set(uint16_t ep, uint8_t cause, uint8_t n, const int64_t *quads)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;
    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot: defensive, the push branches'
         * answer. */
        return MT_ATTR_ERR_FAILED;
    }
    if (!d->with_pa()) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }

    if (n > MT_DEM_CAP_MAX_ENTRIES) {
        /* Defensive; cmd_mtdemcap's <n> range check answers first. */
        return MT_ATTR_ERR_VALUE;
    }
    if (cause >= chip::to_underlying(PowerAdjustReasonEnum::kUnknownEnumValue)) {
        return MT_ATTR_ERR_VALUE;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (quads[4 * i] > quads[4 * i + 1] ||     /* minPower > maxPower */
            quads[4 * i + 2] > quads[4 * i + 3]) { /* minDuration > maxDuration */
            return MT_ATTR_ERR_VALUE;
        }
    }

    d->demcap_apply(static_cast<PowerAdjustReasonEnum>(cause), n, quads);
    return MT_ATTR_OK;
}

/*
 * DE397 live read for the six DEM scalars, dispatched from
 * mt_matter_attr_read() under its StackLock: the delegate cache is the
 * served truth (the enum shadows are seeds, the two powers have no slot
 * at all). None of the six is nullable; signedness per attribute.
 */
static int mt_dem_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    namespace DEM = chip::app::Clusters::DeviceEnergyManagement;
    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    bool u = true;
    int64_t v;
    switch (attr) {
    case DEM::Attributes::ESAType::Id:       v = d->m_esa_type; break;
    case DEM::Attributes::ESACanGenerate::Id: v = d->m_can_generate ? 1 : 0; break;
    case DEM::Attributes::ESAState::Id:      v = chip::to_underlying(d->m_esa_state); break;
    case DEM::Attributes::OptOutState::Id:   v = chip::to_underlying(d->m_opt_out); break;
    case DEM::Attributes::AbsMinPower::Id:   v = d->m_abs_min_power; u = false; break;
    case DEM::Attributes::AbsMaxPower::Id:   v = d->m_abs_max_power; u = false; break;
    default:
        /* Unreachable: only the six k_instance_served rows route here. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    if (is_unsigned) {
        *is_unsigned = u;
    }
    *out = v;
    return MT_ATTR_OK;
}

/*
 * ---- water heater: HearthWhmDelegate and its pool (batch 7b) --------------
 *
 * The C6's HearthWhmDelegate (main.cpp:4405-4564) ported whole: the cached
 * attribute store the Instance's AAI reads, the Boost/CancelBoost command
 * forwards, and the firmware-derived Boost event pair. The cluster is FULLY
 * delegate-served (its six values answer from the Delegate getters,
 * water-heater-management-server.cpp:114-152; the C6 creates all six
 * MANAGED_INTERNALLY, esp_matter_attribute.cpp:4485-4519); the split
 * ownership rule is the AT+MTLOCK/AT+MTVALVE one: an ALLOWED Boost does NOT
 * touch the cached BoostState here, because "the host decided" (the command
 * verdict) and "the host actually did it" (the AT+MTMEAS BoostState push)
 * are different moments, and only the second one moves the served state.
 *
 * =========================================================================
 * THE BOOST EVENT DERIVATION STATE MACHINE, exhaustively, because it is
 * stateful firmware logic with no SDK support and no build check
 * (AT_MT_SPEC.md 3.25:2734-2749 and core/include/mt_matter.h:874-883,
 * both binding; the C6's mt_meas_whm_apply() implements it identically).
 *
 * State: the delegate's cached BoostState (kInactive/kActive,
 * WaterHeaterManagement/Enums.h:32-36), written ONLY by the AT+MTMEAS 0x94
 * push path below. Input: each applied MT_WHM_F_BOOST_STATE pair, IN ORDER
 * within one command (sequential last-writer semantics, so one push
 * carrying 2,1,2,0 legitimately emits BoostStarted then BoostEnded).
 * Transitions, evaluated per applied pair against the cache value it is
 * about to replace:
 *
 *   Inactive -> Active: emit BoostStarted, its boostInfo carrying the
 *     CACHED PARAMETERS OF THE LAST HOST-ACCEPTED BOOST COMMAND (the
 *     m_boost lifecycle below), then CONSUME the cache (reset to the
 *     duration-0/no-optionals default).
 *   Active -> Inactive: emit BoostEnded (no payload beyond the event).
 *   Same state -> same state: emit NOTHING. This is also what makes
 *     HandleCancelBoost()'s in-state guard's "no event sent" claim hold by
 *     construction: no forward means no host actuation, no BoostState
 *     push, no transition.
 *
 * Attribute reporting is SEPARATE from event derivation and fires per
 * sample: every applied pair, BoostState included and SAME-STATE pushes
 * included, is reported dirty (one MatterReportingAttributeChangeCallback
 * per applied pair, the EPM rule), the deliberate CONTRAST to DEM's
 * ESAState, which reports on change only. Two clusters, two spec'd
 * shapes; do not "fix" one to match the other.
 *
 * THE PARAMETER-CACHE LIFECYCLE (m_boost), the round's delicate piece:
 *
 *   SET: in HandleBoost(), on ALLOW only, after the host's verdict comes
 *     back positive. A denied Boost never started anything, so there is
 *     nothing for a later BoostStarted to describe, and caching on deny
 *     would attribute a stale command's parameters to some future
 *     host-initiated boost.
 *   CONSUMED: by the Inactive-to-Active emission, exactly once. The
 *     parameters describe the boost that just started; a LATER Active
 *     transition without a fresh accepted Boost in between (a
 *     host-initiated boost from a physical button, no controller
 *     involved) must emit duration 0 and no optionals, "parameters
 *     unknown", rather than restate a finished boost's numbers. That is
 *     the round B rule mt_matter.h:874-883 fixes, and the reset after
 *     emission is what implements it.
 *   ALSO CONSUMED: by the guarded (Inactive-state) CancelBoost, RULED
 *     this round (ruled at B410; batch 7b review I1). An accepted Boost a
 *     controller then cancels before the host ever pushes Active is a
 *     boost that never started, and the spec's own no-cache-on-deny
 *     rationale ("a denied Boost never started anything") applies to it
 *     verbatim: without this reset the next genuinely unrelated
 *     host-initiated boost would emit BoostStarted carrying the
 *     CANCELLED command's duration and optionals. Introduced here as a
 *     disclosed C6 divergence while the C6's guard still left its cache
 *     set (B410); AT_MT_SPEC.md was amended to this
 *     consume-on-guarded-cancel rule and the C6 caught up in the parity
 *     round of 2026-09-01, so both ports now consume. Only the guarded
 *     arm consumes: an ALLOWED
 *     forwarded CancelBoost (Active state) still touches nothing, since
 *     the boost it cancels already consumed the cache at its
 *     BoostStarted.
 *   REPLACED: a second accepted Boost before any Active push simply
 *     overwrites the cache; the last accepted command is the one whose
 *     parameters the eventual BoostStarted carries.
 *   NEVER cleared by BoostEnded, a forwarded CancelBoost or a deny: only
 *     an allow writes it, and only the two consumers above (emission,
 *     the guarded cancel) reset it.
 *
 *   ONE DISCLOSED WIRE TENSION rides on the duration-0 emission, spec-
 *   mandated and deliberately NOT "fixed" here (review M2):
 *   WaterHeaterBoostInfoStruct declares Duration min="1"
 *   (water-heater-management-cluster.xml:42), so the "parameters unknown"
 *   BoostStarted sends a value the struct's own constraint excludes. The
 *   wire rule stands (AT_MT_SPEC.md 3.25 and mt_matter.h pin duration 0;
 *   the C6 emits the same 0), but a certification harness or a strict
 *   controller validating the decoded event against the constraint may
 *   reject it: a named TH risk for the bench, not a port defect.
 *
 * Emission discipline (the DEM section's rule, restated per path): the
 * derivation runs ONLY from mt_meas_whm_apply(), i.e. under the StackLock
 * mt_matter_meas_set() already holds, so GenerateBoostStartedEvent()/
 * GenerateBoostEndedEvent() (Delegate base helpers wrapping LogEvent,
 * water-heater-management-server.cpp:48-86) run lock-held. A Generate*
 * failure (event buffer exhaustion) is logged by the helper itself and
 * deliberately does NOT fail the push: the pushed state is applied and
 * served either way, so answering an error would tell the host "nothing
 * changed" about a change that took (the EEM branch's error-after-apply
 * caveat, without the misleading answer).
 * =========================================================================
 *
 * The CancelBoost in-state guard, and WHERE it lives, traced on both
 * platforms because the batch brief asks exactly that: the SDK server has
 * NO guard (Instance::HandleCancelBoost, water-heater-management-server
 * .cpp:232-241, is a bare delegate call; contrast DEM, where the server
 * itself answers InvalidInState). The guard is DELEGATE property on both
 * platforms: the SDK's own reference delegate skips the cancel body and
 * answers Success when not boosting (WhmDelegateImpl::HandleCancelBoost,
 * examples/energy-management-app), and the C6's HearthWhmDelegate returns
 * Status::Success without waking the host when the cached BoostState is
 * already Inactive, because the cluster test plan requires "status
 * SUCCESS and no event sent" for exactly that case (TC_EWATERHTR_2_2 step
 * 26; AT_MT_SPEC.md 3.17:1487-1495). This port mirrors the observable
 * behaviour (Success, no +MTCMD, no event) and ADDS the ruled
 * cache consume the lifecycle block above records (the B410 ruling). It
 * remains a divergence from the SDK reference delegate, which does not
 * clear its cache there; the C6 carried the same window until the parity
 * round of 2026-09-01 closed it, and the two ports now agree. The guard
 * reads the host-pushed BoostState cache, the same value a controller
 * reads.
 *
 * The ALLOWED (Active-state) CancelBoost emits no BoostEnded here, BY
 * DESIGN (review M4): the split-ownership rule again, the host owns the
 * state, so the event fires only when the host's BoostState 0 push
 * reaches the derivation. The SDK's own reference delegate emits
 * BoostEnded inside HandleCancelBoost instead; a host that actuates the
 * cancel and never pushes leaves the served BoostState Active with no
 * event, which is the host library documentation's line to carry, not a
 * firmware guard's.
 *
 * Lock discipline, the file's standing rule: HandleBoost/HandleCancelBoost
 * run on the CHIP task from the Instance's CHI invoke path, NO StackLock
 * taken; the push-path derivation runs from mt_meas_whm_apply(), which
 * inherits mt_matter_meas_set()'s.
 */
class HearthWhmDelegate : public chip::app::Clusters::WaterHeaterManagement::Delegate
{
public:
    /* Host-pushed cached state, written only by mt_meas_whm_apply(). The
     * defaults are the spec's pre-first-push answers (AT_MT_SPEC.md 3.25):
     * everything 0, BoostState Inactive; they match the zero-filled inert
     * shadows of whmAttrs (the seed table sets only FeatureMap and
     * ClusterRevision, mt_devtypes_sl_seeds.inc). */
    uint8_t  m_heater_types = 0;
    uint8_t  m_heat_demand  = 0;
    chip::app::Clusters::WaterHeaterManagement::BoostStateEnum m_boost_state =
        chip::app::Clusters::WaterHeaterManagement::BoostStateEnum::kInactive;
    uint16_t m_tank_volume  = 0;
    int64_t  m_est_heat_req = 0;
    uint8_t  m_tank_percent = 0;

    /* The parameters of the last host-ACCEPTED Boost command, cached for
     * the derived BoostStarted event. The full lifecycle contract is the
     * section comment's PARAMETER-CACHE block; the value-initialized
     * default (duration 0, every Optional missing) IS the "parameters
     * unknown" emission, so consuming the cache is one aggregate reset. */
    struct BoostParams {
        uint32_t                      duration = 0;
        chip::Optional<bool>          one_shot;
        chip::Optional<bool>          emergency;
        chip::Optional<int16_t>       setpoint;
        chip::Optional<chip::Percent> target_pct;
        chip::Optional<chip::Percent> reheat;
    } m_boost;

    chip::EndpointId endpoint() const { return mEndpointId; }

    /* The endpoint's WHM feature bits, stamped by mt_matter_whm_register()
     * from the create path's variant predicate, the single source the
     * FeatureMap seed and the Instance mask share (the DEM with_pa
     * divergence's reasoning: the C6 reads its live FeatureMap attribute
     * back, but this port's ember copy is a seed shadow, so the port
     * record stamped from the same predicate is the honest source). Read
     * by mt_meas_whm_apply()'s field gates. */
    void set_features(uint32_t mask) { m_features = mask; }
    bool has_feature(chip::app::Clusters::WaterHeaterManagement::Feature f) const
    {
        return (m_features & chip::to_underlying(f)) != 0;
    }

    /*
     * Boost: pack the five Optionals into the wire form
     * "<duration>,<mask>[,<v1>[,<v2>[,<v3>]]]" (AT_MT_SPEC.md 3.17's
     * five-field tail, the first five-field +MTCMD consumer):
     * MT_BOOST_P_* presence bits in canonical order, MT_BOOST_V_* carrying
     * the two bools' VALUES when present (a present bool needs no appended
     * field), then ONLY the present numeric optionals appended in
     * canonical order, temporarySetpoint / targetPercentage / targetReheat
     * (core/include/mt_matter.h:986-992; worked example: duration 3600,
     * oneShot true, targetPercentage 80 forwards as "3600,265,80"). The
     * Instance has already range-checked targetPercentage/targetReheat and
     * their feature conformance before calling this
     * (water-heater-management-server.cpp:171-222), so the packing needs
     * no validation of its own. The verdict is the wire response raw:
     * HandleBoost's Status lands in AddStatus unmapped (:224-225).
     * Accepted parameters are cached ON ALLOW ONLY (the lifecycle
     * contract above).
     */
    chip::Protocols::InteractionModel::Status HandleBoost(
        uint32_t duration, chip::Optional<bool> oneShot, chip::Optional<bool> emergencyBoost,
        chip::Optional<int16_t> temporarySetpoint, chip::Optional<chip::Percent> targetPercentage,
        chip::Optional<chip::Percent> targetReheat) override
    {
        using chip::Protocols::InteractionModel::Status;

        unsigned mask = 0;
        if (oneShot.HasValue()) {
            mask |= MT_BOOST_P_ONESHOT;
            if (oneShot.Value()) {
                mask |= MT_BOOST_V_ONESHOT;
            }
        }
        if (emergencyBoost.HasValue()) {
            mask |= MT_BOOST_P_EMERGENCY;
            if (emergencyBoost.Value()) {
                mask |= MT_BOOST_V_EMERGENCY;
            }
        }
        if (temporarySetpoint.HasValue()) {
            mask |= MT_BOOST_P_SETPOINT;
        }
        if (targetPercentage.HasValue()) {
            mask |= MT_BOOST_P_TARGET_PCT;
        }
        if (targetReheat.HasValue()) {
            mask |= MT_BOOST_P_REHEAT;
        }

        char fields[48];
        int n = snprintf(fields, sizeof(fields), "%lu,%u", (unsigned long)duration, mask);
        if (temporarySetpoint.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%d", (int)temporarySetpoint.Value());
        }
        if (targetPercentage.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%u",
                          (unsigned)targetPercentage.Value());
        }
        if (targetReheat.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%u", (unsigned)targetReheat.Value());
        }
        (void)n;

        bool allow = mt_cmd_forward_fields(
            mEndpointId, chip::app::Clusters::WaterHeaterManagement::Id,
            chip::app::Clusters::WaterHeaterManagement::Commands::Boost::Id, fields);
        if (!allow) {
            return Status::Failure;
        }
        m_boost.duration   = duration;
        m_boost.one_shot   = oneShot;
        m_boost.emergency  = emergencyBoost;
        m_boost.setpoint   = temporarySetpoint;
        m_boost.target_pct = targetPercentage;
        m_boost.reheat     = targetReheat;
        return Status::Success;
    }

    /*
     * CancelBoost: in-state guard FIRST, without waking the host, since
     * there is nothing for the host to adjudicate (the section comment
     * traces where the guard lives on each platform; the SDK server has
     * none). The guarded answer is SUCCESS-and-silence, not a failure,
     * AND it consumes the parameter cache (B410 ruling, the lifecycle
     * block's ALSO CONSUMED arm: a cancelled not-yet-started boost is a
     * boost that never started, so its parameters must not survive to
     * describe some future unrelated one; the C6 closed its identical
     * stale window in the 2026-09-01 parity round). The "no event" half
     * holds by
     * construction: no forward means no host actuation, no BoostState
     * push, no Active-to-Inactive transition, so the derivation never
     * emits BoostEnded. Otherwise forward command 1 payload-less (NULL
     * fields reproduces mt_cmd_forward()'s exact four-field +MTCMD line)
     * and pass the verdict through raw, same as Boost above; the
     * forwarded arm touches no state and emits nothing here, the M4
     * design note in the section comment.
     */
    chip::Protocols::InteractionModel::Status HandleCancelBoost() override
    {
        using chip::Protocols::InteractionModel::Status;
        using chip::app::Clusters::WaterHeaterManagement::BoostStateEnum;

        if (m_boost_state == BoostStateEnum::kInactive) {
            m_boost = BoostParams{};
            return Status::Success;
        }
        bool allow = mt_cmd_forward_fields(
            mEndpointId, chip::app::Clusters::WaterHeaterManagement::Id,
            chip::app::Clusters::WaterHeaterManagement::Commands::CancelBoost::Id, NULL);
        return allow ? Status::Success : Status::Failure;
    }

    /* The six getters, serving the host-pushed cache. */
    chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>
    GetHeaterTypes() override
    {
        return chip::BitMask<
            chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(
            m_heater_types);
    }
    chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>
    GetHeatDemand() override
    {
        return chip::BitMask<
            chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(
            m_heat_demand);
    }
    uint16_t GetTankVolume() override { return m_tank_volume; }
    chip::Energy_mWh GetEstimatedHeatRequired() override { return m_est_heat_req; }
    chip::Percent GetTankPercentage() override { return m_tank_percent; }
    chip::app::Clusters::WaterHeaterManagement::BoostStateEnum GetBoostState() override
    {
        return m_boost_state;
    }

private:
    uint32_t m_features = 0;
};

/*
 * The pool, MT_WHM_MAX (4, core/include/mt_matter.h:952, the C6 depth per
 * DE407) delegates plus raw aligned Instance storage, the DEM pool's exact
 * shape: this port constructs the Instance in mt_matter_whm_register()
 * with the variant's own mask, where the C6 lets esp-matter's
 * WaterHeaterManagementDelegateInitCB new it from a FeatureMap snapshot at
 * endpoint enable. Endpoint id stamped in the SUCCESS-ONLY second half by
 * the Instance constructor (water-heater-management-server.h:142-149),
 * never at handout: the alloc takes the id per the header's contract
 * (mt_matter.h:994-1016) and discards it, fix round M1, the full reasoning at
 * the alloc below. Exhaustion aborts the create before anything is spent;
 * never destroyed (~Instance() would Shutdown() cleanly, but this
 * platform has no teardown path, the standing allocate-only policy).
 */
static HearthWhmDelegate *s_whm_delegates[MT_WHM_MAX];
static size_t            s_whm_next;

/* Pinned (catalogue batch 7b, DE584): the WHM pair's arena cost and the
 * water heater admission maximum with its v0 draw (the pair, the
 * WaterHeaterMode ModeBase pair and the EPM and PowerTopology graft). */
static_assert(hearth_arena_cost(obj_pair_bytes<HearthWhmDelegate,
                                                chip::app::Clusters::WaterHeaterManagement::Instance>()) == 88,
              "the WHM pair changed size; redo the water heater admission arithmetic");
static_assert(MT_WHM_MAX * (88 + 80 + 152 + 40) <= HEARTH_OBJ_ARENA_BYTES,
              "MT_WHM_MAX water heaters' objects no longer fit the cluster-object arena (DE584)");

/* Fix round M1, one discipline for both id-at-alloc pools (this one and
 * the DEM's): the id passed per the header's alloc(ep) contract is
 * DELIBERATELY NOT stamped here. s_next_ep_id is only the id this create
 * WILL assign if nothing after the claim fails, and a claim stranded by a
 * later failure kept that stamp while the NEXT successful create took the
 * same id: whm_for()'s first-match walk would then bind pushes and
 * carve-out reads to the dead delegate while the fabric served the live
 * Instance, a silent split. The sentinel makes a stranded claim
 * unmatchable (dynamic endpoints are 1..N, never kInvalidEndpointId), and
 * the REAL id lands in the success-only second half, where
 * mt_matter_whm_register()'s Instance construction runs
 * mDelegate.SetEndpointId(ep) (water-heater-management-server.h:142-149):
 * the id-in-the-second-half discipline every other pool in this file
 * already follows (ModeBase, opstate, valve, EPM/PTOP), now uniform. The
 * header's stamp-at-handout rationale is C6 init-callback timing that
 * does not exist here (header read-only, the standing staleness
 * disposition). */
extern "C" void *mt_matter_whm_delegate_alloc(uint16_t ep)
{
    (void)ep;
    if (s_whm_next >= MT_WHM_MAX) {
        return nullptr;
    }
    HearthWhmDelegate *d =
        obj_pair_new<HearthWhmDelegate,
                     chip::app::Clusters::WaterHeaterManagement::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->SetEndpointId(chip::kInvalidEndpointId);
    s_whm_delegates[s_whm_next++] = d;
    return d;
}

/* The pool lookup the AT+MTMEAS 0x94 branch and the DE397 live reader
 * use, the meas_epm_for() shape. */
static HearthWhmDelegate *whm_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_whm_next; i++) {
        if (s_whm_delegates[i]->endpoint() == ep) {
            return s_whm_delegates[i];
        }
    }
    return nullptr;
}

/*
 * The WHM second half: construct the Instance with the variant's feature
 * mask and Init() it (CHI registration then AAI, SOFT on both,
 * water-heater-management-server.cpp:94-100: no emberAfContainsServer
 * check anywhere, so ordering mistakes cannot panic; below the successful
 * create by the same strand-nothing reasoning as the measurement halves).
 * with_em_tp is the create path's variant predicate, the single source
 * the variant-qualified WHM FeatureMap seed rows mirror
 * (mt_devtypes_sl_seeds.inc), and is recorded on the delegate for
 * mt_meas_whm_apply()'s field gates. The ctor takes a single Feature value
 * that seeds a BitMask (server.h:142-149), so the two-bit mask is cast
 * through the underlying type, the DEM register's own idiom.
 */
extern "C" void mt_matter_whm_register(void *delegate, uint16_t ep, bool with_em_tp)
{
    using namespace chip::app::Clusters::WaterHeaterManagement;
    auto *d = static_cast<HearthWhmDelegate *>(delegate);
    uint32_t mask = with_em_tp ? (chip::to_underlying(Feature::kEnergyManagement) |
                                  chip::to_underlying(Feature::kTankPercent))
                               : 0;
    d->set_features(mask);
    auto *inst = new (obj_inst_storage<HearthWhmDelegate, Instance>(d))
        Instance(ep, *d, static_cast<Feature>(mask));
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "WHM Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; the cluster will serve nothing and its commands will not dispatch",
                (unsigned)ep, err.Format());
    }
}

/*
 * AT+MTMEAS's WaterHeaterManagement (0x0094) branch. Called only from
 * mt_matter_meas_set(), which already holds the StackLock and resolved the
 * endpoint and cluster lookups; this function owns the field table
 * (AT_MT_SPEC.md 3.25's 0x0094 rows) and the derivation state machine the
 * section comment above fixes.
 *
 * Feature gate, bridge-side by design (mt_at.c only classifies
 * signedness): TankVolume and EstimatedHeatRequired exist only under
 * EnergyManagement, TankPercentage only under TankPercent
 * (water-heater-management-cluster.xml:73-87), and a push to a gated field
 * on an endpoint without the feature answers MT_ATTR_ERR_CLUSTER
 * (+MTERR:3), the same data-model code an energy push gets on a
 * power-only electrical endpoint: "this endpoint does not serve that
 * data" is one condition, not two (the spec's own words). The bits come
 * from the delegate's port record, stamped from the identical variant
 * predicate the FeatureMap seed and the Instance mask derive from, so the
 * gate and the served FeatureMap cannot disagree (the section comment's
 * honest-source note; the C6 reads its live FeatureMap attribute instead
 * because esp-matter's copy is authoritative there).
 *
 * Value bounds, the cluster XML: HeaterTypes/HeatDemand are
 * WaterHeaterHeatSourceBitmap (defined bits 0x01..0x10, so 0..0x1F);
 * BoostState is enum8 0/1; TankVolume u16; EstimatedHeatRequired is
 * energy_mwh with min 0 (int64 on the wire for pipeline symmetry, so the
 * negative half is cut here, +MTERR:1 through MT_ATTR_ERR_VALUE);
 * TankPercentage is percent 0..100. Two passes, the family's hard
 * contract: every pair is validated (range AND feature gate) before any
 * pair is applied.
 */
static int mt_meas_whm_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count)
{
    using namespace chip::app::Clusters::WaterHeaterManagement;

    HearthWhmDelegate *d = whm_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run; defensive, the pool
         * bridges' standing answer. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Pass 1: validate everything, range and feature gate both. */
    for (uint8_t i = 0; i < count; i++) {
        switch (fields[i]) {
        case MT_WHM_F_HEATER_TYPES:
        case MT_WHM_F_HEAT_DEMAND:
            if (values[i] < 0 || values[i] > 0x1F) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_BOOST_STATE:
            if (values[i] < 0 || values[i] > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_TANK_VOLUME:
            if (!d->has_feature(Feature::kEnergyManagement)) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > 0xFFFF) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_EST_HEAT_REQ:
            if (!d->has_feature(Feature::kEnergyManagement)) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > kMeasValueAbsMax) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_TANK_PERCENT:
            if (!d->has_feature(Feature::kTankPercent)) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > 100) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Pass 2: apply, one subscription report per applied field (same-state
     * BoostState pushes included, the per-sample rule the section comment
     * contrasts with DEM), deriving the boost events on BoostState
     * transitions AS THEY ARE APPLIED, in pair order. */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t attr_id;
        switch (fields[i]) {
        case MT_WHM_F_HEATER_TYPES:
            d->m_heater_types = (uint8_t)values[i];
            attr_id = Attributes::HeaterTypes::Id;
            break;
        case MT_WHM_F_HEAT_DEMAND:
            d->m_heat_demand = (uint8_t)values[i];
            attr_id = Attributes::HeatDemand::Id;
            break;
        case MT_WHM_F_BOOST_STATE: {
            /* THE STATE MACHINE (the section comment's transition table).
             * prev is the cache value this pair replaces; the write
             * happens before the emission so a Generate* helper that
             * somehow read the attribute back would see the new state,
             * and the same-state arm falls through to the per-sample
             * dirty report below with no event. */
            BoostStateEnum next = (values[i] != 0) ? BoostStateEnum::kActive
                                                   : BoostStateEnum::kInactive;
            BoostStateEnum prev = d->m_boost_state;
            d->m_boost_state    = next;
            if (prev != next) {
                if (next == BoostStateEnum::kActive) {
                    /* Inactive -> Active: BoostStarted with the cached
                     * last-accepted parameters, then CONSUME the cache
                     * (aggregate reset to duration 0 / no optionals, the
                     * "parameters unknown" default a later
                     * host-initiated boost must emit). Lock-held per the
                     * emission discipline; failure logged by the helper,
                     * never failing the push. */
                    (void)d->GenerateBoostStartedEvent(d->m_boost.duration, d->m_boost.one_shot,
                                                       d->m_boost.emergency, d->m_boost.setpoint,
                                                       d->m_boost.target_pct, d->m_boost.reheat);
                    d->m_boost = HearthWhmDelegate::BoostParams{};
                } else {
                    /* Active -> Inactive: BoostEnded. The parameter cache
                     * is NOT touched here (only emission consumes it,
                     * only an allow writes it). */
                    (void)d->GenerateBoostEndedEvent();
                }
            }
            attr_id = Attributes::BoostState::Id;
            break;
        }
        case MT_WHM_F_TANK_VOLUME:
            d->m_tank_volume = (uint16_t)values[i];
            attr_id = Attributes::TankVolume::Id;
            break;
        case MT_WHM_F_EST_HEAT_REQ:
            d->m_est_heat_req = values[i];
            attr_id = Attributes::EstimatedHeatRequired::Id;
            break;
        default: /* MT_WHM_F_TANK_PERCENT, pass 1 admits nothing else */
            d->m_tank_percent = (uint8_t)values[i];
            attr_id = Attributes::TankPercentage::Id;
            break;
        }
        MatterReportingAttributeChangeCallback(ep, Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * DE397 live read for the six WHM values, dispatched from
 * mt_matter_attr_read() under its StackLock: the delegate cache is the
 * served truth (the five narrow shadows are seeds, EstimatedHeatRequired
 * has no slot at all). None of the six is nullable; EstimatedHeatRequired
 * is the one signed answer (energy_mwh, the INT64S alias family).
 */
static int mt_whm_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    namespace WHM = chip::app::Clusters::WaterHeaterManagement;
    HearthWhmDelegate *d = whm_for(ep);
    if (d == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    bool u = true;
    int64_t v;
    switch (attr) {
    case WHM::Attributes::HeaterTypes::Id:    v = d->m_heater_types; break;
    case WHM::Attributes::HeatDemand::Id:     v = d->m_heat_demand; break;
    case WHM::Attributes::TankVolume::Id:     v = d->m_tank_volume; break;
    case WHM::Attributes::TankPercentage::Id: v = d->m_tank_percent; break;
    case WHM::Attributes::BoostState::Id:     v = chip::to_underlying(d->m_boost_state); break;
    case WHM::Attributes::EstimatedHeatRequired::Id:
        v = d->m_est_heat_req;
        u = false;
        break;
    default:
        /* Unreachable: only the six k_instance_served rows route here. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    if (is_unsigned) {
        *is_unsigned = u;
    }
    *out = v;
    return MT_ATTR_OK;
}
