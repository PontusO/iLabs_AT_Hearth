/*
 * Host unit tests for the transport selection module's pure parts (parse
 * and name). The NVS-backed functions (mt_transport_stored/store/
 * latch_active/active) are compiled out under MT_HOST_TEST, since they need
 * real NVS and are device-only, exercised on hardware instead. No
 * framework: build and run with `make -C test/host run`.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mt_transport.h"

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

static void test_parse_accepts(void)
{
    mt_transport_t t;

    t = MT_TRANSPORT_THREAD;
    check("parse accepts \"WIFI\"", mt_transport_parse("WIFI", &t) == 0);
    check("parse of \"WIFI\" yields MT_TRANSPORT_WIFI", t == MT_TRANSPORT_WIFI);

    t = MT_TRANSPORT_WIFI;
    check("parse accepts \"THREAD\"", mt_transport_parse("THREAD", &t) == 0);
    check("parse of \"THREAD\" yields MT_TRANSPORT_THREAD", t == MT_TRANSPORT_THREAD);
}

static void test_parse_rejects(void)
{
    mt_transport_t t;

    check("parse rejects an empty string", mt_transport_parse("", &t) != 0);
    check("parse rejects lower-case \"wifi\"", mt_transport_parse("wifi", &t) != 0);
    check("parse rejects \"BOTH\"", mt_transport_parse("BOTH", &t) != 0);
    check("parse rejects \"WIFI2\"", mt_transport_parse("WIFI2", &t) != 0);
    check("parse rejects a NULL argument", mt_transport_parse(NULL, &t) != 0);
}

static void test_name_roundtrip(void)
{
    check("name(WIFI) is \"WIFI\"",
          strcmp(mt_transport_name(MT_TRANSPORT_WIFI), "WIFI") == 0);
    check("name(THREAD) is \"THREAD\"",
          strcmp(mt_transport_name(MT_TRANSPORT_THREAD), "THREAD") == 0);
}

int main(void)
{
    printf("\n===== mt_transport parse/name tests =====\n");
    test_parse_accepts();
    test_parse_rejects();
    test_name_roundtrip();
    printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
