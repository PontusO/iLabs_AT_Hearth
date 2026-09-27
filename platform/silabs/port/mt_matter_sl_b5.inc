/*
 * mt_matter_sl_b5.inc: a fragment of mt_matter_sl.cpp, NOT a compile unit.
 *
 * Holds bridge code: catalogue batch 5a (nRF mt_matter_zephyr.cpp
 * 3130-3167, the AT+MTSWITCH bridge) and catalogue batch 5b (nRF
 * mt_matter_zephyr.cpp 3314-4005, the RVC's ModeBase clusters, and later
 * 4367-4579), included at the end of mt_matter_sl.cpp after
 * mt_matter_sl_b4.inc, so the whole is still one translation unit. Do not
 * add it to hearth.slcp.
 */

/* ---- generic switch (0x000F) ------------------------------------------ */

/*
 * AT+MTSWITCH bridge (AT_MT_SPEC.md 3.15): emit the Switch cluster's
 * InitialPress event at position 1, the upstream arduino-esp32 class's
 * click(). mt_at.c's cmd_mtswitch has already rejected any action other
 * than 0 with +MTERR:1, so this bridge only ever emits InitialPress.
 *
 * The StackLock is load-bearing, not just uniformity: EventLogging.h says
 * "The consumer has to either lock the Matter stack lock or queue the
 * event to the Matter event queue when using LogEvent. This function is
 * not safe to call outside of the main Matter processing context", and
 * OnInitialPress() is a bare LogEvent() passthrough
 * (switch-server.cpp:66-78). Same quote the C6 cites at its own call site
 * (platform/esp32c6/main/main.cpp mt_matter_switch_click()).
 *
 * The two lookups are attr_locate()'s, for the usual error division.
 * Unlike the C6's esp-matter helper (send_initial_press, which returns
 * esp_err_t), this tree's OnInitialPress() returns VOID: a LogEvent
 * failure is logged by the server and not reported to the caller, so
 * mt_matter.h's MT_ATTR_ERR_FAILED arm ("the event send itself fails") is
 * unreachable on this platform and the checks above are the whole failure
 * surface. Nothing echoes on the AT link; subscribed controllers see the
 * event. CurrentPosition is deliberately not written (the C6 does not
 * either; the cluster's event path never touches it).
 */
extern "C" int mt_matter_switch_click(uint16_t ep)
{
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::Switch::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    chip::app::Clusters::SwitchServer::Instance().OnInitialPress(ep, 1);
    return MT_ATTR_OK;
}

/* ---- robotic vacuum cleaner: the two ModeBase clusters ----------------
 *
 * RvcRunMode (0x0054) and RvcCleanMode (0x0055) are concrete derivations
 * of the abstract ModeBase cluster, one shared Delegate interface and one
 * shared Instance class parameterised by cluster id at CONSTRUCTION (the
 * four-argument ctor, mode-base-server.h:46: delegate, endpoint, cluster,
 * feature). One delegate OBJECT plus one Instance per (endpoint, cluster)
 * PAIR: an RVC endpoint carries both clusters at once, so it consumes two
 * pool slots, unlike every earlier per-endpoint pool in this file.
 *
 * THE TWO WAYS THIS MACHINERY KILLS OR SILENTLY CRIPPLES THE DEVICE, and
 * why the code below is shaped the way it is. Instance::Init()
 * (mode-base-server.cpp:71-84) does, in order:
 *
 *   1. ReturnErrorOnFailure(mDelegate->GetModeValueByIndex(0, mCurrentMode))
 *      (:74). This runs BEFORE anything else, before any AT+MTMODES could
 *      possibly have fed a list. If the delegate answers
 *      PROVIDER_LIST_EXHAUSTED at index 0, Init() returns early and the
 *      Instance is NEVER REGISTERED: no abort, no log from the SDK, reads
 *      and ChangeToMode on that cluster simply go unanswered. That is why
 *      the placeholder-mode-0 policy below is MANDATORY, not a nicety: an
 *      empty store still answers index 0 with mode 0, the cluster's tag-0
 *      default and label "Mode0" (mirrored from the C6, main.cpp:
 *      2429-2470). The port checks Init()'s return and logs loudly
 *      besides, which the C6's SDK init-callback path cannot (it discards
 *      the return outright).
 *
 *   2. VerifyOrDie(emberAfContainsServer(mEndpointId, mClusterId)) (:77).
 *      A PANIC, not the soft CHIP_ERROR_INVALID_ARGUMENT bail the
 *      OperationalState machinery gives the same mistake
 *      (operational-state-server.cpp:63-70). Constructing and Init()ing a
 *      ModeBase Instance before emberAfSetDynamicEndpoint() succeeds
 *      ABORTS THE DEVICE. The construct-and-Init therefore lives only in
 *      mt_matter_modebase_delegate_set_endpoint(), which
 *      mt_devtype_create() calls strictly below a successful
 *      emberAfSetDynamicEndpoint(); nothing else may ever construct one.
 *
 * One-delegate-per-Instance is DISCIPLINE here, not an enforced abort:
 * ModeBase::Delegate::SetInstance() is a plain setter
 * (`void SetInstance(Instance * aInstance) { mInstance = aInstance; }`,
 * mode-base-server.h:256), with NO VerifyOrDie. This contradicts the C6's
 * own comment (main.cpp:2408-2412 claims the identical VerifyOrDie
 * contract as OperationalState, whose SetInstance really does die,
 * operational-state-server.h:349-353); verified against this tree and the
 * C6's citation deliberately NOT copied. A shared delegate would not
 * abort, it would silently answer through the wrong Instance, which is
 * worse; the one-pair-per-slot pool below is what keeps the rule.
 *
 * Init() also LoadPersistentAttributes() (:79): three KVS reads per
 * (endpoint, cluster) at every boot, and UpdateCurrentMode() persists on
 * every controller ChangeToMode (:178), through the KVS (NVM3 on this
 * port; the settings region the README keeps under a sizing watch). Two
 * ModeBase clusters per RVC endpoint makes this a second wear source
 * after the chime's. Also live but
 * inert: Init()'s OnOff coupling block compiles here
 * (MATTER_DM_PLUGIN_ON_OFF_SERVER is defined, OnOff is in hearth.zap) and
 * is skipped at :132 because no RVC endpoint carries an OnOff server.
 *
 * The header defines `static IntrusiveList<Instance>
 * gModeBaseAliasesInstances` IN THE HEADER (mode-base-server.h:279), so
 * this translation unit gets a private copy; only the SDK's own
 * GetModeBaseInstanceList() accessor touches the real one, and this file
 * must never name the symbol.
 *
 * The mode store is block-resident (mt_mb_store_t, mt_dyn_store.h),
 * reached through mt_dyn_mb_store(ep, cluster): only RVC endpoints pay,
 * and count 0 is the placeholder state. Unlike mt_mode_store_t there is
 * no struct/CharSpan half: every ModeBase read COPIES into caller-owned
 * memory (GetModeLabelByIndex fills a MutableCharSpan, GetModeTagsByIndex
 * a caller-supplied List, mode-base-server.h:205, :235), so no lifetime
 * argument is needed beyond the lock.
 */
class HearthModeBaseDelegate : public chip::app::Clusters::ModeBase::Delegate
{
public:
    void set_cluster(chip::ClusterId cluster) { m_cluster = cluster; }
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }
    chip::ClusterId cluster() const { return m_cluster; }

    /* GetInstance() is protected in the base Delegate; this passthrough
     * is how mt_matter_modebase_set() clamps CurrentMode and how the
     * DE397 live reader answers it, the HearthOpStateDelegate mechanism. */
    chip::app::Clusters::ModeBase::Instance *instance() { return GetInstance(); }

    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }

    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, chip::MutableCharSpan &label) override
    {
        mt_mb_store_t *slot = mt_dyn_mb_store(m_ep, m_cluster);
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            /* The placeholder: index 0 must answer (see the section
             * comment; Instance::Init() reads it before any host list can
             * exist). Superseded the moment AT+MTMODES stores a real
             * list; no separate flag, count > 0 simply wins. */
            return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan::fromCharString("Mode0"),
                                                       label);
        }
        if (modeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        return chip::CopyCharSpanToMutableCharSpan(
            chip::CharSpan::fromCharString(slot->entries[modeIndex].label), label);
    }

    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t &value) override
    {
        mt_mb_store_t *slot = mt_dyn_mb_store(m_ep, m_cluster);
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            value = 0;
            return CHIP_NO_ERROR;
        }
        if (modeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        value = slot->entries[modeIndex].mode;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetModeTagsByIndex(
        uint8_t modeIndex,
        chip::app::DataModel::List<chip::app::Clusters::detail::Structs::ModeTagStruct::Type> &tags)
        override
    {
        mt_mb_store_t *slot = mt_dyn_mb_store(m_ep, m_cluster);
        uint16_t tag_value;
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            tag_value = placeholder_tag();
        } else {
            if (modeIndex >= slot->count) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            tag_value = slot->entries[modeIndex].tag;
        }
        /* The SDK hands in a buffer sized for its own tag maximum; this
         * delegate publishes exactly one tag per mode, so size 1 always
         * fits. Guarded anyway, the C6's defensive shape. */
        if (tags.size() < 1) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        tags[0].value = tag_value;
        tags.reduce_size(1);
        return CHIP_NO_ERROR;
    }

    /*
     * ChangeToMode::Id is 0x00 for both RVC mode clusters (each generated
     * CommandIds.h declares it), so one constant serves both and
     * m_cluster, fixed at alloc time, is what routes the +MTCMD to the
     * right wire cluster. The SDK's own pre-check answers UnsupportedMode
     * before this is ever called (Instance::HandleChangeToMode validates
     * membership first, mode-base-server.cpp:382-421), and a
     * same-as-current mode answers success without calling it either
     * (:406), so only genuine transitions reach the host, and only
     * kSuccess/kGenericFailure are this function's business. The verdict
     * IS the wire response: HandleChangeToMode copies response.status
     * straight out. Runs on the CHIP task (the Instance's own
     * CommandHandlerInterface; both clusters are CHI-only), no StackLock,
     * the standing hook discipline.
     */
    void HandleChangeToMode(
        uint8_t NewMode,
        chip::app::Clusters::ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        bool allow = mt_cmd_forward_payload(
            m_ep, m_cluster, chip::app::Clusters::RvcRunMode::Commands::ChangeToMode::Id, NewMode);
        response.status =
            chip::to_underlying(allow ? chip::app::Clusters::ModeBase::StatusCode::kSuccess
                                      : chip::app::Clusters::ModeBase::StatusCode::kGenericFailure);
    }

private:
    /* Tag-0 defaults, placeholder case only (index 0 of an unfed store):
     * RvcRunMode kIdle (0x4000), RvcCleanMode kVacuum (0x4001), the
     * AT_MT_SPEC.md 3.20 table's exact policy, read from the enums rather
     * than transcribed. mt_matter_modebase_set() applies the same table
     * to real host entries at store time. */
    uint16_t placeholder_tag() const
    {
        using namespace chip::app::Clusters;
        if (m_cluster == RvcRunMode::Id) {
            return chip::to_underlying(RvcRunMode::ModeTag::kIdle);
        }
        if (m_cluster == DeviceEnergyManagementMode::Id) {
            /* Batch 7a: kNoOptimization on every mode, the AT_MT_SPEC.md
             * 3.20 table row (no mandatory tag in the cluster XML; a mode
             * the host has not described makes no optimization
             * promise). */
            return chip::to_underlying(DeviceEnergyManagementMode::ModeTag::kNoOptimization);
        }
        if (m_cluster == WaterHeaterMode::Id) {
            /* Batch 7b: kManual (0x4001) on every mode, the AT_MT_SPEC.md
             * 3.20 table row (Mode_WaterHeater.xml names no mandatory tag
             * either; Manual is the everyday operating mode a host that
             * does not care about tags most plausibly means, the OvenMode
             * reasoning the spec row records). */
            return chip::to_underlying(WaterHeaterMode::ModeTag::kManual);
        }
        if (m_cluster == MicrowaveOvenMode::Id) {
            /* Batch 8: kNormal (0x4000) on every mode, the AT_MT_SPEC.md
             * 3.20 table row, and THE ONE ARM IN THIS FUNCTION THAT IS
             * LOAD-BEARING BEYOND ITS OWN CLUSTER. MicrowaveOvenControl's
             * SetCookingParameters resolves an omitted cookMode through
             * GetModeValueByModeTag(kNormal), so this value is what makes a
             * freshly composed microwave answer cooking commands before the
             * host has ever sent AT+MTMODES. Get it wrong and every
             * SetCookingParameters answers InvalidCommand with no +MTCMD
             * raised, forever and silently. On the nRF, mt_matter_mwoc_register()
             * reads it back at create time precisely so that cannot happen
             * unnoticed; no microwave device type is ported to this one and
             * the readback has no counterpart here, so this arm is inert in
             * this build, kept for the bridge's copy-whole shape. */
            return chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal);
        }
        if (m_cluster == OvenMode::Id) {
            /* Batch 8: kBake (0x4000) on every mode, the AT_MT_SPEC.md 3.20
             * table row. Mode_Oven.xml mandates no tag, and Bake is the
             * everyday oven mode a host that does not care about tags most
             * plausibly means, the WaterHeaterMode kManual reasoning. */
            return chip::to_underlying(OvenMode::ModeTag::kBake);
        }
        if (m_cluster == RefrigeratorAndTemperatureControlledCabinetMode::Id) {
            /* Batch 8: kAuto on every mode, the AT_MT_SPEC.md 3.20 table
             * row. Worth pointing at, because it is the one arm here whose
             * substituted tag VALUE is itself 0: kAuto is a ModeBase COMMON
             * tag (0x0000), not a cluster-specific one in the 0x4000 range
             * like every other arm in this function. The substitution is
             * still real and still the right answer; it just happens to be
             * a no-op on the wire, and a reader who assumes every default
             * tag is 0x4000-something will misread this line. */
            return chip::to_underlying(
                RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kAuto);
        }
        if (m_cluster == EnergyEvseMode::Id) {
            /* The EVSE round: kManual (0x4000) on the placeholder.
             * Mode_EVSE.xml mandates no tag, and Manual is the
             * everyday "charge when plugged in" mode a host that has not
             * described its modes most plausibly means, the WaterHeaterMode
             * and OvenMode reasoning. It is also the value the C6 already
             * produces here, by falling through to kNormal (0x4000) rather
             * than by choosing: written out on this arm so the two platforms
             * agree ON PURPOSE. Without it this port would fall through to
             * kVacuum (0x4001), which on this cluster's enum is kTimeOfUse:
             * a defensible-sounding tag arrived at entirely by accident, and
             * a different one from the C6's.
             *
             * This arm WAS the only tag an EnergyEvseMode endpoint ever
             * carried, because AT+MTMODES did not admit cluster 0x009D. Spec
             * 3.20.1 admits it since 2026-08-31 and
             * mt_matter_modebase_set() below does too, so the placeholder is
             * a pre-feed state again, like every other arm here. The value
             * did not move: the spec's tag-0 default row for this cluster is
             * the kManual this arm already chose. */
            return chip::to_underlying(EnergyEvseMode::ModeTag::kManual);
        }
        return chip::to_underlying(RvcCleanMode::ModeTag::kVacuum);
    }

    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    chip::ClusterId m_cluster = chip::kInvalidClusterId;
};

/*
 * Pool sizing, this port's own (the nRF port's DE404 arithmetic did not
 * copy over, because this port's composition is the RVC alone): 20 slots,
 * which is 2 x 9 plus headroom, two ModeBase pairs per RVC endpoint times
 * the NINE RVC endpoints this build admits (graph DE566). On the nRF the
 * number was argued from its 8,112 usable block heap and a 864 B RVC
 * block, against four or more ModeBase consumers; none of that applies
 * here, where the endpoint arena is 9,728 usable bytes against a 936 B
 * RVC block and the RVC is the ONLY ModeBase consumer in this build. The
 * tenth RVC is refused by the cluster-object arena, not by this pool:
 * nine RVCs draw nine x (264 + 2 x 80) = 3,816 B of it and the tenth's
 * 4,240 B total would not fit the 4,096 B arena, while the pool still has
 * two slots of its own left (the assert beside the RvcOperationalState
 * pair below pins both walls). 20 = 18 + 2, the two being headroom the
 * nRF's batch 7a ruling added, kept as is. The pool is checked first in
 * mt_matter_modebase_delegate_alloc() and exhaustion aborts the create
 * before anything is spent, loudly either way. MT_MB_MAX_LISTS is gone
 * from core/include/mt_matter.h (pay-per-composition round, task 7): the
 * C6 no longer has a fixed ModeBase pool at all, it allocates its
 * mt_mb_store_t per (endpoint, cluster) through its store index
 * (mt_store_index.c) against the runtime heap floor; this pool is
 * unrelated to that, it is this port's own ModeBase Instance/delegate
 * pool.
 *
 * The Instance pool is alignas raw storage (the four-argument ctor needs
 * create-time values), placement-constructed exactly once per slot in
 * mt_matter_modebase_delegate_set_endpoint() and never destroyed:
 * ~Instance() would Shutdown() and unregister cleanly
 * (mode-base-server.cpp:55-69), but this platform has no teardown path
 * (AT+MTEP edits apply by reboot, which resets every pool wholesale), the
 * standing allocate-only policy.
 */
constexpr size_t kModeBasePoolSlots = 20;

/* Memory reclaim round A: a table of pointers into the cluster-object
 * heap, each block holding the delegate with the raw storage for its
 * ModeBase Instance behind it. kModeBasePoolSlots is unchanged and is
 * still checked first, so the acceptance cap DE404 argued for is exactly
 * what it was. */
static HearthModeBaseDelegate *s_mb_delegates[kModeBasePoolSlots];
static size_t s_mb_delegate_next;
/* The ModeBase Delegate + Instance pair, 80 B on the nRF's measurement;
 * this pins the MG24's (graph DE566: an RVC draws two of it). */
static_assert(hearth_arena_cost(obj_pair_bytes<HearthModeBaseDelegate,
                                                chip::app::Clusters::ModeBase::Instance>()) == 80,
              "the ModeBase pair changed size; redo the RVC's admission arithmetic");

extern "C" void *mt_matter_modebase_delegate_alloc(uint32_t cluster_id)
{
    if (s_mb_delegate_next >= kModeBasePoolSlots) {
        return nullptr;
    }
    HearthModeBaseDelegate *d =
        obj_pair_new<HearthModeBaseDelegate, chip::app::Clusters::ModeBase::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->set_cluster(cluster_id);
    s_mb_delegates[s_mb_delegate_next++] = d;
    return d;
}

/*
 * The second half, and where the ModeBase Instance is BORN. Everything
 * about the placement of this call is the section comment's two-failure
 * story: it may only run below a successful emberAfSetDynamicEndpoint()
 * (Init()'s VerifyOrDie on emberAfContainsServer PANICS otherwise, unlike
 * every soft-bailing Init this file already hosts), and the endpoint's
 * block-resident store must already be constructed (Init()'s first act
 * reads the delegate's index 0, which the placeholder policy answers for
 * an unfed store). The Instance is constructed with feature 0:
 * DirectModeChange, the aliases' only feature, is optional and not taken,
 * matching the FeatureMap 0 arena seed so the AAI's FeatureMap answer and
 * the inert slot agree.
 *
 * Init()'s return is CHECKED and a failure logged loudly, the shape the
 * opstate handout set: the C6's SDK init path discards it, and the
 * silent-deregistration failure mode (placeholder missing, index-0 read
 * fails, Instance never registers, no diagnostic anywhere) is exactly the
 * kind of quiet cripple this port refuses to ship without a shout. It
 * does not abort: below a successful create the contains-server check
 * cannot fail, the placeholder answers index 0 by construction, and the
 * registry Register() calls fail only on a duplicate registration the
 * construct-once pool rules out; if it ever fired anyway, the endpoint is
 * live and correct in every other respect, the standing lesser-evil
 * argument.
 */
/* Split for the microwave's ordering function, the opstate helper's reason
 * exactly (mt_matter.h's setter is void and read-only). */
static CHIP_ERROR modebase_construct_and_init(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthModeBaseDelegate *>(delegate);
    d->set_endpoint(ep);
    auto *inst =
        new (obj_inst_storage<HearthModeBaseDelegate, chip::app::Clusters::ModeBase::Instance>(d))
            chip::app::Clusters::ModeBase::Instance(d, ep, d->cluster(), 0);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "ModeBase Instance::Init failed for endpoint %u cluster 0x%08X: "
                "%" CHIP_ERROR_FORMAT
                "; the cluster is NOT registered and will answer nothing on the fabric",
                (unsigned)ep, (unsigned)d->cluster(), err.Format());
    }
    return err;
}

extern "C" void mt_matter_modebase_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)modebase_construct_and_init(delegate, ep);
}

/*
 * AT+MTMODES's cluster-aware form (AT_MT_SPEC.md 3.20). Grammar and
 * content rules (count bounds, mode uniqueness, tag range, label content)
 * are enforced by cmd_mtmodes() in mt_at.c; the bounds re-checked here
 * are defensive. This bridge is the sole place that validates cluster
 * against the ModeBase ids this build serves, since mt_at.c has no CHIP
 * header to read them from. The accept set is all eight ModeBase ids (the
 * check below is copied whole from the C6's; the in-code comment on it
 * records the widening from the RVC's two, through the 7a/7b/8 consumers,
 * to EVSE), but a cluster that no live endpoint carries is refused by the
 * emberAfContainsServer carries-server check that follows, which answers
 * MT_ATTR_ERR_CLUSTER, so the extra ids are harmless here.
 *
 * Tag-0 defaults are substituted at STORE time so every read is
 * branch-free, the C6's policy verbatim (main.cpp:2762-2766): RvcRunMode's
 * FIRST declared mode gets kIdle and every later one kCleaning (which is
 * what keeps a host that always writes tag 0 conformant, since a
 * RvcRunMode list must contain an Idle-tagged mode), RvcCleanMode gets
 * kVacuum on every mode. A nonzero tag passes through unvalidated beyond
 * the u16 range check mt_at.c performed; tag semantics are the host's.
 *
 * All labels are validated before any entry is overwritten (the fix-round
 * M5 shape). Strictly optional here, unlike the ModeSelect twin: ModeBase
 * reads copy per access and cache no spans, so a half-written list could
 * never publish stale bytes; kept anyway so the two bridges reason
 * identically.
 *
 * Ends with the SupportedModes dirty-mark and the CurrentMode CLAMP: if
 * the replacement list dropped the mode the Instance currently holds,
 * UpdateCurrentMode(first entry) through the SDK's own setter, which
 * persists and reports the change itself when it actually changes. A
 * clamp reaches the fabric as that report only, never as a +MTATTR URC:
 * CurrentMode's ember slot is an inert shadow (the k_instance_served
 * split), and the host that just replaced the list can read the clamped
 * value back live over AT+MTATTR.
 */
extern "C" int mt_matter_modebase_set(uint16_t ep, uint32_t cluster, const uint8_t *modes,
                                      const uint16_t *tags, const char *const *labels,
                                      uint8_t count)
{
    using namespace chip::app::Clusters;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (cluster != RvcRunMode::Id && cluster != RvcCleanMode::Id &&
        cluster != DeviceEnergyManagementMode::Id && cluster != WaterHeaterMode::Id &&
        cluster != RefrigeratorAndTemperatureControlledCabinetMode::Id &&
        cluster != OvenMode::Id && cluster != MicrowaveOvenMode::Id &&
        cluster != EnergyEvseMode::Id) {
        /* Batch 7a widened the accept set to three of the C6's ModeBase ids,
         * batch 7b to four (WaterHeaterMode), batch 8 to seven (0x0052 on the
         * refrigerator and the cooler cabinet, OvenMode on the heater
         * cabinet, MicrowaveOvenMode on the microwave), and the EVSE round to
         * EIGHT, which is every ModeBase alias this firmware builds on either
         * platform.
         *
         * ENERGYEVSEMODE (0x009D) IS THE EIGHTH, AND IT WAS OUT UNTIL
         * 2026-08-31. The record of why is worth keeping, because the
         * reasoning was correct at the time and only the facts under it
         * changed. It was first out because its device type was out of tier
         * (ruling DE408, LM20). When the EVSE round brought the device type
         * in, it STAYED out for a different and better reason: the C6's own
         * bridge listed the same seven ids and AT_MT_SPEC.md 3.20.1's cluster
         * list omitted 0x009D, so admitting it here alone would have made
         * this platform accept a cluster id the published contract called an
         * error, which is a wire divergence and not a fix.
         *
         * What that left was a degenerate pairing, and it is what the spec
         * change closed: an EnergyEvseMode endpoint's SupportedModes was the
         * firmware placeholder for ever, its CurrentMode 0, while
         * ChangeToMode still forwarded to the host for adjudication, so a
         * controller was being asked to choose between one option. Spec
         * 3.20.1 now names 157/0x9D with a tag-0 default of kManual
         * (0x4000), which is the value BOTH platforms already produced (the
         * C6 by fall-through, this port by the explicit placeholder arm), so
         * admitting it changes no value any host or controller could already
         * observe. It only makes a previously-refused command succeed. */
        return MT_ATTR_ERR_CLUSTER;
    }
    if (!emberAfContainsServer(ep, cluster)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_MB_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_mb_store_t *slot = mt_dyn_mb_store(ep, cluster);
    if (!slot) {
        /* Cannot happen: the lookups above proved ep live with this
         * cluster, and every such endpoint's block carries both stores
         * from create. Defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_MB_MAX_LABEL_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
    }

    /*
     * THE MICROWAVE'S ONE HOST-REACHABLE WAY TO CRIPPLE ITSELF, refused here
     * (batch 8 fix round, RULED DE417).
     *
     * MicrowaveOvenControl resolves an omitted cookMode by asking this
     * cluster's Instance for the mode carrying the kNormal tag, and answers
     * InvalidCommand if there is none (microwave-oven-control-server.cpp:
     * 277-280). A host feed whose entries all carry EXPLICIT non-zero tags,
     * none of them kNormal, is accepted verbatim by the substitution loop
     * below (it only rewrites tag 0), and from that moment every
     * SetCookingParameters on that endpoint answers InvalidCommand with no
     * +MTCMD raised, forever and silently. On the nRF the class is closed by
     * two checks, the boot readback in mt_matter_mwoc_register() over the
     * placeholder path and this one over the host-fed path; on this port the
     * readback has no counterpart (no microwave device type is ported, so
     * that function exists only in the nRF files), and this check alone
     * closes the host-fed path here.
     *
     * A tag of 0 counts as satisfying the rule, because the loop below turns
     * it into kNormal for this cluster; only a list that is explicitly and
     * entirely something else is refused.
     *
     * THIS IS A FIRMWARE-IMPOSED RULE, NOT A CONFORMANCE CHECK, and the
     * distinction is deliberate rather than hedging. The pinned tree's
     * machine-readable model does NOT mandate a Normal-tagged mode:
     * Mode_MicrowaveOven.xml (revision 2) lists Normal in its ModeTag enum
     * and constrains nothing about which tags a SupportedModes list must
     * carry, and no Mode_*.xml in the tree encodes such a mandate for ANY
     * mode cluster, Mode_RVCRun.xml included, where the cluster spec's prose
     * certainly does require an Idle-tagged mode. MicrowaveOvenControl.xml's
     * CookMode field carries default="desc" and <constraint><desc/></constraint>,
     * which is the XML deferring to prose this tree does not ship. What the
     * tree DOES contain is the SDK's own behaviour (the InvalidCommand above)
     * and the SDK's reference microwave, whose two-mode list always carries
     * Normal (examples/microwave-oven-app/microwave-oven-common/include/
     * microwave-oven-device.h:254-264). So the rule is this firmware's, taken
     * because the alternative is a silently dead endpoint, and AT_MT_SPEC.md
     * says so in those terms.
     *
     * Introduced here as a deliberate, disclosed divergence from the C6,
     * which had the identical gap and accepted such a list (B418). The C6
     * caught up in the parity round of 2026-09-01 and now refuses the same
     * lists with the same +MTERR:1, so the two platforms agree again; the
     * label-validation pass runs before this check on both, so a doubly
     * malformed list returns the same error on either.
     */
    if (cluster == MicrowaveOvenMode::Id) {
        bool has_normal = false;
        for (uint8_t i = 0; i < count && !has_normal; i++) {
            has_normal = (tags[i] == 0) ||
                         (tags[i] == chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal));
        }
        if (!has_normal) {
            HEARTH_LOGE("matter", "AT+MTMODES on endpoint %u cluster 0x%08X refused: no mode carries the "
                    "kNormal tag (0x%04X), and MicrowaveOvenControl resolves an omitted "
                    "cookMode through it, so every SetCookingParameters would answer "
                    "InvalidCommand. Send tag 0 on at least one mode, or kNormal explicitly",
                    (unsigned)ep, (unsigned)cluster,
                    (unsigned)chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal));
            return MT_ATTR_ERR_VALUE;
        }
    }

    for (uint8_t i = 0; i < count; i++) {
        uint16_t tag = tags[i];
        if (tag == 0) {
            if (cluster == RvcRunMode::Id) {
                tag = chip::to_underlying(i == 0 ? RvcRunMode::ModeTag::kIdle
                                                 : RvcRunMode::ModeTag::kCleaning);
            } else if (cluster == DeviceEnergyManagementMode::Id) {
                /* Batch 7a: kNoOptimization on every mode, first or not
                 * (the placeholder_tag() arm's reasoning). */
                tag = chip::to_underlying(DeviceEnergyManagementMode::ModeTag::kNoOptimization);
            } else if (cluster == WaterHeaterMode::Id) {
                /* Batch 7b: kManual on every mode, first or not (the
                 * placeholder_tag() arm's reasoning; the AT_MT_SPEC.md
                 * 3.20 table row). */
                tag = chip::to_underlying(WaterHeaterMode::ModeTag::kManual);
            } else if (cluster == MicrowaveOvenMode::Id) {
                /* Batch 8: kNormal on every mode, first or not. A host that
                 * writes tag 0 for every mode still gets a list in which
                 * SetCookingParameters can resolve a default cookMode,
                 * which is the placeholder arm's reason applied to real
                 * entries. */
                tag = chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal);
            } else if (cluster == OvenMode::Id) {
                /* Batch 8: kBake on every mode, first or not (the
                 * placeholder_tag() arm's reasoning). */
                tag = chip::to_underlying(OvenMode::ModeTag::kBake);
            } else if (cluster == RefrigeratorAndTemperatureControlledCabinetMode::Id) {
                /* Batch 8: kAuto on every mode, first or not. The one
                 * substitution in this loop whose result is itself 0 (a
                 * ModeBase COMMON tag, not a 0x4000-range cluster-specific
                 * one), so it is a no-op on the wire; see placeholder_tag()'s
                 * matching arm. */
                tag = chip::to_underlying(
                    RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kAuto);
            } else if (cluster == EnergyEvseMode::Id) {
                /* The EVSE round: kManual (0x4000) on every mode, first or
                 * not, the AT_MT_SPEC.md 3.20 table row added 2026-08-31.
                 * Explicit rather than left to the fall-through below, which
                 * would have substituted kVacuum's 0x4001 and landed on
                 * kTimeOfUse on this cluster's own enum: a plausible-looking
                 * tag arrived at entirely by accident. placeholder_tag()
                 * above carries the same value for the same reason, which is
                 * what keeps a host-fed list and an unfed one describing the
                 * same kind of mode. */
                tag = chip::to_underlying(EnergyEvseMode::ModeTag::kManual);
            } else {
                tag = chip::to_underlying(RvcCleanMode::ModeTag::kVacuum);
            }
        }
        slot->entries[i].mode = modes[i];
        slot->entries[i].tag = tag;
        memcpy(slot->entries[i].label, labels[i], strlen(labels[i]) + 1);
    }
    slot->count = count;

    /* One Instance lookup serves both tail steps (batch 5 fix round,
     * review Minor 3): the dirty-mark goes through the SDK's own
     * Instance::ReportSupportedModesChange(), the method the delegate
     * contract says the device SHALL call (mode-base-server.h:109, the
     * SHALL at :203/:215/:233; its body is exactly the raw
     * MatterReportingAttributeChangeCallback this bridge used to make,
     * mode-base-server.cpp:241-244, so the wire is unchanged and a future
     * SDK revision that adds bookkeeping there is picked up for free).
     * The raw callback survives only in the defensive no-Instance arm, so
     * a host feed still reaches subscriptions even in the
     * cannot-happen-once-rebuilt state. */
    ModeBase::Instance *inst = nullptr;
    for (size_t i = 0; i < s_mb_delegate_next; i++) {
        HearthModeBaseDelegate *d = s_mb_delegates[i];
        if (d->endpoint() == ep && d->cluster() == cluster) {
            inst = d->instance();
            break;
        }
    }
    if (inst == nullptr) {
        MatterReportingAttributeChangeCallback(ep, cluster,
                                               ModeBase::Attributes::SupportedModes::Id);
        return MT_ATTR_OK;
    }
    inst->ReportSupportedModesChange();

    if (!inst->IsSupportedMode(inst->GetCurrentMode())) {
        /* The clamp target is entries[0].mode, stored two loops above, so
         * UpdateCurrentMode()'s ConstraintError arm (an unsupported mode)
         * is unreachable here; checked and logged anyway rather than
         * discarded, this file's convention for SDK returns (batch 5 fix
         * round, review Minor 4). */
        Status st = inst->UpdateCurrentMode(slot->entries[0].mode);
        if (st != Status::Success) {
            HEARTH_LOGE("matter", "modebase CurrentMode clamp to %u failed on endpoint %u cluster 0x%08X "
                    "(status 0x%02X)",
                    (unsigned)slot->entries[0].mode, (unsigned)ep, (unsigned)cluster,
                    (unsigned)chip::to_underlying(st));
        }
    }
    return MT_ATTR_OK;
}

/*
 * DE397 live read for the two ModeBase CurrentModes, dispatched from
 * mt_matter_attr_read() under its StackLock. CurrentMode is unsigned,
 * never nullable; the Instance's member is the served truth (KVS-restored
 * at Init, updated by ChangeToMode and the clamp above), the inert arena
 * seed is not.
 */
static int mt_mb_attr_read_live(uint16_t ep, uint32_t cluster, int64_t *out, bool *is_unsigned)
{
    if (is_unsigned) {
        *is_unsigned = true;
    }
    for (size_t i = 0; i < s_mb_delegate_next; i++) {
        HearthModeBaseDelegate *d = s_mb_delegates[i];
        if (d->endpoint() == ep && d->cluster() == cluster) {
            chip::app::Clusters::ModeBase::Instance *inst = d->instance();
            if (inst == nullptr) {
                break;
            }
            *out = inst->GetCurrentMode();
            return MT_ATTR_OK;
        }
    }
    /* Cannot happen once the boot rebuild has run; defensive. */
    return MT_ATTR_ERR_FAILED;
}

/* ---- robotic vacuum cleaner: RvcOperationalState ----------------------
 *
 * The derived opstate cluster needs its own Delegate SUBCLASS
 * (RvcOperationalState::Delegate adds HandleGoHomeCommandCallback and
 * stubs Start/Stop to kUnknownEnumValue, operational-state-server.h:
 * 378-406) and therefore its own pool: the base Delegate::SetInstance()
 * VerifyOrDies when a second Instance shares a delegate
 * (operational-state-server.h:349-353), so lending trio delegates to RVC
 * Instances is fatal, and the trio's HearthOpStateDelegate has no GoHome
 * hook anyway (mt_matter.h:526-528 states the split rule directly).
 *
 * No new translation unit: RvcOperationalState::Instance lives in the
 * already-compiled operational-state-server.cpp (:520-570), constructed
 * Instance(Delegate*, ep), which forwards RvcOperationalState::Id to the
 * protected three-argument base ctor. Init() is the base's, and
 * soft-bails on emberAfContainsServer (operational-state-server.cpp:
 * 63-70), so this half of the RVC is the trio's ordering story, NOT the
 * ModeBase panic above.
 *
 * Command routing (the reason the metadata list in mt_devtypes_zephyr.cpp
 * is exactly {Pause, Resume, GoHome}): the base InvokeCommand() switches
 * on Pause/Resume/Start/Stop unconditionally and sends everything else to
 * InvokeDerivedClusterCommand(), where GoHome is handled (:296-334,
 * :534-546). The server-side guards resolve the state cases before any
 * delegate call (Pause also compatible with kSeekingCharger, Resume with
 * kCharging/kDocked; GoHome from kCharging/kDocked answers
 * kCommandInvalidInState and from kSeekingCharger answers success, both
 * WITHOUT calling the delegate, :522-531 and :548-567), so the three
 * forwards below only carry genuine adjudication requests. Allow is
 * kNoError, deny kUnableToCompleteOperation, copied by the SDK straight
 * into the OperationalCommandResponse: the verdict IS the wire response,
 * the trio's contract. GoHome forwards payload-less (the command has no
 * fields), via mt_cmd_forward like its siblings.
 *
 * Instance reachability: the base GetInstance() passthrough, the smaller
 * change against the C6's typed m_instance member (which exists there
 * only because the C6 writes its own init callback); the pointer is the
 * identical one, upcast to OperationalState::Instance, and everything
 * this file calls on it (SetOperationalState, GetCurrentOperationalState,
 * GetCurrentPhase) is base API.
 */
class HearthRvcOpStateDelegate : public chip::app::Clusters::RvcOperationalState::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    chip::app::Clusters::OperationalState::Instance *instance() { return GetInstance(); }

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::NullNullable;
    }

    /* Seven states in the RVC's own enum: the four base states plus the
     * three derived-number-space ones AT+MTOPSTATE may set (0x40
     * kSeekingCharger, 0x41 kCharging, 0x42 kDocked,
     * RvcOperationalState/Enums.h:67-69), mirroring the C6's list
     * (main.cpp:3300-3313). kEmptyingDustBin and friends (0x43-0x46)
     * exist in the enum but are outside the AT contract's union and not
     * published. kError is listed because the spec requires it even
     * though AT+MTOPSTATE can never set it directly. */
    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index,
        chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override
    {
        using chip::app::Clusters::RvcOperationalState::OperationalStateEnum;
        static const OperationalStateEnum states[] = {
            OperationalStateEnum::kStopped,        OperationalStateEnum::kRunning,
            OperationalStateEnum::kPaused,         OperationalStateEnum::kError,
            OperationalStateEnum::kSeekingCharger, OperationalStateEnum::kCharging,
            OperationalStateEnum::kDocked,
        };
        if (index >= (sizeof(states) / sizeof(states[0]))) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState.Set(chip::to_underlying(states[index]));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index,
                                          chip::MutableCharSpan &operationalPhase) override
    {
        /* NOT_FOUND at index 0 makes PhaseList read null, the trio's
         * convention; this firmware publishes no phases. */
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandlePauseStateCallback(
        chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::Pause::Id, err);
    }

    void HandleResumeStateCallback(
        chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::Resume::Id, err);
    }

    void HandleGoHomeCommandCallback(
        chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::GoHome::Id, err);
    }

private:
    void forward(uint32_t command,
                 chip::app::Clusters::OperationalState::GenericOperationalError &err)
    {
        using chip::app::Clusters::OperationalState::ErrorStateEnum;
        bool allow = mt_cmd_forward(m_ep, chip::app::Clusters::RvcOperationalState::Id, command);
        err.Set(chip::to_underlying(allow ? ErrorStateEnum::kNoError
                                          : ErrorStateEnum::kUnableToCompleteOperation));
    }

    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/* One RvcOperationalState per RVC endpoint, so this pool is
 * kServiceableEndpoints deep like the trio's (the heap admits only nine
 * RVC endpoints, but a slot here is 260-odd bytes against the ModeBase
 * pool's DE404 argument for departing from 16; not worth a second special
 * sizing). Memory reclaim round A: the pool is a table of POINTERS into
 * the cluster-object heap, and the raw storage for the
 * non-default-constructible Instance sits behind the delegate in the same
 * block, placement-constructed once and never destroyed: the standing pool
 * policy. */
static HearthRvcOpStateDelegate *s_rvc_opstate_delegates[kServiceableEndpoints];
static size_t s_rvc_opstate_delegate_next;
/* The RvcOperationalState Delegate + Instance pair, 264 B on the nRF's
 * measurement; this pins the MG24's. It is wider than the 256 B pair the
 * object arena is sixteen-of, so the RVC is admitted up to nine (graph
 * DE566): nine RVCs draw this pair and two ModeBase pairs (80 B each). */
static_assert(hearth_arena_cost(obj_pair_bytes<HearthRvcOpStateDelegate,
                                                chip::app::Clusters::RvcOperationalState::Instance>()) == 264,
              "the RvcOperationalState pair changed size; redo the RVC's admission arithmetic");
static_assert(9 * (264 + 2 * 80) <= HEARTH_OBJ_ARENA_BYTES,
              "nine RVCs' objects no longer fit the cluster-object arena (DE566)");
static_assert(10 * (264 + 2 * 80) > HEARTH_OBJ_ARENA_BYTES,
              "a tenth RVC now fits the cluster-object arena: nothing else enforces the nine (DE566); add an explicit cap before growing the arena");

extern "C" void *mt_matter_rvc_opstate_delegate_alloc(void)
{
    if (s_rvc_opstate_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthRvcOpStateDelegate *d =
        obj_pair_new<HearthRvcOpStateDelegate,
                     chip::app::Clusters::RvcOperationalState::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    s_rvc_opstate_delegates[s_rvc_opstate_delegate_next++] = d;
    return d;
}

/*
 * The second half: constructs the RvcOperationalState::Instance in its
 * slot and runs Init(), the trio's shape verbatim (Init soft-bails on
 * emberAfContainsServer, so this belongs below the successful
 * emberAfSetDynamicEndpoint(); a failure is logged loudly and does not
 * abort, the unreachable-by-ordering argument).
 */
extern "C" void mt_matter_rvc_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthRvcOpStateDelegate *>(delegate);
    d->set_endpoint(ep);
    auto *inst =
        new (obj_inst_storage<HearthRvcOpStateDelegate,
                              chip::app::Clusters::RvcOperationalState::Instance>(d))
            chip::app::Clusters::RvcOperationalState::Instance(d, ep);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "RvcOperationalState Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; commands on it will not reach the host and its attributes will not be served",
                (unsigned)ep, err.Format());
    }
}

/* The pool's Instance lookup, shared by mt_matter_opstate_set()'s RVC
 * branch and the DE397 live reader below. */
static chip::app::Clusters::OperationalState::Instance *mt_rvc_opstate_instance(uint16_t ep)
{
    for (size_t i = 0; i < s_rvc_opstate_delegate_next; i++) {
        HearthRvcOpStateDelegate *d = s_rvc_opstate_delegates[i];
        if (d->endpoint() == ep) {
            return d->instance();
        }
    }
    return nullptr;
}

/*
 * DE397 live read for the RVC opstate pair, the trio's reader against the
 * RVC pool: OperationalState answers the live enum (including the
 * derived-number-space values), CurrentPhase answers +MTERR:5 while null,
 * which is its steady state here since no phases are published.
 */
static int mt_rvc_opstate_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out,
                                         bool *is_unsigned)
{
    namespace OpState = chip::app::Clusters::OperationalState;
    if (is_unsigned) {
        *is_unsigned = true;
    }
    OpState::Instance *inst = mt_rvc_opstate_instance(ep);
    if (inst == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    if (attr == OpState::Attributes::OperationalState::Id) {
        *out = inst->GetCurrentOperationalState();
        return MT_ATTR_OK;
    }
    chip::app::DataModel::Nullable<uint8_t> phase = inst->GetCurrentPhase();
    if (phase.IsNull()) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = phase.Value();
    return MT_ATTR_OK;
}
