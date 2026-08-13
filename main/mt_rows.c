/*
 * mt_rows.c - stage and validate the rows of a nested AT payload against a
 * per-kind field table.
 *
 * Deliberately free of IDF dependencies so the codec can be unit-tested on
 * the host (test/host). The AT command layer owns wire parsing and error
 * mapping; this file only knows field ranges, presence rules and whole-set
 * rules, keyed by row kind.
 */

#include <string.h>

#include "mt_rows.h"

/* One field's validation rule: an int64 range plus whether the field may be
 * absent from a row. */
typedef struct {
    int64_t min;
    int64_t max;
    bool    optional;
} mt_row_field_t;

/* A row kind: its field table, how many fields it has, and how many rows a
 * set of this kind may hold. max_rows is a VALIDATION limit, distinct from
 * MT_ROW_MAX_ROWS, which is the STORAGE bound of mt_row_stage_t.row[]. They
 * coincide for the EVSE target kind below; a compile-time assertion keeps
 * that true for every kind that is ever added. */
typedef struct {
    uint8_t               kind;
    uint8_t               nfields;
    uint16_t              max_rows;
    const mt_row_field_t *field;
} mt_row_kind_t;

/* EVSE charging target: day bitmap, minutes past midnight, SoC, added
 * energy. Fields 2 and 3 are each individually optional, but a target with
 * neither is meaningless (nothing to charge to); mt_rows_stage() enforces
 * that "at least one of the two" rule as a kind-specific addition on top of
 * this table. */
static const mt_row_field_t s_evse_target_fields[] = {
    { 1,    0x7F,      false },  /* day bitmap, at least one of bits 0..6 */
    { 0,    1439,      false },  /* minutes past midnight */
    { 0,    100,       true  },  /* target SoC, percent */
    { 0,    INT64_MAX, true  },  /* added energy, mWh, non-negative */
};

/* 7 days x 10 targets per day. */
#define MT_EVSE_TARGET_MAX_ROWS 70

_Static_assert(MT_EVSE_TARGET_MAX_ROWS <= MT_ROW_MAX_ROWS,
                "EVSE target max_rows exceeds mt_row_stage_t's storage bound");

static const mt_row_kind_t s_kinds[] = {
    { MT_ROW_KIND_EVSE_TARGET, 4, MT_EVSE_TARGET_MAX_ROWS, s_evse_target_fields },
};

static const mt_row_kind_t *find_kind(uint8_t kind)
{
    for (size_t i = 0; i < sizeof(s_kinds) / sizeof(s_kinds[0]); i++) {
        if (s_kinds[i].kind == kind) {
            return &s_kinds[i];
        }
    }
    return NULL;
}

void mt_rows_init(mt_row_stage_t *s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

int mt_rows_field_count(uint8_t kind)
{
    const mt_row_kind_t *k = find_kind(kind);
    if (!k) {
        return -1;
    }
    return (int)k->nfields;
}

int mt_rows_stage(mt_row_stage_t *s, uint16_t ep, uint8_t kind, uint16_t idx,
                   const mt_row_t *row)
{
    if (!s || !row) {
        return MT_ROW_ERR_FORM;
    }

    const mt_row_kind_t *k = find_kind(kind);
    if (!k) {
        return MT_ROW_ERR_KIND;
    }

    if (idx >= k->max_rows) {
        return MT_ROW_ERR_VALUE;
    }

    for (uint8_t i = 0; i < k->nfields; i++) {
        const mt_row_field_t *f = &k->field[i];
        if (row->present[i]) {
            if (row->value[i] < f->min || row->value[i] > f->max) {
                return MT_ROW_ERR_VALUE;
            }
        } else if (!f->optional) {
            return MT_ROW_ERR_VALUE;
        }
    }

    /* EVSE target only: SoC and added energy are each optional on their
     * own, but a row with neither says nothing about what the target is. */
    if (kind == MT_ROW_KIND_EVSE_TARGET && !row->present[2] && !row->present[3]) {
        return MT_ROW_ERR_VALUE;
    }

    /* Every check passed: commit. A set belonging to a different (ep, kind)
     * is discarded first, so a host mid-upload for one target never has
     * rows from an earlier, abandoned upload silently merged into a new
     * one. */
    if (s->active && (s->ep != ep || s->kind != kind)) {
        memset(s->row, 0, sizeof(s->row));
        s->count = 0;
    }

    s->active = true;
    s->ep = ep;
    s->kind = kind;

    s->row[idx] = *row;
    /* Canonicalize nfields from the kind table rather than trust the
     * caller's value: mt_rows_validate() relies on nfields != 0 meaning
     * "this index was staged" to find gaps in the index sequence, and that
     * must hold regardless of what the caller happened to put in row. */
    s->row[idx].nfields = k->nfields;

    if ((uint16_t)(idx + 1) > s->count) {
        s->count = (uint16_t)(idx + 1);
    }

    return MT_ROW_OK;
}

int mt_rows_clear(mt_row_stage_t *s, uint16_t ep, uint8_t kind)
{
    if (!s) {
        return MT_ROW_ERR_FORM;
    }

    /* Refuse rather than no-op on a mismatch: silently "succeeding" would
     * let a host believe it had cleared a set it never touched. */
    if (!s->active || s->ep != ep || s->kind != kind) {
        return MT_ROW_ERR_VALUE;
    }

    mt_rows_init(s);
    return MT_ROW_OK;
}

int mt_rows_validate(const mt_row_stage_t *s, uint16_t count)
{
    if (!s) {
        return MT_ROW_ERR_FORM;
    }

    if (!s->active || count != s->count) {
        return MT_ROW_ERR_VALUE;
    }

    /* Staged indices must be dense: 0..count-1 with no gaps. An index that
     * was never staged still has nfields == 0 (mt_rows_init cleared it and
     * nothing has canonicalized it since). */
    for (uint16_t i = 0; i < count; i++) {
        if (s->row[i].nfields == 0) {
            return MT_ROW_ERR_VALUE;
        }
    }

    /* EVSE target only: a day bit may not appear in two DIFFERENT bitmaps.
     * Two rows that carry the identical bitmap are one schedule entry (the
     * day has more than one target) and are fine. */
    if (s->kind == MT_ROW_KIND_EVSE_TARGET) {
        int64_t owner[7];
        for (int b = 0; b < 7; b++) {
            owner[b] = -1;
        }
        for (uint16_t i = 0; i < count; i++) {
            int64_t bitmap = s->row[i].value[0];
            for (int b = 0; b < 7; b++) {
                if ((bitmap & ((int64_t)1 << b)) == 0) {
                    continue;
                }
                if (owner[b] == -1) {
                    owner[b] = bitmap;
                } else if (owner[b] != bitmap) {
                    return MT_ROW_ERR_VALUE;
                }
            }
        }
    }

    return MT_ROW_OK;
}
