/*
 * mt_matter_sl_b4.inc: a fragment of mt_matter_sl.cpp, NOT a compile unit.
 *
 * Holds bridge code: catalogue batch 4 (nRF mt_matter_zephyr.cpp 1894-3121),
 * included at the end of mt_matter_sl.cpp, one translation unit.
 * mt_matter_sl.cpp #includes it after its own final function, so the whole
 * is still one translation unit: file-local names stay file-local and the
 * generated code is the same as when this text lived in the .cpp. Split
 * out 2026-09-26 so each file fits a small model's context. Do not add
 * it to hearth.slcp.
 */

/* ============ catalogue batch 4: the appliance and notification types ====
 *
 * The smoke/co alarm (0x0076), the OperationalState trio (0x0073, 0x0075,
 * 0x007C), the mode select (0x0027) and the chime (0x0146) land here in
 * build order. The same file discipline as batch 3 holds throughout: SDK
 * hooks that run on the CHIP task take no StackLock, AT bridges called from
 * the parser thread always do, and nothing here reimplements any of core's
 * verdict protocol.
 */

/* ---- smoke/co alarm (0x0076) ------------------------------------------ */

/*
 * The cluster's one mandatory link symbol: declared at
 * smoke-co-alarm-server.h:178, called from smoke-co-alarm-server.cpp:112
 * (RequestSelfTest) and :450 (HandleRemoteSelfTestRequest), and defined
 * nowhere under src/, weak or otherwise; the only definitions in the tree
 * are example apps'. Leaving it out is a link error, the same shape the C6
 * documents for its own copy (design spec F4).
 *
 * Notify-only, the +MTCMD seq-0 form's first consumer on this platform
 * (mt_cmd_notify(), core/mt/mt_at.c): no mailbox slot, never blocks, no
 * verdict to give. In THIS tree HandleRemoteSelfTestRequest() has already
 * set TestInProgress true (:447) and ExpressedState kTesting (:448) when it
 * calls this hook (:450), and it answers the controller Status::Success
 * only AFTER the hook returns (:452). That is the reverse of the C6
 * comment's "already answered Success before calling this" ordering, re-read
 * here rather than copied; the functional conclusion is unchanged, because
 * the Success is hardcoded and nothing this hook does can influence it, so
 * fire-and-forget notify remains the correct shape. The busy pre-check
 * (:438-445) answers Status::Busy without ever reaching this hook, so a
 * self-test requested while ExpressedState is any alarm/testing state
 * raises no +MTCMD at all.
 *
 * Runs on the CHIP task (generated IMClusterCommandHandler dispatch), so no
 * StackLock, the same reasoning as the door lock hooks above.
 *
 * The host is expected to run its own test and report completion with
 * AT+MTALARM=<ep>,5,0 (TestInProgress false), which fires SelfTestComplete
 * and recomputes ExpressedState below (bug B165).
 */
void emberAfPluginSmokeCoAlarmSelfTestRequestCommand(chip::EndpointId endpointId)
{
    mt_cmd_notify(endpointId, chip::app::Clusters::SmokeCoAlarm::Id,
                  chip::app::Clusters::SmokeCoAlarm::Commands::SelfTestRequest::Id);
}

/*
 * ExpressedState priority order (bug B165, C6 bench finding F-C10-2). The
 * SDK's recompute contract, SetExpressedStateByPriority
 * (smoke-co-alarm-server.h:54-55), leaves the order entirely to the
 * application and ships NO default; every reference app in this tree pairs
 * its state-changing setters with a recompute call. Without one the
 * endpoint wedges: SetTestInProgress() fires SelfTestComplete but never
 * touches ExpressedState, so a completed self-test would stay stuck at
 * kTesting forever. Order ported from the C6
 * (platform/esp32c6/main/main.cpp:5598-5608), which itself copied the SDK's
 * dedicated example; the identical eight entries appear in THIS tree at
 * examples/smoke-co-alarm-app/silabs/src/SmokeCoAlarmManager.cpp:32-36,
 * verified rather than assumed: smoke alarm and its interconnect echo
 * outrank CO alarm and its echo, then hardware fault, an in-progress self
 * test, end-of-service, and low battery last.
 */
static const std::array<SmokeCoAlarmServer::ExpressedStateEnum, SmokeCoAlarmServer::kPriorityOrderLength>
    s_alarm_expressed_state_priority = {
        SmokeCoAlarmServer::ExpressedStateEnum::kSmokeAlarm,
        SmokeCoAlarmServer::ExpressedStateEnum::kInterconnectSmoke,
        SmokeCoAlarmServer::ExpressedStateEnum::kCOAlarm,
        SmokeCoAlarmServer::ExpressedStateEnum::kInterconnectCO,
        SmokeCoAlarmServer::ExpressedStateEnum::kHardwareFault,
        SmokeCoAlarmServer::ExpressedStateEnum::kTesting,
        SmokeCoAlarmServer::ExpressedStateEnum::kEndOfService,
        SmokeCoAlarmServer::ExpressedStateEnum::kBatteryAlert,
    };

/*
 * AT+MTALARM bridge, ported from the C6's cluster-dispatched form
 * (platform/esp32c6/main/main.cpp mt_matter_alarm_set()). mt_at.c's
 * cmd_mtalarm only checks the UNION bound 0..11 before calling this (it
 * cannot know which cluster <ep> carries); this bridge dispatches on the
 * cluster the endpoint actually has. TWO clusters since catalogue batch 8,
 * which added the RefrigeratorAlarm branch below rather than restructuring
 * (batch 8's branch is not on this port yet);
 * this function, exactly as the pre-batch-8 version of this comment
 * predicted it would; an endpoint carrying neither still answers
 * MT_ATTR_ERR_CLUSTER through the single fall-through at the end, the same
 * code the C6 answers.
 *
 * Field 0 (ExpressedState) is derived by the server from the other fields
 * and never settable directly; rejected here rather than in mt_at.c because
 * field 0 is a legal RefrigeratorAlarm bit on the C6 (batch 8's, not on this
 * port yet) and the union bound
 * must stay shared (core/include/mt_matter.h:647-651, binding).
 *
 * Every legal field maps to the matching SmokeCoAlarmServer setter, never a
 * raw attribute write: the setters emit the cluster's spec-mandated events
 * (SmokeAlarm, COAlarm, LowBattery, HardwareFault, EndOfService,
 * SelfTestComplete, AllClear) which a raw write to the same ember storage
 * would silently skip. <value> is range-checked against THAT field's own
 * enum bound before the cast, the two boolean fields against 0/1. Fields
 * whose attributes this composition does not declare (4, 8..11) pass their
 * range check and then fail inside the setter's attribute write,
 * MT_ATTR_ERR_FAILED, identical to the C6.
 *
 * needs_recompute mirrors the C6 row for row (B165): every field that
 * feeds s_alarm_expressed_state_priority recomputes after a successful
 * set; DeviceMuted, ContaminationState and SmokeSensitivityLevel do not,
 * on the reference apps' own evidence (no ExpressedStateEnum value
 * corresponds to any of the three). SetExpressedState() is idempotent
 * against the current value, so field 5's completion path emits
 * SelfTestComplete once and the recompute's AllClear only when the
 * priority verdict actually changes.
 */
extern "C" int mt_matter_alarm_set(uint16_t ep, uint8_t field, uint8_t value)
{
    using namespace chip::app::Clusters;
    chip::DeviceLayer::StackLock lock;
    /* attr_locate()'s two lookups, the same error division as every other
     * bridge in this file. */
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }

    if (emberAfContainsServer(ep, SmokeCoAlarm::Id)) {
        if (field == 0) {
            /* ExpressedState is derived; see the function comment. */
            return MT_ATTR_ERR_VALUE;
        }

        SmokeCoAlarmServer &srv = SmokeCoAlarmServer::Instance();
        bool ok;
        bool needs_recompute = false;
        switch (field) {
        case 1: /* SmokeState */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetSmokeState(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 2: /* COState */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetCOState(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 3: /* BatteryAlert */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetBatteryAlert(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 4: /* DeviceMuted: not declared here; the setter fails. Not an
                 * ExpressedState priority, no recompute. */
            if (value >= (uint8_t)SmokeCoAlarmServer::MuteStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetDeviceMuted(ep, (SmokeCoAlarmServer::MuteStateEnum)value);
            break;
        case 5: /* TestInProgress: bool, the self-test completion path at 0 */
            if (value > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetTestInProgress(ep, value != 0);
            needs_recompute = true;
            break;
        case 6: /* HardwareFaultAlert: bool */
            if (value > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetHardwareFaultAlert(ep, value != 0);
            needs_recompute = true;
            break;
        case 7: /* EndOfServiceAlert */
            if (value >= (uint8_t)SmokeCoAlarmServer::EndOfServiceEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetEndOfServiceAlert(ep, (SmokeCoAlarmServer::EndOfServiceEnum)value);
            needs_recompute = true;
            break;
        case 8: /* InterconnectSmokeAlarm: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetInterconnectSmokeAlarm(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 9: /* InterconnectCOAlarm: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetInterconnectCOAlarm(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 10: /* ContaminationState: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::ContaminationStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetContaminationState(ep, (SmokeCoAlarmServer::ContaminationStateEnum)value);
            break;
        case 11: /* SmokeSensitivityLevel: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::SensitivityEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetSmokeSensitivityLevel(ep, (SmokeCoAlarmServer::SensitivityEnum)value);
            break;
        default:
            /* cmd_mtalarm caps field at 11; unreachable, kept defensive. */
            return MT_ATTR_ERR_VALUE;
        }
        if (ok && needs_recompute) {
            srv.SetExpressedStateByPriority(ep, s_alarm_expressed_state_priority);
        }
        return ok ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
    }

    return MT_ATTR_ERR_CLUSTER;
}
/* ---- laundry washer / dishwasher / laundry dryer (OperationalState) ----
 *
 * One Instance PLUS one Delegate object per endpoint; the audit trail for
 * why (SetInstance's VerifyOrDie, the AAI-served attribute set, the forced
 * construct-after-create ordering, the never-destroy policy) lives on
 * opStateAttrs in mt_devtypes_zephyr.cpp. This section owns the objects.
 *
 * The verdict here IS the wire response, unlike the valve: the only caller
 * of each Handle*StateCallback copies err straight into the
 * OperationalCommandResponse (Instance::HandlePauseState,
 * operational-state-server.cpp:414-447, "mDelegate->HandlePauseStateCallback
 * (err); ... response.commandResponseState = err", and the
 * Stop/Start/Resume handlers at :449, :467, :485 follow the same shape). So
 * allow sets kNoError and deny sets kUnableToCompleteOperation, the same
 * pair the C6 maps (kUnableToCompleteOperation rather than
 * kUnableToStartOrResume matches the SDK's own dishwasher placeholder's
 * choice for a generic delegate-side denial).
 *
 * The delegate deliberately does NOT call SetOperationalState() on allow.
 * Same split-ownership rule as AT+MTLOCK and AT+MTVALVE: the verdict says
 * the host MAY act; only the host knows when the physical appliance
 * actually completed the transition, and it reports that separately with
 * AT+MTOPSTATE (mt_matter_opstate_set below), which emits the attribute
 * report a subscribed controller expects.
 *
 * The command hooks run on the CHIP task (the Instance's own
 * CommandHandlerInterface; this cluster is CHI-only, config-data.yaml:63),
 * so no StackLock in them, the batch 3 discipline.
 *
 * Instance reachability needs no registry: the Instance constructor calls
 * mDelegate->SetInstance(this) (operational-state-server.cpp:48), the base
 * Delegate stores it, and the protected GetInstance() accessor
 * (operational-state-server.h:364-371) is reachable from this subclass;
 * instance() below is the public passthrough mt_matter_opstate_set() uses,
 * the C6's exact mechanism.
 */
class HearthOpStateDelegate : public chip::app::Clusters::OperationalState::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    void set_cluster(chip::ClusterId cluster) { m_cluster = cluster; }
    chip::EndpointId endpoint() const { return m_ep; }
    chip::ClusterId cluster() const { return m_cluster; }

    /* GetInstance() is protected in the base Delegate; this passthrough is
     * how mt_matter_opstate_set() reaches the Instance the constructor
     * attached via SetInstance(). */
    chip::app::Clusters::OperationalState::Instance *instance() { return GetInstance(); }

    /* Always null, and since catalogue batch 8 something DEPENDS on that
     * beyond this method. The microwave declares CountdownTime
     * (MicrowaveOven.xml mandates it on OperationalState) and seeds its arena
     * slot to the uint32 null sentinel with NO k_instance_served carve-out,
     * precisely because the seeded null and this answer agree, so an AT
     * reader gets +MTERR:5 either way. A future round that gives this
     * delegate a real countdown must add that carve-out in the same commit
     * or the AT-visible shadow silently goes stale. The full reasoning is at
     * microwaveOpStateAttrs in mt_devtypes_zephyr.cpp; the note is here
     * because this is where the invariant would break. */
    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::NullNullable;
    }

    /* Exactly the four base states, in enum order. The SDK reads this list
     * to serve OperationalStateList; kError is a member because the spec
     * requires the error state to be listed even though AT+MTOPSTATE can
     * never set it directly (kError is reserved for the error-detection
     * path, and SetOperationalState() refuses it independently,
     * operational-state-server.cpp:101-107). */
    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index,
        chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override
    {
        using chip::app::Clusters::OperationalState::OperationalStateEnum;
        static const OperationalStateEnum states[] = {
            OperationalStateEnum::kStopped,
            OperationalStateEnum::kRunning,
            OperationalStateEnum::kPaused,
            OperationalStateEnum::kError,
        };
        if (index >= (sizeof(states) / sizeof(states[0]))) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState.Set(chip::to_underlying(states[index]));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override
    {
        /* NOT_FOUND at index 0 makes PhaseList read null: these appliances
         * publish no phases (operational-state-server.h:305-317 documents
         * the convention). */
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandlePauseStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Pause::Id, err);
    }

    void HandleResumeStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Resume::Id, err);
    }

    void HandleStartStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Start::Id, err);
    }

    void HandleStopStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Stop::Id, err);
    }

private:
    void forward(uint32_t command, chip::app::Clusters::OperationalState::GenericOperationalError &err)
    {
        using chip::app::Clusters::OperationalState::ErrorStateEnum;
        bool allow = mt_cmd_forward(m_ep, m_cluster, command);
        err.Set(chip::to_underlying(allow ? ErrorStateEnum::kNoError
                                          : ErrorStateEnum::kUnableToCompleteOperation));
    }

    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    /* Only the base cluster exists in this catalogue; carried anyway so the
     * +MTCMD cluster field always comes from the slot, not a literal, and a
     * future derived-cluster consumer (the oven cavity) reuses this class
     * unchanged, the C6's shape. */
    chip::ClusterId m_cluster = chip::app::Clusters::OperationalState::Id;
};

/* The OperationalState Delegate + Instance pair, the largest per-endpoint
 * object of batch 4 and the one the cluster-object arena is sized by
 * (graph DE555: sixteen of it). 256 is the nRF's measurement; this pins
 * the MG24's. */
static_assert(hearth_arena_cost(obj_pair_bytes<HearthOpStateDelegate,
                                                chip::app::Clusters::OperationalState::Instance>()) == 256,
              "the OperationalState pair changed size; redo HEARTH_OBJ_ARENA_BYTES and the README");
static_assert(hearth_arena_cost(obj_pair_bytes<HearthOpStateDelegate,
                                                chip::app::Clusters::OperationalState::Instance>()) *
                      kServiceableEndpoints <= HEARTH_OBJ_ARENA_BYTES,
              "sixteen OperationalState pairs no longer fit the cluster-object arena");

/*
 * The delegate pool, kServiceableEndpoints deep like the valve's (a
 * seventeenth endpoint of ANY type fails its create before it could ask
 * for a delegate; mt_matter.h's MT_COMP_MAX_ENDPOINTS sizing is the C6's,
 * which serves all 28). Memory reclaim round A: the pool is a table of
 * POINTERS into the cluster-object heap, and the Instance's raw storage
 * sits BEHIND the delegate in the same heap block rather than in a
 * parallel .bss array. OperationalState::Instance has no default
 * constructor (it takes the delegate and the endpoint id, neither known
 * until create time), so it is placement-constructed exactly once in
 * mt_matter_opstate_delegate_set_endpoint() and never destroyed, the
 * allocate-only policy the valve pool and the endpoint block heap already
 * follow: blocks are handed out monotonically by the one boot rebuild and
 * a reboot resets the heap wholesale, so no block is ever re-constructed
 * over a live Instance and no explicit destructor call is needed (or made;
 * see the opStateAttrs audit note for what ~Instance() would do).
 */
static HearthOpStateDelegate *s_opstate_delegates[kServiceableEndpoints];
static size_t s_opstate_delegate_next;

extern "C" void *mt_matter_opstate_delegate_alloc(uint32_t cluster_id)
{
    if (s_opstate_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthOpStateDelegate *d =
        obj_pair_new<HearthOpStateDelegate, chip::app::Clusters::OperationalState::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->set_cluster(cluster_id);
    s_opstate_delegates[s_opstate_delegate_next++] = d;
    return d;
}

/*
 * The second half of the handout, and on this platform it is where the
 * Instance is born: constructed into the raw storage behind the delegate
 * in its own heap block (memory reclaim round A; the delegate pointer IS
 * the block) and Init()ed, which registers the endpoint-scoped
 * CommandHandlerInterface and AttributeAccessInterface. Must run below a
 * successful emberAfSetDynamicEndpoint(): Init() opens with an
 * emberAfContainsServer() check and bails otherwise
 * (operational-state-server.cpp:65-70).
 *
 * An Init() failure is logged loudly and does not abort the create, the
 * valve read-back's reasoning exactly: below a successful
 * emberAfSetDynamicEndpoint() the contains-server check cannot fail, and
 * the registry Register() calls fail only on a duplicate registration,
 * which the construct-once pool rules out. If it ever fired anyway the
 * endpoint is live and correctly seeded, and an appliance that reports
 * state but does not adjudicate, plus a shouting log, beats tearing down
 * a healthy endpoint.
 */
/*
 * Catalogue batch 8 gave this function its second Instance type, and the
 * delegate class was written for it three batches ago: m_cluster exists
 * precisely so "a future derived-cluster consumer (the oven cavity) reuses
 * this class unchanged".
 *
 * OvenCavityOperationalState::Instance is a public subclass of
 * OperationalState::Instance whose entire body is a two-argument constructor
 * forwarding to the protected three-argument base with the cluster id baked
 * in (operational-state-server.h:459-478). It declares NO data members and
 * NO overrides, so its sizeof and alignof equal the base's and the raw
 * storage obj_pair_new() reserved for the base holds it exactly. That is
 * asserted rather than trusted: a future SDK that gave the subclass a member
 * would otherwise placement-construct past the end of the block, silently.
 */
static_assert(sizeof(chip::app::Clusters::OvenCavityOperationalState::Instance) ==
                  sizeof(chip::app::Clusters::OperationalState::Instance),
              "OvenCavityOperationalState::Instance grew past the base Instance's storage in "
              "the opstate pool's blocks");
static_assert(alignof(chip::app::Clusters::OvenCavityOperationalState::Instance) ==
                  alignof(chip::app::Clusters::OperationalState::Instance),
              "OvenCavityOperationalState::Instance out-aligns the base Instance's storage");

/* Catalogue batch 8 split the body out so the microwave's ordering function
 * can CHECK this Init()'s return rather than only hear about it in a log.
 * mt_matter.h's setter signature is void and that header is read-only, so
 * the public entry point below stays void and this helper carries the
 * error; the log text is emitted here so both callers produce the identical
 * line. */
static CHIP_ERROR opstate_construct_and_init(void *delegate, uint16_t ep)
{
    namespace OpState     = chip::app::Clusters::OperationalState;
    namespace OvenCavity  = chip::app::Clusters::OvenCavityOperationalState;
    auto *d = static_cast<HearthOpStateDelegate *>(delegate);
    d->set_endpoint(ep);
    uint8_t *storage = obj_inst_storage<HearthOpStateDelegate, OpState::Instance>(d);
    /* The cluster id was fixed at alloc time, before anything could be
     * spent, and it is what picks the Instance type here. Both arms yield an
     * OperationalState::Instance * (the derived one by upcast), which is
     * what the delegate's own GetInstance() passthrough hands back later, so
     * everything downstream of this point is cluster-agnostic. */
    OpState::Instance *inst = nullptr;
    if (d->cluster() == OvenCavity::Id) {
        inst = new (storage) OvenCavity::Instance(d, ep);
    } else {
        inst = new (storage) OpState::Instance(d, ep);
    }
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "opstate Instance::Init failed for endpoint %u cluster 0x%08X: "
                              "%" CHIP_ERROR_FORMAT
                              "; commands on it will not reach the host and its attributes will not be "
                              "served",
                    (unsigned)ep, (unsigned)d->cluster(), err.Format());
    }
    return err;
}

extern "C" void mt_matter_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)opstate_construct_and_init(delegate, ep);
}

/*
 * AT+MTOPSTATE bridge. mt_at.c's cmd_mtopstate has already rejected
 * anything outside the UNION of every opstate cluster family's legal set
 * (and 3, kError, outright) before this is called; this bridge narrows to
 * the legal set of the cluster ep actually carries, two branches since
 * catalogue batch 5: the base cluster's {0 Stopped, 1 Running, 2 Paused}
 * for the washer/dishwasher/dryer trio, and RvcOperationalState's widened
 * {0, 1, 2, 0x40 kSeekingCharger, 0x41 kCharging, 0x42 kDocked} for the
 * RVC. A union-legal RVC state (0x40..0x42) on a washer answers
 * MT_ATTR_ERR_VALUE in the base branch, exactly as the C6's does; the
 * oven-cavity branch arrives with its device type in the
 * composed-appliance batch. SetOperationalState() enforces each cluster's
 * rule independently (operational-state-server.cpp:101-107, kError or an
 * unsupported state answers CHIP_ERROR_INVALID_ARGUMENT), so a state that
 * slipped both checks would still map to MT_ATTR_ERR_FAILED rather than
 * being silently accepted.
 *
 * Called from the AT parser thread, so the StackLock IS taken, unlike the
 * delegate hooks above.
 */
extern "C" int mt_matter_opstate_set(uint16_t ep, uint8_t state)
{
    namespace OpState = chip::app::Clusters::OperationalState;
    namespace RvcOpState = chip::app::Clusters::RvcOperationalState;
    namespace OvenCavityOperationalState = chip::app::Clusters::OvenCavityOperationalState;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }

    if (emberAfContainsServer(ep, OpState::Id)) {
        if (state > 2) {
            return MT_ATTR_ERR_VALUE;
        }
        OpState::Instance *inst = nullptr;
        for (size_t i = 0; i < s_opstate_delegate_next; i++) {
            HearthOpStateDelegate *d = s_opstate_delegates[i];
            if (d->endpoint() == ep && d->cluster() == OpState::Id) {
                inst = d->instance();
                break;
            }
        }
        if (inst == nullptr) {
            /* Cannot happen once the boot rebuild has run: every endpoint
             * carrying the cluster got its pair in mt_devtype_create().
             * Defensive, the C6's same arm. */
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK
                                                                   : MT_ATTR_ERR_FAILED;
    }

    /* Catalogue batch 5: the RVC branch the pre-batch comment promised.
     * The derived cluster's state space widens to {0, 1, 2, 0x40
     * kSeekingCharger, 0x41 kCharging, 0x42 kDocked} (RvcOperationalState/
     * Enums.h:67-69); mt_at.c's union check has already admitted these,
     * and this is where a union-legal value lands on the RIGHT cluster
     * (0x40 on a washer still answers MT_ATTR_ERR_VALUE in the base
     * branch above, mirroring the C6's mt_matter_opstate_set() branch
     * order exactly). SetOperationalState() enforces the same set
     * independently through IsSupportedOperationalState(), so a state
     * that slipped both checks maps to MT_ATTR_ERR_FAILED rather than
     * being silently accepted. The Instance is reached through the RVC's
     * own delegate pool below, keyed by endpoint alone (one
     * RvcOperationalState per RVC endpoint), returned upcast to the base
     * Instance the way the base GetInstance() passthrough hands it over. */
    if (emberAfContainsServer(ep, RvcOpState::Id)) {
        if (!(state <= 2 || state == 0x40 || state == 0x41 || state == 0x42)) {
            return MT_ATTR_ERR_VALUE;
        }
        /* Catalogue batch 5 restores this: the RVC's own delegate pool and
         * mt_rvc_opstate_instance() are not in this batch, so an RVC endpoint
         * cannot yet be reached through the shared pool. The branch stays (the
         * RULING keeps the RVC arm) and answers FAILED until then. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Catalogue batch 8: the third branch this function's own comment has
     * promised since batch 5, and it is the RVC's mirror image
     * (AT_MT_SPEC.md §3.21, its OvenCavityOperationalState bullet).
     * OvenCavityOperationalState is a DERIVED
     * cluster that adds no derived-number-space states at all, so its legal
     * set is identical to the base cluster's {0 Stopped, 1 Running,
     * 2 Paused} and 0x40..0x42 answer +MTERR:1 on a cavity exactly as they
     * do on a washer. mt_at.c's union check needed no edit for this branch
     * for the same reason: the cavity's set is a subset of the union it
     * already admits. What is narrower than the base is the COMMAND set
     * (Stop and Start only), and that is the declared list's business, not
     * this function's: a host may report a Paused cavity even though no
     * controller can ask for one. The Instance comes from the shared opstate
     * pool, matched on both endpoint and cluster like the base branch. */
    if (emberAfContainsServer(ep, OvenCavityOperationalState::Id)) {
        if (state > 2) {
            return MT_ATTR_ERR_VALUE;
        }
        OpState::Instance *inst = nullptr;
        for (size_t i = 0; i < s_opstate_delegate_next; i++) {
            HearthOpStateDelegate *d = s_opstate_delegates[i];
            if (d->endpoint() == ep && d->cluster() == OvenCavityOperationalState::Id) {
                inst = d->instance();
                break;
            }
        }
        if (inst == nullptr) {
            /* Cannot happen once the boot rebuild has run; defensive, the
             * base branch's arm. */
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK
                                                                   : MT_ATTR_ERR_FAILED;
    }

    return MT_ATTR_ERR_CLUSTER;
}

/*
 * DE397 live read for the trio's two Instance-served scalars, dispatched
 * from mt_matter_attr_read() (see the carve-out comment above
 * attr_locate()). Runs under that bridge's StackLock; the pool scan is
 * the same one mt_matter_opstate_set() uses. Both attributes are unsigned
 * (OperationalStateEnum enum8, CurrentPhase uint8), and a null
 * CurrentPhase answers MT_ATTR_ERR_TYPE with the flag already set, the
 * generic null-nullable contract (the AT grammar has no null literal).
 * These appliances publish no phases, so CurrentPhase in practice always
 * answers +MTERR:5 here, matching what a controller reads as null.
 */
static int mt_opstate_attr_read_live(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                                     bool *is_unsigned)
{
    namespace OpState = chip::app::Clusters::OperationalState;
    if (is_unsigned) {
        *is_unsigned = true;
    }
    /* Catalogue batch 8: keyed on (endpoint, CLUSTER) rather than endpoint
     * and the base cluster id, because the shared pool now serves the oven
     * cavity's derived cluster too. The base cluster's answers are
     * unchanged: no device type carries both, so the same slot is found
     * either way. */
    OpState::Instance *inst = nullptr;
    for (size_t i = 0; i < s_opstate_delegate_next; i++) {
        HearthOpStateDelegate *d = s_opstate_delegates[i];
        if (d->endpoint() == ep && d->cluster() == cluster) {
            inst = d->instance();
            break;
        }
    }
    if (inst == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive, the
         * same arm as mt_matter_opstate_set(). */
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

