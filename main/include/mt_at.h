/*
 * mt_at.h - Matter AT+MT command interface entry point.
 *
 * Kept C++-safe (extern "C" guard, no at_core headers pulled in) so the C++
 * app entry point can start the AT interface without name-mangling issues.
 * The command handlers, engine config and the at_core includes stay inside
 * mt_at.c (compiled as C).
 */

#pragma once

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
 */
void mt_at_urc(const char *line);

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

/* Commissioning group only: what the firmware emitted before the mask existed. */
#define MT_EVT_MASK_DEFAULT  0x0000003FU

/*
 * Emit "+MTEVT:<bit>" (or "+MTEVT:<bit>,<detail>" when detail is non-NULL),
 * if the host has subscribed to that bit. Safe to call from the Matter task.
 */
void mt_at_event(int bit, const char *detail);

#ifdef __cplusplus
}
#endif
