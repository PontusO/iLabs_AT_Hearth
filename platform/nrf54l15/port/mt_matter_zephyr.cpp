/*
 * mt_matter_zephyr.cpp - the mt_matter.h commissioning/state/network
 * slice against CHIP on Zephyr (Matter core spec section 6). The AT
 * parser thread calls these; anything touching CHIP state takes the
 * stack lock (the bridge_manager.cpp discipline).
 */

#include <app/ConcreteAttributePath.h>
#include <app/clusters/boolean-state-server/CodegenIntegration.h>
/* Catalogue batch 3, the two command-verdict types. door-lock-server.h also
 * pulls DlLockState, OperationSourceEnum, OperationErrorEnum, Nullable and
 * Optional into the global namespace itself (:46-64), which is why the
 * ember hook definitions further down can spell their signatures exactly as
 * the header declares them. */
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/clusters/valve-configuration-and-control-server/valve-configuration-and-control-cluster.h>
#include <app/clusters/valve-configuration-and-control-server/valve-configuration-and-control-delegate.h>
/* Catalogue batch 4: the smoke/co alarm singleton server and its
 * ExpressedState priority recompute (std::array is the recompute's own
 * parameter type). */
#include <app/clusters/smoke-co-alarm-server/smoke-co-alarm-server.h>
/* Catalogue batch 4: the OperationalState Instance-plus-Delegate machinery
 * for the washer/dishwasher/dryer trio; <new> for the placement-new the
 * non-default-constructible Instance pool needs. */
#include <app/clusters/operational-state-server/operational-state-server.h>
/* Catalogue batch 4: the process-global SupportedModesManager for mode
 * select, and MatterReportingAttributeChangeCallback() for the host-fed
 * SupportedModes list's dirty-marking. */
#include <app/clusters/mode-select-server/supported-modes-manager.h>
/* Catalogue batch 4: the per-endpoint ChimeServer and its delegate. */
#include <app/clusters/chime-server/chime-server.h>
#include <app/reporting/reporting.h>

#include <array>
#include <new>
#include <app/server/Server.h>
#include <clusters/AirQuality/Enums.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/util/attribute-storage.h>
#include <app/util/attribute-table.h>
/* AttributeBaseType(): the tree's own alias-to-base ZCL type mapping, read
 * by attr_type_info() below so the AT+MTATTR integer family cannot drift
 * from the SDK's definition of it. */
#include <app/util/ember-io-storage.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConnectivityManager.h>
#include <platform/ThreadStackManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <openthread/dataset.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>

extern "C" {
#include "mt_at.h"
#include "mt_composition.h"
#include "mt_matter.h"
}
#include "mt_port_ids.h"

LOG_MODULE_REGISTER(hearth_matter, LOG_LEVEL_INF);

using chip::DeviceLayer::ConnectivityMgr;
using chip::DeviceLayer::ThreadStackMgr;
using chip::DeviceLayer::ThreadStackMgrImpl;
using chip::Protocols::InteractionModel::Status;

extern "C" int mt_matter_state(void)
{
    chip::DeviceLayer::StackLock lock;
    if (chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen()) {
        return MT_STATE_COMMISSIONING;
    }
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
        return MT_STATE_OPERATIONAL;
    }
    return MT_STATE_UNINIT;
}

extern "C" int mt_matter_fabric_count(void)
{
    chip::DeviceLayer::StackLock lock;
    return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

extern "C" int mt_matter_open_commissioning(int timeout_s)
{
    chip::DeviceLayer::StackLock lock;
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds32(timeout_s));
    return (err == CHIP_NO_ERROR) ? 0 : -1;
}

extern "C" int mt_matter_onboarding_codes(char *qr, size_t qr_len, char *manual, size_t manual_len)
{
    /*
     * StackLock here is C6-parity discipline, not a bug fix for an observed
     * race: fix round 1's bare-ERROR bench finding turned out to be a
     * controller test-form error (AT+MTCODES, the EXEC form, sent instead
     * of the AT+MTCODES? query form that cmd_mtcodes actually requires;
     * core/mt/mt_at.c:202-215), caught and confirmed on this same build.
     * No settings-I/O race was ever observed, and none should be claimed
     * here. The original task review judged this call lock-free-safe, and
     * that judgment stands; the lock is kept anyway because the C6 takes
     * ChipStackLock for this exact function and uniform locking across
     * every CHIP-touching function in this file is cheaper to reason about
     * than a per-function safety argument for the one exception.
     *
     * The LOG_ERR calls below stay for a different, real reason: chasing
     * fix round 1's bare ERROR back to its actual cause (a caller-side
     * command-form mismatch, not this function) took longer than it should
     * have precisely because this path logged nothing on failure. Keeping
     * these means any future failure here, whatever its cause, is visible
     * on the console instead of forcing that same trace again.
     */
    if (qr_len == 0 || manual_len == 0) {
        /* qr_len - 1 / manual_len - 1 below would underflow a size_t of 0
         * into SIZE_MAX, handing MutableCharSpan a buffer size no caller
         * ever meant. Refused before the lock is even taken. */
        return -1;
    }
    chip::DeviceLayer::StackLock lock;

    chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kBLE);
    chip::MutableCharSpan qr_span(qr, qr_len - 1);
    chip::MutableCharSpan manual_span(manual, manual_len - 1);

    CHIP_ERROR err = GetQRCode(qr_span, flags);
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("mt_matter_onboarding_codes: GetQRCode failed: %" CHIP_ERROR_FORMAT, err.Format());
        return -1;
    }
    err = GetManualPairingCode(manual_span, flags);
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("mt_matter_onboarding_codes: GetManualPairingCode failed: %" CHIP_ERROR_FORMAT, err.Format());
        return -1;
    }
    qr[qr_span.size()] = '\0';
    manual[manual_span.size()] = '\0';
    return 0;
}

extern "C" void mt_matter_factory_reset(void)
{
    /* Schedules erase-and-reboot on the CHIP thread; cmd_mtfreset's OK
     * is already on the wire by the time the reboot lands, mirroring
     * the C6 ordering. */
    chip::Server::GetInstance().ScheduleFactoryReset();
}

extern "C" int mt_matter_net_info(int *transport, int *enabled, int *connected)
{
    chip::DeviceLayer::StackLock lock;
    if (transport) *transport = MT_NET_THREAD;
    if (enabled)   *enabled   = ConnectivityMgr().IsThreadEnabled() ? 1 : 0;
    if (connected) *connected = ConnectivityMgr().IsThreadAttached() ? 1 : 0;
    return 0;
}

extern "C" int mt_matter_transport_mismatch(void)
{
    /* Thread is the only transport this image can ever have
     * commissioned on, so the stored-credentials-on-the-wrong-transport
     * state is unreachable here. The C6's marker comparison collapses
     * to a constant. */
    return 0;
}

/*
 * OT role to Matter RoutingRoleEnum (ThreadNetworkDiagnostics), matching
 * CHIP's own derivation (thread-network-diagnostics-provider.cpp:112-118)
 * and AT_MT_SPEC.md's `<role>` table (~2894-2907): DISABLED is
 * kUnspecified (0), DETACHED is kUnassigned (1) - the two are NOT the
 * same token, unlike a plain "no dataset" or "not attached" read. A CHILD
 * with the radio off when idle is kSleepyEndDevice (2) regardless of
 * router eligibility; a CHILD with the radio on is kReed (4) when
 * router-eligible, kEndDevice (3) otherwise. Both build arms compile
 * CONFIG_OPENTHREAD_FTD=y, so otThreadIsRouterEligible() is always
 * reachable here and no #if guard is needed the way CHIP's own
 * CHIP_DEVICE_CONFIG_THREAD_FTD one is.
 */
static uint8_t ot_role_to_matter(otInstance *ot)
{
    switch (otThreadGetDeviceRole(ot)) {
    case OT_DEVICE_ROLE_DISABLED: return 0;  /* kUnspecified */
    case OT_DEVICE_ROLE_DETACHED: return 1;  /* kUnassigned */
    case OT_DEVICE_ROLE_CHILD: {
        otLinkModeConfig mode = otThreadGetLinkMode(ot);
        if (!mode.mRxOnWhenIdle) {
            return 2;  /* kSleepyEndDevice */
        }
        return otThreadIsRouterEligible(ot) ? 4 /* kReed */ : 3 /* kEndDevice */;
    }
    case OT_DEVICE_ROLE_ROUTER: return 5;    /* kRouter */
    case OT_DEVICE_ROLE_LEADER: return 6;    /* kLeader */
    default: return 0;                       /* kUnspecified */
    }
}

extern "C" int mt_matter_thread_info(mt_thread_info_t *out)
{
    if (!out) {
        return MT_ATTR_ERR_FAILED;
    }
    memset(out, 0, sizeof(*out));

    ThreadStackMgr().LockThreadStack();
    otInstance *ot = ThreadStackMgrImpl().OTInstance();
    if (!ot) {
        /* No OpenThread instance yet (should not happen once the Thread
         * stack has started, but this read must never dereference a null
         * otInstance). */
        ThreadStackMgr().UnlockThreadStack();
        return MT_ATTR_ERR_FAILED;
    }
    out->role = ot_role_to_matter(ot);
    otDeviceRole role = otThreadGetDeviceRole(ot);
    out->attached = (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
                     role == OT_DEVICE_ROLE_LEADER);

    /*
     * The four id fields (and the network name) are gated on "dataset
     * installed" (otDatasetIsCommissioned()), NOT on the attached
     * predicate above: AT_MT_SPEC.md's binding derivation (~2927-2949)
     * is explicit that a device holding a dataset but still detached
     * already reports its channel, PAN ID, extended PAN ID, partition id
     * and network name, matching CHIP's own gate
     * (thread-network-diagnostics-provider.cpp:68,
     * `if (!otDatasetIsCommissioned(otInst))`). Using `attached` here
     * would make every one of those fields read null for the entire
     * window between dataset install and attachment, which is exactly
     * the false negative the spec calls out.
     */
    if (otDatasetIsCommissioned(ot)) {
        out->has_channel = true;
        out->channel = otLinkGetChannel(ot);
        out->has_panid = true;
        out->panid = otLinkGetPanId(ot);
        const otExtendedPanId *ext = otThreadGetExtendedPanId(ot);
        out->has_extpanid = true;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            v = (v << 8) | ext->m8[i];
        }
        out->extpanid = v;
        out->has_partitionid = true;
        out->partitionid = otThreadGetPartitionId(ot);

        /* Same gate as the four ids above: with no dataset installed,
         * OpenThread's otThreadGetNetworkName() still answers its
         * compiled-in default ("OpenThread"), which is not this
         * device's network name and must not reach the wire. out->name
         * is already zeroed (memset above), so leaving this block
         * unentered is what makes the unconfigured case render "". */
        const char *name = otThreadGetNetworkName(ot);
        if (name) {
            strncpy(out->name, name, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
        }
    }
    ThreadStackMgr().UnlockThreadStack();
    return MT_ATTR_OK;
}

extern "C" const char *mt_thread_role_name(uint8_t role)
{
    switch (role) {
    case 0: return "UNSPECIFIED";
    case 1: return "UNASSIGNED";
    case 2: return "SLEEPY_END_DEVICE";
    case 3: return "END_DEVICE";
    case 4: return "REED";
    case 5: return "ROUTER";
    case 6: return "LEADER";
    default: return nullptr;
    }
}

/* ---- the live endpoint table ----------------------------------------- */

/*
 * What the boot rebuild actually created, in creation order. The stored
 * composition (mt_comp_store.h) is the intent; this is the outcome, and
 * AT+MTEP? reports this one. Parallel arrays rather than a struct array,
 * mirroring the C6's s_live_* tables so the two read alike.
 *
 * Written only from the boot path in main.cpp, before mt_at_start() lets
 * any AT command run, so there is no concurrent access to guard.
 *
 * Depth is kServiceableEndpoints (capacity), not MT_COMP_MAX_ENDPOINTS
 * (acceptance): the only writer is mt_matter_record_endpoint(), which
 * main.cpp calls once per endpoint that mt_devtype_create() actually stood
 * up, so an entry here can only exist for an endpoint this build is
 * serving. A composition may DECLARE 28, but the seventeenth never gets
 * created and so is never recorded. Nothing indexes these by composition
 * index either: mt_matter_endpoint_info() bounds its caller against
 * s_live_count, which is what main.cpp's parent lookup passes through.
 */
static uint32_t s_live_devtype[kServiceableEndpoints];
static uint16_t s_live_ep_id[kServiceableEndpoints];
static uint8_t s_live_variant[kServiceableEndpoints];
static uint8_t s_live_parent[kServiceableEndpoints];
static uint16_t s_live_count;

extern "C" uint16_t mt_matter_endpoint_count(void)
{
    return s_live_count;
}

extern "C" int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id,
                                       uint8_t *variant, uint8_t *parent_idx)
{
    if (index >= s_live_count) {
        if (devtype) *devtype = 0;
        if (ep_id) *ep_id = 0;
        if (variant) *variant = 0;
        if (parent_idx) *parent_idx = 0;
        return -1;
    }
    if (devtype) *devtype = s_live_devtype[index];
    if (ep_id) *ep_id = s_live_ep_id[index];
    if (variant) *variant = s_live_variant[index];
    if (parent_idx) *parent_idx = s_live_parent[index];
    return 0;
}

extern "C" void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id, uint8_t variant,
                                          uint8_t parent_idx)
{
    if (s_live_count >= kServiceableEndpoints) {
        return;
    }
    s_live_devtype[s_live_count] = devtype;
    s_live_ep_id[s_live_count] = ep_id;
    s_live_variant[s_live_count] = variant;
    s_live_parent[s_live_count] = parent_idx;
    s_live_count++;
}

/* ---- attribute bridge (Task 5, AT+MTATTR) ------------------------------ */

/*
 * ZCL type to signedness and byte width; anything outside the AT+MTATTR
 * integer family is MT_ATTR_ERR_TYPE.
 *
 * Fix round 2 (bench bug). The switch below knows only the BASE integer,
 * bool, enum and bitmap type codes. Matter also has a large family of ALIAS
 * type codes that are integers wearing a semantic name: ZCL_TEMPERATURE
 * (0xD8), ZCL_PERCENT (0xE6), ZCL_PERCENT100THS (0xE7), ZCL_EPOCH_S,
 * ZCL_ELAPSED_S, ZCL_VENDOR_ID and about thirty more. Catalogue batch 2 was
 * the first round to declare any of them (the thermostat's TEMPERATURE
 * setpoints, the fan's PERCENT settings, the window covering's
 * PERCENT100THS lift positions), and every one of those attributes fell
 * straight through to `default: return false`. The damage: an AT+MTATTR
 * read answered +MTERR:5 and MatterPostAttributeChangeCallback dropped the
 * +MTATTR URC for controller-driven changes, while the very same attributes
 * worked perfectly over a controller's IM. Bench-confirmed on all three
 * clusters.
 *
 * Nasty because at the AT layer the failure is indistinguishable from
 * correct behaviour: mt_matter.h:178-181 makes a NULL nullable read answer
 * MT_ATTR_ERR_TYPE too, so +MTERR:5 is the honest answer for a null value
 * AND the answer for "not an integer attribute". A tester reading +MTERR:5
 * on a fresh fan sees exactly what a null PercentSetting would look like.
 *
 * The fix normalises through the SDK's own alias table rather than
 * transcribing one here. There is no shared core-side helper to reuse:
 * core/include/mt_at.h exports no type-family classifier, and the C6's
 * (main.cpp's attr_val_is_unsigned()/attr_val_to_i64()) is keyed on
 * esp_matter_val_type_t, esp-matter's own enum, which is why the C6 never
 * had this bug: esp-matter maps the alias types down to its INT16/UINT8/
 * UINT16 val types before its AT bridge ever sees them. This port reads
 * ember metadata directly, so it must do that mapping itself.
 *
 * chip::app::Compatibility::Internal::AttributeBaseType()
 * (app/util/ember-io-storage.cpp:36-126, declared in ember-io-storage.h) is
 * THE definition of the family in this tree: it maps every alias to its
 * basic int(8|16|32|64)(s|u) code and, critically, returns any type it does
 * not recognise unchanged (`default: return type`, :123-124). So the switch
 * below is reached with exactly the value it used to be reached with for
 * every base type and for every string, array, struct and float:
 * base-type behaviour is unchanged by construction and no arm below is
 * touched. What changes is only that aliases now arrive pre-translated.
 *
 * The mappings this catalogue needs, from that function:
 *   ZCL_TEMPERATURE   (0xD8) -> ZCL_INT16S  (signed, 2)    :120-121
 *   ZCL_PERCENT       (0xE6) -> ZCL_INT8U   (unsigned, 1)  :40-48
 *   ZCL_PERCENT100THS (0xE7) -> ZCL_INT16U  (unsigned, 2)  :50-62
 * and, covered for free the moment a future device type needs them:
 * ENUM16/BITMAP16/VENDOR_ID/GROUP_ID/ENDPOINT_NO -> INT16U;
 * EPOCH_S/ELAPSED_S/CLUSTER_ID/ATTRIB_ID/COMMAND_ID/EVENT_ID/DEVTYPE_ID/
 * DATA_VER/BITMAP32 -> INT32U; EPOCH_US/POSIX_MS/SYSTIME_MS/SYSTIME_US/
 * NODE_ID/FABRIC_ID/EVENT_NO/BITMAP64 -> INT64U; the electrical-measurement
 * family (POWER_MW, POWER_MVA, POWER_MVAR, AMPERAGE_MA, VOLTAGE_MV,
 * ENERGY_MWH, ENERGY_MVAH, ENERGY_MVARH, MONEY) -> INT64S;
 * ACTION_ID/FABRIC_IDX/STATUS -> INT8U.
 *
 * Signedness agrees with chip::app::IsSignedAttributeType()
 * (app-common/zap-generated/attribute-type.h), the tree's other oracle:
 * TEMPERATURE is listed signed there and maps to INT16S here.
 *
 * attr_null_sentinel() below needs no change of its own. It is keyed on the
 * (is_unsigned, bytes) pair this function produces, so TEMPERATURE gets the
 * signed 2-byte sentinel 0x8000, PERCENT the unsigned 1-byte 0xFF and
 * PERCENT100THS the unsigned 2-byte 0xFFFF: exactly the bytes
 * mt_devtypes_zephyr.cpp's seed rows write for those attributes.
 */
static bool attr_type_info(EmberAfAttributeType t, bool *is_unsigned, uint8_t *bytes)
{
    switch (chip::app::Compatibility::Internal::AttributeBaseType(t)) {
    case ZAP_TYPE(BOOLEAN): case ZAP_TYPE(BITMAP8): case ZAP_TYPE(ENUM8): case ZAP_TYPE(INT8U):
        *is_unsigned = true;  *bytes = 1; return true;
    case ZAP_TYPE(BITMAP16): case ZAP_TYPE(ENUM16): case ZAP_TYPE(INT16U):
        *is_unsigned = true;  *bytes = 2; return true;
    case ZAP_TYPE(INT24U): *is_unsigned = true; *bytes = 3; return true;
    case ZAP_TYPE(BITMAP32): case ZAP_TYPE(INT32U):
        *is_unsigned = true;  *bytes = 4; return true;
    case ZAP_TYPE(INT48U): *is_unsigned = true; *bytes = 6; return true;
    case ZAP_TYPE(BITMAP64): case ZAP_TYPE(INT64U):
        *is_unsigned = true;  *bytes = 8; return true;
    case ZAP_TYPE(INT8S):  *is_unsigned = false; *bytes = 1; return true;
    case ZAP_TYPE(INT16S): *is_unsigned = false; *bytes = 2; return true;
    case ZAP_TYPE(INT32S): *is_unsigned = false; *bytes = 4; return true;
    case ZAP_TYPE(INT64S): *is_unsigned = false; *bytes = 8; return true;
    default: return false;
    }
}

/*
 * Locate an attribute's metadata, splitting endpoint/cluster/attribute
 * absence the way mt_matter.h's mt_attr_result_t comments require it split:
 * an unknown endpoint index is MT_ATTR_ERR_ENDPOINT (emberAfIndexFromEndpoint
 * against kEmberInvalidEndpointIndex, attribute-storage.h), an endpoint that
 * exists but does not carry the cluster is MT_ATTR_ERR_CLUSTER
 * (emberAfContainsServer, attribute-storage.h:98-104), and a present cluster
 * with no such attribute is MT_ATTR_ERR_ATTRIBUTE (emberAfLocateAttributeMetadata).
 *
 * emberAfContainsServer(), not emberAfFindServerCluster(): the latter is not
 * declared in attribute-storage.h (this file's only ember include), only in
 * app/util/endpoint-config-api.h; emberAfContainsServer() is its exact
 * wrapper (attribute-storage.cpp:828-830, `return
 * emberAfFindServerCluster(endpoint, clusterId) != nullptr;`), so calling it
 * is equivalent and needs no extra include.
 */
static mt_attr_result_t attr_locate(uint16_t ep, uint32_t cluster, uint32_t attr,
                                    const EmberAfAttributeMetadata **out_md)
{
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, cluster)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    const EmberAfAttributeMetadata *md = emberAfLocateAttributeMetadata(ep, cluster, attr);
    if (md == nullptr) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    *out_md = md;
    return MT_ATTR_OK;
}

/*
 * NumericAttributeTraits::GetNullValue() (attribute-storage-null-handling.h):
 * the type maximum for unsigned, the type minimum for signed - same
 * sentinels mt_devtypes_zephyr.cpp's s_seeds table documents and uses.
 * Shared by mt_matter_attr_read()'s null check and
 * MatterPostAttributeChangeCallback()'s (fix round 1, C2) so the two cannot
 * drift apart.
 */
static uint64_t attr_null_sentinel(bool is_unsigned, uint8_t bytes)
{
    return is_unsigned
        ? (bytes >= 8 ? (uint64_t)-1 : ((1ULL << (8 * bytes)) - 1))
        : (bytes >= 8 ? (1ULL << 63) : (1ULL << (8 * bytes - 1)));
}

/*
 * out is a required, non-null out-parameter (unlike is_unsigned, which
 * mt_matter.h documents as "may be NULL"): mt_at.c's cmd_mtattr always
 * passes the address of a stack local, the same precondition the C6's
 * mt_matter_attr_read() (main.cpp) relies on without a guard of its own.
 */
extern "C" int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                                   bool *is_unsigned)
{
    chip::DeviceLayer::StackLock lock;

    const EmberAfAttributeMetadata *md = nullptr;
    mt_attr_result_t r = attr_locate(ep, cluster, attr, &md);
    if (r != MT_ATTR_OK) {
        return r;
    }

    bool unsigned_type;
    uint8_t bytes;
    if (!attr_type_info(md->attributeType, &unsigned_type, &bytes)) {
        /* Genuinely not an AT+MTATTR type (string/array/struct/float): no
         * definite signedness exists, but the header's "the flag is also
         * valid when the result is MT_ATTR_ERR_TYPE" covers this arm too,
         * and cmd_mtattr (mt_at.c) always lets MT_ATTR_ERR_TYPE fall through
         * to a write that reads this flag before it re-derives the same
         * failure. Same choice as the C6 (main.cpp's mt_matter_attr_read(),
         * the ESP_ERR_NOT_SUPPORTED/ARRAY arm): false selects the signed
         * parse, and the write fails on its own attr_type_info() check for
         * the identical reason. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_TYPE;
    }
    /* Set before the null check below, so the flag stays valid even when
     * this read goes on to answer MT_ATTR_ERR_TYPE for a null nullable
     * value (core/include/mt_matter.h:172-176, binding). */
    if (is_unsigned) {
        *is_unsigned = unsigned_type;
    }

    uint8_t buf[8] = { 0 };
    Status st = emberAfReadAttribute(ep, cluster, attr, buf, sizeof(buf));
    if (st != Status::Success) {
        /* Cannot happen once attr_locate() has already proven the
         * endpoint/cluster/attribute triple exists; defensive only. */
        return MT_ATTR_ERR_FAILED;
    }

    uint64_t raw = 0;
    for (uint8_t i = 0; i < bytes; i++) {
        raw |= ((uint64_t)buf[i]) << (8 * i);
    }

    if (md->IsNullable() && raw == attr_null_sentinel(unsigned_type, bytes)) {
        /* The AT grammar has no null literal: answer MT_ATTR_ERR_TYPE
         * with is_unsigned already set above, so a caller about to
         * WRITE can still fetch the signedness first. */
        return MT_ATTR_ERR_TYPE;
    }

    if (unsigned_type) {
        *out = (int64_t)raw;  /* reinterpret through uint64_t, per the header */
    } else {
        int shift = 64 - 8 * bytes;
        *out = (int64_t)(raw << shift) >> shift;
    }
    return MT_ATTR_OK;
}

extern "C" int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t val, bool notify)
{
    chip::DeviceLayer::StackLock lock;

    const EmberAfAttributeMetadata *md = nullptr;
    mt_attr_result_t r = attr_locate(ep, cluster, attr, &md);
    if (r != MT_ATTR_OK) {
        return r;
    }

    bool unsigned_type;
    uint8_t bytes;
    if (!attr_type_info(md->attributeType, &unsigned_type, &bytes)) {
        return MT_ATTR_ERR_TYPE;
    }

    /* Width bounds check ahead of the write: a value outside the
     * attribute's own width is MT_ATTR_ERR_VALUE, not a silent truncation
     * (core/include/mt_matter.h:185-188, binding). */
    if (bytes < 8) {
        uint64_t u = (uint64_t)val;
        if (unsigned_type) {
            uint64_t max_u = (1ULL << (8 * bytes)) - 1;
            if (u > max_u) {
                return MT_ATTR_ERR_VALUE;
            }
        } else {
            int64_t min_s = -(int64_t)(1ULL << (8 * bytes - 1));
            int64_t max_s = (int64_t)((1ULL << (8 * bytes - 1)) - 1);
            if (val < min_s || val > max_s) {
                return MT_ATTR_ERR_VALUE;
            }
        }
    }

    /*
     * No metadata->IsWritable() gate here - a deliberate delta from the
     * brief's sketch, recorded in the task report with tree evidence on
     * both platforms:
     *
     * emAfWriteAttribute()'s writable/data-type guard runs only when
     * overrideReadOnlyAndDataType is false (NCS v3.3.4
     * attribute-table.cpp:350-364), and BOTH public emberAfWriteAttribute()
     * overloads pass true for it (attribute-table.cpp:189-199): "This
     * function will not check to see if the attribute is writable since the
     * read only/writable characteristic of an attribute only pertains to
     * external devices writing over the air... it assumes the device knows
     * what it is doing" (the function's own doc comment). A locally
     * originated write is trusted the same way on the C6: esp-matter's
     * set_val()/update() (esp_matter_data_model.cpp:1091) skip their
     * ATTRIBUTE_FLAG_WRITABLE-less refusal entirely for a plain,
     * not-managed-internally attribute, which is what TemperatureMeasurement
     * MeasuredValue and Identify IdentifyType both are on the C6 too
     * (esp_matter_attribute.cpp: create_measured_value() is
     * ATTRIBUTE_FLAG_NULLABLE only, create_identify_type() is
     * ATTRIBUTE_FLAG_NONE - neither carries WRITABLE or
     * MANAGED_INTERNALLY). This bridge's write is that same "the caller is
     * the local host, not a controller over the air" concession, which is
     * exactly why the controller bench gate's own MeasuredValue write has to
     * succeed even though ZAP declares it without WRITABLE
     * (mt_devtypes_zephyr.cpp:163-164).
     *
     * mt_matter.h's MT_ATTR_ERR_READONLY comment is scoped narrower than "no
     * WRITABLE flag": it names a specific mechanism, "served by a cluster
     * Instance (ATTRIBUTE_FLAG_MANAGED_INTERNALLY without
     * ATTRIBUTE_FLAG_WRITABLE)". This milestone's registry has no
     * Instance-managed cluster at all (no delegate pool exists yet on this
     * platform), so that condition cannot occur here - MT_ATTR_ERR_READONLY
     * is unreachable from this bridge today, the same as it was on the C6
     * before its first Instance-managed cluster shipped. A gate on
     * IsWritable() alone would answer READONLY for MeasuredValue too, which
     * the bench gate explicitly requires to succeed; it is not implemented
     * here for that reason. Concretely: the brief's own bench gate line "a
     * write to IdentifyType (read-only) answers the read-only error" will
     * NOT reproduce with this bridge - IdentifyType is locally writable, at
     * parity with the C6 at the same point in its history. Flagged for the
     * controller in the task report; the fix, if this parity is not what is
     * wanted here, is a future round's Instance-managed IdentifyCluster (or
     * an explicit non-generic carve-out), not a blanket IsWritable() check
     * that breaks MeasuredValue.
     */

    uint8_t buf[8] = { 0 };
    uint64_t u = (uint64_t)val;
    for (uint8_t i = 0; i < bytes; i++) {
        buf[i] = (uint8_t)(u >> (8 * i));
    }

    chip::app::ConcreteAttributePath path(ep, cluster, attr);
    EmberAfWriteDataInput input(buf, md->attributeType);
    input.SetMarkDirty(notify ? chip::app::MarkAttributeDirty::kIfChanged
                              : chip::app::MarkAttributeDirty::kNo);

    /*
     * notify selects ONLY MarkAttributeDirty - kIfChanged (notify=true) marks
     * the change for the CHIP reporting engine, so subscribed/bound
     * controllers see it; kNo (notify=false) changes local storage without
     * that fabric-facing report. It does NOT gate the +MTATTR URC: fix round
     * 1 (controller ruling, C1) overturned this file's original
     * s_suppress_attr_urc design, which assumed notify=false host writes
     * should echo silently. The wire spec says otherwise on both platforms:
     * AT_MT_SPEC.md 3.8 states plainly, with no mode carve-out, "A write to
     * an ember-managed attribute that actually changes its value echoes a
     * +MTATTR URC ... then OK", and 3.25's AirQuality write is called out as
     * the ONE exception that "never echoes the +MTATTR URC, in either mode"
     * - an exception that only makes sense if the general rule already
     * covers both modes. On the C6, esp-matter's set_val()
     * (esp_matter_data_model.cpp:784-788) fires its POST_UPDATE callback
     * unconditionally whenever call_callbacks defaults true, with no
     * per-mode gate either; mt_matter_attr_write() there never suppresses
     * app_attribute_update_cb(). So a notify=false reflection of a
     * controller-driven change DOES echo back to the host as a +MTATTR URC
     * on both platforms - the host is expected to see its own reflection
     * confirmed, not silenced, and de-duplication (if ever needed) is a host
     * concern, not this bridge's.
     */
    Status st = emberAfWriteAttribute(path, input);

    switch (st) {
    case Status::Success:
        /* A same-value re-push also answers Success in both notify modes:
         * emAfWriteAttribute()'s AttributeValueIsChanging() early-out
         * (attribute-table.cpp:408-426) returns Success without touching
         * MarkAttributeDirty's callback path at all. OK by construction; no
         * special case needed here (binding per core/include/mt_matter.h's
         * same-value comment). */
        return MT_ATTR_OK;
    case Status::ConstraintError:
        /* ember's own MIN_MAX bounds check (attribute-table.cpp:366-406)
         * runs unconditionally, independent of the override flag noted
         * above. */
        return MT_ATTR_ERR_VALUE;
    default:
        return MT_ATTR_ERR_FAILED;
    }
}

/*
 * Strong override of the weak default in NCS's generic-callback-stubs.cpp.
 * Every attribute write that actually changes a value passes through here,
 * whether it came from this bridge's write (either notify mode - see the
 * comment in mt_matter_attr_write() above, fix round 1/C1) or from a
 * controller's own IM write - so any change surfaces to the host as a
 * +MTATTR URC the same way the C6's app_attribute_update_cb() does. Format
 * is byte-identical to platform/esp32c6/main/main.cpp:838-842.
 */
void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &path, uint8_t type,
                                       uint16_t size, uint8_t *value)
{
    if (path.mEndpointId == 0 || path.mEndpointId == kCatalogueEndpointId) return;
    bool is_unsigned; uint8_t bytes;
    if (!attr_type_info((EmberAfAttributeType)type, &is_unsigned, &bytes) || size < bytes) return;
    uint64_t raw = 0;
    for (uint8_t i = 0; i < bytes; i++) raw |= ((uint64_t)value[i]) << (8 * i);

    /*
     * Fix round 1, C2: a transition TO null must not echo the raw sentinel
     * as if it were a real value (a host reading +MTATTR:...,255 has no way
     * to tell that from a genuine 255, and a follow-up AT+MTATTR read
     * answers +MTERR:5 for the same attribute, an inconsistency the C6 does
     * not have - attr_val_to_i64()'s nullable arms there return false for a
     * null value, so main.cpp's app_attribute_update_cb() never builds a URC
     * line for one, see attr_val_to_i64() and its callers in main.cpp).
     * emberAfLocateAttributeMetadata() is a plain table lookup (no I/O, no
     * lock of its own), safe to call here under whatever lock the caller
     * already holds (this bridge's own StackLock for a local write, or
     * CHIP's own lock for a controller-driven one - the same lock
     * discipline this whole file follows). Shares attr_null_sentinel() with
     * mt_matter_attr_read() so the two null tests cannot drift apart.
     */
    const EmberAfAttributeMetadata *md =
        emberAfLocateAttributeMetadata(path.mEndpointId, path.mClusterId, path.mAttributeId);
    if (md != nullptr && md->IsNullable() && raw == attr_null_sentinel(is_unsigned, bytes)) {
        return;
    }

    /*
     * Fix round 2, C1: the BooleanState bridge (Contact/Rain/Water Freeze/
     * Water Leak). BooleanState is one of the clusters CHIP serves through
     * the newer registered ServerClusterInterface path
     * (src/app/clusters/boolean-state-server/): CodegenDataModelProvider_
     * Read.cpp:116 checks that registry before ember's external-storage
     * fallback is ever consulted, so a real controller's read of
     * StateValue was answering from the registered BooleanStateCluster
     * object's own mStateValue, not this bridge's arena - bench-confirmed
     * (an AT write of StateValue=1 fired the URC below, but chip-tool
     * still read FALSE) and mechanically confirmed against the tree cited
     * above. This block bridges the two: BooleanState::
     * FindClusterOnEndpoint() (CodegenIntegration.h) returns the very
     * object emberAfSetDynamicEndpoint()'s init dispatch already
     * constructed for this endpoint (see the comment on booleanStateAttrs
     * in mt_devtypes_zephyr.cpp for why that construction happens at all),
     * and SetStateValue() pushes this write into it AND emits the
     * StateChange event (boolean-state-cluster.cpp:57-64) - strictly
     * better than an ember-only fix, since a bare ember write never emits
     * that event.
     *
     * No recursion risk. SetStateValue() only touches the object's own
     * mStateValue plus the event/reporting path; it never calls back into
     * emberAfWriteAttribute, so it cannot re-enter this callback. And a
     * controller's own IM write to StateValue never reaches this function
     * in the first place: StateValue is read-only in the Matter sense and
     * BooleanStateCluster does not override WriteAttribute, so the
     * registry rejects the write before ember's external-storage path
     * (and this callback) is ever reached. The only writer that reaches
     * here for this attribute is this bridge's own AT+MTATTR path.
     */
    if (path.mClusterId == chip::app::Clusters::BooleanState::Id &&
        path.mAttributeId == chip::app::Clusters::BooleanState::Attributes::StateValue::Id) {
        auto *booleanState = chip::app::Clusters::BooleanState::FindClusterOnEndpoint(path.mEndpointId);
        if (booleanState != nullptr) {
            booleanState->SetStateValue(raw != 0);
        }
    }

    char line[64];
    if (is_unsigned) {
        snprintf(line, sizeof(line), "+MTATTR:%u,%lu,%lu,%llu", path.mEndpointId,
                 (unsigned long)path.mClusterId, (unsigned long)path.mAttributeId,
                 (unsigned long long)raw);
    } else {
        int64_t sv = (int64_t)(raw << (64 - 8 * bytes)) >> (64 - 8 * bytes);
        snprintf(line, sizeof(line), "+MTATTR:%u,%lu,%lu,%lld", path.mEndpointId,
                 (unsigned long)path.mClusterId, (unsigned long)path.mAttributeId,
                 (long long)sv);
    }
    mt_at_urc(line);
}

/*
 * Catalogue batch 2, the air quality sensor (0x002C). Until this batch this
 * function was a stub in mt_matter_stub.c returning 0, because no device
 * type on this platform carried the AirQuality cluster.
 *
 * mt_matter.h's contract for it: one accessor, two callers, so the ember
 * feature map and a server Instance's BitMask<Feature> cannot be edited
 * apart. On the C6 those two callers are mk_air_quality_sensor() and
 * mt_air_quality_register_all(). On this platform only the first exists:
 * mt_devtypes_zephyr.cpp's seed_slots() fills the AirQuality FeatureMap slot
 * from this value, and nothing constructs an AirQuality::Instance (see the
 * audit note on airQualityAttrs for why - with none registered, ember serves
 * the arena). If a later round adds one, it reads its BitMask from here and
 * the two agree by construction.
 *
 * All four optional features are enabled. The base cluster only mandates
 * Unknown/Good/Poor; Fair, Moderate, VeryPoor and ExtremelyPoor are each
 * behind their own bit, and the host library's AirQuality_t publishes all
 * seven SDK values, so anything less would let the library report a value
 * this endpoint's feature map does not admit. Values are the CHIP enum's own
 * (clusters/AirQuality/Enums.h:49-55), never transcribed literals.
 */
extern "C" uint32_t mt_air_quality_feature_mask(void)
{
    using chip::app::Clusters::AirQuality::Feature;
    return chip::to_underlying(Feature::kFair) | chip::to_underlying(Feature::kModerate) |
           chip::to_underlying(Feature::kVeryPoor) | chip::to_underlying(Feature::kExtremelyPoor);
}

/*
 * ============ catalogue batch 3: the command verdict frame ==============
 *
 * The door lock (0x000A) and the water valve (0x0042) are the first device
 * types on this platform whose commands need an APPLICATION verdict, and
 * this section is where that verdict is fetched. Everything about the
 * verdict protocol itself, the sequence numbers, the 1000 ms window, the
 * default-deny, the +MTCMDTO URC, belongs to core (mt_at.h, mt_cmdbox.h)
 * and is identical on both platforms; nothing here reimplements any of it.
 * All this code does is call mt_cmd_forward() from the right SDK hook and
 * translate the boolean it returns into whatever that hook's caller expects.
 *
 * Both types forward the PAYLOAD-LESS form. AT_MT_SPEC.md 3.17 registers
 * the door lock as cluster 257 commands 0/1 and the valve as cluster 129
 * commands 0/1, with no trailing fields on either, so mt_cmd_forward() is
 * the right entry point and neither mt_cmd_forward_payload() nor
 * mt_cmd_forward_fields() is involved.
 *
 * NO StackLock in the command hooks, unlike every mt_matter_* bridge
 * function in this file. They already run ON the CHIP event-loop task: the
 * lock's two are ember command callbacks and the valve's two are Delegate
 * methods the SDK calls synchronously from that same task.
 * mt_cmd_forward() takes no lock of its own either, for the same reason,
 * and mt_at.c's AT+MTCMDRESP handler must never take the stack lock on the
 * reply path (see its comment) or the two would deadlock. The AT+MTLOCK
 * and AT+MTVALVE bridges below DO take it, because those are called from
 * the AT parser thread like every other bridge function here.
 */

/* ---- door lock (0x000A) ----------------------------------------------- */

/*
 * One helper for both commands, differing only in the command id forwarded,
 * mirroring the C6's mt_door_lock_adjudicate() exactly.
 *
 * The verdict-to-status mapping is the SDK's, not a choice made here:
 * HandleRemoteLockOperation() answers the controller
 * `success ? Status::Success : Status::Failure` (door-lock-server.cpp:3726)
 * straight off this function's return value, and reads `err` only to
 * annotate the LockOperationError event it emits on the failure path
 * (:3762-3765). So an allow is Success and a deny is Failure on the wire,
 * with an event carrying the reason.
 *
 * kUnspecified is the reason on every deny, because mt_cmd_forward()'s
 * false is deliberately undifferentiated: it covers an explicit host deny,
 * a missed 1000 ms window, the AT link not being up, and no host callback
 * registered. A lock fails closed, and the firmware has no honest way to
 * tell a controller which of those four it was. Not kInvalidCredential in
 * particular: that value additionally triggers HandleWrongCodeEntry()
 * (:3716) and would count a host deny towards the wrong-code lockout, which
 * would be a lie about what happened.
 */
static bool mt_door_lock_adjudicate(chip::EndpointId endpointId, uint32_t command,
                                    OperationErrorEnum &err)
{
    if (mt_cmd_forward(endpointId, chip::app::Clusters::DoorLock::Id, command)) {
        return true;
    }
    err = OperationErrorEnum::kUnspecified;
    return false;
}

/*
 * Strong overrides of the weak defaults in door-lock-server-callback.cpp
 * (:46 and :55). Signatures verbatim from door-lock-server.h:1112-1114 and
 * :1128-1130; note the argument order, fabricIdx and nodeId BEFORE the PIN,
 * and that pinCode is an Optional, not a Nullable.
 *
 * fabricIdx/nodeId/pinCode go unused. With FeatureMap 0 this endpoint
 * advertises no credential feature at all, so a PIN-carrying invoke is
 * refused by the server before the app is consulted, and fabricIdx/nodeId
 * are event-annotation fields with no adjudication use: the host is told
 * which endpoint and which command, which is what the +MTCMD frame carries.
 */
bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId endpointId,
                                            const Nullable<chip::FabricIndex> &fabricIdx,
                                            const Nullable<chip::NodeId> &nodeId,
                                            const Optional<chip::ByteSpan> &pinCode,
                                            OperationErrorEnum &err)
{
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    return mt_door_lock_adjudicate(endpointId, chip::app::Clusters::DoorLock::Commands::LockDoor::Id,
                                   err);
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId endpointId,
                                              const Nullable<chip::FabricIndex> &fabricIdx,
                                              const Nullable<chip::NodeId> &nodeId,
                                              const Optional<chip::ByteSpan> &pinCode,
                                              OperationErrorEnum &err)
{
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    return mt_door_lock_adjudicate(endpointId,
                                   chip::app::Clusters::DoorLock::Commands::UnlockDoor::Id, err);
}

/*
 * The lock's per-endpoint init hook, emberAfDoorLockClusterInitCallback(),
 * is deliberately NOT here. It lives in mt_devtypes_zephyr.cpp, next to the
 * door lock's device type declaration, because its failure has to abort
 * mt_devtype_create() and that file owns the create. See the comment on the
 * override there for the InitEndpoint-versus-InitServer reasoning.
 */

/*
 * AT+MTLOCK bridge (AT_MT_SPEC.md 3.18). Reports the host's OWN actuation
 * as LockState, through the source-taking SetLockState() overload so the
 * LockOperation event a controller subscribes to is actually emitted; the
 * 2-arg overload (door-lock-server.h:160) writes the attribute silently and
 * is deliberately not used. mt_matter.h calls this the 6-arg form after the
 * C6's SDK; in this tree it is 7-arg with defaults
 * (door-lock-server.h:144-148), same call.
 *
 * The firmware never calls this itself after an allowed +MTCMD verdict:
 * actuation timing belongs to the host, which calls AT+MTLOCK once its own
 * mechanism confirms the bolt moved.
 */
extern "C" int mt_matter_lock_state_set(uint16_t ep, uint8_t state, uint8_t source)
{
    chip::DeviceLayer::StackLock lock;
    /* Same two lookups, in the same order, that attr_locate() above uses to
     * separate MT_ATTR_ERR_ENDPOINT from MT_ATTR_ERR_CLUSTER, so AT+MTLOCK's
     * error division (AT_MT_SPEC.md 3.18) matches AT+MTATTR's by
     * construction rather than by two hand-written lookups agreeing. */
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::DoorLock::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    bool ok = DoorLockServer::Instance().SetLockState(ep, (DlLockState)state,
                                                      (OperationSourceEnum)source);
    return ok ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
}

extern "C" uint8_t mt_matter_lock_source_manual(void)
{
    return (uint8_t)OperationSourceEnum::kManual;
}

extern "C" uint8_t mt_matter_lock_source_max(void)
{
    /* kUnknownEnumValue is one past the last real source and is not itself
     * a valid one to report (mt_matter.h); kAliro is the highest that is.
     * Read from the pinned header at call time rather than transcribed, so
     * an SDK that grows the enum grows AT+MTLOCK's accepted range with it.
     * This tree agrees with the C6's: AT_MT_SPEC.md 3.18's documented
     * 0..10 range holds on both. */
    return (uint8_t)OperationSourceEnum::kAliro;
}

/* ---- water valve (0x0042) ---------------------------------------------
 *
 * ValveConfigurationAndControl::Delegate (delegate.h:34) has exactly three
 * pure virtuals (valve-configuration-and-control-delegate.h:40-42):
 *
 *   virtual DataModel::Nullable<chip::Percent> HandleOpenValve(DataModel::Nullable<chip::Percent> level) = 0;
 *   virtual CHIP_ERROR HandleCloseValve()                                                                = 0;
 *   virtual void HandleRemainingDurationTick(uint32_t duration)                                          = 0;
 *
 * The verdict cannot fail either command on the wire, and this is an SDK
 * property rather than a firmware choice (AT_MT_SPEC.md 3.19). Traced in
 * this tree, not assumed from the C6's note: CloseValve() calls
 * HandleCloseValve() (cluster.cpp:303) and neither assigns, tests nor logs
 * the CHIP_ERROR it returns, then returns CHIP_NO_ERROR unconditionally
 * (:306), so the Close command handler's Failure arm (:520-522) is
 * unreachable through the delegate. Open is worse: HandleOpenValve() has no
 * error channel at all, since it returns a LEVEL rather than a status, and
 * SetValveLevel() reads that level only to decide whether to republish
 * CurrentLevel (:361-365) before returning CHIP_NO_ERROR (:370); the Open
 * handler's status Optional (:440) is only ever filled from argument
 * constraint checks (:451, :456, :462, :483), never from anything the
 * delegate did.
 *
 * So the verdict here gates ONE thing: whether the host actually moves the
 * valve. That is still the whole point of forwarding it, which is why both
 * hooks forward anyway.
 *
 * One caller of HandleCloseValve() is NOT a controller invoke: the server's
 * own auto-close timer re-enters it when a timed open expires, so the host
 * sees an unsolicited 129/1 forward. Fix round I1; the full mechanism and
 * why it is documented rather than fixed are on the water valve's audit
 * note in mt_devtypes_zephyr.cpp, and the host-facing consequence is in the
 * platform README.
 */
class HearthValveDelegate : public chip::app::Clusters::ValveConfigurationAndControl::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }

    /*
     * Return the level asked for on allow (the caller republishes it as
     * CurrentLevel when the Level feature is present, which it is not on
     * this build), and NullNullable on deny: "no level to report", which is
     * the nearest thing to a refusal this signature can express. Not an
     * error, because there is no error to express.
     */
    chip::app::DataModel::Nullable<chip::Percent> HandleOpenValve(
        chip::app::DataModel::Nullable<chip::Percent> level) override
    {
        if (mt_cmd_forward(m_ep, chip::app::Clusters::ValveConfigurationAndControl::Id,
                           chip::app::Clusters::ValveConfigurationAndControl::Commands::Open::Id)) {
            return level;
        }
        return chip::app::DataModel::NullNullable;
    }

    /*
     * Forward for adjudication and answer CHIP_NO_ERROR regardless: the
     * caller discards this return (see the section comment), so returning
     * an error on deny would change nothing on the wire while making this
     * function's contract with its caller a lie.
     */
    CHIP_ERROR HandleCloseValve() override
    {
        mt_cmd_forward(m_ep, chip::app::Clusters::ValveConfigurationAndControl::Id,
                       chip::app::Clusters::ValveConfigurationAndControl::Commands::Close::Id);
        return CHIP_NO_ERROR;
    }

    /*
     * Fires once a second from a SystemLayer timer on the CHIP event loop
     * while a timed open counts down (cluster.cpp:214, :235, :245).
     * Nothing in this firmware's AT surface exposes that countdown as an
     * event to forward, so there is nothing to do; RemainingDuration itself
     * still ticks in the server's shadow store and is what a subscribed
     * controller reads (see the seed note in mt_devtypes_zephyr.cpp).
     *
     * THIS FUNCTION must stay non-blocking: it runs on the event loop once
     * per second, and a mt_cmd_forward() from here would stall the stack
     * for up to 1000 ms on every tick.
     *
     * That rule is about this function only, and it is worth being exact,
     * because the TERMINAL tick does reach mt_cmd_forward() anyway, just
     * not through here. At zero the tick does not call this callback at
     * all: startRemainingDurationTick() takes the else branch and calls
     * CloseValve() (:250-252), which calls HandleCloseValve() (:303), which
     * forwards and blocks. So the auto-close stall exists, it is one
     * blocking forward per timed open rather than one per second, and it is
     * the SDK's structure rather than something this class chooses.
     * Documented, not fixed; fix round I1, full mechanism on the water
     * valve's audit note in mt_devtypes_zephyr.cpp.
     */
    void HandleRemainingDurationTick(uint32_t duration) override { (void)duration; }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/*
 * Pool of delegate objects, handed out in composition order by
 * mt_devtype_create() (mt_devtypes_zephyr.cpp). Static rather than heap:
 * the composition rebuild runs once at boot and these must outlive the
 * device, so a fixed array is both simpler and cheaper than a block per
 * object.
 *
 * Sized kServiceableEndpoints, not MT_COMP_MAX_ENDPOINTS. mt_matter.h's
 * contract names the latter because it was written for the C6, which
 * serves all 28; this port serves 16 (mt_port_ids.h, acceptance versus
 * capacity), so a seventeenth endpoint of ANY device type fails its create
 * before it could ask for a delegate. Sizing for 28 would reserve twelve
 * slots no composition on this part can reach. The exhaustion behaviour
 * the contract specifies is unchanged: nullptr, and the caller aborts the
 * boot rebuild.
 */
static HearthValveDelegate s_valve_delegates[kServiceableEndpoints];
static size_t s_valve_delegate_next;

extern "C" void *mt_matter_valve_delegate_alloc(void)
{
    if (s_valve_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    return &s_valve_delegates[s_valve_delegate_next++];
}

/*
 * Fixes the slot's endpoint AND registers it with the cluster server. Both
 * halves belong here because both need the endpoint id, which is not known
 * until emberAfSetDynamicEndpoint() has succeeded; mt_devtype_create()'s
 * comment has the full ordering argument and why it differs from the C6's.
 *
 * SetDefaultDelegate() returns void and drops the registration silently if
 * the endpoint index does not resolve (cluster.cpp:264-269: a lone
 * `if (ep < table size)` with no else and no log). A valve in that state
 * still answers Open and Close with Success and still looks healthy to a
 * controller, while no +MTCMD is ever raised and the server's auto-close
 * tick never runs. So the registration is read back rather than trusted,
 * and a failure is logged loudly at the moment it happens. It does not
 * abort the create; mt_devtype_create()'s own comment says why that is
 * consistent with the pool check aborting rather than at odds with it.
 */
extern "C" void mt_matter_valve_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthValveDelegate *>(delegate);
    d->set_endpoint(ep);
    chip::app::Clusters::ValveConfigurationAndControl::SetDefaultDelegate(ep, d);
    if (chip::app::Clusters::ValveConfigurationAndControl::GetDefaultDelegate(ep) != d) {
        LOG_ERR("valve delegate registration dropped for endpoint %u: commands on it will not "
                "reach the host and auto-close will not run",
                (unsigned)ep);
    }
}

/*
 * AT+MTVALVE bridge (AT_MT_SPEC.md 3.19). Reports the host's own actuation
 * through the cluster's own UpdateCurrentState()/UpdateCurrentLevel() calls
 * rather than a raw attribute write, so the ValveStateChanged event a
 * subscribed controller expects is emitted. Same split as AT+MTLOCK, and
 * doubly so here: the verdict never reached the wire anyway, so only the
 * host can say the valve moved.
 *
 * level == -1 means absent; mt_at.c has already validated 0..100 for any
 * value it passes through. As on the C6, <level> publishes nowhere on this
 * build: FeatureMap is 0, so CurrentLevel is not a declared attribute and
 * UpdateCurrentLevel() checks the feature before touching it and returns
 * success either way. The argument is accepted and does nothing observable,
 * which is exactly what AT_MT_SPEC.md 3.19 documents.
 */
extern "C" int mt_matter_valve_state_set(uint16_t ep, uint8_t state, int level)
{
    chip::DeviceLayer::StackLock lock;
    namespace Valve = chip::app::Clusters::ValveConfigurationAndControl;
    /* attr_locate()'s two lookups again, for AT_MT_SPEC.md 3.19's error
     * division; see the note in mt_matter_lock_state_set() above. */
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, Valve::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    CHIP_ERROR err = Valve::UpdateCurrentState(ep, (Valve::ValveStateEnum)state);
    if (err != CHIP_NO_ERROR) {
        return MT_ATTR_ERR_FAILED;
    }
    if (level >= 0) {
        err = Valve::UpdateCurrentLevel(ep, (chip::Percent)level);
        if (err != CHIP_NO_ERROR) {
            return MT_ATTR_ERR_FAILED;
        }
    }
    return MT_ATTR_OK;
}

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
 * cluster the endpoint actually has. On THIS platform only SmokeCoAlarm
 * exists so far: RefrigeratorAlarm arrives with the composed-appliance
 * types in a later batch, and until then an endpoint carrying neither
 * answers MT_ATTR_ERR_CLUSTER through the single fall-through below, the
 * same code the C6 answers. The dispatch shape is kept so that batch adds
 * a branch rather than restructuring this function.
 *
 * Field 0 (ExpressedState) is derived by the server from the other fields
 * and never settable directly; rejected here rather than in mt_at.c because
 * field 0 is a legal RefrigeratorAlarm bit on the C6 and the union bound
 * must stay shared (core/include/mt_matter.h:663-669, binding).
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

    /* RefrigeratorAlarm is not in this platform's catalogue yet; when the
     * composed-appliance batch brings it, its branch lands here. */
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

/*
 * The delegate pool, kServiceableEndpoints deep like the valve's (a
 * seventeenth endpoint of ANY type fails its create before it could ask
 * for a delegate; mt_matter.h's MT_COMP_MAX_ENDPOINTS sizing is the C6's,
 * which serves all 28). The INSTANCE pool beside it is raw aligned
 * storage: OperationalState::Instance has no default constructor (it
 * takes the delegate and the endpoint id, neither known until create
 * time), so each slot is placement-constructed exactly once in
 * mt_matter_opstate_delegate_set_endpoint() and never destroyed, the
 * allocate-only policy the valve pool and the endpoint block heap already
 * follow: pools are handed out monotonically by the one boot rebuild and a
 * reboot resets them wholesale, so no slot is ever re-constructed over a
 * live Instance and no explicit destructor call is needed (or made; see
 * the opStateAttrs audit note for what ~Instance() would do).
 */
static HearthOpStateDelegate s_opstate_delegates[kServiceableEndpoints];
alignas(chip::app::Clusters::OperationalState::Instance) static uint8_t
    s_opstate_instances[kServiceableEndpoints][sizeof(chip::app::Clusters::OperationalState::Instance)];
static size_t s_opstate_delegate_next;

extern "C" void *mt_matter_opstate_delegate_alloc(uint32_t cluster_id)
{
    if (s_opstate_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthOpStateDelegate *d = &s_opstate_delegates[s_opstate_delegate_next++];
    d->set_cluster(cluster_id);
    return d;
}

/*
 * The second half of the handout, and on this platform it is where the
 * Instance is born: constructed into the slot's raw storage (the pool
 * index is recovered from the delegate pointer, the two pools being
 * parallel) and Init()ed, which registers the endpoint-scoped
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
extern "C" void mt_matter_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthOpStateDelegate *>(delegate);
    d->set_endpoint(ep);
    size_t idx = (size_t)(d - s_opstate_delegates);
    auto *inst = new (s_opstate_instances[idx])
        chip::app::Clusters::OperationalState::Instance(d, ep);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("opstate Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; commands on it will not reach the host and its attributes will not be served",
                (unsigned)ep, err.Format());
    }
}

/*
 * AT+MTOPSTATE bridge. mt_at.c's cmd_mtopstate has already rejected
 * anything outside the UNION of every opstate cluster family's legal set
 * (and 3, kError, outright) before this is called; this bridge narrows to
 * the one family this catalogue serves, the base cluster's {0 Stopped,
 * 1 Running, 2 Paused}. A union-legal RVC state (0x40..0x42) on a washer
 * answers MT_ATTR_ERR_VALUE here, exactly as the C6's base branch does;
 * the RVC and oven-cavity branches arrive with their device types in later
 * batches. SetOperationalState() enforces the same rule independently
 * (operational-state-server.cpp:101-107, kError or an unsupported state
 * answers CHIP_ERROR_INVALID_ARGUMENT), so a state that slipped both
 * checks would still map to MT_ATTR_ERR_FAILED rather than being silently
 * accepted.
 *
 * Called from the AT parser thread, so the StackLock IS taken, unlike the
 * delegate hooks above.
 */
extern "C" int mt_matter_opstate_set(uint16_t ep, uint8_t state)
{
    namespace OpState = chip::app::Clusters::OperationalState;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, OpState::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (state > 2) {
        return MT_ATTR_ERR_VALUE;
    }

    OpState::Instance *inst = nullptr;
    for (auto &d : s_opstate_delegates) {
        if (d.endpoint() == ep && d.cluster() == OpState::Id) {
            inst = d.instance();
            break;
        }
    }
    if (inst == nullptr) {
        /* Cannot happen once the boot rebuild has run: every endpoint
         * carrying the cluster got its pair in mt_devtype_create().
         * Defensive, the C6's same arm. */
        return MT_ATTR_ERR_FAILED;
    }
    return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
}

/* ---- mode select (0x0027) ---------------------------------------------
 *
 * ONE process-global manager for the whole device, not a pool: the SDK
 * fetches it fresh through getSupportedModesManager() on every
 * SupportedModes read and every ChangeToMode
 * (mode-select-server.cpp:84, :123, :469), and setSupportedModesManager()
 * (:62) stores a bare global pointer, so a second mode select endpoint
 * re-registering it is harmless. The manager dispatches on endpoint id
 * itself against the host-fed store below.
 *
 * The store is kServiceableEndpoints (16) deep, NOT MT_COMP_MAX_ENDPOINTS
 * (28): a seventeenth endpoint of any type fails its create before
 * AT+MTMODES could ever address it, the acceptance-versus-capacity split
 * every pool in this file follows.
 *
 * CharSpan lifetime and the in-place rebuild, the C6's argument re-checked
 * against THIS tree: both the label bytes and the ModeOptionStruct array
 * live in static storage (s_mode_slots), never freed, and the struct
 * array is rebuilt IN PLACE on every AT+MTMODES write to the same slot,
 * never reallocated. What rules out a reader observing a half-rebuilt
 * entry is mutual exclusion, not the rebuild order:
 * mt_matter_modes_set() holds the StackLock for the whole rebuild, and
 * the reads run on the CHIP task under the same lock. The SDK also
 * re-fetches the provider per access rather than caching one, which
 * rules out a stale provider outliving a rebuild; secondary, the lock is
 * the guarantee.
 */
struct mt_mode_entry_t {
    uint8_t mode;
    char label[MT_MODES_MAX_LABEL_LEN + 1];
};

struct mt_mode_slot_t {
    bool used;
    uint16_t ep;
    uint8_t count;
    mt_mode_entry_t entries[MT_MODES_MAX_COUNT];
    chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type structs[MT_MODES_MAX_COUNT];
};
static mt_mode_slot_t s_mode_slots[kServiceableEndpoints];

class HearthSupportedModesManager : public chip::app::Clusters::ModeSelect::SupportedModesManager
{
private:
    /*
     * SupportedModesManager declares its ModeOptionStructType alias BEFORE
     * its "public:" (supported-modes-manager.h:36), so the name is private
     * to the base class even though a public virtual's signature uses it:
     * a derived class inherits the member but not access to the name. The
     * same private-alias shadow the esp32 platform's own
     * StaticSupportedModesManager uses, mirrored from the C6.
     */
    using ModeOptionStructType = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;

public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId endpointId) const override
    {
        for (auto &slot : s_mode_slots) {
            if (slot.used && slot.ep == endpointId) {
                return ModeOptionsProvider(slot.structs, slot.structs + slot.count);
            }
        }
        /* begin == end == nullptr: no entry for this endpoint, an empty
         * SupportedModes list until the host feeds one. */
        return ModeOptionsProvider();
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(
        chip::EndpointId endpointId, uint8_t mode, const ModeOptionStructType **dataPtr) const override
    {
        for (auto &slot : s_mode_slots) {
            if (slot.used && slot.ep == endpointId) {
                for (uint8_t i = 0; i < slot.count; i++) {
                    if (slot.structs[i].mode == mode) {
                        *dataPtr = &slot.structs[i];
                        return chip::Protocols::InteractionModel::Status::Success;
                    }
                }
                return chip::Protocols::InteractionModel::Status::InvalidCommand;
            }
        }
        return chip::Protocols::InteractionModel::Status::UnsupportedCluster;
    }
};
static HearthSupportedModesManager s_mode_select_manager;

/*
 * The accessor doubles as the registration: on this platform there is no
 * esp_matter init-callback pass to defer to, so the global is (re)set here,
 * each call, before mt_devtype_create() spends anything on the endpoint.
 * Idempotent by construction (same pointer every time), and safe at any
 * point in boot: setSupportedModesManager() is a bare pointer store with
 * no CHIP state behind it.
 */
extern "C" void *mt_matter_mode_select_manager(void)
{
    chip::app::Clusters::ModeSelect::setSupportedModesManager(&s_mode_select_manager);
    return &s_mode_select_manager;
}

/*
 * AT+MTMODES bridge (the ModeSelect form). Grammar and content rules are
 * enforced by cmd_mtmodes() in mt_at.c before this is called; the bounds
 * re-checked here are defensive. Mirrors the C6's
 * mt_matter_modes_set() with the ember lookups in place of esp_matter's
 * and the same terminal MatterReportingAttributeChangeCallback() so an
 * active subscription sees the new list; CurrentMode is deliberately not
 * touched here (a controller's ChangeToMode owns it, and the host
 * observes that as a +MTATTR URC).
 */
extern "C" int mt_matter_modes_set(uint16_t ep, const uint8_t *modes, const char *const *labels,
                                   uint8_t count)
{
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::ModeSelect::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_MODES_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_mode_slot_t *slot = nullptr;
    for (auto &sl : s_mode_slots) {
        if (sl.used && sl.ep == ep) {
            slot = &sl;
            break;
        }
    }
    if (!slot) {
        for (auto &sl : s_mode_slots) {
            if (!sl.used) {
                slot = &sl;
                break;
            }
        }
    }
    if (!slot) {
        /* Cannot happen: one slot per serviceable endpoint, and an
         * unserved endpoint fails the lookups above first. Defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_MODES_MAX_LABEL_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
        slot->entries[i].mode = modes[i];
        memcpy(slot->entries[i].label, labels[i], len + 1);
    }
    slot->ep = ep;
    slot->count = count;
    slot->used = true;

    /* Rebuild the struct array in place, each CharSpan pointing into the
     * static label buffer above; the lock held across this whole function
     * is what makes the rebuild atomic against CHIP-task readers (see the
     * section comment). SemanticTags publishes a default-constructed
     * empty List per entry, mandatory-but-empty-legal. */
    for (uint8_t i = 0; i < count; i++) {
        slot->structs[i].mode = slot->entries[i].mode;
        slot->structs[i].label = chip::CharSpan::fromCharString(slot->entries[i].label);
        slot->structs[i].semanticTags = chip::app::DataModel::List<
            const chip::app::Clusters::ModeSelect::Structs::SemanticTagStruct::Type>();
    }

    MatterReportingAttributeChangeCallback(
        ep, chip::app::Clusters::ModeSelect::Id,
        chip::app::Clusters::ModeSelect::Attributes::SupportedModes::Id);
    return MT_ATTR_OK;
}

/* ---- chime (0x0146) ---------------------------------------------------
 *
 * Per-endpoint ChimeServer plus ChimeDelegate; the composition-side audit
 * (CHI-only, no ServerInit, the AAI-shadowed KVS-persisted pair, the
 * missing NotFound pre-check and event) is on chimeAttrs in
 * mt_devtypes_zephyr.cpp. This section owns the objects and the two AT
 * bridges.
 *
 * The verdict is the one on this firmware's whole +MTCMD surface that
 * reaches the controller exactly as given: HandlePlayChimeSound() copies
 * whatever Status the delegate returns straight into the command's
 * response (chime-server.cpp:260-274), with no SDK-side remapping.
 * Status::Success on allow, Status::Failure on deny.
 *
 * The payload, DECIDED (user ruling DE396): this tree's PlayChimeSound()
 * takes NO argument (chime-server.h:164; the command XML declares none),
 * so the C6's invoke-supplied chimeID does not exist to forward. The
 * single trailing +MTCMD field instead carries the owning server's
 * GetSelectedChime(): the wire arity stays byte-identical to the C6 and
 * the payload means "the chime id that will play", which is also what the
 * server will play by its own rules. The server pointer comes from the
 * base class's own back-reference (the ctor calls SetChimeServer(this),
 * chime-server.cpp:46, readable through the protected GetChimeServer(),
 * chime-server.h:173), the OperationalState GetInstance() mechanism under
 * another name.
 */
struct mt_chime_entry_t {
    uint8_t id;
    char name[MT_CHIME_MAX_NAME_LEN + 1];
};

struct mt_chime_slot_t {
    bool used;
    uint16_t ep;
    uint8_t count;
    mt_chime_entry_t entries[MT_CHIME_MAX_SOUNDS];
};
/* kServiceableEndpoints deep, not MT_COMP_MAX_ENDPOINTS: the same
 * acceptance-versus-capacity split as every store and pool in this file.
 * At 16 endpoints of 8 names of 33 bytes this is the batch's largest new
 * .bss item, about 4.3 KB. */
static mt_chime_slot_t s_chime_slots[kServiceableEndpoints];

static mt_chime_slot_t *mt_chime_find_slot(uint16_t ep)
{
    for (auto &sl : s_chime_slots) {
        if (sl.used && sl.ep == ep) {
            return &sl;
        }
    }
    return nullptr;
}

class HearthChimeDelegate : public chip::app::Clusters::ChimeDelegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    /* GetChimeServer() is protected in the base; this passthrough is how
     * the AT bridges below reach each endpoint's server object. */
    chip::app::Clusters::ChimeServer *server() { return GetChimeServer(); }

    CHIP_ERROR GetChimeSoundByIndex(uint8_t chimeIndex, uint8_t &chimeID,
                                    chip::MutableCharSpan &name) override
    {
        mt_chime_slot_t *slot = mt_chime_find_slot(m_ep);
        if (slot == nullptr || chimeIndex >= slot->count) {
            /* PROVIDER_LIST_EXHAUSTED, NOT the CHIP_ERROR_NOT_FOUND the
             * header's doc comment names (chime-server.h:137-138): the
             * implementation's two iteration loops terminate ONLY on
             * PROVIDER_LIST_EXHAUSTED (EncodeSupportedChimeSounds,
             * chime-server.cpp:148-151, and IsSupportedChimeID, :168).
             * Answering NOT_FOUND would fail every InstalledChimeSounds
             * read outright and, worse, spin IsSupportedChimeID forever
             * on an unmatched id (its uint8_t index wraps and nothing
             * else stops it), hanging the CHIP task. The code wins over
             * its comment; same sentinel the C6 returns. */
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = slot->entries[chimeIndex].id;
        chip::CharSpan src(slot->entries[chimeIndex].name, strlen(slot->entries[chimeIndex].name));
        return chip::CopyCharSpanToMutableCharSpan(src, name);
    }

    CHIP_ERROR GetChimeIDByIndex(uint8_t chimeIndex, uint8_t &chimeID) override
    {
        mt_chime_slot_t *slot = mt_chime_find_slot(m_ep);
        if (slot == nullptr || chimeIndex >= slot->count) {
            /* Same sentinel note as GetChimeSoundByIndex above. */
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = slot->entries[chimeIndex].id;
        return CHIP_NO_ERROR;
    }

    /* Only reached when Enabled is true (chime-server.cpp:266-271; a
     * disabled chime answers Success with no delegate call and no
     * +MTCMD). Runs on the CHIP task, no StackLock. */
    chip::Protocols::InteractionModel::Status PlayChimeSound() override
    {
        using chip::Protocols::InteractionModel::Status;
        chip::app::Clusters::ChimeServer *srv = GetChimeServer();
        if (srv == nullptr) {
            /* Unreachable by construction (the server's ctor set the
             * back-reference before any command could dispatch);
             * defensive, and failing closed beats forwarding a payload
             * this delegate cannot know. */
            return Status::Failure;
        }
        bool allow = mt_cmd_forward_payload(m_ep, chip::app::Clusters::Chime::Id,
                                            chip::app::Clusters::Chime::Commands::PlayChimeSound::Id,
                                            srv->GetSelectedChime());
        return allow ? Status::Success : Status::Failure;
    }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/* Delegate pool plus raw storage for the non-default-constructible
 * ChimeServer, parallel arrays indexed together, the OperationalState
 * pools' exact shape and the same construct-once, never-destroy policy
 * (chime's ~ChimeServer would unregister both interfaces,
 * chime-server.cpp:49-57, and no teardown path exists on this
 * platform). */
static HearthChimeDelegate s_chime_delegates[kServiceableEndpoints];
alignas(chip::app::Clusters::ChimeServer) static uint8_t
    s_chime_servers[kServiceableEndpoints][sizeof(chip::app::Clusters::ChimeServer)];
static size_t s_chime_delegate_next;

extern "C" void *mt_matter_chime_delegate_alloc(void)
{
    if (s_chime_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    return &s_chime_delegates[s_chime_delegate_next++];
}

/*
 * The second half: placement-constructs the ChimeServer (its ctor stores
 * the delegate reference and sets the delegate's back-reference) and runs
 * Init(), which loads the two KVS-persisted attributes and registers the
 * endpoint-scoped AAI and command handler (chime-server.cpp:59-66). Below
 * a successful emberAfSetDynamicEndpoint() by the same reasoning as the
 * OperationalState pool; an Init() failure is logged loudly and does not
 * abort, the same unreachable-by-ordering argument.
 */
extern "C" void mt_matter_chime_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthChimeDelegate *>(delegate);
    d->set_endpoint(ep);
    size_t idx = (size_t)(d - s_chime_delegates);
    auto *srv = new (s_chime_servers[idx]) chip::app::Clusters::ChimeServer(ep, *d);
    CHIP_ERROR err = srv->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("ChimeServer::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; the chime cluster on it will not be served",
                (unsigned)ep, err.Format());
    }
}

/*
 * AT+MTCHIMESOUNDS bridge. Grammar and content rules are enforced by
 * cmd_mtchimesounds() in mt_at.c; the bounds re-checked here are
 * defensive. Ends by marking InstalledChimeSounds dirty with
 * MatterReportingAttributeChangeCallback(), and NOT with the SDK's
 * ReportInstalledChimeSoundsChange(): that method is DECLARED on
 * ChimeServer (chime-server.h:92) and its own doc comments demand
 * calling it, but it has NO DEFINITION anywhere in this tree (grep over
 * src/, examples/ and zzz_generated/ finds only the declaration and the
 * two doc mentions), so calling it is a link error. The same
 * documented-but-nonexistent hole the C6 recorded against ITS tree
 * (core/include/mt_matter.h's note), one revision later and one symbol
 * shape over; the substitute is the same one, and it works identically
 * here because the reporting engine's dirty-marking is not
 * ember-specific.
 */
extern "C" int mt_matter_chime_sounds_set(uint16_t ep, const uint8_t *ids,
                                          const char *const *names, uint8_t count)
{
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::Chime::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_CHIME_MAX_SOUNDS) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_chime_slot_t *slot = mt_chime_find_slot(ep);
    if (!slot) {
        for (auto &sl : s_chime_slots) {
            if (!sl.used) {
                slot = &sl;
                break;
            }
        }
    }
    if (!slot) {
        /* Cannot happen: one slot per serviceable endpoint. Defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(names[i]);
        if (len < 1 || len > MT_CHIME_MAX_NAME_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
        slot->entries[i].id = ids[i];
        memcpy(slot->entries[i].name, names[i], len + 1);
    }
    slot->ep = ep;
    slot->count = count;
    slot->used = true;

    MatterReportingAttributeChangeCallback(
        ep, chip::app::Clusters::Chime::Id,
        chip::app::Clusters::Chime::Attributes::InstalledChimeSounds::Id);
    return MT_ATTR_OK;
}

/*
 * AT+MTCHIME bridge. mt_at.c's cmd_mtchime has already checked <what> is
 * 0 or 1 and <value> fits a u8. Goes through the server object's own
 * SetSelectedChime()/SetEnabled(), never a raw attribute write: the
 * members are the served truth (the ember slots are inert shadows), and
 * the setters persist to KVS and mark the path dirty themselves
 * (chime-server.cpp:206-248). SetSelectedChime answers Status::NotFound
 * for an id AT+MTCHIMESOUNDS never installed, mapped to MT_ATTR_ERR_VALUE
 * (+MTERR:1), the C6's mapping.
 */
extern "C" int mt_matter_chime_set(uint16_t ep, uint8_t what, uint8_t value)
{
    using chip::Protocols::InteractionModel::Status;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::Chime::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }

    chip::app::Clusters::ChimeServer *srv = nullptr;
    for (auto &d : s_chime_delegates) {
        if (d.endpoint() == ep) {
            srv = d.server();
            break;
        }
    }
    if (srv == nullptr) {
        /* Cannot happen once the boot rebuild has run. Defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    Status st;
    switch (what) {
    case 0: /* SelectedChime */
        st = srv->SetSelectedChime(value);
        break;
    case 1: /* Enabled */
        st = srv->SetEnabled(value != 0);
        break;
    default:
        /* cmd_mtchime already rejects <what> outside 0..1. Defensive. */
        return MT_ATTR_ERR_VALUE;
    }
    return (st == Status::Success) ? MT_ATTR_OK : MT_ATTR_ERR_VALUE;
}
