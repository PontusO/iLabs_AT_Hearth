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

#ifdef __cplusplus
}
#endif
