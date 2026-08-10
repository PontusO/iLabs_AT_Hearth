/*
 * mt_composition.h - the endpoint composition: an ordered list of Matter
 * device type IDs describing which endpoints this device presents, each
 * with an optional variant byte (e.g. which cabinet flavour of a device
 * type an endpoint is) and a parent endpoint index for composed devices.
 *
 * Pure C with no IDF dependency, so it is unit-testable on the host (see
 * test/host). Persistence lives in mt_comp_store.h; this file is only the
 * in-memory type and its wire encoding.
 *
 * Three wire formats, all little-endian so a blob written by one build is
 * readable by another:
 *
 *   v1 (legacy, decode-only): u16 count, then count * u32 device type IDs.
 *   No version byte at all. Every legal v1 composition has 0..16 endpoints,
 *   so byte0 (the low byte of count) is always <= MT_COMP_MAX_ENDPOINTS.
 *   That is the load-bearing fact that makes 0xFF safe as an unambiguous v2
 *   sentinel below: no legal v1 blob can ever start with it.
 *
 *   v2 (encode and decode): sentinel byte 0xFF, then a version byte 0x02,
 *   then u16 count, then per entry a u32 device type ID followed by one
 *   variant byte. mt_comp_encode() accepts v2 for decode (filling parent with
 *   MT_COMP_NO_PARENT).
 *
 *   v3 (current, encode and decode): sentinel byte 0xFF, then a version byte
 *   0x03, then u16 count, then per entry a u32 device type ID, one variant
 *   byte, and one parent index byte. mt_comp_encode() always writes v3;
 *   mt_comp_decode() dispatches on byte0 and accepts v1, v2, and v3. Parent
 *   must be either MT_COMP_NO_PARENT (0xFF) or an endpoint index lower than
 *   the current position (enforced on decode).
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

/* v3 blob discriminator: see the encoding note above. A v1 blob's first
 * byte is a count (0..16), so 0xFF is free to mark v2/v3 unambiguously. */
#define MT_COMP_BLOB_V2_SENTINEL 0xFF
#define MT_COMP_BLOB_VERSION_V2  0x02
#define MT_COMP_BLOB_VERSION     0x03

/* Parent index for root endpoints (not a child of any other endpoint). */
#define MT_COMP_NO_PARENT        0xFF

/* Maximum encoded blob size: sentinel + version + u16 count, then up to 16
 * entries of (u32 device type ID + u8 variant + u8 parent). */
#define MT_COMP_BLOB_MAX (2 + 2 + 6 * MT_COMP_MAX_ENDPOINTS)

typedef struct {
    uint16_t count;
    uint32_t devtype[MT_COMP_MAX_ENDPOINTS];
    uint8_t  variant[MT_COMP_MAX_ENDPOINTS];
    uint8_t  parent[MT_COMP_MAX_ENDPOINTS];
} mt_composition_t;

/*
 * Serialise comp into buf. Returns the number of bytes written, or -1 if
 * comp->count exceeds MT_COMP_MAX_ENDPOINTS or buf is too small.
 */
int mt_comp_encode(const mt_composition_t *comp, uint8_t *buf, size_t buf_len);

/*
 * Deserialise len bytes of buf into out. Accepts v1, v2, and v3 blobs (see
 * the encoding note above). v1 blobs decode with every variant and parent
 * set to 0 and MT_COMP_NO_PARENT respectively. v2 blobs decode with every
 * parent set to MT_COMP_NO_PARENT. Returns 0 on success, or -1 if the buffer
 * is truncated, declares more endpoints than MT_COMP_MAX_ENDPOINTS, carries
 * trailing bytes beyond the declared count, names a version byte other than
 * MT_COMP_BLOB_VERSION_V2 or MT_COMP_BLOB_VERSION after the sentinel, or (v3
 * only) declares a parent index >= its own position or outside 0x00..0xFE.
 */
int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out);

/* True when a and b have the same count, the same IDs in the same order,
 * the same variant for each entry, and the same parent for each entry. */
bool mt_comp_equal(const mt_composition_t *a, const mt_composition_t *b);

#ifdef __cplusplus
}
#endif
