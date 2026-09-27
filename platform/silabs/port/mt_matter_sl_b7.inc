/*
 * mt_matter_sl_b7.inc: a fragment of mt_matter_sl.cpp, NOT a compile unit.
 *
 * Holds bridge code: catalogue batch 7a-1 (nRF mt_matter_zephyr.cpp
 * 4581-5104 and 5106-5346: the measurement family's ElectricalPowerMeasurement
 * and PowerTopology delegates and pools, the ElectricalEnergyMeasurement
 * reserve and register, the EPM live read and the AT+MTMEAS bridge),
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
 * Cluster admission is a batch boundary on this port: catalogue batch 7a-1
 * serves 0x0090 and 0x0091 only; 0x0098 returns with batch 7a-2, 0x0094
 * with batch 7b and 0x0099 with the EVSE round (the accept test below
 * names them). An id outside the served set answers MT_ATTR_ERR_CLUSTER.
 */
extern "C" int mt_matter_meas_set(uint16_t ep, uint32_t cluster, const uint8_t *fields,
                                  const int64_t *values, uint8_t count)
{
    using namespace chip::app::Clusters;
    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    /* Catalogue batch 7a-1 on this port: the measurement family only. The
     * nRF also routes DeviceEnergyManagement (0x0098, back in batch 7a-2),
     * WaterHeaterManagement (0x0094, batch 7b) and EnergyEvse (0x0099, the
     * EVSE round) here; until their batches those answer "no such cluster". */
    if (cluster != ElectricalPowerMeasurement::Id && cluster != ElectricalEnergyMeasurement::Id) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (!emberAfContainsServer(ep, cluster)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1) {
        /* Defensive; mt_at.c never passes an empty list. */
        return MT_ATTR_ERR_FAILED;
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

