/*
 * Host unit tests for the composition codec. No framework: the project has
 * no test dependency and this needs none. Build and run with:
 *     make -C test/host run
 */

#include <stdio.h>
#include <string.h>

#include "mt_composition.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void test_roundtrip(void)
{
    mt_composition_t in = { .count = 3, .devtype = { 0x0100, 0x0101, 0x0302 } };
    uint8_t buf[MT_COMP_BLOB_MAX];

    int n = mt_comp_encode(&in, buf, sizeof(buf));
    check("encode of 3 endpoints writes 19 bytes (v2 header + 3*5)", n == 19);

    mt_composition_t out;
    memset(&out, 0xAA, sizeof(out));
    check("decode succeeds", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("roundtrip preserves the composition", mt_comp_equal(&in, &out));
}

static void test_empty(void)
{
    mt_composition_t in = { .count = 0 };
    uint8_t buf[MT_COMP_BLOB_MAX];

    int n = mt_comp_encode(&in, buf, sizeof(buf));
    check("empty composition encodes to 4 bytes (v2 header only)", n == 4);

    mt_composition_t out = { .count = 99 };
    check("empty decodes", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("empty decodes to count 0", out.count == 0);
}

static void test_endianness(void)
{
    /* Pinned byte-for-byte: a blob written by one build must read on
     * another. v2 layout: sentinel, version, count LE u16, then per entry
     * a u32 LE device type id followed by a single variant byte. */
    mt_composition_t in = { .count = 1, .devtype = { 0x010C }, .variant = { 0x07 } };
    uint8_t buf[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(&in, buf, sizeof(buf));

    const uint8_t expect[9] = {
        0xFF, 0x02,             /* sentinel, version */
        0x01, 0x00,             /* count = 1, LE */
        0x0C, 0x01, 0x00, 0x00, /* devtype 0x010C, LE u32 */
        0x07                    /* variant */
    };
    check("encoding is little-endian and stable", n == 9 && memcmp(buf, expect, 9) == 0);
}

static void test_full(void)
{
    mt_composition_t in = { .count = MT_COMP_MAX_ENDPOINTS };
    for (int i = 0; i < MT_COMP_MAX_ENDPOINTS; i++) {
        in.devtype[i] = 0x0100u + (uint32_t)i;
        in.variant[i] = (uint8_t)i;
    }
    uint8_t buf[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(&in, buf, sizeof(buf));
    check("a full composition encodes", n == MT_COMP_BLOB_MAX);

    mt_composition_t out;
    check("a full composition decodes", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("a full composition roundtrips", mt_comp_equal(&in, &out));
}

static void test_encode_rejects(void)
{
    mt_composition_t too_many = { .count = MT_COMP_MAX_ENDPOINTS + 1 };
    uint8_t buf[MT_COMP_BLOB_MAX];
    check("encode rejects an over-long composition",
          mt_comp_encode(&too_many, buf, sizeof(buf)) == -1);

    mt_composition_t ok = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    check("encode rejects a short buffer", mt_comp_encode(&ok, buf, 5) == -1);
}

static void test_decode_rejects(void)
{
    mt_composition_t out;

    check("decode rejects a zero-length buffer", mt_comp_decode(NULL, 0, &out) == -1);

    /* v1: declares 2 endpoints but carries only 1 (regression coverage for
     * decode_v1's exact-length check, mt_composition.c). */
    const uint8_t v1_truncated[6] = { 0x02, 0x00, 0x00, 0x01, 0x00, 0x00 };
    check("decode rejects a truncated v1 blob", mt_comp_decode(v1_truncated, 6, &out) == -1);

    /* v1: declares 1 endpoint but carries 2. */
    const uint8_t v1_trailing[10] = {
        0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00
    };
    check("decode rejects trailing bytes after a v1 blob", mt_comp_decode(v1_trailing, 10, &out) == -1);

    /* v1: byte0 = 0 (<= cap) but byte1 is nonzero, so the full u16 count is
     * 256, over MT_COMP_MAX_ENDPOINTS. byte0 alone is not enough to reject
     * before decode_v1 combines both bytes. */
    const uint8_t v1_count_overflow[2] = { 0x00, 0x01 };
    check("decode rejects a v1 count made oversize by a nonzero high byte",
          mt_comp_decode(v1_count_overflow, 2, &out) == -1);

    /* v2: declares 2 endpoints but carries only 1 entry (header + 5 bytes,
     * needs header + 10). */
    const uint8_t truncated[9] = {
        0xFF, 0x02, 0x02, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00
    };
    check("decode rejects a truncated v2 blob", mt_comp_decode(truncated, 9, &out) == -1);

    /* v2: declares 1 endpoint but carries 2 entries worth of trailing bytes. */
    const uint8_t trailing[14] = {
        0xFF, 0x02, 0x01, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x00, 0x00, 0x00
    };
    check("decode rejects trailing bytes after a v2 blob", mt_comp_decode(trailing, 14, &out) == -1);

    /* v2: declares more endpoints than the cap. */
    const uint8_t oversize[4] = { 0xFF, 0x02, (uint8_t)(MT_COMP_MAX_ENDPOINTS + 1), 0x00 };
    check("decode rejects an over-long v2 count", mt_comp_decode(oversize, 4, &out) == -1);

    /* v2 sentinel present but the version byte is not the one we speak. */
    const uint8_t bad_version[4] = { 0xFF, 0x03, 0x00, 0x00 };
    check("decode rejects an unknown version byte after the sentinel",
          mt_comp_decode(bad_version, 4, &out) == -1);

    /* v2 sentinel but too short to even carry a version byte and count. */
    const uint8_t short_v2[2] = { 0xFF, 0x02 };
    check("decode rejects a v2 blob too short for its own header",
          mt_comp_decode(short_v2, 2, &out) == -1);

    /* byte0 in 17..0xFE is neither a legal v1 count (0..16) nor the v2
     * sentinel (0xFF): the dispatch-ambiguity branch, mt_composition.c's
     * final "return -1" in mt_comp_decode(). Exercise both ends of that
     * range so it is not merely off-by-one lucky. */
    const uint8_t ambiguous_low[2] = { 0x11, 0x00 };  /* 17 */
    check("decode rejects a discriminator byte just above the v1 cap",
          mt_comp_decode(ambiguous_low, 2, &out) == -1);

    const uint8_t ambiguous_high[2] = { 0xC8, 0x00 }; /* 200 */
    check("decode rejects a discriminator byte in the middle of the ambiguous range",
          mt_comp_decode(ambiguous_high, 2, &out) == -1);

    const uint8_t ambiguous_near_sentinel[2] = { 0xFE, 0x00 }; /* one below 0xFF */
    check("decode rejects a discriminator byte just below the v2 sentinel",
          mt_comp_decode(ambiguous_near_sentinel, 2, &out) == -1);
}

static void test_equal(void)
{
    mt_composition_t a = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    mt_composition_t b = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    mt_composition_t reordered = { .count = 2, .devtype = { 0x0101, 0x0100 } };
    mt_composition_t shorter = { .count = 1, .devtype = { 0x0100 } };
    mt_composition_t variant_differs = { .count = 2, .devtype = { 0x0100, 0x0101 }, .variant = { 0, 1 } };

    check("equal compositions compare equal", mt_comp_equal(&a, &b));
    check("order matters", !mt_comp_equal(&a, &reordered));
    check("count matters", !mt_comp_equal(&a, &shorter));
    check("variant matters", !mt_comp_equal(&a, &variant_differs));
}

static void test_v1_golden_blob_decodes_with_zero_variants(void)
{
    /* A blob exactly as an old build (pre-variant) would have written it:
     * u16 count, then count * u32 LE device type IDs, no version byte at
     * all. byte0 is the low byte of count, which for any legal v1
     * composition (0..16 endpoints) is <= MT_COMP_MAX_ENDPOINTS and so is
     * never confused with the 0xFF v2 sentinel. */
    const uint8_t v1_golden[14] = {
        0x03, 0x00,             /* count = 3 */
        0x00, 0x01, 0x00, 0x00, /* devtype 0x0100 */
        0x01, 0x01, 0x00, 0x00, /* devtype 0x0101 */
        0x02, 0x03, 0x00, 0x00  /* devtype 0x0302 */
    };

    mt_composition_t out;
    memset(&out, 0xAA, sizeof(out));
    check("v1 golden blob decodes", mt_comp_decode(v1_golden, sizeof(v1_golden), &out) == 0);
    check("v1 golden blob yields count 3", out.count == 3);
    check("v1 golden blob preserves device type order and values",
          out.devtype[0] == 0x0100 && out.devtype[1] == 0x0101 && out.devtype[2] == 0x0302);
    check("v1 golden blob decodes with every variant zero",
          out.variant[0] == 0 && out.variant[1] == 0 && out.variant[2] == 0);
}

static void test_v2_roundtrip_with_mixed_variants(void)
{
    mt_composition_t in = {
        .count = 3,
        .devtype = { 0x0100, 0x0101, 0x0302 },
        .variant = { 0, 5, 255 }
    };
    uint8_t buf[MT_COMP_BLOB_MAX];

    int n = mt_comp_encode(&in, buf, sizeof(buf));
    check("v2 encode of 3 mixed-variant endpoints writes 19 bytes", n == 19);

    mt_composition_t out;
    memset(&out, 0, sizeof(out));
    check("v2 decode succeeds", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("v2 roundtrip preserves devtype and variant together", mt_comp_equal(&in, &out));
    check("v2 roundtrip preserves each variant exactly",
          out.variant[0] == 0 && out.variant[1] == 5 && out.variant[2] == 255);
}

static void test_v1_and_v2_equal_when_variants_are_zero(void)
{
    /* Same golden bytes as test_v1_golden_blob_decodes_with_zero_variants:
     * a device migrating from a v1-only build to this one must see its
     * existing composition compare equal to a freshly-encoded v2 blob of
     * the same devices with variants left at zero. */
    const uint8_t v1_golden[14] = {
        0x03, 0x00,
        0x00, 0x01, 0x00, 0x00,
        0x01, 0x01, 0x00, 0x00,
        0x02, 0x03, 0x00, 0x00
    };

    mt_composition_t from_v1;
    check("v1 golden blob decodes for the equality check",
          mt_comp_decode(v1_golden, sizeof(v1_golden), &from_v1) == 0);

    mt_composition_t built_directly = { .count = 3, .devtype = { 0x0100, 0x0101, 0x0302 } };

    check("a v1-decoded composition equals the same composition built directly (zero variants)",
          mt_comp_equal(&from_v1, &built_directly));

    /* And round-tripping the directly-built composition through v2 encode
     * still compares equal to the v1 decode. */
    uint8_t buf[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(&built_directly, buf, sizeof(buf));
    mt_composition_t from_v2;
    check("re-encoding through v2 decodes back", mt_comp_decode(buf, (size_t)n, &from_v2) == 0);
    check("v1 decode equals v2 roundtrip of the same composition", mt_comp_equal(&from_v1, &from_v2));
}

int main(void)
{
    printf("\n===== mt_composition codec tests =====\n");
    test_roundtrip();
    test_empty();
    test_endianness();
    test_full();
    test_encode_rejects();
    test_decode_rejects();
    test_equal();
    test_v1_golden_blob_decodes_with_zero_variants();
    test_v2_roundtrip_with_mixed_variants();
    test_v1_and_v2_equal_when_variants_are_zero();
    printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
