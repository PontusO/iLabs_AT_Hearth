/*
 * hearth_ring.h - a fixed-capacity byte ring, single producer (ISR),
 * single consumer (the parser task). Capacity is a power of two.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf; uint32_t cap; volatile uint32_t head; volatile uint32_t tail;
    volatile uint32_t dropped;
} hearth_ring_t;

void     hearth_ring_init(hearth_ring_t *r, uint8_t *storage, uint32_t cap_pow2);
bool     hearth_ring_put(hearth_ring_t *r, uint8_t b);        /* false + dropped++ when full */
uint32_t hearth_ring_get(hearth_ring_t *r, uint8_t *out, uint32_t n);
uint32_t hearth_ring_count(const hearth_ring_t *r);
void     hearth_ring_reset(hearth_ring_t *r);

/* The deadline loop the link read contract needs, with the wait injected so
 * it is testable: wait_fn(ms) blocks until data may be available or ms
 * elapsed, returning true if woken by data. now_fn() is monotonic ms. */
int hearth_ring_read_deadline(hearth_ring_t *r, uint8_t *buf, uint32_t len,
                              uint32_t timeout_ms,
                              bool (*wait_fn)(uint32_t ms), uint32_t (*now_fn)(void));

#ifdef __cplusplus
}
#endif
