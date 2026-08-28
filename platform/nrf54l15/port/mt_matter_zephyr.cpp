/*
 * mt_matter_zephyr.cpp - the mt_matter.h commissioning/state/network
 * slice against CHIP on Zephyr (Matter core spec section 6). The AT
 * parser thread calls these; anything touching CHIP state takes the
 * stack lock (the bridge_manager.cpp discipline).
 */

#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConnectivityManager.h>
#include <platform/ThreadStackManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <openthread/link.h>
#include <openthread/thread.h>

#include <string.h>

#include <zephyr/logging/log.h>

extern "C" {
#include "mt_matter.h"
}

LOG_MODULE_REGISTER(hearth_matter, LOG_LEVEL_INF);

using chip::DeviceLayer::ConnectivityMgr;
using chip::DeviceLayer::PlatformMgr;
using chip::DeviceLayer::ThreadStackMgr;
using chip::DeviceLayer::ThreadStackMgrImpl;

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
     * Fix round 1 (bench-found): this used to run lock-free, on the belief
     * that formatting an already-resolved payload needed no CHIP stack
     * access. That belief was wrong. GetQRCode()/GetManualPairingCode() both
     * call GetPayloadContents(), which reads the passcode and discriminator
     * through GetCommissionableDataProvider() (ConfigurationManagerImpl on
     * this platform), which in turn reads them via
     * GenericConfigurationManagerImpl::ReadConfigValue() ->
     * ZephyrConfig::ReadConfigValueImpl() -> Zephyr's
     * settings_load_subtree_direct(). That is live Device Layer state, not
     * a static snapshot: the CHIP event-loop thread touches the same
     * settings subsystem for its own background persistence (session and
     * fabric table state, group data, fail-safe bookkeeping) for the whole
     * time it is not blocked waiting on the next event, which per CHIP's
     * own threading model (chip::DeviceLayer::StackLock's own doc comment,
     * PlatformManager.h) is exactly the condition the CHIP big lock exists
     * to serialize against. Calling into this path from the AT parser
     * thread without that lock races the CHIP thread's own settings I/O;
     * ZephyrConfig::ReadConfigValueImpl() reports a bare
     * CHIP_ERROR_PERSISTED_STORAGE_FAILED on that race with no logging of
     * its own (only the OnboardingCodesUtil.cpp wrappers above it log, and
     * by the time execution reaches them the underlying cause is already a
     * generic storage error), which is exactly the bench symptom: silent
     * ERROR, no console output. The C6 already takes ChipStackLock for this
     * same function (main.cpp); this platform's port dropped it. Taking
     * the lock here blocks until the CHIP thread's event loop releases its
     * own hold on it, giving this read the same exclusion the CHIP thread
     * gives its own settings access.
     */
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

/* OT role to Matter RoutingRoleEnum (ThreadNetworkDiagnostics). The
 * REED refinement (a child eligible for promotion) is not derived here;
 * a child reports END_DEVICE or SLEEPY_END_DEVICE by its link mode.
 * Recorded as a milestone simplification in the task report. */
static uint8_t ot_role_to_matter(otInstance *ot)
{
    switch (otThreadGetDeviceRole(ot)) {
    case OT_DEVICE_ROLE_DISABLED:
    case OT_DEVICE_ROLE_DETACHED: return 1;  /* kUnassigned */
    case OT_DEVICE_ROLE_CHILD: {
        otLinkModeConfig mode = otThreadGetLinkMode(ot);
        return mode.mRxOnWhenIdle ? 3 : 2;   /* kEndDevice : kSleepyEndDevice */
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
    out->role = ot_role_to_matter(ot);
    otDeviceRole role = otThreadGetDeviceRole(ot);
    bool attached = (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
                     role == OT_DEVICE_ROLE_LEADER);
    out->attached = attached;
    if (attached) {
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
    }
    const char *name = otThreadGetNetworkName(ot);
    if (name) {
        strncpy(out->name, name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
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
