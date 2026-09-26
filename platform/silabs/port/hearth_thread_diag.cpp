/*
 * hearth_thread_diag.cpp: DIAGNOSTIC ONLY (graph B573, finding F576).
 *
 * After some Thread joins this device registers its SRP host with only its
 * mesh-local address and never updates it, so the OTBR's advertising proxy
 * publishes a host with no routable address and a controller's operational
 * discovery times out. CHIP runs the SRP client in auto host-address mode,
 * which should re-register when an off-mesh-routable (OMR) address appears.
 * This logs, on every Thread address or network-data change, the unicast
 * addresses (origin and preferred flag), the on-mesh prefixes (SLAAC flag)
 * and the SRP host's state, so a failing join shows whether the OMR address
 * never formed or formed without an SRP update. Remove or rule on it before
 * any release.
 */
#include "hearth_thread_diag.h"

#include <platform/CHIPDeviceLayer.h>
#include <platform/ThreadStackManager.h>

#include <openthread/ip6.h>
#include <openthread/netdata.h>
#include <openthread/srp_client.h>

#include "hearth_log.h"

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

void log_thread_state(const char *why)
{
    otInstance *ot = ThreadStackMgrImpl().OTInstance();
    if (ot == nullptr) {
        return;
    }
    char addr[OT_IP6_ADDRESS_STRING_SIZE];
    char prefix[OT_IP6_PREFIX_STRING_SIZE];

    ThreadStackMgr().LockThreadStack();
    for (const otNetifAddress *a = otIp6GetUnicastAddresses(ot); a != nullptr; a = a->mNext) {
        otIp6AddressToString(&a->mAddress, addr, sizeof(addr));
        HEARTH_LOGI("thread", "%s: addr %s/%u origin %u preferred %u", why, addr,
                    (unsigned)a->mPrefixLength, (unsigned)a->mAddressOrigin, (unsigned)a->mPreferred);
    }
    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig cfg;
    while (otNetDataGetNextOnMeshPrefix(ot, &it, &cfg) == OT_ERROR_NONE) {
        otIp6PrefixToString(&cfg.mPrefix, prefix, sizeof(prefix));
        HEARTH_LOGI("thread", "%s: on-mesh prefix %s slaac %u preferred %u", why, prefix,
                    (unsigned)cfg.mSlaac, (unsigned)cfg.mPreferred);
    }
    const otSrpClientHostInfo *host = otSrpClientGetHostInfo(ot);
    HEARTH_LOGI("thread", "%s: srp host %s state %s auto-address %u", why,
                (host->mName != nullptr) ? host->mName : "(none)",
                otSrpClientItemStateToString(host->mState), (unsigned)host->mAutoAddress);
    ThreadStackMgr().UnlockThreadStack();
}

void on_device_event(const ChipDeviceEvent *event, intptr_t)
{
    if (event->Type != DeviceEventType::kThreadStateChange) {
        return;
    }
    if (event->ThreadStateChange.AddressChanged) {
        log_thread_state("addr-changed");
    } else if (event->ThreadStateChange.NetDataChanged) {
        log_thread_state("netdata-changed");
    }
}

} // namespace

void hearth_thread_diag_start(void)
{
    PlatformMgr().LockChipStack();
    CHIP_ERROR err = PlatformMgr().AddEventHandler(on_device_event, 0);
    PlatformMgr().UnlockChipStack();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("thread", "B573 diagnostic not registered: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    HEARTH_LOGI("thread", "B573 diagnostic armed: addresses, on-mesh prefixes and the SRP host on every change");
}
