/*
 * mt_at.h - Matter AT+MT command interface entry point.
 *
 * Kept C++-safe (extern "C" guard, no at_core headers pulled in) so the C++
 * app entry point can start the AT interface without name-mangling issues.
 * The command handlers, engine config and the at_core includes stay inside
 * mt_at.c (compiled as C).
 */

#pragma once

#include <stdbool.h>

#include <stdint.h>

/* mt_row_stage_t, for the inbound (fabric-originated) row stage at the end
 * of this header. Pure C with no IDF dependency, so it costs a C++ caller
 * nothing. */
#include "mt_rows.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the AT+MT interface (AT UART + parser + command table) and emit
 * the "+MTREADY" boot marker. Call once at boot, after esp_matter::start().
 */
void mt_at_start(void);

/*
 * Emit a single URC line (e.g. "+MTIDENT:1,1") over the AT link.
 * Safe to call from the Matter task; used by the C++ event callbacks.
 *
 * Returns true if the line reached the wire, false if it was dropped because
 * the AT interface is not up yet. Callers may ignore it; the one that must not
 * is app_main's boot replay, which has to know whether an event it is about to
 * repeat has already been delivered.
 */
bool mt_at_urc(const char *line);

/* ---- event surface (C3) ----------------------------------------------- *
 * Platform events reach the host as "+MTEVT:<bit>[,<detail>]", filtered by a
 * 32-bit subscription mask the host sets with AT+MTEVT=<hexmask>. Bits are
 * grouped so a host can enable a whole class cheaply.                      */

enum {
    /* Commissioning (bits 0-5, the default mask). */
    MT_EVT_COMMISSION_WINDOW_OPEN     = 0,
    MT_EVT_COMMISSION_SESSION_STARTED = 1,
    MT_EVT_COMMISSION_SESSION_STOPPED = 2,
    MT_EVT_COMMISSION_COMPLETE        = 3,
    MT_EVT_COMMISSION_WINDOW_CLOSED   = 4,
    MT_EVT_FAIL_SAFE_EXPIRED          = 5,

    /* Fabric (bits 6-9). */
    MT_EVT_FABRIC_WILL_BE_REMOVED     = 6,
    MT_EVT_FABRIC_REMOVED             = 7,
    MT_EVT_FABRIC_COMMITTED           = 8,
    MT_EVT_FABRIC_UPDATED             = 9,

    /* Connectivity (bits 10-15). */
    MT_EVT_WIFI_CONNECTIVITY          = 10,
    MT_EVT_INTERNET_CONNECTIVITY      = 11,
    MT_EVT_INTERFACE_IP_CHANGED       = 12,
    MT_EVT_OPERATIONAL_NETWORK_STARTED = 13,
    MT_EVT_DNSSD_INITIALIZED          = 14,
    MT_EVT_SERVER_READY               = 15,

    /* BLE (bits 16-19). */
    MT_EVT_BLE_CONNECTED              = 16,
    MT_EVT_BLE_DISCONNECTED           = 17,
    MT_EVT_BLE_ADVERTISING_CHANGE     = 18,
    MT_EVT_BLE_DEINITIALIZED          = 19,

    /* Misc (bits 20-23). */
    MT_EVT_OTA_STATE_CHANGED          = 20,
    MT_EVT_BINDINGS_CHANGED           = 21,
    MT_EVT_TIME_SYNC_CHANGE           = 22,

    /* Thread (bits 24-26). Allocated but never emitted on a WiFi image: the
     * transport is a build-time choice, and fixing the layout now avoids
     * renumbering a published mask when a Thread build arrives. */
    MT_EVT_THREAD_CONNECTIVITY        = 24,
    MT_EVT_THREAD_STATE_CHANGE        = 25,
    /* Never emitted on ANY image, WiFi or Thread: CHIP declares
     * DeviceEventType::kThreadInterfaceStateChange (CHIPDeviceEvent.h:189)
     * but nothing in the pinned SDK ever posts it, confirmed by an
     * unfiltered grep of the whole connectedhomeip tree turning up only the
     * declaration. The bit stays allocated regardless (see the block
     * comment above and AT_MT_SPEC.md 3.11). */
    MT_EVT_THREAD_IF_STATE_CHANGE     = 26,

    /* Device state (bit 27). Raised once at boot when the stored fabric was
     * commissioned on a different transport than this image provides, so the
     * device holds credentials it cannot use (spec 3.12.1). Bit 27 rather than
     * the nominally reserved 23, which the host library already maps to
     * MATTER_ESP32_SPECIFIC_EVENT. */
    MT_EVT_TRANSPORT_MISMATCH         = 27,

    /* Thread role change (bit 28, 0.11.0). Registered here rather than folded
     * into the bits 24-26 Thread group above: that group predates
     * AT+MTTHREAD?/mt_matter_thread_info() and was already numbered before
     * this event existed, and bit 27 sits between them, so renumbering would
     * touch a published mask for no gain. Fires only on a routing-role
     * transition (CHIP's own ThreadStateChange.RoleChanged bit, never on
     * address/network-data/child-table churn), payload is the same decoded
     * <role> token AT+MTTHREAD? reports (design spec 2.2). Bits 29-31 remain
     * free in the 32-bit mask. */
    MT_EVT_THREAD_ROLE_CHANGED        = 28,
};

/*
 * Commissioning group (bits 0-5), what the firmware emitted before the mask
 * existed, PLUS bit 27.
 *
 * Bit 27 is in the default out of necessity rather than preference. The mask
 * lives in RAM and resets to this value on every boot, so an event that only
 * ever fires during boot can never be subscribed to in time: there is no
 * moment at which a host could have set a mask that would catch it. Bit 27 is
 * the only such event in the protocol, so excluding it would not make it
 * optional, it would make it unreachable. Verified the hard way on 2026-07-29,
 * when AT+MTEVT=0xFFFFFFFF followed by a reset produced no +MTEVT:27, because
 * the reset discarded the mask that had just been set.
 */
#define MT_EVT_MASK_DEFAULT  0x0800003FU

/*
 * Emit "+MTEVT:<bit>" (or "+MTEVT:<bit>,<detail>" when detail is non-NULL),
 * if the host has subscribed to that bit. Safe to call from the Matter task.
 *
 * Returns true only if the line actually reached the host: false when the bit
 * is not in the subscription mask, and false when the AT interface is not up
 * yet. Both are ordinary outcomes rather than errors.
 */
bool mt_at_event(int bit, const char *detail);

/* ---- command forwarding (the verdict mailbox, C1) ---------------------- *
 * Some Matter commands need an app-level decision the firmware cannot make
 * on its own (e.g. a door lock's LockDoor). Those are forwarded to the host
 * and answered within a deadline; see mt_cmdbox.h for the slot state
 * machine backing this.                                                    */

/*
 * Forward a Matter command needing an app-level decision to the host, and
 * block for its verdict. Raises "+MTCMD:<seq>,<ep>,<cluster>,<command>" as a
 * URC, waits up to 1000 ms for the host to answer with AT+MTCMDRESP, and
 * returns the verdict: true only for an explicit allow (verdict 1) that
 * arrives inside the window. Every other outcome, a timeout, the AT link
 * not being up yet, or an unregistered host callback, returns false. A lock
 * fails closed. Callers run on the CHIP event loop task, either directly
 * from an ember cluster callback (the door lock's LockDoor/UnlockDoor) or
 * from a Delegate method the SDK calls synchronously from that same task
 * (the water valve's HandleOpenValve/HandleCloseValve, seven-type batch
 * task C2, cluster 0x0081 commands 0/1: unlike the door lock, its verdict
 * cannot fail the command on the wire, since the SDK discards what the
 * delegate returns and answers Success regardless, see AT_MT_SPEC.md 3.19;
 * the OperationalState trio's HandlePause/Resume/Start/StopStateCallback,
 * seven-type batch task C4, cluster 0x0060 commands 0-3, are the same
 * Delegate-method shape but the OPPOSITE of the valve on this point: the
 * filled GenericOperationalError becomes the command response directly, so
 * here a deny genuinely does fail the command on the wire, see
 * AT_MT_SPEC.md 3.21); see mt_at.c's cmd_mtcmdresp handler for why the
 * reply path must never take the CHIP stack lock.
 */
bool mt_cmd_forward(uint16_t ep, uint32_t cluster, uint32_t command);

/*
 * Identical to mt_cmd_forward(), plus one single trailing payload field on
 * the wire: raises "+MTCMD:<seq>,<ep>,<cluster>,<command>,<payload>" and
 * blocks the same way for the same verdict. AT_MT_SPEC.md 3.17's arity
 * table documents this as the one-field case of the general documented-
 * arity tail, so existing parsers, which read four fields and ignore
 * anything past the fourth comma, keep working unchanged. First consumer:
 * chime's PlayChimeSound chimeID (C6/C7).
 */
bool mt_cmd_forward_payload(uint16_t ep, uint32_t cluster, uint32_t command, uint32_t payload);

/*
 * The general form behind mt_cmd_forward() and mt_cmd_forward_payload():
 * raises "+MTCMD:<seq>,<ep>,<cluster>,<command>[,<fields>]" and blocks the
 * same way for the same verdict. fields is a caller-formatted, comma-joined
 * tail, not a value this function parses or counts: the caller decides how
 * many fields the command's wire form carries and joins them itself (for
 * example "1,30,80,1"), and renders an absent optional field as an empty
 * string between its two commas (for example "1,,80,") rather than omitting
 * the field position. NULL or an empty string means no tail at all, the same
 * "+MTCMD:<seq>,<ep>,<cluster>,<command>" shape mt_cmd_forward() raises.
 * Arity is documented per command in AT_MT_SPEC.md §3.17, not enforced here.
 * First consumers: RVC and microwave oven command forwards (Tasks 3-4 of the
 * RVC + microwave batch).
 */
bool mt_cmd_forward_fields(uint16_t ep, uint32_t cluster, uint32_t command, const char *fields);

/*
 * Raise a notify-only "+MTCMD:0,<ep>,<cluster>,<command>" URC and return
 * immediately: no mailbox slot, no semaphore, no blocking. seq 0 is
 * reserved for this form (mt_cmdbox_open() never issues it, and
 * mt_cmdbox_answer(0, ...) is rejected structurally), so
 * AT+MTCMDRESP=0,... always answers +MTERR:1: there is nothing pending to
 * answer. Safe to call from the CHIP task for a command whose ember
 * callback is void, with no result to report back up the stack. First
 * consumer: SelfTestRequest.
 */
void mt_cmd_notify(uint16_t ep, uint32_t cluster, uint32_t command);

/* ---- the inbound row stage (energy round C2, task 6) ------------------- *
 *
 * A fabric-originated command that carries a MULTI-ROW payload (today only
 * EnergyEvse SetTargets) cannot put that payload in its "+MTCMD" line: a
 * full charging schedule is 70 rows, about 2450 bytes, and MT_CMD_LINE_MAX
 * is 112 with snprintf truncating silently. Pushing it as unsolicited URCs
 * while the sketch is inside loop() is defect B166 at scale, which is the
 * one failure this protocol has already been bitten by on hardware. So the
 * rows are PULLED, not pushed: the delegate stages them here, the "+MTCMD"
 * line carries a row count and one kind-specific scalar, and the host
 * fetches the rows with AT+MTROWGET inside a command where it is doing
 * nothing but draining the response.
 *
 * The fetch must NAME THE SEQUENCE NUMBER from the "+MTCMD" line
 * (AT+MTROWGET=<ep>,<kind>,,<seq> for all of them,
 * AT+MTROWGET=<ep>,<kind>,<idx>,<seq> for one). An unqualified AT+MTROWGET
 * always reads the live store, never the proposal. See cmd_mtrowget() in
 * mt_at.c for why that is enforced in the protocol rather than asked for in
 * the specification.
 *
 * These three functions are called from the CHIP event loop task, in this
 * order, exactly once each per forwarded command:
 *
 *   1. mt_rows_inbound_claim(ep, kind)
 *                                take the buffer, fill it with mt_rows_stage()
 *   2. mt_rows_inbound_forward() raise the +MTCMD, block for the verdict
 *   3. mt_rows_inbound_release() give the buffer back, ALWAYS
 *
 * Between 2 and 3 the caller may read the stage (that is where the merge
 * happens on an allow); it must not write it.
 *
 * ---- WHY THIS IS A SEPARATE BUFFER FROM THE HOST'S OWN STAGE ----
 *
 * The AT+MTROW family stages the HOST's row sets in its own file-static
 * buffer, written only by the AT parser task. This one is written only by
 * the CHIP event loop task. They are deliberately not the same buffer, and
 * the ~5.6 KB that costs buys the elimination of a defect class that
 * nothing but a bench run could ever have caught: sharing one buffer means
 * a controller's SetTargets overwrites whatever set the host happens to
 * have half-uploaded for the same (ep, kind), and if the two happen to hold
 * the same number of rows, the host's next AT+MTROWAPPLY passes every check
 * and commits THE CONTROLLER'S rows as if they were its own. Separate
 * buffers make that unrepresentable rather than merely unlikely, and they
 * leave cmd_mtrowapply()'s existing unlocked pass of the host stage by
 * pointer exactly as safe as it was before this task, because the second
 * writer it was warned about never materialised in that buffer.
 */

/*
 * Take ownership of the inbound row stage and return it, emptied and
 * stamped with (ep, kind), ready to be filled with mt_rows_stage(). The
 * identity is stamped at claim time so that a set of ZERO rows is still
 * addressable by AT+MTROWGET; staging no rows at all is a legitimate
 * outcome (a controller may send an empty schedule list) and the host must
 * be able to fetch "nothing" as promptly as it fetches seventy rows.
 *
 * Returns NULL if the stage is already owned by another forward, or if the
 * AT parser task is streaming a previous set out; the caller must then
 * refuse the command (Status::Busy is the honest answer) rather than
 * proceed.
 */
mt_row_stage_t *mt_rows_inbound_claim(uint16_t ep, uint8_t kind);

/*
 * Publish the staged set and forward the command: raises
 * "+MTCMD:<seq>,<ep>,<cluster>,<command>,<rowcount>,<aux>", where <rowcount>
 * is read from the stage itself so the count on the wire can never disagree
 * with what AT+MTROWGET will serve, then blocks for the host's verdict.
 * Returns true on allow, false on deny or timeout (the timeout also raises
 * "+MTCMDTO:<seq>", exactly as a scalar forward does).
 *
 * aux is one kind-specific scalar whose meaning belongs to
 * (cluster, command), the same way every other +MTCMD tail field's does. It
 * exists because a row count is not always enough to describe a proposal:
 * EVSE SetTargets passes the affected-DAY mask, since a day whose targets
 * are being deleted contributes no row and would otherwise be invisible to
 * the host adjudicating it.
 *
 * The window is 3000 ms, not the 1000 ms every scalar forward uses, because
 * this is the only forward whose verdict requires a ROUND TRIP first: the
 * host has to pull the rows before it can judge them. The host library's
 * HearthLink::command() refuses a re-entrant call, so a sketch that
 * receives the +MTCMD inside its URC dispatch cannot fetch from there and
 * has to defer to the next loop() iteration. THE CONSEQUENCE A SKETCH
 * AUTHOR MUST KNOW: a loop() iteration that blocks for more than roughly
 * 2.7 seconds (3000 ms less the fetch and the two line turnarounds) has its
 * schedules DENIED. Denied, not silently mis-applied: the deny path returns
 * a failure status to the controller and leaves the stored schedule
 * byte-identical, so a slow sketch loses the update visibly.
 *
 * <rowcount> may be 0. The row kind is not on the wire: it is implied by
 * (cluster, command), which is how the host knows which AT+MTROWGET <kind>
 * to ask for.
 */
bool mt_rows_inbound_forward(uint16_t ep, uint32_t cluster, uint32_t command, uint32_t aux);

/*
 * Give the inbound stage back. Must be called on every path out of the
 * command handler, including the ones that never forwarded: nothing else
 * ever frees the claim, and a leaked claim makes every later forward on any
 * endpoint answer Busy until reboot.
 */
void mt_rows_inbound_release(void);

#ifdef __cplusplus
}
#endif
