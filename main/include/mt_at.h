/*
 * mt_at.h - Matter AT+MT command interface entry point.
 *
 * Kept C++-safe (extern "C" guard, no at_core headers pulled in) so the C++
 * app entry point can start the AT interface without name-mangling issues.
 * The command handlers, engine config and the at_core includes stay inside
 * mt_at.c (compiled as C).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the AT+MT interface (AT UART + parser + command table) and emit
 * the "+MTREADY" boot marker. Call once at boot, after esp_matter::start().
 */
void mt_at_start(void);

/*
 * Emit a single URC line (e.g. "+MTCOMMISSION:COMPLETE") over the AT link.
 * Safe to call from the Matter task; used by the C++ event callbacks.
 */
void mt_at_urc(const char *line);

#ifdef __cplusplus
}
#endif
