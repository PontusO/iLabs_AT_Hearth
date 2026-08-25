/*
 * mt_transport.h - runtime transport selection for the combined WiFi+Thread
 * image.
 *
 * Exists only because the combined-image build carries both stacks and
 * needs one persisted choice between them, read once at boot before
 * esp_matter::start() so command registration and stack launch key on the
 * same value for the whole boot (see mt_transport_latch_active()). Pure C,
 * no esp_matter/CHIP headers: mt_at.c's C-only rule extends to every C file
 * in this app. mt_transport_parse() and mt_transport_name() have no NVS
 * dependency either and are host-testable (see test/host).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MT_TRANSPORT_WIFI   = 0,
    MT_TRANSPORT_THREAD = 1,
} mt_transport_t;

/*
 * Parse an exact, upper-case "WIFI" or "THREAD" into *out, matching the
 * AT+MT grammar's upper-case convention. Returns 0 on success, -1 for
 * anything else: NULL, empty, lower-case, or an unknown token.
 */
int mt_transport_parse(const char *arg, mt_transport_t *out);

/* "WIFI" or "THREAD". */
const char *mt_transport_name(mt_transport_t t);

/*
 * The persisted choice (NVS namespace "mt_cfg", key "transport").
 * Returns MT_TRANSPORT_WIFI when nothing is stored yet or on any read
 * failure, so a factory-fresh or corrupt device boots WiFi rather than
 * refusing to boot.
 */
mt_transport_t mt_transport_stored(void);

/* Persist t. Returns 0 on success, -1 on an NVS failure. */
int mt_transport_store(mt_transport_t t);

/*
 * Read mt_transport_stored() once into a static latch. Call exactly once,
 * from app_main, before esp_matter::start(): AT+MTTRANSPORT= can change the
 * stored value at any time while the device is running, but registration
 * and stack launch must both key on whatever the value was AT BOOT, not on
 * whatever it has since become.
 */
void mt_transport_latch_active(void);

/*
 * The latched value. Returns MT_TRANSPORT_WIFI if mt_transport_latch_active()
 * has not run yet; this does not happen in practice, since app_main latches
 * before anything can query.
 */
mt_transport_t mt_transport_active(void);

/*
 * The weak hook the pinned esp-matter patchset calls
 * (sdk-patches/esp-matter/0001-hearth-runtime-transport-selection.patch).
 * Defined in mt_transport.c, in plain C with no SDK headers. Compiled into
 * every image; on a single-stack build the SDK never calls it and the
 * linker keeps or drops the unreferenced symbol harmlessly.
 */
int mt_active_transport_is_thread(void);

#ifdef __cplusplus
}
#endif
