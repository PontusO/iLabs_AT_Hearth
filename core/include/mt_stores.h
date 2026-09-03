/*
 * mt_stores.h - the host-fed list shapes, one definition for every port.
 * Sized by the MT_*_MAX bounds in mt_matter.h. A store IS one endpoint's
 * (or one (endpoint, cluster) pair's) and carries no endpoint key: whoever
 * holds the pointer already knows which endpoint it belongs to. count 0
 * means "the host has not fed a list yet". Plain C11, no CHIP types, so it
 * crosses the core boundary the host Makefile's `boundary` target guards.
 *
 * The mode store's second half is a pre-rendered ModeOptionStruct array
 * that the SDK reads as a ModeOptionsProvider; that type is CHIP C++ and
 * cannot live here, so this header defines only the plain mt_mode_list_t
 * (the entries) and each platform wraps it with its own structs[] tail
 * (spec section 3).
 */
#pragma once

#include <stdint.h>

#include "mt_matter.h"   /* the MT_*_MAX bounds */

typedef struct { uint8_t mode; char label[MT_MODES_MAX_LABEL_LEN + 1]; }
    mt_mode_entry_t;
typedef struct { uint8_t id;   char name[MT_CHIME_MAX_NAME_LEN + 1]; }
    mt_chime_entry_t;
typedef struct { uint8_t mode; uint16_t tag; char label[MT_MB_MAX_LABEL_LEN + 1]; }
    mt_mb_entry_t;

typedef struct { uint8_t count; mt_mode_entry_t  entries[MT_MODES_MAX_COUNT];  }
    mt_mode_list_t;
typedef struct { uint8_t count; mt_chime_entry_t entries[MT_CHIME_MAX_SOUNDS]; }
    mt_chime_store_t;
typedef struct { uint8_t count; mt_mb_entry_t    entries[MT_MB_MAX_COUNT];     }
    mt_mb_store_t;
typedef struct { uint8_t count;
    char labels[MT_TEMP_LEVEL_MAX_COUNT][MT_TEMP_LEVEL_MAX_LEN + 1]; }
    mt_temp_levels_store_t;
