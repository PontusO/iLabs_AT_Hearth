/*
 * mt_matter_sl.cpp - the mt_matter.h bridge against CHIP on Silicon Labs
 * (EFR32MG24 / MGM240P). The AT parser task calls these; anything touching
 * CHIP state takes the CHIP stack lock, and anything touching OpenThread
 * directly takes the OpenThread lock pair instead.
 *
 * The source is the nRF54L15 port, platform/nrf54l15/port/mt_matter_zephyr.cpp
 * in the firmware repository, transferred section by section so the two ports
 * stay diffable. Every section below names the nRF line range it came from;
 * a future round that changes one arm can find the other by that range.
 *
 * Sections present so far:
 *
 *   - commissioning state, network, Thread   nRF 388-602   (round 2 task 4)
 *   - the live endpoint table                nRF 604-663   (round 2 task 5)
 *   - the attribute bridge and +MTATTR       nRF 665-1505  (round 2 task 6)
 *
 * Everything else in mt_matter.h is still answered by port/mt_matter_stub.c,
 * and the linker plus test/host's check_decls.py prove between them that every
 * declaration has exactly one definition across the pair.
 *
 * The stack lock is not advisory here: SL_MATTER_STACK_LOCK_TRACKING_MODE is
 * SL_MATTER_STACK_LOCK_TRACKING_FATAL, so a CHIP call made from the AT parser
 * task without chip::DeviceLayer::StackLock kills the device rather than
 * racing quietly. That setting is NOT a file in this repository: it is the
 * Matter extension's own slc/config/sl_matter_config.h (where FATAL is also
 * the documented default), which slc copies into the generated project as
 * config/sl_matter_config.h.
 */

#include <app/ConcreteAttributePath.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <app/util/attribute-table.h>
#include <app/util/ember-io-storage.h>
/*
 * generic-callbacks.h is included where the nRF arm does not include it, and
 * deliberately: MatterPostAttributeChangeCallback below is a STRONG override
 * of the weak default in the SDK's generic-callback-stubs.cpp, and a strong
 * override whose signature has drifted does not fail to link, it simply stops
 * being called. Including the declaring header makes the compiler prove the
 * definition matches, which is cheaper than a bench session spent asking why
 * no +MTATTR URC appears.
 */
#include <app/util/generic-callbacks.h>
#include <lib/support/TypeTraits.h>
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

extern "C" {
#include "mt_at.h"
#include "mt_matter.h"
}

#include "hearth_log.h"
#include "mt_port_ids.h"

using chip::DeviceLayer::ConnectivityMgr;
using chip::DeviceLayer::ThreadStackMgr;
using chip::DeviceLayer::ThreadStackMgrImpl;

/* ---- commissioning state, network, Thread (nRF 388-602) ---------------- */

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
     * StackLock here is C6-parity discipline carried across from the nRF
     * arm, not a bug fix for an observed race: the nRF's fix round 1
     * bare-ERROR bench finding turned out to be a controller test-form
     * error (AT+MTCODES, the EXEC form, sent instead of the AT+MTCODES?
     * query form that cmd_mtcodes actually requires; core/mt/mt_at.c:202-215).
     * No settings-I/O race was ever observed, and none should be claimed
     * here. The lock is kept because the C6 takes ChipStackLock for this
     * exact function and uniform locking across every CHIP-touching
     * function in this file is cheaper to reason about than a per-function
     * safety argument for the one exception. On this platform there is a
     * second reason not to skip it: lock tracking is FATAL, so the
     * exception would have to be right, not merely plausible.
     *
     * The HEARTH_LOGE calls below stay for a different, real reason:
     * chasing that bare ERROR back to its actual cause took longer than it
     * should have precisely because this path logged nothing on failure.
     * Keeping these means any future failure here, whatever its cause, is
     * visible on the console instead of forcing that same trace again.
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
        HEARTH_LOGE("matter", "mt_matter_onboarding_codes: GetQRCode failed: %" CHIP_ERROR_FORMAT,
                    err.Format());
        return -1;
    }
    err = GetManualPairingCode(manual_span, flags);
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "mt_matter_onboarding_codes: GetManualPairingCode failed: %" CHIP_ERROR_FORMAT,
                    err.Format());
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
 * and AT_MT_SPEC.md's `<role>` table (3.27): DISABLED is kUnspecified (0),
 * DETACHED is kUnassigned (1) - the two are NOT the same token, unlike a
 * plain "no dataset" or "not attached" read. A CHILD with the radio off
 * when idle is kSleepyEndDevice (2) regardless of router eligibility; a
 * CHILD with the radio on is kReed (4) when router-eligible, kEndDevice (3)
 * otherwise. This image is an FTD (CHIP_DEVICE_CONFIG_THREAD_FTD is 1 in
 * src/CHIPProjectConfig.h, and hearth_matter_init.cpp's InitOpenThread()
 * sets the device type to Router on the strength of it), so
 * otThreadIsRouterEligible() is always reachable here and no #if guard is
 * needed the way CHIP's own CHIP_DEVICE_CONFIG_THREAD_FTD one is.
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
     * predicate above: AT_MT_SPEC.md's binding derivation (3.27) is
     * explicit that a device holding a dataset but still detached
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

/* ---- the live endpoint table (nRF 604-663) ---------------------------- */

/*
 * What the boot rebuild actually created, in creation order. The stored
 * composition (mt_comp_store.h) is the intent; this is the outcome, and
 * AT+MTEP? reports this one. Parallel arrays rather than a struct array,
 * mirroring the C6's s_live_* tables so the three ports read alike.
 *
 * Written only from the boot path in src/main.cpp, before mt_at_start() lets
 * any AT command run, so there is no concurrent access to guard. It also
 * touches no CHIP state, which is why it takes no stack lock while every
 * other function in this file does.
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

/* ---- attribute bridge, AT+MTATTR and the +MTATTR URC (nRF 665-1505) ----- */

/*
 * ZCL type to signedness and byte width; anything outside the AT+MTATTR
 * integer family is MT_ATTR_ERR_TYPE.
 *
 * The switch below knows only the BASE integer, bool, enum and bitmap type
 * codes. Matter also has a large family of ALIAS type codes that are integers
 * wearing a semantic name: ZCL_TEMPERATURE (0xD8), ZCL_PERCENT (0xE6),
 * ZCL_PERCENT100THS (0xE7), ZCL_EPOCH_S, ZCL_ELAPSED_S, ZCL_VENDOR_ID and
 * about thirty more. On the nRF arm every one of them fell straight through to
 * `default: return false` until its catalogue batch 2 declared the first of
 * them, and the damage was invisible at the AT layer: an AT+MTATTR read
 * answered +MTERR:5 and MatterPostAttributeChangeCallback dropped the +MTATTR
 * URC, while the very same attributes worked perfectly over a controller's IM.
 * Nasty because +MTERR:5 is ALSO the honest answer for a null nullable value
 * (mt_matter.h:178-181), so a tester cannot tell the two apart.
 *
 * NOT ONE alias type is declared by this image's two device types: the light's
 * and the sensor's attribute lists are BOOLEAN, ENUM8, INT16U, INT16S and
 * BITMAP32 throughout (port/mt_devtypes_sl.cpp). The normalisation is carried
 * anyway, ahead of need, because it is one line and because the batch that
 * adds the first thermostat setpoint or fan percentage would otherwise
 * reproduce the nRF's bug here from scratch. Do not "simplify" it away.
 *
 * chip::app::Compatibility::Internal::AttributeBaseType()
 * (app/util/ember-io-storage.cpp:36-124, declared in ember-io-storage.h) is
 * THE definition of the family in this tree, and its line numbers were
 * re-verified against the copy this project builds against
 * ($MATTER_EXT_ROOT/third_party/matter_sdk): the function opens at :36,
 * ZCL_TEMPERATURE -> ZCL_INT16S is at :120-121 and the catch-all
 * `default: return type` at :123-124, the same offsets the nRF arm cites. That
 * catch-all is what makes the normalisation safe: every base type and every
 * string, array, struct and float arrives at the switch below with exactly the
 * value it would have arrived with before, so no arm below changes behaviour
 * by construction. Signedness agrees with chip::app::IsSignedAttributeType()
 * (app-common/zap-generated/attribute-type.h), the tree's other oracle.
 *
 * attr_null_sentinel() below needs no change of its own: it is keyed on the
 * (is_unsigned, bytes) pair this function produces, so an alias gets the
 * sentinel its base type gets, which is exactly the bytes mt_devtypes_sl.cpp's
 * seed rows write.
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
 * ---- the Instance-served attribute carve-out (nRF 755-935, DE397) -------
 *
 * ABSENT THIS ROUND, and absent rather than reduced: the carve-out's table,
 * its instance_attr_served() predicate, the per-cluster read dispatch in
 * mt_matter_attr_read() and the two-way write disposition in
 * mt_matter_attr_write() are all left out together, because not one of the
 * clusters they name is compiled into this image. This build declares exactly
 * four clusters (OnOff, Identify, Descriptor, TemperatureMeasurement,
 * port/mt_devtypes_sl.cpp) and none of them is served by a per-endpoint C++
 * object. An empty table would not even compile (a zero-length array), and a
 * predicate with no caller is a warning in a build whose standing claim is
 * that it has none.
 *
 * What the carve-out IS, so the batch that brings it back knows what it is
 * bringing: a list of (cluster, attribute) pairs whose ember slot is an inert
 * shadow because a registered Instance answers for them, matched only AFTER
 * attr_locate() has proven the endpoint carries the cluster, so it can never
 * change the ENDPOINT/CLUSTER/ATTRIBUTE error division below. Reads answer the
 * live object; writes answer MT_ATTR_ERR_READONLY, except the Chime pair,
 * which routes to the live ChimeServer's own setters and raises no +MTATTR URC
 * in either notify mode because it bypasses emberAfWriteAttribute entirely.
 *
 * Every row of the nRF's table, and the batch that ports the device type it
 * belongs to (nRF README batch roster):
 *
 *   OperationalState OperationalState, CurrentPhase           batch 4
 *   Chime SelectedChime, Enabled                              batch 4
 *   RvcRunMode CurrentMode, RvcCleanMode CurrentMode          batch 5
 *   RvcOperationalState OperationalState, CurrentPhase        batch 5
 *   ElectricalPowerMeasurement Voltage, ActiveCurrent,
 *     ActivePower, RMSVoltage, RMSCurrent, Frequency,
 *     PowerFactor                                             batch 7a
 *   MeterIdentification MeterType                             batch 7a
 *   DeviceEnergyManagement ESAType, ESACanGenerate, ESAState,
 *     AbsMinPower, AbsMaxPower, OptOutState                   batch 7a
 *   DeviceEnergyManagementMode CurrentMode                    batch 7a
 *   WaterHeaterManagement HeaterTypes, HeatDemand, TankVolume,
 *     EstimatedHeatRequired, TankPercentage, BoostState       batch 7b
 *   WaterHeaterMode CurrentMode                               batch 7b
 *   RefrigeratorAndTemperatureControlledCabinetMode
 *     CurrentMode                                             batch 8
 *   OvenMode CurrentMode                                      batch 8
 *   OvenCavityOperationalState OperationalState, CurrentPhase  batch 8
 *   MicrowaveOvenMode CurrentMode                             batch 8
 *   EnergyEvse State, SupplyState, FaultState,
 *     ChargingEnabledUntil, CircuitCapacity, MinimumChargeCurrent,
 *     MaximumChargeCurrent, NextChargeStartTime,
 *     NextChargeTargetTime, NextChargeRequiredEnergy,
 *     NextChargeTargetSoC, StateOfCharge, BatteryCapacity,
 *     SessionID, SessionDuration, SessionEnergyCharged        EVSE round
 *   EnergyEvseMode CurrentMode                                EVSE round
 *
 * The rule the nRF's comment states for which rows exist is the half worth
 * carrying even with no table: a row exists only where the Instance's own
 * Read() intercepts the attribute, so a ClusterRevision whose Instance has no
 * case for it stays OUT (the arena seed IS the live answer), and an attribute
 * the port never DECLARES stays out too, because attr_locate()'s metadata miss
 * answers MT_ATTR_ERR_ATTRIBUTE long before any table is consulted.
 *
 * Also absent with it: the nRF's BooleanState bridge inside
 * MatterPostAttributeChangeCallback (its fix round 2, C1), for the same
 * reason. BooleanState reaches the contact/rain/leak sensors of a later batch;
 * this image declares no such endpoint, and FindClusterOnEndpoint() would
 * answer nullptr on every call.
 */

/*
 * Locate an attribute's metadata, splitting endpoint/cluster/attribute absence
 * the way mt_matter.h's mt_attr_result_t comments require it split: an unknown
 * endpoint index is MT_ATTR_ERR_ENDPOINT (emberAfIndexFromEndpoint against
 * kEmberInvalidEndpointIndex, attribute-storage.h:34, :132), an endpoint that
 * exists but does not carry the cluster is MT_ATTR_ERR_CLUSTER
 * (emberAfContainsServer, attribute-storage.h:100), and a present cluster with
 * no such attribute is MT_ATTR_ERR_ATTRIBUTE (emberAfLocateAttributeMetadata,
 * attribute-storage.h:91).
 *
 * emberAfContainsServer(), not emberAfFindServerCluster(): the latter is
 * declared only in app/util/endpoint-config-api.h, and emberAfContainsServer()
 * is its exact wrapper, so calling it needs no extra include. That mattered on
 * the nRF and it matters here for a second reason: task 5 left an unused
 * endpoint-config-api.h include behind in mt_devtypes_sl.cpp and this task
 * removed it, so the tree now has no include of that header at all.
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
 * the type maximum for unsigned, the type minimum for signed, the same
 * sentinels port/mt_devtypes_sl.cpp's s_seeds table documents and writes.
 * Shared by mt_matter_attr_read()'s null check and
 * MatterPostAttributeChangeCallback()'s so the two cannot drift apart.
 */
static uint64_t attr_null_sentinel(bool is_unsigned, uint8_t bytes)
{
    return is_unsigned
        ? (bytes >= 8 ? (uint64_t)-1 : ((1ULL << (8 * bytes)) - 1))
        : (bytes >= 8 ? (1ULL << 63) : (1ULL << (8 * bytes - 1)));
}

/*
 * out is a required, non-null out-parameter (unlike is_unsigned, which
 * mt_matter.h documents as "may be NULL"): core/mt/mt_at.c's cmd_mtattr always
 * passes the address of a stack local.
 *
 * The read goes through emberAfReadAttribute() rather than
 * mt_dyn_attr_slot(), which port/mt_dyn_store.h offers and whose own header
 * comment says plainly not to use it for this: the ember path validates
 * against the metadata and is the same path a controller's read takes, so the
 * two cannot disagree.
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
         * definite signedness exists, but the header's "the flag is also valid
         * when the result is MT_ATTR_ERR_TYPE" covers this arm too, and
         * cmd_mtattr lets MT_ATTR_ERR_TYPE fall through to a write that reads
         * this flag before it re-derives the same failure. false selects the
         * signed parse, and the write fails on its own attr_type_info() check
         * for the identical reason. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_TYPE;
    }
    /* Set before the null check below, so the flag stays valid even when this
     * read goes on to answer MT_ATTR_ERR_TYPE for a null nullable value
     * (core/include/mt_matter.h:172-176, binding). */
    if (is_unsigned) {
        *is_unsigned = unsigned_type;
    }

    uint8_t buf[8] = { 0 };
    chip::Protocols::InteractionModel::Status st =
        emberAfReadAttribute(ep, cluster, attr, buf, sizeof(buf));
    if (st != chip::Protocols::InteractionModel::Status::Success) {
        /*
         * The nRF arm calls this arm defensive ("cannot happen once
         * attr_locate() has proven the triple exists"). IT IS REACHABLE IN
         * THIS TREE and the difference is worth stating where it bites.
         *
         * This SDK's CHIP serves the fixed endpoints' framework clusters
         * (BasicInformation, AccessControl, GeneralCommissioning, Descriptor
         * and the rest of autogen/zap-generated/CodeDrivenInitShutdown.cpp)
         * through registered cluster objects, and ZAP therefore declares
         * their attributes EXTERNAL_STORAGE so ember holds no bytes for them.
         * A CONTROLLER's read never notices: the data model provider asks the
         * cluster registry first. emberAfReadAttribute() does not: it sees
         * EXTERNAL_STORAGE and calls emberAfExternalAttributeReadCallback(),
         * which is this port's (mt_devtypes_sl.cpp) and answers only for the
         * dynamic endpoints' arena, so it returns UnsupportedAttribute for
         * every fixed-endpoint attribute.
         *
         * Bench: AT+MTATTR=0,0x0028,0x0002 (the root VendorID) answers a bare
         * ERROR here, and it is the harness Phase 1 row "MTATTR root VendorID
         * read". MT_ATTR_ERR_FAILED is the honest code for it: the attribute
         * exists and is an integer, and this path could not read it.
         * MT_ATTR_ERR_ATTRIBUTE would claim it does not exist.
         *
         * Reaching those values needs a read path through the data model
         * provider for endpoints this port did not create, which is a section
         * of its own and has no nRF counterpart to transfer (the nRF's arm is
         * genuinely unreachable, so the row passes there through ember). The
         * log line is here so the next bench session reads the cause instead
         * of deriving it again.
         */
        HEARTH_LOGE("matter", "attr read ep %u cluster 0x%04lX attr 0x%04lX: ember status %u "
                              "(a fixed endpoint's code-driven cluster is not reachable through "
                              "the ember path)",
                    (unsigned)ep, (unsigned long)cluster, (unsigned long)attr,
                    (unsigned)chip::to_underlying(st));
        return MT_ATTR_ERR_FAILED;
    }

    uint64_t raw = 0;
    for (uint8_t i = 0; i < bytes; i++) {
        raw |= ((uint64_t)buf[i]) << (8 * i);
    }

    if (md->IsNullable() && raw == attr_null_sentinel(unsigned_type, bytes)) {
        /* The AT grammar has no null literal: answer MT_ATTR_ERR_TYPE with
         * is_unsigned already set above, so a caller about to WRITE can still
         * fetch the signedness first. */
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

extern "C" int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t val,
                                    bool notify)
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

    /* Width bounds check ahead of the write: a value outside the attribute's
     * own width is MT_ATTR_ERR_VALUE, not a silent truncation
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
     * No md->IsWritable() gate here, carried from the nRF with its reasoning
     * re-checked against THIS tree:
     *
     * emAfWriteAttribute()'s writable/data-type guard runs only when
     * overrideReadOnlyAndDataType is false, and both public
     * emberAfWriteAttribute() overloads pass true for it. The header says so
     * itself (app/util/attribute-table.h:74-85): "This function will not check
     * to see if the attribute is writable since the read only / writable
     * characteristic of an attribute only pertains to external devices writing
     * over the air. Because this function is being called locally it assumes
     * that the device knows what it is doing." A locally originated write is
     * trusted; this bridge's caller IS the local host.
     *
     * mt_matter.h's MT_ATTR_ERR_READONLY comment is scoped narrower than "no
     * WRITABLE flag": it names a specific mechanism, an attribute "served by a
     * cluster Instance (ATTRIBUTE_FLAG_MANAGED_INTERNALLY without
     * ATTRIBUTE_FLAG_WRITABLE)". This image has no Instance-served cluster at
     * all (see the absent carve-out above), so that condition cannot occur and
     * MT_ATTR_ERR_READONLY IS UNREACHABLE FROM THIS BRIDGE THIS ROUND, exactly
     * as it is on the nRF arm at the same point in its history. A gate on
     * IsWritable() alone would be a different and wrong thing: it would answer
     * READONLY for TemperatureMeasurement MeasuredValue and Identify
     * IdentifyType, both declared here without WRITABLE, and the bench
     * requires a host write of MeasuredValue to succeed. The batch that adds
     * the first Instance-served cluster makes the code reachable by bringing
     * back the carve-out, not by adding a blanket flag check.
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
     * notify selects ONLY MarkAttributeDirty: kIfChanged (notify=true) marks
     * the change for the CHIP reporting engine, so subscribed and bound
     * controllers see it; kNo (notify=false) changes local storage without
     * that fabric-facing report. It does NOT gate the +MTATTR URC. That is the
     * as-built rule on both arms and it is what AT_MT_SPEC.md 3.8 states with
     * no mode carve-out: "A write to an ember-managed attribute that actually
     * changes its value echoes a +MTATTR URC ... then OK". 3.25's AirQuality
     * write is called out as the ONE exception that never echoes "in either
     * mode", an exception that only makes sense if the general rule already
     * covers both. A notify=false reflection of a controller-driven change
     * therefore DOES echo back to the host; de-duplication, if ever needed, is
     * a host concern and not this bridge's.
     */
    chip::Protocols::InteractionModel::Status st = emberAfWriteAttribute(path, input);

    switch (st) {
    case chip::Protocols::InteractionModel::Status::Success:
        /* A same-value re-push also answers Success in both notify modes:
         * emAfWriteAttribute()'s AttributeValueIsChanging() early-out returns
         * Success without touching MarkAttributeDirty's callback path at all,
         * so no URC is raised either. OK by construction, no special case
         * needed here (binding per core/include/mt_matter.h's same-value
         * comment and AT_MT_SPEC.md 3.8's same-value paragraph). */
        return MT_ATTR_OK;
    case chip::Protocols::InteractionModel::Status::ConstraintError:
        /* ember's own MIN_MAX bounds check runs unconditionally, independent
         * of the override flag noted above. */
        return MT_ATTR_ERR_VALUE;
    default:
        /* Reachable for the same reason the read's failure arm is: a fixed
         * endpoint's code-driven cluster has no ember bytes to write, so the
         * external write callback answers UnsupportedAttribute. See the long
         * comment in mt_matter_attr_read(). */
        HEARTH_LOGE("matter", "attr write ep %u cluster 0x%04lX attr 0x%04lX: ember status %u",
                    (unsigned)ep, (unsigned long)cluster, (unsigned long)attr,
                    (unsigned)chip::to_underlying(st));
        return MT_ATTR_ERR_FAILED;
    }
}

/*
 * Strong override of the weak default in the SDK's
 * app/util/generic-callback-stubs.cpp:59. Every attribute write that actually
 * changes a value passes through here, whether it came from this bridge's
 * write (either notify mode, see mt_matter_attr_write() above) or from a
 * controller's own IM write, so any change surfaces to the host as a +MTATTR
 * URC. Format is byte-identical to the C6's and the nRF's.
 *
 * mt_at_urc() is the core's single URC path and carries its own s_at_up guard,
 * so a change raised during Server::Init, before mt_at_start() has run, is
 * dropped rather than written into a transport that does not exist yet. That
 * guard is why this function needs none of its own.
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
     * A transition TO null must not echo the raw sentinel as if it were a real
     * value: a host reading +MTATTR:...,255 has no way to tell that from a
     * genuine 255, and a follow-up AT+MTATTR read answers +MTERR:5 for the
     * same attribute. emberAfLocateAttributeMetadata() is a plain table lookup
     * (no I/O, no lock of its own), safe to call under whatever lock the
     * caller already holds: this bridge's own StackLock for a local write, or
     * CHIP's own lock for a controller-driven one. Shares
     * attr_null_sentinel() with mt_matter_attr_read() so the two null tests
     * cannot drift apart.
     */
    const EmberAfAttributeMetadata *md =
        emberAfLocateAttributeMetadata(path.mEndpointId, path.mClusterId, path.mAttributeId);
    if (md != nullptr && md->IsNullable() && raw == attr_null_sentinel(is_unsigned, bytes)) {
        return;
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
 * ---- what is NOT in this file, and what the batch that needs it brings ---
 *
 * THE CLUSTER-OBJECT ARENA (nRF mt_matter_zephyr.cpp 133-386, and its sizing
 * tail at 9137-9472). On the nRF that arena replaced fourteen fixed pools,
 * one slot per endpoint that COULD carry the family: the OperationalState,
 * RvcOperationalState, ModeBase, Chime, valve, EPM, PowerTopology, WHM, DEM,
 * MeterIdentification and EVSE Delegates and the raw storage for their
 * cluster Instances, 14,368 B of .bss and .data before the change. Every one
 * of those families belongs to a device type round 2 task 5 does not build:
 * the two types it does build (the on/off light and the temperature sensor)
 * carry OnOff, TemperatureMeasurement, Identify and Descriptor, none of which
 * has a per-endpoint delegate at all.
 *
 * So the arena is absent rather than reduced. Carrying it would mean a static
 * array sized for allocations nothing makes, a set of template helpers
 * (obj_pair_new, obj_inst_storage, obj_new, obj_inst_new) with no caller, and
 * a sizing tail every constant of which is a sizeof() of a class this image
 * does not compile. The batch that ports the first delegate-bearing device
 * type brings three things together, and they only make sense together: the
 * arena, the family's pool with its depth constant, and mt_devtype_create()'s
 * claim block in port/mt_devtypes_sl.cpp (the note there says the same thing
 * from the other side). The arena mechanism itself is already here:
 * hearth_arena in port/mt_dyn_store.h is the allocator both arenas use, and
 * that batch adds an instance and a budget in port/mt_port_ids.h beside
 * HEARTH_EP_ARENA_BYTES, not a new mechanism.
 */
