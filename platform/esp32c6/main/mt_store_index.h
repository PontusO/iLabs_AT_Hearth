/*
 * mt_store_index.h - the C6's endpoint -> host-fed-store map. Populated as
 * rebuild_composition() creates endpoints (main.cpp); a store is malloc'd
 * for each endpoint whose device type carries one, and recorded here. Pure
 * C, no CHIP types, boot-long and allocate-only: nothing is ever removed or
 * freed, matching the stores it points at, which is what keeps the mode
 * store's CharSpans and the EVSE list valid for the boot (spec section 5).
 * Replaces the four 28-/16-deep .bss pools the C6 used to scan.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MT_STORE_MODE  = 0,   /* ModeSelect list, one per endpoint            */
    MT_STORE_TEMP  = 1,   /* TemperatureControl levels, one per endpoint  */
    MT_STORE_CHIME = 2,   /* Chime sounds, one per endpoint               */
    MT_STORE_MB    = 3,   /* ModeBase list, keyed by (endpoint, cluster)  */
} mt_store_kind_t;

/* Allocate the index for `capacity` entries. Call once at the start of the
 * rebuild with 2 * endpoint_count (an RVC endpoint carries two MB stores;
 * every other type carries at most one store). Returns false if already
 * initialised or the allocation fails. */
bool mt_store_index_init(size_t capacity);

/* Record `store` for (ep, cluster, kind). `cluster` is used only for
 * MT_STORE_MB; the other kinds ignore it. Returns false if full or
 * uninitialised. */
bool mt_store_index_add(uint16_t ep, uint32_t cluster,
                        mt_store_kind_t kind, void *store);

/* The store for (ep, cluster, kind), or NULL. Pass cluster 0 for the
 * cluster-agnostic kinds. A NULL return is the defensive
 * cannot-happen-once-rebuilt arm, since the AT surface's own endpoint and
 * cluster checks fire first. */
void *mt_store_index_find(uint16_t ep, uint32_t cluster,
                          mt_store_kind_t kind);

#ifdef MT_HOST_TEST
/* Host tests only: free the table and clear state between cases. Never
 * called by firmware, which inits exactly once per boot. */
void mt_store_index_reset(void);
#endif
