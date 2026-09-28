/* mt_chip_events.h - the +MTEVT URCs from stock CHIP, shared by the nRF54L15
 * and MGM240P ports. The C6 keeps its own mapping over esp-matter's forked
 * events (platform/esp32c6/main/main.cpp app_event_cb). Design:
 * superpowers/specs/2026-09-21-mtevt-thread-parity-design.md. */
#pragma once

#include <lib/core/CHIPError.h>

/* Register the device-event handler, the commissioning-window AppDelegate
 * and the fabric-table delegate. Call after chip::Server::Init and before
 * mt_at_start(), holding the CHIP stack lock. Idempotent: a second call
 * returns CHIP_NO_ERROR and registers nothing. */
CHIP_ERROR mt_chip_events_register(void);

/* The boot replay: if a commissioning window is open and its +MTEVT:0 was
 * dropped by the pre-+MTREADY guard, emit it now, once. Call right after
 * mt_at_start() with no lock held (it takes and releases the stack lock
 * through mt_matter_state()). */
void mt_chip_events_after_ready(void);
