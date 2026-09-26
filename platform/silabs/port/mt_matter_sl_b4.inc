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
