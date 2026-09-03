#include <assert.h>
#include <stdio.h>
#include "mt_store_index.h"

int main(void) {
    int a, b, c, d;
    mt_store_index_reset();
    assert(mt_store_index_init(4));
    assert(!mt_store_index_init(4));                 /* already initialised */

    assert(mt_store_index_add(2, 0, MT_STORE_MODE,  &a));
    assert(mt_store_index_add(2, 0, MT_STORE_CHIME, &b));   /* same ep, different kind */
    /* RVC: two ModeBase stores on ONE endpoint, disambiguated by cluster */
    assert(mt_store_index_add(3, 0x0051, MT_STORE_MB, &c)); /* RvcRunMode  */
    assert(mt_store_index_add(3, 0x0052, MT_STORE_MB, &d)); /* RvcCleanMode */
    assert(!mt_store_index_add(9, 0, MT_STORE_MODE, &a));   /* table full (cap 4) */

    assert(mt_store_index_find(2, 0, MT_STORE_MODE)  == &a);
    assert(mt_store_index_find(2, 0, MT_STORE_CHIME) == &b);
    assert(mt_store_index_find(3, 0x0051, MT_STORE_MB) == &c);
    assert(mt_store_index_find(3, 0x0052, MT_STORE_MB) == &d);
    assert(mt_store_index_find(2, 0, MT_STORE_MB)    == NULL); /* no such store */
    assert(mt_store_index_find(7, 0, MT_STORE_MODE)  == NULL); /* no such ep */

    printf("test_mt_store_index: all passed\n");
    return 0;
}
