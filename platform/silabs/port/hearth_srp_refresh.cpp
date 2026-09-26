/*
 * hearth_srp_refresh.cpp: re-advertise once an off-mesh address exists
 * (graph B573, findings F576 and F577).
 *
 * After some Thread joins this device's first SRP registration carried only
 * its mesh-local address and was never refreshed, although the off-mesh-
 * routable (OMR) address formed by SLAAC moments later: the OTBR's
 * advertising proxy then published a host with no routable address and a
 * controller's operational discovery timed out. OpenThread's SRP client in
 * auto host-address mode should refresh on that address change; upstream
 * has the same logic, and the exact race is not identified (it vanished
 * under diagnostic logging). This does not depend on it: on every Thread
 * address change while the node has a fabric it arms a short settle timer,
 * and when that fires with a preferred SLAAC address present it re-runs
 * CHIP's operational advertisement, which sends a fresh SRP update composed
 * with the current addresses. The event handler only restarts the timer, so
 * it adds no work or lock on the path the race lives on.
 */
#include "hearth_srp_refresh.h"

#include <app/server/Dnssd.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ThreadStackManager.h>

#include <openthread/ip6.h>

#include "hearth_log.h"

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

constexpr System::Clock::Milliseconds32 kSettle(1500);

bool has_preferred_slaac_address(void)
{
    otInstance *ot = ThreadStackMgrImpl().OTInstance();
    if (ot == nullptr) {
        return false;
    }
    bool found = false;
    ThreadStackMgr().LockThreadStack();
    for (const otNetifAddress *a = otIp6GetUnicastAddresses(ot); a != nullptr; a = a->mNext) {
        if (a->mAddressOrigin == OT_ADDRESS_ORIGIN_SLAAC && a->mPreferred && a->mValid) {
            found = true;
            break;
        }
    }
    ThreadStackMgr().UnlockThreadStack();
    return found;
}

void refresh(System::Layer *, void *)
{
    if (Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        return;
    }
    if (!has_preferred_slaac_address()) {
        return;
    }
    app::DnssdServer::Instance().StartServer();
    HEARTH_LOGI("thread", "off-mesh address present: operational advertisement refreshed");
}

void on_device_event(const ChipDeviceEvent *event, intptr_t)
{
    if (event->Type == DeviceEventType::kThreadStateChange && event->ThreadStateChange.AddressChanged) {
        SystemLayer().CancelTimer(refresh, nullptr);
        CHIP_ERROR err = SystemLayer().StartTimer(kSettle, refresh, nullptr);
        if (err != CHIP_NO_ERROR) {
            HEARTH_LOGE("thread", "srp refresh timer did not start: %" CHIP_ERROR_FORMAT, err.Format());
        }
    }
}

} // namespace

void hearth_srp_refresh_start(void)
{
    PlatformMgr().LockChipStack();
    CHIP_ERROR err = PlatformMgr().AddEventHandler(on_device_event, 0);
    PlatformMgr().UnlockChipStack();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("thread", "srp refresh not registered: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    HEARTH_LOGI("thread", "srp refresh armed (B573)");
}
