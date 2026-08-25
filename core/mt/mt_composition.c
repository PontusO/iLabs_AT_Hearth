/*
 * mt_composition.c - encode, decode and compare endpoint compositions.
 *
 * Deliberately free of IDF dependencies so the codec can be unit-tested on
 * the host (test/host). Persistence is mt_comp_store.c's job.
 */

#include <string.h>

#include "mt_composition.h"

int mt_comp_encode(const mt_composition_t *comp, uint8_t *buf, size_t buf_len)
{
    if (!comp || !buf || comp->count > MT_COMP_MAX_ENDPOINTS) {
        return -1;
    }

    /* Always v3: sentinel + version + u16 count, then per entry a u32 id
     * followed by a variant byte and a parent byte. */
    size_t need = 4u + 6u * (size_t)comp->count;
    if (buf_len < need) {
        return -1;
    }

    buf[0] = MT_COMP_BLOB_V2_SENTINEL;
    buf[1] = MT_COMP_BLOB_VERSION;
    buf[2] = (uint8_t)(comp->count & 0xFF);
    buf[3] = (uint8_t)((comp->count >> 8) & 0xFF);

    for (uint16_t i = 0; i < comp->count; i++) {
        uint32_t v = comp->devtype[i];
        size_t off = 4u + 6u * (size_t)i;
        buf[off + 0] = (uint8_t)(v & 0xFF);
        buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        buf[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        buf[off + 3] = (uint8_t)((v >> 24) & 0xFF);
        buf[off + 4] = comp->variant[i];
        buf[off + 5] = comp->parent[i];
    }

    return (int)need;
}

/* Decode a v1 (legacy) blob: u16 count, then count * u32 device type IDs,
 * no version byte, variants all zero. Caller has already confirmed byte0
 * is a legal v1 discriminator (<= MT_COMP_MAX_ENDPOINTS). */
static int decode_v1(const uint8_t *buf, size_t len, mt_composition_t *out)
{
    uint16_t count = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    if (count > MT_COMP_MAX_ENDPOINTS) {
        return -1;
    }

    /* Exact length: a truncated blob and a blob with trailing bytes are both
     * corruption, and silently accepting either would build the wrong data
     * model on a commissioned device. */
    if (len != 2u + 4u * (size_t)count) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->count = count;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = &buf[2 + 4 * i];
        out->devtype[i] = (uint32_t)p[0]
                        | ((uint32_t)p[1] << 8)
                        | ((uint32_t)p[2] << 16)
                        | ((uint32_t)p[3] << 24);
        /* variant already 0 from the memset above */
    }

    memset(out->parent, MT_COMP_NO_PARENT, sizeof(out->parent));

    return 0;
}

/* Decode a v2 blob: sentinel already matched by the caller. */
static int decode_v2(const uint8_t *buf, size_t len, mt_composition_t *out)
{
    if (len < 4u) {
        return -1;
    }
    if (buf[1] != MT_COMP_BLOB_VERSION_V2) {
        return -1;
    }

    uint16_t count = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    if (count > MT_COMP_MAX_ENDPOINTS) {
        return -1;
    }

    if (len != 4u + 5u * (size_t)count) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->count = count;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = &buf[4 + 5 * i];
        out->devtype[i] = (uint32_t)p[0]
                        | ((uint32_t)p[1] << 8)
                        | ((uint32_t)p[2] << 16)
                        | ((uint32_t)p[3] << 24);
        out->variant[i] = p[4];
    }

    memset(out->parent, MT_COMP_NO_PARENT, sizeof(out->parent));

    return 0;
}

/* Decode a v3 blob: sentinel already matched by the caller. */
static int decode_v3(const uint8_t *buf, size_t len, mt_composition_t *out)
{
    if (len < 4u) {
        return -1;
    }
    if (buf[1] != MT_COMP_BLOB_VERSION) {
        return -1;
    }

    uint16_t count = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    if (count > MT_COMP_MAX_ENDPOINTS) {
        return -1;
    }

    if (len != 4u + 6u * (size_t)count) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->count = count;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = &buf[4 + 6 * i];
        out->devtype[i] = (uint32_t)p[0]
                        | ((uint32_t)p[1] << 8)
                        | ((uint32_t)p[2] << 16)
                        | ((uint32_t)p[3] << 24);
        out->variant[i] = p[4];
        uint8_t parent = p[5];

        /* Parent must be either MT_COMP_NO_PARENT or a lower endpoint index.
         * This enforces a data model ordering: composed endpoints come after
         * their parents. */
        if (parent != MT_COMP_NO_PARENT && parent >= i) {
            return -1;
        }
        out->parent[i] = parent;
    }

    return 0;
}

int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out)
{
    if (!buf || !out || len < 2u) {
        return -1;
    }

    if (buf[0] == MT_COMP_BLOB_V2_SENTINEL) {
        /* Dispatch on version byte: 0x02 is v2, 0x03 is v3. */
        if (buf[1] == MT_COMP_BLOB_VERSION_V2) {
            return decode_v2(buf, len, out);
        }
        if (buf[1] == MT_COMP_BLOB_VERSION) {
            return decode_v3(buf, len, out);
        }
        /* Neither v2 nor v3: unknown version. */
        return -1;
    }

    if (buf[0] <= MT_COMP_MAX_ENDPOINTS) {
        return decode_v1(buf, len, out);
    }

    /* Neither a legal v1 count nor the v2/v3 sentinel: not a blob this decoder
     * has ever written or is willing to guess at. */
    return -1;
}

bool mt_comp_equal(const mt_composition_t *a, const mt_composition_t *b)
{
    if (!a || !b || a->count != b->count) {
        return false;
    }
    for (uint16_t i = 0; i < a->count; i++) {
        if (a->devtype[i] != b->devtype[i]) {
            return false;
        }
        if (a->variant[i] != b->variant[i]) {
            return false;
        }
        if (a->parent[i] != b->parent[i]) {
            return false;
        }
    }
    return true;
}
