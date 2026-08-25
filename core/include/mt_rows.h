/*
 * mt_rows.h - the row codec for nested AT payloads: multi-row data sets
 * (e.g. an EVSE charging schedule) that are too large for one 255-byte AT
 * line and so travel one row per line, staged, then validated and applied
 * as a whole set.
 *
 * Deliberately free of IDF dependencies so the codec can be unit-tested on
 * the host (test/host). The AT command layer, the EVSE delegate and the
 * host library all encode against this same field table; this file is the
 * single definition of the rules.
 *
 * A stage holds at most one set at a time, identified by (ep, kind). Staging
 * a row for a different (ep, kind) than the one currently held discards the
 * old set first: a host that is mid-upload for one target never has stale
 * rows from an earlier, abandoned upload silently merged into a new one.
 *
 * ---- THIS CODEC HOLDS NO MUTABLE STATE OF ITS OWN ----
 *
 * Every function here is pure with respect to file scope: all state lives in
 * the caller's mt_row_stage_t, and mt_rows.c's only statics are the const
 * field tables. That is not an aesthetic preference, it is the fact two
 * separate stages depend on. mt_at.c runs two of them concurrently, one
 * written by the AT parser task and one by the CHIP event loop task, and
 * they are safe against each other only because calling into this codec for
 * one stage cannot touch the other. A single mutable static added here (a
 * cached last-kind, a scratch row, an error code) would silently couple the
 * two tasks through a file nobody would think to look in, and the symptom
 * would be a charging schedule from the wrong source. Add nothing to file
 * scope here that is not const.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Row kinds. Each has its own field table and its own max_rows. */
#define MT_ROW_KIND_EVSE_TARGET 1

/* Widest row shape across all kinds. */
#define MT_ROW_MAX_FIELDS 8

/* Storage bound of the row array in mt_row_stage_t. This is a capacity, not
 * a validation limit: a kind's own max_rows (see mt_rows.c) is what
 * mt_rows_stage() actually enforces, and it can be smaller than this. The
 * EVSE target kind happens to use exactly 70 (7 days x 10 targets), which
 * is why the two numbers coincide there; mt_rows.c asserts at compile time
 * that no kind's max_rows exceeds this. */
#define MT_ROW_MAX_ROWS 70

typedef struct {
    bool    present[MT_ROW_MAX_FIELDS];
    int64_t value[MT_ROW_MAX_FIELDS];
    uint8_t nfields;
} mt_row_t;

typedef struct {
    uint8_t  kind;
    uint16_t ep;
    uint16_t count;
    bool     active;
    mt_row_t row[MT_ROW_MAX_ROWS];
} mt_row_stage_t;

/* Return codes shared by every entry point, matching how the AT command
 * layer must answer the host. */
#define MT_ROW_OK        0
#define MT_ROW_ERR_FORM  (-1) /* caller answers a bare ERROR */
#define MT_ROW_ERR_VALUE (-2) /* caller answers +MTERR:1 */
#define MT_ROW_ERR_KIND  (-3) /* caller answers +MTERR:3 */

/* Reset s to the empty, inactive stage. */
void mt_rows_init(mt_row_stage_t *s);

/* Number of fields a row of this kind carries, or a negative value if kind
 * is not a known row kind. */
int mt_rows_field_count(uint8_t kind);

/*
 * Validate row against kind's field table and store it at idx. If the stage
 * currently holds a set for a different (ep, kind), that set is discarded
 * first and this row starts a new one.
 *
 * Returns MT_ROW_OK, MT_ROW_ERR_KIND (kind is not known), or MT_ROW_ERR_VALUE
 * (idx is out of range for the kind, or a field is missing or out of range).
 */
int mt_rows_stage(mt_row_stage_t *s, uint16_t ep, uint8_t kind, uint16_t idx,
                   const mt_row_t *row);

/*
 * Discard the set staged for (ep, kind) and return the stage to inactive.
 * Returns MT_ROW_ERR_VALUE, not MT_ROW_OK, if the stage is inactive or holds
 * a set for a different (ep, kind): silently succeeding would let a host
 * believe it had cleared a set that was never touched.
 */
int mt_rows_clear(mt_row_stage_t *s, uint16_t ep, uint8_t kind);

/*
 * Run the whole-set rules against the currently staged set: count must
 * equal s->count, the staged indices must be dense from 0 to count - 1, and
 * (for MT_ROW_KIND_EVSE_TARGET) no day bit may appear in two different day
 * bitmaps. Called by the apply handler before anything is committed.
 *
 * Returns MT_ROW_OK or MT_ROW_ERR_VALUE.
 */
int mt_rows_validate(const mt_row_stage_t *s, uint16_t count);

#ifdef __cplusplus
}
#endif
