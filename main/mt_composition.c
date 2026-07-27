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

    size_t need = 2u + 4u * (size_t)comp->count;
    if (buf_len < need) {
        return -1;
    }

    buf[0] = (uint8_t)(comp->count & 0xFF);
    buf[1] = (uint8_t)((comp->count >> 8) & 0xFF);

    for (uint16_t i = 0; i < comp->count; i++) {
        uint32_t v = comp->devtype[i];
        buf[2 + 4 * i + 0] = (uint8_t)(v & 0xFF);
        buf[2 + 4 * i + 1] = (uint8_t)((v >> 8) & 0xFF);
        buf[2 + 4 * i + 2] = (uint8_t)((v >> 16) & 0xFF);
        buf[2 + 4 * i + 3] = (uint8_t)((v >> 24) & 0xFF);
    }

    return (int)need;
}

int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out)
{
    if (!buf || !out || len < 2u) {
        return -1;
    }

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
    }

    return 0;
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
    }
    return true;
}
