/*
 * mt_composition.h - the endpoint composition: an ordered list of Matter
 * device type IDs describing which endpoints this device presents, each
 * with an optional variant byte (e.g. which cabinet flavour of a device
 * type an endpoint is).
 *
 * Pure C with no IDF dependency, so it is unit-testable on the host (see
 * test/host). Persistence lives in mt_comp_store.h; this file is only the
 * in-memory type and its wire encoding.
 *
 * Two wire formats, both little-endian so a blob written by one build is
 * readable by another:
 *
 *   v1 (legacy, decode-only): u16 count, then count * u32 device type IDs.
 *   No version byte at all. Every legal v1 composition has 0..16 endpoints,
 *   so byte0 (the low byte of count) is always <= MT_COMP_MAX_ENDPOINTS.
 *   That is the load-bearing fact that makes 0xFF safe as an unambiguous v2
 *   sentinel below: no legal v1 blob can ever start with it.
 *
 *   v2 (current, encode and decode): sentinel byte 0xFF, then a version
 *   byte, then u16 count, then per entry a u32 device type ID followed by
 *   one variant byte. mt_comp_encode() always writes v2; mt_comp_decode()
 *   dispatches on byte0 and accepts both, filling variant with 0 for a v1
 *   blob (a device with no variant ever stored has no variant to recover).
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

/* v2 blob discriminator: see the encoding note above. A v1 blob's first
 * byte is a count (0..16), so 0xFF is free to mark v2 unambiguously. */
#define MT_COMP_BLOB_V2_SENTINEL 0xFF
#define MT_COMP_BLOB_VERSION     0x02

/* Maximum encoded blob size: sentinel + version + u16 count, then up to 16
 * entries of (u32 device type ID + u8 variant). */
#define MT_COMP_BLOB_MAX (2 + 2 + 5 * MT_COMP_MAX_ENDPOINTS)

typedef struct {
    uint16_t count;
    uint32_t devtype[MT_COMP_MAX_ENDPOINTS];
    uint8_t  variant[MT_COMP_MAX_ENDPOINTS];
} mt_composition_t;

/*
 * Serialise comp into buf. Returns the number of bytes written, or -1 if
 * comp->count exceeds MT_COMP_MAX_ENDPOINTS or buf is too small.
 */
int mt_comp_encode(const mt_composition_t *comp, uint8_t *buf, size_t buf_len);

/*
 * Deserialise len bytes of buf into out. Accepts both v1 and v2 blobs (see
 * the encoding note above); a v1 blob decodes with every variant set to 0.
 * Returns 0 on success, or -1 if the buffer is truncated, declares more
 * endpoints than MT_COMP_MAX_ENDPOINTS, carries trailing bytes beyond the
 * declared count, or (v2 only) names a version byte other than
 * MT_COMP_BLOB_VERSION.
 */
int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out);

/* True when a and b have the same count, the same IDs in the same order,
 * and the same variant for each entry. */
bool mt_comp_equal(const mt_composition_t *a, const mt_composition_t *b);

#ifdef __cplusplus
}
#endif
