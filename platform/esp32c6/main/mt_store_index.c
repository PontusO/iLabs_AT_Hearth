#include "mt_store_index.h"

#include <stdlib.h>

struct mt_store_entry {
    uint16_t        ep;
    uint32_t        cluster;   /* 0 for the cluster-agnostic kinds */
    uint8_t         kind;
    void           *store;
};

static struct mt_store_entry *s_tab;
static size_t s_cap;
static size_t s_len;

bool mt_store_index_init(size_t capacity)
{
    if (s_tab != NULL) {
        return false;
    }
    if (capacity == 0) {
        capacity = 1;
    }
    s_tab = calloc(capacity, sizeof(*s_tab));
    if (s_tab == NULL) {
        return false;
    }
    s_cap = capacity;
    s_len = 0;
    return true;
}

bool mt_store_index_add(uint16_t ep, uint32_t cluster,
                        mt_store_kind_t kind, void *store)
{
    if (s_tab == NULL || s_len == s_cap) {
        return false;
    }
    s_tab[s_len].ep      = ep;
    s_tab[s_len].cluster = (kind == MT_STORE_MB) ? cluster : 0u;
    s_tab[s_len].kind    = (uint8_t)kind;
    s_tab[s_len].store   = store;
    s_len++;
    return true;
}

void *mt_store_index_find(uint16_t ep, uint32_t cluster,
                          mt_store_kind_t kind)
{
    uint32_t c = (kind == MT_STORE_MB) ? cluster : 0u;
    for (size_t i = 0; i < s_len; i++) {
        if (s_tab[i].ep == ep && s_tab[i].kind == (uint8_t)kind
            && s_tab[i].cluster == c) {
            return s_tab[i].store;
        }
    }
    return NULL;
}

#ifdef MT_HOST_TEST
void mt_store_index_reset(void)
{
    free(s_tab);
    s_tab = NULL;
    s_cap = 0;
    s_len = 0;
}
#endif
