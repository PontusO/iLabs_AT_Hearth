#include <stdio.h>
#include <string.h>
#include "../../platform/silabs/port/hearth_ring.h"
#include "../../platform/silabs/port/hearth_kvid.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A fake clock and a scripted wait: each wait call advances the clock by
 * `step_ms` and, on the k-th call, "delivers" the scripted bytes. */
static uint32_t s_now;
static uint32_t s_step_ms = 10;
static hearth_ring_t *s_ring;
static const uint8_t *s_deliver; static uint32_t s_deliver_n; static int s_deliver_on_call; static int s_calls;

static uint32_t fake_now(void) { return s_now; }
static bool fake_wait(uint32_t ms)
{
    s_calls++;
    s_now += (ms < s_step_ms) ? ms : s_step_ms;
    if (s_calls == s_deliver_on_call) {
        for (uint32_t i = 0; i < s_deliver_n; i++) hearth_ring_put(s_ring, s_deliver[i]);
        return true;
    }
    return false;
}

int main(void)
{
    uint8_t storage[16];
    hearth_ring_t r;
    hearth_ring_init(&r, storage, 16);
    s_ring = &r;

    /* put/get round trip and count */
    CHECK(hearth_ring_count(&r) == 0);
    CHECK(hearth_ring_put(&r, 'A'));
    CHECK(hearth_ring_count(&r) == 1);
    uint8_t out[16];
    CHECK(hearth_ring_get(&r, out, 16) == 1 && out[0] == 'A');
    CHECK(hearth_ring_count(&r) == 0);

    /* full ring drops and counts, never overwrites */
    for (int i = 0; i < 15; i++) CHECK(hearth_ring_put(&r, (uint8_t)i));
    CHECK(!hearth_ring_put(&r, 99));
    CHECK(r.dropped == 1);
    hearth_ring_reset(&r);
    CHECK(hearth_ring_count(&r) == 0);

    /* deadline read: bytes arrive on the 3rd wait, before the deadline */
    static const uint8_t at[] = "AT\r\n";
    s_now = 1000; s_calls = 0; s_deliver = at; s_deliver_n = 4; s_deliver_on_call = 3;
    int got = hearth_ring_read_deadline(&r, out, 4, 100, fake_wait, fake_now);
    CHECK(got == 4 && memcmp(out, "AT\r\n", 4) == 0);

    /* deadline read: nothing arrives, returns 0 at (not before) the deadline */
    s_now = 5000; s_calls = 0; s_deliver_on_call = 9999;
    got = hearth_ring_read_deadline(&r, out, 1, 50, fake_wait, fake_now);
    CHECK(got == 0);
    CHECK(s_now >= 5050);

    /* timeout 0 is a single non-blocking poll: no wait call at all */
    s_calls = 0;
    got = hearth_ring_read_deadline(&r, out, 1, 0, fake_wait, fake_now);
    CHECK(got == 0 && s_calls == 0);

    /* the one-hour contract: 3,600,000 ms must not overflow or spin.
     * With a 10 ms fake step the loop would take 360,000 iterations, so
     * this case only checks the remaining-time arithmetic near the end. */
    s_now = 10; s_calls = 0; s_deliver_on_call = 9999; s_step_ms = 3600000;
    got = hearth_ring_read_deadline(&r, out, 1, 3600000, fake_wait, fake_now);
    CHECK(got == 0 && s_calls == 1);
    s_step_ms = 10;

    /* partial delivery then more: accumulates to len */
    static const uint8_t ab[] = "AB";
    s_now = 0; s_calls = 0; s_deliver = ab; s_deliver_n = 2; s_deliver_on_call = 1;
    hearth_ring_put(&r, 'X');                       /* one byte already queued */
    got = hearth_ring_read_deadline(&r, out, 3, 100, fake_wait, fake_now);
    CHECK(got == 3 && memcmp(out, "XAB", 3) == 0);

    /* Every (ns, key) the core persists must hash to a distinct NVM3 id.
     * core/ never passes string literals to hearth_kv_*; it passes the
     * NVS_NAMESPACE / NVS_KEY macros defined next to each call site, so a
     * grep for literal-string calls finds nothing. The pairs below are
     * resolved from those macros:
     *   grep -rn 'hearth_kv_' core/ --include=*.c
     *     core/mt/mt_comp_store.c: hearth_kv_get_blob/set_blob/delete(
     *       MT_COMP_NVS_NAMESPACE, MT_COMP_NVS_KEY)
     *       #define MT_COMP_NVS_NAMESPACE "mt_ep"   (mt_comp_store.c:14)
     *       #define MT_COMP_NVS_KEY       "comp"    (mt_comp_store.c:15)
     *     core/mt/mt_transport.c: hearth_kv_get_u8/set_u8(
     *       MT_TRANSPORT_NVS_NAMESPACE, MT_TRANSPORT_NVS_KEY)
     *       #define MT_TRANSPORT_NVS_NAMESPACE "mt_cfg"      (mt_transport.c:37)
     *       #define MT_TRANSPORT_NVS_KEY       "transport"   (mt_transport.c:38)
     * A newly persisted pair must be added here too. */
    const char *pairs[][2] = {
        {"mt_ep", "comp"}, {"mt_cfg", "transport"},
    };
    size_t np = sizeof(pairs) / sizeof(pairs[0]);
    for (size_t i = 0; i < np; i++)
        for (size_t j = i + 1; j < np; j++)
            CHECK(hearth_kv_id(pairs[i][0], pairs[i][1]) != hearth_kv_id(pairs[j][0], pairs[j][1]));

    printf("test_hearth_ring: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
