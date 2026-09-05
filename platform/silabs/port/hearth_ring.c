/*
 * hearth_ring.c - byte ring for the AT link RX path. Single producer (the
 * EUSART RX ISR), single consumer (the parser task), so head/tail need no
 * lock: the producer only writes head, the consumer only writes tail, and
 * each reads the other's index once per operation. Capacity is a power of
 * two so index wrap is a mask.
 */

#include <string.h>

#include "hearth_ring.h"

void hearth_ring_init(hearth_ring_t *r, uint8_t *storage, uint32_t cap_pow2)
{
    r->buf = storage;
    r->cap = cap_pow2;
    r->head = r->tail = 0;
    r->dropped = 0;
}

uint32_t hearth_ring_count(const hearth_ring_t *r)
{
    return (r->head - r->tail) & (r->cap - 1);
}

bool hearth_ring_put(hearth_ring_t *r, uint8_t b)
{
    uint32_t next = (r->head + 1) & (r->cap - 1);
    if (next == r->tail) {          /* full: keep what is queued, count the loss */
        r->dropped++;
        return false;
    }
    r->buf[r->head] = b;
    r->head = next;
    return true;
}

uint32_t hearth_ring_get(hearth_ring_t *r, uint8_t *out, uint32_t n)
{
    uint32_t got = 0;
    while (got < n && r->tail != r->head) {
        out[got++] = r->buf[r->tail];
        r->tail = (r->tail + 1) & (r->cap - 1);
    }
    return got;
}

void hearth_ring_reset(hearth_ring_t *r)
{
    r->tail = r->head;
}

/*
 * The contract (hearth_port.h, hearth_link_read): return when len bytes
 * are accumulated or the deadline passes; timeout_ms 0 is one non-blocking
 * attempt; timeouts up to 3,600,000 ms must be honoured with a real
 * blocking wait, no busy loop and no overflow. Remaining time is recomputed
 * each lap in uint32 ms (a deadline of now + timeout cannot overflow a
 * uint32 tick count wrap because both sides use the same wrapping
 * arithmetic: (deadline - now) is the remaining span while it is smaller
 * than half the range, which 3,600,000 ms is by a wide margin).
 */
int hearth_ring_read_deadline(hearth_ring_t *r, uint8_t *buf, uint32_t len,
                              uint32_t timeout_ms,
                              bool (*wait_fn)(uint32_t ms), uint32_t (*now_fn)(void))
{
    uint32_t got = hearth_ring_get(r, buf, len);
    if (got >= len || timeout_ms == 0) return (int)got;

    uint32_t deadline = now_fn() + timeout_ms;
    while (got < len) {
        uint32_t now = now_fn();
        uint32_t left = deadline - now;
        if (left == 0 || left > timeout_ms) break;   /* deadline reached or passed */
        (void)wait_fn(left);
        got += hearth_ring_get(r, buf + got, len - got);
    }
    return (int)got;
}
