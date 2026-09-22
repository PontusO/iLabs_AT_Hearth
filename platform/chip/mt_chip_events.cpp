/*
 * mt_chip_events.cpp - see the header. Every emission goes through
 * mt_at_event(), so the RAM mask and the pre-+MTREADY guard apply unchanged.
 * Nothing here takes the CHIP stack lock: the device-event handler and both
 * delegates run on the CHIP task with it held, and mt_at_event() writes the
 * UART under the link mutex only.
 *
 * This is the C6's app_event_cb() (platform/esp32c6/main/main.cpp:574-800)
 * translated to stock CHIP. The difference that forces a translation rather
 * than a copy: esp-matter's fork posts device events for the commissioning
 * window, the commissioning sessions and the fabric table, and stock CHIP
 * posts none of them. Those ten bits come from
 * CommissioningWindowManager::AppDelegate and FabricTable::Delegate instead
 * (design spec section 1.3). Everything else is a device event with the same
 * payload fields on both SDKs.
 */
#include "mt_chip_events.h"

#include <app/server/AppDelegate.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/CHIPDeviceLayer.h>

#include <cstdio>

/* Both headers carry their own `extern "C"` guard (mt_at.h:21, mt_matter.h:15,
 * and mt_at.h's own banner says it is kept C++-safe), so they are included
 * plainly rather than wrapped in a second one here. */
#include "mt_at.h"
#include "mt_matter.h"

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

bool s_registered;

/*
 * Whether a +MTEVT:0 has been delivered for the window that is open now.
 *
 * volatile because two tasks touch it: the CHIP event loop through the
 * AppDelegate, and the boot task through mt_chip_events_after_ready(). The
 * C6 carries the same `volatile` for the same pair (main.cpp:250), and the
 * same residual race with it: between the replay reading the flag and
 * mt_matter_state() acquiring the stack lock. Its worst outcome is the
 * duplicate +MTEVT:0 the replay exists to prevent, and on the C6 the window
 * opened ~10 ms before the replay ran, so the window for it is microseconds
 * against milliseconds.
 *
 * Ownership sits with the emitter, not with one particular emitter: an
 * earlier C6 cut had the handler set the flag and only the replay test it,
 * which cannot work when the two race.
 */
volatile bool s_window_evt_sent;

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
/*
 * Last role token reported on +MTEVT:28, and whether one has been reported
 * at all since the Thread interface last came up. Only the device-event
 * handler touches these, and only from the CHIP event loop, so they need
 * neither lock nor volatile.
 *
 * The cache is not a flourish: it is C6 bench defect A. CHIP's
 * ThreadStateChange.RoleChanged tracks the OPENTHREAD role while this event
 * carries the MATTER RoutingRole token, and that mapping is many-to-one
 * (thread-network-diagnostics-provider.cpp derives the token from
 * otThreadGetDeviceRole() combined with otThreadGetLinkMode() and
 * otThreadIsRouterEligible(), so OT_DEVICE_ROLE_CHILD alone renders as
 * SLEEPY_END_DEVICE, END_DEVICE or REED). Two genuine OT role changes can
 * land on one Matter token, and the C6 bench saw three bit-28 events inside
 * 10 ms carrying UNASSIGNED, REED, REED. Gating on RoleChanged is necessary
 * and not sufficient.
 *
 * The role byte is cached rather than the rendered string because
 * mt_thread_role_name() is injective: seven enum values, seven distinct
 * tokens, and the out-of-range fallback renders the byte itself in decimal.
 * Byte equality is token equality, in two bytes instead of eighteen.
 */
uint8_t s_last_role_tok;
bool s_last_role_tok_valid;
#endif

/*
 * The deferred half of the window-closed decision. Scheduled onto the CHIP
 * event queue by OnCommissioningWindowClosed(), which cannot answer the
 * question itself; the long comment there says why.
 */
void window_closed_check(intptr_t)
{
    const bool still_open = Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
    if (s_window_evt_sent && !still_open)
    {
        /* Cleared even if the URC is masked out: the window is gone, and the
         * next one must be reported as a fresh +MTEVT:0. */
        s_window_evt_sent = false;
        mt_at_event(MT_EVT_COMMISSION_WINDOW_CLOSED, nullptr);
    }
}

/* Bits 0, 4, 1, 2: stock CHIP signals the window and the sessions through the
 * commissioning window manager's AppDelegate, not device events. AppDelegate
 * is a global-namespace class (app/server/AppDelegate.h), not chip::. */
class WindowDelegate final : public AppDelegate
{
public:
    void OnCommissioningWindowOpened() override
    {
        /* Skip if the boot replay already reported this window. */
        if (!s_window_evt_sent && mt_at_event(MT_EVT_COMMISSION_WINDOW_OPEN, nullptr))
        {
            s_window_evt_sent = true;
        }
    }

    void OnCommissioningWindowClosed() override
    {
        /*
         * CHIP stops advertising for PASE at three different moments: a
         * commissioner established a session (the window still open, only
         * paused to new commissioners), the window genuinely ended
         * (completion, timeout, or the 20-attempt limit), and a failed open
         * cleaning up a window that never existed. The host contract is one
         * +MTEVT:4 per reported +MTEVT:0, at the moment the window is really
         * gone, so the decision has to distinguish them.
         *
         * The C6 distinguishes them by asking IsCommissioningWindowOpen()
         * inside the callback. THAT DOES NOT WORK ON STOCK CHIP, and this is
         * the one place where the two SDKs' shapes differ rather than their
         * names (bench-measured 2026-09-21, the first cut of this file: a
         * full commissioning produced 1, 25, 28, 3 and no 4 at all, for 148 s
         * after completion). esp-matter POSTS a kCommissioningWindowClosed
         * DEVICE EVENT, which is dispatched from the queue after the window
         * manager has finished; stock CHIP CALLS this delegate synchronously
         * from StopAdvertisement(), and on the real-close path
         * Cleanup() is StopAdvertisement() followed by ResetState()
         * (CommissioningWindowManager.cpp:142-146), so mWindowStatus is still
         * kBasicWindowOpen when this runs. Both moments therefore look
         * identical from in here, and the gate suppressed the one emission it
         * exists to make.
         *
         * So do what esp-matter does: ask again off the CHIP event queue.
         * ScheduleWork() posts a kCallWorkFunct onto the SAME FIFO queue the
         * device events come from, so the re-query is processed only once the
         * entire dispatch of the event that caused this close has finished.
         * That is what makes ResetState() have run by then, and it is also
         * what puts the +MTEVT:4 after the +MTEVT:3 of the same
         * kCommissioningComplete: our handler's bit-3 emission is part of
         * that same dispatch. The queue is the whole reason, and handler
         * order has nothing to do with it. (Do not reason from registration
         * order here: the window manager registers its platform handler in
         * OnSessionEstablished(), CommissioningWindowManager.cpp:241, not in
         * Server::Init, and _AddEventHandler PREPENDS,
         * GenericPlatformManagerImpl.ipp:199-203, so the order is both
         * different from the obvious guess and not something to depend on.)
         *
         * No stack lock either way: this callback and the scheduled work both
         * run on the CHIP task.
         */
        CHIP_ERROR err = PlatformMgr().ScheduleWork(window_closed_check, 0);
        if (err != CHIP_NO_ERROR)
        {
            /* The event queue is full, and there is no fallback to degrade
             * to: every path into this callback runs before ResetState(), so
             * an immediate re-query here would answer "still open" and emit
             * nothing at all. This window's +MTEVT:4 is therefore lost. s_window_evt_sent
             * is cleared so the NEXT window's +MTEVT:0 is not lost with it (the pair rule
             * is already broken for this window; leaving the flag set would break every
             * later one until a reboot). The trade: if this close was the paused-session
             * variety and the manager re-advertises the same window, that re-open emits a
             * second 0 for one window. A logged, bounded duplicate beats an unbounded
             * silence. Not reached in any bench run so far.
             *
             * One corner this trade does not cover: if an earlier
             * OnCommissioningWindowClosed() already queued window_closed_check
             * successfully and that item is still pending when a second call's
             * ScheduleWork fails here, this synchronous clear makes the pending
             * check's own s_window_evt_sent && !still_open guard read false when it
             * finally runs, so that earlier, real close loses its +MTEVT:4 too, and
             * silently, since window_closed_check's own no-op path logs nothing. That
             * needs two ScheduleWork failures in a row, one queued and one refused,
             * which is accepted here: two consecutive queue-full events on the CHIP
             * task have never been observed, and the fix, a per-window token so the
             * pending check could tell this failure is not about its own window, is
             * machinery for a path no bench has reached. */
            s_window_evt_sent = false;
            ChipLogError(AppServer, "Hearth: +MTEVT:4 lost, ScheduleWork failed: %" CHIP_ERROR_FORMAT,
                         err.Format());
        }
    }

    void OnCommissioningSessionStarted() override { mt_at_event(MT_EVT_COMMISSION_SESSION_STARTED, nullptr); }
    void OnCommissioningSessionStopped() override { mt_at_event(MT_EVT_COMMISSION_SESSION_STOPPED, nullptr); }
};

/* Bits 6 to 9: the fabric table's own delegate interface, one emission per
 * callback. The C6 gets the same four as device events from esp-matter's
 * fork. */
class FabricDelegate final : public FabricTable::Delegate
{
public:
    void FabricWillBeRemoved(const FabricTable &, FabricIndex) override
    {
        mt_at_event(MT_EVT_FABRIC_WILL_BE_REMOVED, nullptr);
    }
    void OnFabricRemoved(const FabricTable &, FabricIndex) override { mt_at_event(MT_EVT_FABRIC_REMOVED, nullptr); }
    void OnFabricCommitted(const FabricTable &, FabricIndex) override { mt_at_event(MT_EVT_FABRIC_COMMITTED, nullptr); }
    void OnFabricUpdated(const FabricTable &, FabricIndex) override { mt_at_event(MT_EVT_FABRIC_UPDATED, nullptr); }
};

WindowDelegate s_window_delegate;
FabricDelegate s_fabric_delegate;

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
/*
 * Bit 28's token: the port's own role read and the shared name table.
 *
 * mt_matter_thread_info() is reused here, which the C6 deliberately does NOT
 * do (it mirrors the RoutingRole probe in a lock-free helper instead). The
 * reason the C6 cannot and both Thread ports can is the lock the bridge
 * takes: the C6's opens a ChipStackLock for its AT-parser-task caller, and
 * taking CHIP's PlatformManager lock a second time from the task that
 * already holds it risks a self-deadlock. The Thread ports' bridges take the
 * OT lock alone and no CHIP lock (port/mt_matter_sl.cpp:214-233,
 * platform/nrf54l15/port/mt_matter_zephyr.cpp:525-544), so calling it from
 * the CHIP task with the stack lock held is CHIP then OT, the established
 * order, and the one function stays the one place the role is decoded.
 */
void emit_role_if_changed(void)
{
    mt_thread_info_t info;
    if (mt_matter_thread_info(&info) != MT_ATTR_OK)
    {
        return;
    }
    uint8_t role = info.role; /* Matter RoutingRoleEnum, raw (mt_matter.h:85) */
    if (s_last_role_tok_valid && role == s_last_role_tok)
    {
        return;
    }

    char role_tok[8];
    const char * tok = mt_thread_role_name(role);
    if (tok == nullptr)
    {
        /* The same decimal-degrades-rather-than-lies fallback cmd_mtthread
         * uses for AT+MTTHREAD?: an out-of-range SDK value still reaches the
         * host instead of silently dropping the one notification of this
         * transition. */
        snprintf(role_tok, sizeof(role_tok), "%u", (unsigned) role);
        tok = role_tok;
    }

    /* Cached whether or not the host has bit 28 unmasked: mt_at_event()'s
     * mask test decides delivery, not what the firmware believes it last told
     * the host about the role. Keying the cache on the return value instead
     * would make a mask flip resend a role the host already knows. */
    s_last_role_tok       = role;
    s_last_role_tok_valid = true;
    mt_at_event(MT_EVT_THREAD_ROLE_CHANGED, tok);
}
#endif /* CHIP_DEVICE_CONFIG_ENABLE_THREAD */

/*
 * The device events. Each one we surface maps to a bit in the AT event mask
 * (mt_at.h); mt_at_event() drops the ones the host has not subscribed to, so
 * this switch can stay exhaustive without flooding a 115200 link.
 *
 * Runs on the CHIP event loop with the stack lock held. No lock is taken
 * here, for the same reason the C6's app_event_cb() takes none.
 */
void on_device_event(const ChipDeviceEvent * event, intptr_t)
{
    switch (event->Type)
    {
    /* Commissioning. Bits 0, 1, 2 and 4 are the AppDelegate's above. */
    case DeviceEventType::kCommissioningComplete:
        mt_at_event(MT_EVT_COMMISSION_COMPLETE, nullptr);
        break;
    case DeviceEventType::kFailSafeTimerExpired:
        mt_at_event(MT_EVT_FAIL_SAFE_EXPIRED, nullptr);
        break;

    /* Connectivity. The detail field carries up/down where CHIP gives it. */
    case DeviceEventType::kWiFiConnectivityChange:
        mt_at_event(MT_EVT_WIFI_CONNECTIVITY,
                    event->WiFiConnectivityChange.Result == kConnectivity_Established ? "1" : "0");
        break;
    /* Bit 11 is as unreachable on a Thread image as bit 10: in this CHIP tree
     * kInternetConnectivityChange is posted only by the WiFi and Ethernet
     * platforms (grep InternetConnectivityChange src/platform/), never by the
     * OpenThread one. Kept so the mapping stays one-to-one with the C6's. */
    case DeviceEventType::kInternetConnectivityChange:
        mt_at_event(MT_EVT_INTERNET_CONNECTIVITY,
                    event->InternetConnectivityChange.IPv4 == kConnectivity_Established ? "1" : "0");
        break;
    case DeviceEventType::kInterfaceIpAddressChanged:
        mt_at_event(MT_EVT_INTERFACE_IP_CHANGED, nullptr);
        break;
    case DeviceEventType::kOperationalNetworkStarted:
        mt_at_event(MT_EVT_OPERATIONAL_NETWORK_STARTED, nullptr);
        break;
    case DeviceEventType::kDnssdInitialized:
        mt_at_event(MT_EVT_DNSSD_INITIALIZED, nullptr);
        break;
    case DeviceEventType::kServerReady:
        mt_at_event(MT_EVT_SERVER_READY, nullptr);
        break;

    /* BLE. */
    case DeviceEventType::kCHIPoBLEConnectionEstablished:
        mt_at_event(MT_EVT_BLE_CONNECTED, nullptr);
        break;
    case DeviceEventType::kCHIPoBLEConnectionClosed:
        mt_at_event(MT_EVT_BLE_DISCONNECTED, nullptr);
        break;
    case DeviceEventType::kCHIPoBLEAdvertisingChange:
        mt_at_event(MT_EVT_BLE_ADVERTISING_CHANGE, nullptr);
        break;
    case DeviceEventType::kBLEDeinitialized:
        mt_at_event(MT_EVT_BLE_DEINITIALIZED, nullptr);
        break;

    /* Misc. */
    case DeviceEventType::kOtaStateChanged:
        mt_at_event(MT_EVT_OTA_STATE_CHANGED, nullptr);
        break;
    case DeviceEventType::kBindingsChangedViaCluster:
        mt_at_event(MT_EVT_BINDINGS_CHANGED, nullptr);
        break;
    case DeviceEventType::kTimeSyncChange:
        mt_at_event(MT_EVT_TIME_SYNC_CHANGE, nullptr);
        break;

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    case DeviceEventType::kThreadConnectivityChange:
        mt_at_event(MT_EVT_THREAD_CONNECTIVITY,
                    event->ThreadConnectivityChange.Result == kConnectivity_Established ? "1" : "0");
        /* Thread interface down: forget the last reported role token, so a
         * genuine return to that same role after the interface comes back is
         * reported rather than swallowed as a repeat (design spec 2.2).
         *
         * This is the reset point because it is the only one CHIP actually
         * raises. kThreadInterfaceStateChange looks like the natural hook and
         * is not: it is declared and nothing anywhere in connectedhomeip
         * posts it, so a reset hung there would be dead code. Ordering is
         * safe: GenericThreadStackManagerImpl_OpenThread.hpp posts this event
         * from its own kThreadStateChange handling, so it is queued behind
         * the state change that caused it and the reset lands after that
         * transition's own bit-28 emit, never before it. */
        if (event->ThreadConnectivityChange.Result == kConnectivity_Lost)
        {
            s_last_role_tok_valid = false;
        }
        break;
    case DeviceEventType::kThreadStateChange:
        /* Bit 25 is coarse and unconditional; bit 28 below is additive. */
        mt_at_event(MT_EVT_THREAD_STATE_CHANGE, nullptr);
        /*
         * ThreadStateChange's four independent bool:1 bits (RoleChanged,
         * AddressChanged, NetDataChanged, ChildNodesChanged) are populated by
         * GenericThreadStackManagerImpl_OpenThread.hpp, RoleChanged as
         * (flags & OT_CHANGED_THREAD_ROLE) != 0. Gating on it means bit 28
         * fires on a routing-role transition only, never on address,
         * network-data or child-table churn.
         *
         * The gate is a C6 bench fix, not a design flourish: the first cut
         * described it in a comment and never tested the bit, and
         * registering one test prefix on the border router (pure netdata
         * churn, no role transition possible) produced four
         * +MTEVT:25/+MTEVT:28,ROUTER pairs while the role never left ROUTER.
         */
        if (event->ThreadStateChange.RoleChanged)
        {
            emit_role_if_changed();
        }
        break;
        /* Bit 26 (kThreadInterfaceStateChange) is deliberately absent: no SDK
         * posts that event, on either tree. The bit stays allocated in
         * mt_at.h and AT_MT_SPEC.md 3.11 regardless. */
#endif /* CHIP_DEVICE_CONFIG_ENABLE_THREAD */

    default:
        break;
    }
}

/* Bit 27 (transport mismatch) has no source here by design: it is raised once
 * at boot when a stored fabric was commissioned on a transport this image does
 * not provide, and these are single-transport images (design spec section 3).
 */

} // namespace

CHIP_ERROR mt_chip_events_register(void)
{
    if (s_registered)
    {
        return CHIP_NO_ERROR;
    }
    /* Order matters on a partial failure. SetAppDelegate() is a plain
     * assignment and AddFabricDelegate() walks its own linked list first and
     * answers CHIP_NO_ERROR without re-adding a delegate already on it, so a
     * retry after either fails is harmless; AddEventHandler() PREPENDS
     * unconditionally, so a second call after a failure below it would
     * dispatch every device event twice. It therefore goes last, and the
     * flag is set the moment it succeeds. */
    Server::GetInstance().GetCommissioningWindowManager().SetAppDelegate(&s_window_delegate);
    ReturnErrorOnFailure(Server::GetInstance().GetFabricTable().AddFabricDelegate(&s_fabric_delegate));
    ReturnErrorOnFailure(PlatformMgr().AddEventHandler(on_device_event, 0));
    s_registered = true;
    return CHIP_NO_ERROR;
}

void mt_chip_events_after_ready(void)
{
    /* The window opens inside Server::Init, before +MTREADY, so the
     * delegate's +MTEVT:0 was dropped by the s_at_up guard. Replay it once,
     * so the boot sequence is +MTREADY then exactly one +MTEVT:0 whenever a
     * window is open (the C6's block, main.cpp:6865). mt_matter_state()
     * takes and releases the stack lock; re-test the flag after it, because
     * any event dispatch in flight when the first test ran has completed by
     * the time the lock is granted. */
    if (!s_window_evt_sent && mt_matter_state() == MT_STATE_COMMISSIONING && !s_window_evt_sent)
    {
        if (mt_at_event(MT_EVT_COMMISSION_WINDOW_OPEN, nullptr))
        {
            s_window_evt_sent = true;
        }
    }
}
