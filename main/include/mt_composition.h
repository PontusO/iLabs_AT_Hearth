/*
 * mt_composition.h - the endpoint composition: an ordered list of Matter
 * device type IDs describing which endpoints this device presents.
 *
 * Pure C with no IDF dependency, so it is unit-testable on the host (see
 * test/host). Persistence lives in mt_comp_store.h; this file is only the
 * in-memory type and its wire encoding.
 *
 * Encoding is explicitly little-endian so a blob written by one build is
 * readable by another: u16 count, then count * u32 device type IDs.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum endpoints in a composition (RAM-driven cap).
 *
 * CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT must be at least this, or
 * endpoint::create() refuses everything past the cap and the composition is
 * silently truncated. See sdkconfig.defaults. */
#define MT_COMP_MAX_ENDPOINTS 16

/* Maximum encoded blob size: u16 count + up to 16 u32 device type IDs. */
#define MT_COMP_BLOB_MAX (2 + 4 * MT_COMP_MAX_ENDPOINTS)

typedef struct {
    uint16_t count;
    uint32_t devtype[MT_COMP_MAX_ENDPOINTS];
} mt_composition_t;

/*
 * Serialise comp into buf. Returns the number of bytes written, or -1 if
 * comp->count exceeds MT_COMP_MAX_ENDPOINTS or buf is too small.
 */
int mt_comp_encode(const mt_composition_t *comp, uint8_t *buf, size_t buf_len);

/*
 * Deserialise len bytes of buf into out. Returns 0 on success, or -1 if the
 * buffer is truncated, declares more endpoints than MT_COMP_MAX_ENDPOINTS,
 * or carries trailing bytes beyond the declared count.
 */
int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out);

/* True when a and b have the same count and the same IDs in the same order. */
bool mt_comp_equal(const mt_composition_t *a, const mt_composition_t *b);

#ifdef __cplusplus
}
#endif
