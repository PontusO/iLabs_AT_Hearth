/*
 * mt_matter_zephyr.cpp - the mt_matter.h commissioning/state/network
 * slice against CHIP on Zephyr (Matter core spec section 6). The AT
 * parser thread calls these; anything touching CHIP state takes the
 * stack lock (the bridge_manager.cpp discipline).
 */

#include <app/ConcreteAttributePath.h>
#include <app/clusters/boolean-state-server/CodegenIntegration.h>
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
 */
static uint32_t s_live_devtype[MT_COMP_MAX_ENDPOINTS];
static uint16_t s_live_ep_id[MT_COMP_MAX_ENDPOINTS];
static uint8_t s_live_variant[MT_COMP_MAX_ENDPOINTS];
static uint8_t s_live_parent[MT_COMP_MAX_ENDPOINTS];
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
    if (s_live_count >= MT_COMP_MAX_ENDPOINTS) {
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
