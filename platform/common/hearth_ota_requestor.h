/*
 * hearth_ota_requestor.h - the CHIP-level half of firmware over the air,
 * shared by every port: the requestor, driver, downloader and the Hearth
 * image processor that relays blocks to the core (mt_ota.h). SDK-free: only
 * CHIP headers, and this header itself pulls in none of them.
 *
 * Each port calls ota_requestor_init() once its CHIP server is up. On the
 * ESP32-C6 that is straight after esp_matter::start() returns (the Server is
 * fully initialised by then, chip_init() blocks on it). esp-matter would wire
 * a requestor of its own from its kDnssdInitialized handler; an SDK patch
 * turns that off when the C6 port defines mt_app_owns_ota_requestor() (B665,
 * which showed being first was a race, not a guarantee). See
 * hearth_ota_esp.cpp and the note in main.cpp's app_main.
 */
#pragma once

#include <stdint.h>

namespace hearth {

/* Wire the requestor and register it with chip::SetRequestorInstance().
 * Idempotent; a no-op if an instance already exists. Matter thread, or any
 * thread holding the CHIP stack lock. */
void ota_requestor_init();

/* The port calls this once the CHIP server is up and the network is usable
 * (C6 and MG24: a settle time after an address or a Thread attach; nRF: a
 * lambda queued behind the driver's). It is where a first run of a freshly
 * applied bundle tells the provider, which cannot be done at wiring time
 * because that happens before the radio is up. Cheap and safe to call on
 * every such event; it does nothing unless a notification is owed. */
void ota_requestor_server_ready();

/* The relay's view of AT+MTOTA's mode, for the port's event handlers. */
bool ota_mode_enabled();

/* The persisted product version the port's ConfigurationManager serves; the
 * port sets these once it can read the key-value store (hearth_swver_load),
 * and mt_matter_swver_set updates them live. */
struct SoftwareVersion {
    bool have = false;
    uint32_t version = 0;
    char str[32] = "";
};
SoftwareVersion &software_version();

} // namespace hearth
