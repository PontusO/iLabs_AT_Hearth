/*
 * Host unit tests for the row codec. No framework: the project has no test
 * dependency and this needs none. Build and run with:
 *     make -C test/host run
 */

#include <stdio.h>
#include <string.h>

#include "mt_rows.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) { g_pass++; } else { g_fail++; }
}

/* An EVSE target row: day bitmap, minutes past midnight, SoC, added energy. */
static mt_row_t evse_row(int64_t day, int64_t time, bool has_soc, int64_t soc,
                         bool has_energy, int64_t energy)
{
    mt_row_t r = { 0 };
    r.nfields = 4;
    r.present[0] = true;  r.value[0] = day;
    r.present[1] = true;  r.value[1] = time;
    r.present[2] = has_soc;    r.value[2] = soc;
    r.present[3] = has_energy; r.value[3] = energy;
    return r;
}

static void test_stage_and_validate(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);

    mt_row_t r = evse_row(0x02, 480, true, 80, false, 0);
    check("staging a valid row succeeds",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &r) == MT_ROW_OK);
    check("validate accepts a matching count",
          mt_rows_validate(&s, 1) == MT_ROW_OK);
    check("validate rejects a count that disagrees with what was staged",
          mt_rows_validate(&s, 2) == MT_ROW_ERR_VALUE);

    /* A second row, so a "smaller" count can be tested with a nonzero
     * value: count 0 is the documented clear request (see
     * test_count_zero_is_clear) and always succeeds, so it can no longer
     * stand in for "too small" here. count=3 above is also caught by the
     * dense-index gap check (index 1 was never staged), which would still
     * catch it even with the explicit count comparison removed; count=1
     * below exercises that comparison on its own, since index 0 IS staged
     * and the gap-check loop bounded by count=1 would not see anything
     * wrong on its own. */
    mt_row_t r2 = evse_row(0x04, 600, true, 60, false, 0);
    check("staging a second row succeeds",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 1, &r2) == MT_ROW_OK);
    check("validate rejects a count that disagrees upward with two staged rows",
          mt_rows_validate(&s, 3) == MT_ROW_ERR_VALUE);
    check("validate rejects a count smaller than what was staged",
          mt_rows_validate(&s, 1) == MT_ROW_ERR_VALUE);
}

static void test_field_ranges(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);

    mt_row_t late = evse_row(0x02, 1440, true, 80, false, 0);
    check("time past 1439 is a value error",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &late) == MT_ROW_ERR_VALUE);

    mt_row_t soc = evse_row(0x02, 480, true, 101, false, 0);
    check("SoC above 100 is a value error",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &soc) == MT_ROW_ERR_VALUE);

    mt_row_t neg = evse_row(0x02, 480, false, 0, true, -1);
    check("negative added energy is a value error",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &neg) == MT_ROW_ERR_VALUE);

    mt_row_t noday = evse_row(0x00, 480, true, 80, false, 0);
    check("an empty day bitmap is a value error",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &noday) == MT_ROW_ERR_VALUE);

    mt_row_t badday = evse_row(0x80, 480, true, 80, false, 0);
    check("a day bit above bit 6 is a value error",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &badday) == MT_ROW_ERR_VALUE);
}

static void test_optional_choice(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);

    mt_row_t neither = evse_row(0x02, 480, false, 0, false, 0);
    check("a target with neither SoC nor added energy is a value error",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &neither) == MT_ROW_ERR_VALUE);

    mt_row_t soc_only = evse_row(0x02, 480, true, 80, false, 0);
    check("SoC alone is accepted",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &soc_only) == MT_ROW_OK);

    mt_row_t energy_only = evse_row(0x02, 480, false, 0, true, 15000);
    check("added energy alone is accepted",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 1, &energy_only) == MT_ROW_OK);
}

static void test_day_uniqueness(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);

    /* Two rows may share a bitmap: they group into one schedule entry. */
    mt_row_t a = evse_row(0x02, 480, true, 80, false, 0);
    mt_row_t b = evse_row(0x02, 1080, true, 90, false, 0);
    (void)mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &a);
    (void)mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 1, &b);
    check("two rows sharing one bitmap validate",
          mt_rows_validate(&s, 2) == MT_ROW_OK);

    /* A day bit may not appear in two DIFFERENT bitmaps. */
    mt_row_t c = evse_row(0x06, 600, true, 70, false, 0);  /* bit 1 again */
    (void)mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 2, &c);
    check("a day bit reused across two different bitmaps is a value error",
          mt_rows_validate(&s, 3) == MT_ROW_ERR_VALUE);
}

static void test_index_and_kind(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);
    mt_row_t r = evse_row(0x02, 480, true, 80, false, 0);

    check("an index at the maximum is a value error",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, MT_ROW_MAX_ROWS, &r)
              == MT_ROW_ERR_VALUE);
    check("an unknown kind is a kind error",
          mt_rows_stage(&s, 5, 99, 0, &r) == MT_ROW_ERR_KIND);
    check("field count for the EVSE target kind is 4",
          mt_rows_field_count(MT_ROW_KIND_EVSE_TARGET) == 4);
    check("field count for an unknown kind is negative",
          mt_rows_field_count(99) < 0);
}

static void test_staging_is_single(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);
    mt_row_t r = evse_row(0x02, 480, true, 80, false, 0);

    /* ep 5 stages two rows (count would be 6, not 2, since count tracks
     * highest index + 1); ep 6 then stages one row at index 0. If the old
     * set were not discarded, count would still reflect ep 5's high-water
     * mark of 6, so checking at a DIFFERENT index than the discarded set is
     * what makes this assertion able to actually catch a missing discard,
     * unlike re-using index 0 for both, where count would land on 1 either
     * way. */
    (void)mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &r);
    (void)mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 5, &r);
    (void)mt_rows_stage(&s, 6, MT_ROW_KIND_EVSE_TARGET, 0, &r);
    check("staging for a second endpoint discards the first set",
          s.ep == 6 && s.count == 1);

    check("clear for the wrong endpoint is a value error",
          mt_rows_clear(&s, 5, MT_ROW_KIND_EVSE_TARGET) == MT_ROW_ERR_VALUE);
    check("clear for the staged endpoint succeeds",
          mt_rows_clear(&s, 6, MT_ROW_KIND_EVSE_TARGET) == MT_ROW_OK);
    check("clear leaves the stage inactive", s.active == false);
}

static void test_sparse_indices_are_rejected(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);
    mt_row_t r = evse_row(0x02, 480, true, 80, false, 0);

    (void)mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &r);
    (void)mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 2, &r);  /* gap at 1 */
    check("a gap in the staged indices fails validation",
          mt_rows_validate(&s, 3) == MT_ROW_ERR_VALUE);
}

/*
 * A count of 0 is the documented clear request (design spec 2.6): a host
 * empties a schedule by calling apply with count 0 and nothing staged. It
 * must succeed whether nothing was ever staged, or a set happens to be
 * staged but empty; a count > 0 that claims rows nothing ever staged is
 * still a real value error.
 */
static void test_count_zero_is_clear(void)
{
    mt_row_stage_t fresh;
    mt_rows_init(&fresh);
    check("validate(0) with nothing ever staged is the clear request and succeeds",
          mt_rows_validate(&fresh, 0) == MT_ROW_OK);
    check("validate(N) with nothing ever staged and N > 0 is a value error",
          mt_rows_validate(&fresh, 5) == MT_ROW_ERR_VALUE);

    /* A stage marked active but holding zero rows for its (ep, kind).
     * mt_rows_stage() never produces this state on its own (every
     * successful stage leaves count >= 1), so it is built directly: the
     * struct's fields are public for exactly this kind of test. */
    mt_row_stage_t empty_active = { 0 };
    empty_active.active = true;
    empty_active.ep = 5;
    empty_active.kind = MT_ROW_KIND_EVSE_TARGET;
    check("validate(0) against a staged-but-empty set also succeeds",
          mt_rows_validate(&empty_active, 0) == MT_ROW_OK);
}

/*
 * Every check elsewhere in this suite exercises an INVALID value one step
 * past a boundary. None confirmed the boundary value itself is accepted, so
 * an off-by-one that rejected a legal value (>= where > was meant) would
 * have passed every one of them.
 */
static void test_field_boundaries(void)
{
    mt_row_stage_t s;
    mt_rows_init(&s);

    mt_row_t day_max = evse_row(0x7F, 480, true, 80, false, 0);
    check("day bitmap 0x7F (every day set) is accepted",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 0, &day_max) == MT_ROW_OK);

    mt_row_t time_max = evse_row(0x02, 1439, true, 80, false, 0);
    check("time 1439 (the last minute of the day) is accepted",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 1, &time_max) == MT_ROW_OK);

    mt_row_t soc_max = evse_row(0x02, 480, true, 100, false, 0);
    check("SoC 100 (fully charged) is accepted",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 2, &soc_max) == MT_ROW_OK);

    mt_row_t energy_min = evse_row(0x02, 480, false, 0, true, 0);
    check("added energy 0 (the non-negative floor) is accepted",
          mt_rows_stage(&s, 5, MT_ROW_KIND_EVSE_TARGET, 3, &energy_min) == MT_ROW_OK);
}

int main(void)
{
    printf("mt_rows\n");
    test_stage_and_validate();
    test_field_ranges();
    test_optional_choice();
    test_day_uniqueness();
    test_index_and_kind();
    test_staging_is_single();
    test_sparse_indices_are_rejected();
    test_count_zero_is_clear();
    test_field_boundaries();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
