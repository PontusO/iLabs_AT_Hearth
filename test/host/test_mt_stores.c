#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "mt_stores.h"

/* The sizes the nRF port pins today (mt_devtypes_zephyr.cpp:5198-5204),
 * asserted here so the shared shapes cannot drift from what both ports
 * expect. The plain mode list is the 273 B half; the CHIP structs[] tail
 * is per-platform and not part of this header. */
static void test_store_sizes(void) {
    assert(sizeof(mt_mode_list_t)         == 273);
    assert(sizeof(mt_chime_store_t)       == 273);
    assert(sizeof(mt_mb_store_t)          == 306);
    assert(sizeof(mt_temp_levels_store_t) == 273);
}

static void test_mode_list_roundtrip(void) {
    mt_mode_list_t s;
    memset(&s, 0, sizeof(s));
    assert(s.count == 0);                       /* the "host has not fed a list" state */
    s.count = MT_MODES_MAX_COUNT;               /* fill to the bound */
    for (uint8_t i = 0; i < MT_MODES_MAX_COUNT; i++) {
        s.entries[i].mode = i;
        snprintf(s.entries[i].label, sizeof(s.entries[i].label), "m%u", i);
    }
    assert(s.entries[MT_MODES_MAX_COUNT - 1].mode == MT_MODES_MAX_COUNT - 1);
    assert(strcmp(s.entries[0].label, "m0") == 0);
}

static void test_mb_carries_tag(void) {
    mt_mb_store_t s;
    memset(&s, 0, sizeof(s));
    s.count = 1;
    s.entries[0].mode = 3;
    s.entries[0].tag  = 0x4001;                  /* the spec-mandated per-mode tag */
    assert(s.entries[0].tag == 0x4001);
}

int main(void) {
    test_store_sizes();
    test_mode_list_roundtrip();
    test_mb_carries_tag();
    printf("test_mt_stores: 3 passed, 0 failed\n");
    return 0;
}
