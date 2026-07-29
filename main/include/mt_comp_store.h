/*
 * mt_comp_store.h - NVS persistence for the endpoint composition.
 *
 * A thin wrapper over the mt_composition codec. Kept separate so the codec
 * stays host-testable and this file stays small enough to read in one go.
 *
 * The composition lives in its own NVS namespace, distinct from the platform
 * namespace holding fabrics and attribute persistence, so clearing one does
 * not disturb the other.
 */

#pragma once

#include "mt_composition.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load the stored composition into out.
 * Returns  0 on success,
 *          1 when nothing is stored (a factory-fresh device),
 *         -1 on an NVS or decode failure.
 */
int mt_comp_store_load(mt_composition_t *out);

/* Persist comp. Returns 0 on success, -1 on an encode or NVS failure. */
int mt_comp_store_save(const mt_composition_t *comp);

/*
 * Erase the stored composition, returning the device to unconfigured. Backs
 * AT+MTFRESET; AT+MTRESET deliberately does not call this, since the
 * composition is a product definition rather than user data (spec section 3.7).
 *
 * Returns 0 on success or when nothing was stored, -1 on an NVS failure.
 */
int mt_comp_store_erase(void);

/*
 * The transport (MT_NET_WIFI / MT_NET_THREAD) the stored fabric was
 * commissioned on, kept in this namespace rather than the platform one so it
 * survives AT+MTRESET alongside the composition and is erased with it by
 * AT+MTFRESET.
 *
 * Reflashing between the WiFi and Thread images leaves NVS untouched by
 * design, so a device can hold a fabric that is perfectly valid and completely
 * unreachable: Matter fabric credentials are transport-independent, but the
 * device has no credentials for the new transport and a commissioner cannot
 * deliver any without first reaching it. Comparing this marker against the
 * compiled-in transport at boot is what lets the firmware notice (spec
 * section 3.12.1).
 *
 * Load returns 0 and writes *out on success, 1 when nothing is stored (which
 * is the normal state of a device that has never been commissioned, and of
 * every device built before this marker existed), -1 on an NVS failure.
 */
int mt_comp_store_load_transport(int *out);

/* Persist the transport. Returns 0 on success, -1 on an NVS failure. */
int mt_comp_store_save_transport(int transport);

#ifdef __cplusplus
}
#endif
