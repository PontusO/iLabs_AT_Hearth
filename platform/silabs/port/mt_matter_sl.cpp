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

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConnectivityManager.h>
#include <platform/ThreadStackManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <openthread/dataset.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

#include <string.h>

extern "C" {
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
