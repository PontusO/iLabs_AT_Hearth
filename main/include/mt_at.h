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
    MT_EVT_THREAD_IF_STATE_CHANGE     = 26,

    /* Device state (bit 27). Raised once at boot when the stored fabric was
     * commissioned on a different transport than this image provides, so the
     * device holds credentials it cannot use (spec 3.12.1). Bit 27 rather than
     * the nominally reserved 23, which the host library already maps to
     * MATTER_ESP32_SPECIFIC_EVENT. */
    MT_EVT_TRANSPORT_MISMATCH         = 27,
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
 * Identical to mt_cmd_forward(), plus one reserved fifth payload field on
 * the wire: raises "+MTCMD:<seq>,<ep>,<cluster>,<command>,<payload>" and
 * blocks the same way for the same verdict. The forwarding spec reserved
 * this exact extension so existing parsers, which read four fields and
 * ignore anything past the fourth comma, keep working unchanged. First
 * consumer: chime's PlayChimeSound chimeID (C6/C7).
 */
bool mt_cmd_forward_payload(uint16_t ep, uint32_t cluster, uint32_t command, uint32_t payload);

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

#ifdef __cplusplus
}
#endif
