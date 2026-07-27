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
    check("encode of 3 endpoints writes 14 bytes", n == 14);

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
    check("empty composition encodes to 2 bytes", n == 2);

    mt_composition_t out = { .count = 99 };
    check("empty decodes", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("empty decodes to count 0", out.count == 0);
}

static void test_endianness(void)
{
    /* Pinned byte-for-byte: a blob written by one build must read on another. */
    mt_composition_t in = { .count = 1, .devtype = { 0x010C } };
    uint8_t buf[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(&in, buf, sizeof(buf));

    const uint8_t expect[6] = { 0x01, 0x00, 0x0C, 0x01, 0x00, 0x00 };
    check("encoding is little-endian and stable", n == 6 && memcmp(buf, expect, 6) == 0);
}

static void test_full(void)
{
    mt_composition_t in = { .count = MT_COMP_MAX_ENDPOINTS };
    for (int i = 0; i < MT_COMP_MAX_ENDPOINTS; i++) {
        in.devtype[i] = 0x0100u + (uint32_t)i;
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

    /* Declares 2 endpoints but carries only 1. */
    const uint8_t truncated[6] = { 0x02, 0x00, 0x00, 0x01, 0x00, 0x00 };
    check("decode rejects a truncated blob", mt_comp_decode(truncated, 6, &out) == -1);

    /* Declares 1 endpoint but carries 2. */
    const uint8_t trailing[10] = { 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00 };
    check("decode rejects trailing bytes", mt_comp_decode(trailing, 10, &out) == -1);

    /* Declares more endpoints than the cap. */
    const uint8_t oversize[2] = { (uint8_t)(MT_COMP_MAX_ENDPOINTS + 1), 0x00 };
    check("decode rejects an over-long count", mt_comp_decode(oversize, 2, &out) == -1);
}

static void test_equal(void)
{
    mt_composition_t a = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    mt_composition_t b = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    mt_composition_t reordered = { .count = 2, .devtype = { 0x0101, 0x0100 } };
    mt_composition_t shorter = { .count = 1, .devtype = { 0x0100 } };

    check("equal compositions compare equal", mt_comp_equal(&a, &b));
    check("order matters", !mt_comp_equal(&a, &reordered));
    check("count matters", !mt_comp_equal(&a, &shorter));
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
    printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
