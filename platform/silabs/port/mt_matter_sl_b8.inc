/*
 * mt_matter_sl_b8.inc: a fragment of mt_matter_sl.cpp, NOT a compile unit.
 *
 * Holds catalogue batch 8's bridge code (nRF mt_matter_zephyr.cpp 3169-3312
 * and 4007-4365): the temperature levels section (HearthTempLevelsDelegate,
 * the one global TemperatureControl iterator, its register and
 * AT+MTTEMPLEVELS) and the microwave oven's MicrowaveOvenControl section
 * (HearthMwocDelegate, its pool and the ordered three-way register).
 * Included at the end of mt_matter_sl.cpp after mt_matter_sl_evse.inc, so
 * the whole is still one translation unit. Do not add it to hearth.slcp.
 */

/* ---- temperature levels (TemperatureControl 0x0056) --------------------
 *
 * Catalogue batch 8. The SupportedTemperatureLevels list a
 * TemperatureLevel-variant cabinet or cook surface publishes, fed by
 * AT+MTTEMPLEVELS (AT_MT_SPEC.md 3.16) and read back by the SDK's own
 * wildcard AttributeAccessInterface.
 *
 * ONE process-global iterator delegate for the whole device, not a pool, and
 * that is the SDK's shape rather than a choice: the cluster keeps a single
 * `SupportedTemperatureLevelsIteratorDelegate *sInstance` behind
 * free-function GetInstance()/SetInstance()
 * (temperature-control-server.cpp:37, :56-66), and dispatches per endpoint
 * by calling Reset(endpoint) on it before every iteration
 * (:82, :195). So the object below is stateless between calls except for
 * the endpoint and index the base class's Reset() sets, and it answers out
 * of whichever endpoint's block-resident store mEndpoint names. The mode
 * select manager above has the identical shape for the identical reason.
 *
 * SetInstance() has NO CALLER anywhere in the SDK: the port must register
 * this itself, exactly as the C6 does (main.cpp:1829). Registration is
 * idempotent (a bare pointer store), so mt_matter_temp_levels_register()
 * below is safe to call once per TemperatureControl-bearing endpoint from
 * the create path, and it is called for BOTH variants deliberately: a
 * TemperatureNumber endpoint never reaches the iterator, but registering
 * only on the level variant would make the registration depend on
 * composition order in a way nothing else here does.
 *
 * The store lives in the endpoint's own arena block (mt_temp_levels_store_t,
 * mt_dyn_store.h), so only endpoints that actually carry the level variant
 * pay its 273 B. The C6's equivalent is a 28-slot .bss array of about
 * 7,728 B that every composition pays; that array is the single largest
 * thing this batch declined to transplant.
 */
class HearthTempLevelsDelegate
    : public chip::app::Clusters::TemperatureControl::SupportedTemperatureLevelsIteratorDelegate
{
public:
    /* Both overrides run on the CHIP task under the stack lock (the AAI read
     * and the SetTemperature callback are both stack-thread work), so they
     * take no lock of their own, the standing hook discipline. */
    uint8_t Size() override
    {
        mt_temp_levels_store_t *store = mt_dyn_temp_levels_store(mEndpoint);
        return (store == nullptr) ? 0 : store->count;
    }

    CHIP_ERROR Next(chip::MutableCharSpan &item) override
    {
        mt_temp_levels_store_t *store = mt_dyn_temp_levels_store(mEndpoint);
        if (store == nullptr || mIndex >= store->count) {
            /* Any non-CHIP_NO_ERROR ends the AAI's encode loop
             * (temperature-control-server.cpp:85-90); NOT_FOUND is the same
             * code the SDK's own sample manager answers past its end. */
            return CHIP_ERROR_NOT_FOUND;
        }
        CHIP_ERROR err = chip::CopyCharSpanToMutableCharSpan(
            chip::CharSpan::fromCharString(store->labels[mIndex]), item);
        if (err == CHIP_NO_ERROR) {
            mIndex++;
        }
        return err;
    }
};

static HearthTempLevelsDelegate s_temp_levels_delegate;

/* The create path's one-liner, port-local for the reason every register in
 * this file is (core/include/mt_matter.h is read-only and names no such
 * function; the C6 needs no equivalent because it registers once at boot
 * from its own main). Called for any TemperatureControl-bearing endpoint,
 * after the endpoint is live for tidiness rather than necessity: this
 * registration resolves no endpoint index and could legally run at any
 * point. */
extern "C" void mt_matter_temp_levels_register(void)
{
    chip::app::Clusters::TemperatureControl::SetInstance(&s_temp_levels_delegate);
}

/*
 * AT+MTTEMPLEVELS (AT_MT_SPEC.md 3.16). Grammar, count and label content are
 * enforced by cmd_mttemplevels() in mt_at.c before this is called; the
 * bounds re-checked here are defensive, the AT+MTMODES bridge's discipline.
 *
 * The error division is the header's (core/include/mt_matter.h:247-251) and
 * needs BOTH lookups, because this port can be in a state no earlier command
 * could reach: the endpoint carries TemperatureControl and still has no
 * label store, because the store belongs to the cluster's TemperatureLevel
 * VARIANT. emberAfContainsServer() answers the cluster question
 * (MT_ATTR_ERR_CLUSTER, +MTERR:3) and mt_dyn_temp_levels_store() answers the
 * variant question (MT_ATTR_ERR_ATTRIBUTE, +MTERR:4). On the C6 the second
 * question is asked of esp-matter's attribute::get() instead
 * (main.cpp:1846-1852); here it is asked of the declared list, through the
 * same predicate the store walk and the create-path construction use.
 *
 * Full replacement per call, never a merge, and not persisted: the store
 * starts empty every boot and the host re-sends, the standing contract for
 * host-fed state. Ends with the dirty mark, which is the ONLY way a
 * subscribed controller learns the list changed, since SupportedTemperatureLevels
 * is served by an AAI and never passes through
 * MatterPostAttributeChangeCallback(): no +MTATTR URC fires for it either,
 * and there is no AT+MTATTR path to it at all (AT_MT_SPEC.md 3.16).
 */
extern "C" int mt_matter_temp_levels_set(uint16_t ep, const char *const *labels, uint8_t count)
{
    namespace TemperatureControl = chip::app::Clusters::TemperatureControl;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, TemperatureControl::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    mt_temp_levels_store_t *store = mt_dyn_temp_levels_store(ep);
    if (store == nullptr) {
        /* The cluster is there and the store is not: a TemperatureNumber
         * variant. The one arm of this function that is a normal answer
         * rather than a defensive one. */
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    if (labels == nullptr || count < 1 || count > MT_TEMP_LEVEL_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }
    /* Every label validated before any entry is overwritten, the AT+MTMODES
     * fix-round shape. Strictly optional here (the iterator copies per call
     * and caches no span, so a half-written list could not publish stale
     * bytes) and kept anyway so the host-fed bridges reason identically. */
    for (uint8_t i = 0; i < count; i++) {
        if (labels[i] == nullptr) {
            return MT_ATTR_ERR_FAILED;
        }
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_TEMP_LEVEL_MAX_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        memcpy(store->labels[i], labels[i], strlen(labels[i]) + 1);
    }
    store->count = count;

    MatterReportingAttributeChangeCallback(
        ep, TemperatureControl::Id, TemperatureControl::Attributes::SupportedTemperatureLevels::Id);
    return MT_ATTR_OK;
}

/* ---- microwave oven: MicrowaveOvenControl (0x005F) ---------------------
 *
 * THE SHARPEST HAZARD IN THE COMPOSED-APPLIANCE BATCH, and it deserves the
 * space. Every per-endpoint object this port has built so far is
 * INDEPENDENT: claim a pool slot before emberAfSetDynamicEndpoint(),
 * placement-construct and Init() after it, in any order among themselves.
 * The microwave breaks that. MicrowaveOvenControl::Instance's constructor is
 *
 *   Instance(Delegate *, EndpointId, ClusterId, BitMask<Feature>,
 *            OperationalState::Instance &, ModeBase::Instance &)
 *
 * (microwave-oven-control-server.h:58-59) and those last two are C++
 * REFERENCES to two OTHER clusters' live Instances. So the OperationalState
 * Instance and the MicrowaveOvenMode Instance must both exist before this
 * one can be constructed at all, and its handlers dereference both on the
 * very first invoke (microwave-oven-control-server.cpp:252, :277-281).
 *
 * WHAT MAKES IT DANGEROUS RATHER THAN MERELY FIDDLY is that every way of
 * getting it wrong is QUIET. A reference bound to storage that has not been
 * constructed yet compiles, links, boots, commissions and answers reads; the
 * failure surfaces only when a controller invokes SetCookingParameters, and
 * it surfaces as InvalidInState or InvalidCommand, statuses the server
 * legitimately produces for perfectly ordinary reasons. There is no
 * VerifyOrDie to catch it, no log, and no +MTCMD for a host to notice
 * missing.
 *
 * THE THREE MANDATORY MITIGATIONS, all of them in this one function:
 *
 *   1. ONE function owns the order, and the order is fixed and commented.
 *      The create path calls this instead of the three independent-looking
 *      setters, and its else-branch is what stops the generic opstate setter
 *      from constructing the same Instance twice.
 *   2. ALL THREE Init() returns are checked and logged, not just this
 *      cluster's. The two neighbours' setters were split into
 *      construct-and-init helpers for exactly this, because mt_matter.h's
 *      public setters return void and that header is read-only.
 *   3. A GetModeValueByModeTag(kNormal) READBACK after construction, logged
 *      loudly on failure. That call is the difference between a working
 *      microwave and one that refuses every cooking command in silence
 *      forever: SetCookingParameters resolves its default cookMode through
 *      it (microwave-oven-control-server.cpp:277-281), so if the ModeBase
 *      placeholder tag is not kNormal the command answers InvalidCommand
 *      with nothing raised to the host. It succeeds against the unfed
 *      placeholder by design (placeholder_tag()'s MicrowaveOvenMode arm),
 *      which is what makes a freshly composed microwave usable before the
 *      host has ever sent AT+MTMODES; the readback is here so that the day
 *      that arm is edited wrongly, the bench sees it at boot.
 *
 * A failed neighbour Init() does NOT abort the MWOC construction. Both
 * Instances still exist as constructed objects after a failed Init (Init
 * only registers interfaces), so the references are sound and a microwave
 * with an unregistered opstate cluster is still better than one with no
 * cooking control at all; the loud summary line below is what a bench sees.
 * A NULL delegate is different and does abort: there is no object to bind a
 * reference to, and binding one would be undefined rather than degraded.
 *
 * WHAT THE SERVER GUARANTEES BEFORE THE HOST EVER SEES A COMMAND, which is
 * why the forwards below carry only legitimate adjudication requests:
 * SetCookingParameters requires the operational state to be kStopped or
 * answers InvalidInState (:252); if startAfterSetting is present it asks the
 * data model provider whether OperationalState's Start is accepted on this
 * endpoint and answers InvalidCommand if not (:257-272, which is why
 * kOpStateIncoming keeps Start on the microwave's list); cookTime is
 * range-checked against GetMaxCookTimeSec(); powerSetting is range- and
 * step-checked. AddMoreTime requires the state not be kError and range-checks
 * the sum.
 *
 * CookTime and PowerSetting ownership: CookTime lives in the Instance
 * (SetCookTimeSec(), which this delegate calls on an allowed command, the
 * SDK's own reference implementation's pattern), PowerSetting lives in this
 * delegate object because the Delegate interface has no setter for it at
 * all, only GetPowerSettingNum(). Both are applied only on ALLOW: a denied
 * command is a refusal to accept the new cooking parameters, so nothing is
 * written for the host to have refused.
 *
 * The verdict IS the wire response, the chime's shape rather than the
 * OperationalState family's GenericOperationalError indirection:
 * HandleSetCookingParameters() copies whatever Status these two methods
 * return straight into the InvokeResponse through AddStatus(), no remapping,
 * so allow is Status::Success and deny is Status::Failure (AT_MT_SPEC.md
 * 3.17). Both hooks run on the CHIP task (this cluster is CHI-only), so
 * no StackLock, the standing hook discipline.
 */
class HearthMwocDelegate : public chip::app::Clusters::MicrowaveOvenControl::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    /* GetInstance() is protected in the base Delegate, exactly as it is on
     * the OperationalState and ModeBase delegates, and this passthrough is
     * the same mechanism under a third name. The base offers BOTH overloads,
     * `const Instance *GetInstance() const` at
     * microwave-oven-control-server.h:199 and `Instance *GetInstance()` at
     * :207, and this member is non-const, so overload resolution picks the
     * non-const one and no cast is needed to reach the non-const
     * SetCookTimeSec(). (Fix round M1: an earlier version of this comment
     * claimed the base offered only the const overload, cited a line that is
     * neither, and carried a const_cast on the strength of it.) */
    chip::app::Clusters::MicrowaveOvenControl::Instance *instance() { return GetInstance(); }

    /*
     * SetCookingParameters: the four-field forward,
     * cookMode,cookTime,power,startAfter, in that fixed order (AT_MT_SPEC.md
     * 3.17 and its field-arity rule). THREE of the four arrive
     * already resolved rather than Optional, and that is traced against the
     * server rather than assumed from the Delegate interface's own doc
     * comments: cookMode defaults to whichever mode carries the kNormal tag,
     * cookTime to 30 s and startAfterSetting to false, all before this
     * callback runs, which is why the signature declares them as plain
     * values. Only powerSettingNum is still Optional at this boundary, and
     * on a PowerAsNumber-only build it always has a value too (the server
     * defaults it to MaxPower, 100). So none of the four fields is ever
     * empty on this firmware's actual traffic; the empty-field wire
     * convention still applies verbatim, it simply never triggers here.
     * wattSettingIndex is not forwarded at all: it is always NullOptional on
     * this build and carries the host nothing.
     */
    chip::Protocols::InteractionModel::Status HandleSetCookingParametersCallback(
        uint8_t cookMode, uint32_t cookTimeSec, bool startAfterSetting,
        chip::Optional<uint8_t> powerSettingNum, chip::Optional<uint8_t> wattSettingIndex) override
    {
        using chip::Protocols::InteractionModel::Status;
        (void)wattSettingIndex;
        char fields[40];
        int n = 0;
        n += snprintf(fields + n, sizeof(fields) - n, "%u,", (unsigned)cookMode);
        n += snprintf(fields + n, sizeof(fields) - n, "%lu,", (unsigned long)cookTimeSec);
        n += powerSettingNum.HasValue()
                 ? snprintf(fields + n, sizeof(fields) - n, "%u,",
                            (unsigned)powerSettingNum.Value())
                 : snprintf(fields + n, sizeof(fields) - n, ",");
        n += snprintf(fields + n, sizeof(fields) - n, "%u", startAfterSetting ? 1u : 0u);
        (void)n;
        bool allow = mt_cmd_forward_fields(
            m_ep, chip::app::Clusters::MicrowaveOvenControl::Id,
            chip::app::Clusters::MicrowaveOvenControl::Commands::SetCookingParameters::Id, fields);
        if (allow) {
            if (instance() != nullptr) {
                instance()->SetCookTimeSec(cookTimeSec);
            }
            if (powerSettingNum.HasValue()) {
                m_power_setting = powerSettingNum.Value();
            }
        }
        return allow ? Status::Success : Status::Failure;
    }

    /* AddMoreTime: the single-field forward, finalCookTimeSec. Same
     * allow-applies, deny-refuses and raw-passthrough-verdict shape. */
    chip::Protocols::InteractionModel::Status
    HandleModifyCookTimeSecondsCallback(uint32_t finalCookTimeSec) override
    {
        using chip::Protocols::InteractionModel::Status;
        char fields[16];
        snprintf(fields, sizeof(fields), "%lu", (unsigned long)finalCookTimeSec);
        bool allow = mt_cmd_forward_fields(
            m_ep, chip::app::Clusters::MicrowaveOvenControl::Id,
            chip::app::Clusters::MicrowaveOvenControl::Commands::AddMoreTime::Id, fields);
        if (allow && instance() != nullptr) {
            instance()->SetCookTimeSec(finalCookTimeSec);
        }
        return allow ? Status::Success : Status::Failure;
    }

    /* PowerAsNumber-only. GetWattSettingByIndex() answering NOT_FOUND is not
     * a stub: Init() only counts watt levels when PowerInWatts is set, so it
     * is never called on this build, and the same is true of
     * GetCurrentWattIndex() and GetWattRating(). The three power-limit
     * getters ARE dead for a different and more interesting reason: the
     * server only consults them when PowerNumberLimits is set, which
     * conformance forbids without PowerAsNumber and esp-matter's own helper
     * inverts, so MinPower/MaxPower/PowerStep always read the SDK's
     * compiled-in 10/100/10 on both platforms. They are implemented anyway
     * because the pure-virtual contract requires it, and they return the
     * same constants the SDK would, so a future PowerNumberLimits build
     * would not change behaviour by accident. */
    CHIP_ERROR GetWattSettingByIndex(uint8_t index, uint16_t &wattSetting) override
    {
        (void)index;
        (void)wattSetting;
        return CHIP_ERROR_NOT_FOUND;
    }

    uint32_t GetMaxCookTimeSec() const override { return 86400; }
    uint8_t GetPowerSettingNum() const override { return m_power_setting; }
    uint8_t GetMinPowerNum() const override
    {
        return chip::app::Clusters::MicrowaveOvenControl::kDefaultMinPowerNum;
    }
    uint8_t GetMaxPowerNum() const override
    {
        return chip::app::Clusters::MicrowaveOvenControl::kDefaultMaxPowerNum;
    }
    uint8_t GetPowerStepNum() const override
    {
        return chip::app::Clusters::MicrowaveOvenControl::kDefaultPowerStepNum;
    }
    uint8_t GetCurrentWattIndex() const override { return 0; }
    uint16_t GetWattRating() const override { return 0; }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    /* kDefaultMaxPowerNum, the value the server itself defaults an omitted
     * powerSetting to and the value mwocAttrs seeds its shadow with. */
    uint8_t m_power_setting = chip::app::Clusters::MicrowaveOvenControl::kDefaultMaxPowerNum;
};
