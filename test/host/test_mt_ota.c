/*
 * test_mt_ota.c - the OTA relay module against a captured link and stubbed
 * upward port. Handlers are invoked exactly as at_parser.c invokes them:
 * (type, mutable args), return code mapped by the engine.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hearth_port.h"
#include "mt_at.h"
#include "mt_err.h"
#include "mt_matter.h"
#include "mt_ota.h"

/* ---- captured output ---------------------------------------------------- */

#define CAP_LINES 64
#define CAP_LINE_MAX 300
static char s_lines[CAP_LINES][CAP_LINE_MAX];
static int s_nlines;

static void cap_reset(void) { s_nlines = 0; }

static void cap_add(const char *line)
{
    if (s_nlines < CAP_LINES) {
        strncpy(s_lines[s_nlines], line, CAP_LINE_MAX - 1);
        s_lines[s_nlines][CAP_LINE_MAX - 1] = '\0';
        s_nlines++;
    }
}

void hearth_link_init(void) {}
void hearth_link_write(const void *data, size_t len) { (void)data; (void)len; }
void hearth_link_write_line(const char *fmt, ...)
{
    char line[CAP_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    cap_add(line);
}
int hearth_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{ (void)buf; (void)len; (void)timeout_ms; return 0; }
int hearth_link_get_baud(void) { return 115200; }
int hearth_link_set_baud(int baud) { (void)baud; return 0; }
int hearth_link_get_flowctrl(void) { return 0; }
int hearth_link_set_flowctrl(int mode) { (void)mode; return 0; }

static int s_urc_count;
bool mt_at_urc(const char *line) { s_urc_count++; cap_add(line); return true; }

/* ---- upward-port stubs, recording -------------------------------------- */

static int s_set_mode_rc, s_set_mode_last = -99;
static int s_acked_rc; static uint32_t s_acked_last = 0xFFFFFFFFu;
static int s_staged_rc, s_staged_ok = -1, s_staged_reason = -1;
static uint32_t s_swver_last; static char s_swverstr_last[64];

int mt_matter_ota_set_mode(int mode) { s_set_mode_last = mode; return s_set_mode_rc; }
int mt_matter_ota_block_acked(uint32_t seq) { s_acked_last = seq; return s_acked_rc; }
int mt_matter_ota_staged(int ok, int reason) { s_staged_ok = ok; s_staged_reason = reason; return s_staged_rc; }
int mt_matter_swver_set(uint32_t version, const char *str)
{ s_swver_last = version; strncpy(s_swverstr_last, str, sizeof(s_swverstr_last) - 1); return 0; }
static int s_net_info_rc;
int mt_matter_net_info(int *transport, int *enabled, int *connected)
{ *transport = MT_NET_THREAD; *enabled = 1; *connected = 0; return s_net_info_rc; }

/* port_stub.c's one-shot storage-failure switch. */
extern int port_stub_kv_fail_next;

/* ---- harness ------------------------------------------------------------ */

static int g_pass, g_fail;
static void check(const char *name, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) g_pass++; else g_fail++;
}

static int run(const char *name, at_type_t type, const char *args)
{
    size_t n;
    const at_command_t *t = mt_ota_commands(&n);
    char buf[256];
    strncpy(buf, args ? args : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (strcmp(t[i].name, name) == 0) {
            return t[i].handler(type, buf);
        }
    }
    return -1000;
}

static bool last_line_is(const char *want)
{
    return s_nlines > 0 && strcmp(s_lines[s_nlines - 1], want) == 0;
}

/* ---- tests -------------------------------------------------------------- */

static void test_table(void)
{
    size_t n;
    const at_command_t *t = mt_ota_commands(&n);
    check("five commands in the table", n == 5);
    check("first is MTSWVER", strcmp(t[0].name, "MTSWVER") == 0);
}

static void test_swver(void)
{
    mt_ota_init();
    cap_reset();
    check("MTSWVER? before any set answers 0,\"\"",
          run("MTSWVER", AT_QUERY, NULL) == AT_R_OK && last_line_is("+MTSWVER:0,\"\""));
    uint32_t v = 1; char s[MT_OTA_SWVER_STR_MAX] = "x";
    check("stored read answers 1 (absent)", mt_ota_swver_stored(&v, s, sizeof(s)) == 1);

    check("MTSWVER=65540,\"1.4.0\" -> OK", run("MTSWVER", AT_SET, "65540,\"1.4.0\"") == AT_R_OK);
    check("live update called with 65540", s_swver_last == 65540u && strcmp(s_swverstr_last, "1.4.0") == 0);
    check("stored read answers 0 with the values",
          mt_ota_swver_stored(&v, s, sizeof(s)) == 0 && v == 65540u && strcmp(s, "1.4.0") == 0);
    cap_reset();
    check("MTSWVER? answers the stored pair",
          run("MTSWVER", AT_QUERY, NULL) == AT_R_OK && last_line_is("+MTSWVER:65540,\"1.4.0\""));
    check("unquoted string accepted", run("MTSWVER", AT_SET, "65541,1.4.1") == AT_R_OK);
    check("missing string is a bad parameter", run("MTSWVER", AT_SET, "7") == MT_ERR_BAD_PARAM);
    check("31-char string accepted",
          run("MTSWVER", AT_SET, "1,\"abcdefghijklmnopqrstuvwxyz01234\"") == AT_R_OK);
    check("32-char string is a bad parameter",
          run("MTSWVER", AT_SET, "1,\"abcdefghijklmnopqrstuvwxyz012345\"") == MT_ERR_BAD_PARAM);
    check("a quote inside the version string is a bad parameter",
          run("MTSWVER", AT_SET, "1,\"a\"b\"") == MT_ERR_BAD_PARAM);
    check("exec form is a bare ERROR", run("MTSWVER", AT_EXEC, NULL) == MT_R_ERROR);
    check("a signed version is a bad parameter", run("MTSWVER", AT_SET, "-1,\"x\"") == MT_ERR_BAD_PARAM);
    check("a plus-signed version is a bad parameter", run("MTSWVER", AT_SET, "+1,\"x\"") == MT_ERR_BAD_PARAM);

    port_stub_kv_fail_next = 1;
    check("a failed storage write is +MTERR:7",
          run("MTSWVER", AT_SET, "999,\"9.9.9\"") == MT_ERR_PERSIST);
    port_stub_kv_fail_next = 0;
}

/*
 * The two ways the stored pair can be inconsistent, written straight into the
 * stub's key-value table because no command can produce them: only a power cut
 * between swver_store()'s two non-atomic writes can, and mt_ota_swver_stored()
 * has to call both -1 (corrupt) rather than reporting a half-truth.
 */
static void test_swver_corrupt(void)
{
    mt_ota_init();
    cap_reset();
    uint32_t v = 0;
    char s[MT_OTA_SWVER_STR_MAX] = "";

    const uint8_t raw[4] = { 4, 0, 1, 0 };
    hearth_kv_set_blob("mt_cfg", "swver", raw, sizeof(raw));
    hearth_kv_delete("mt_cfg", "swverstr");
    check("a version with no string reads as corrupt",
          mt_ota_swver_stored(&v, s, sizeof(s)) == -1);
    check("MTSWVER? over a corrupt pair is +MTERR:7",
          run("MTSWVER", AT_QUERY, NULL) == MT_ERR_PERSIST);

    hearth_kv_set_blob("mt_cfg", "swverstr", "1.4.0", 5);   /* no NUL */
    check("a string without its NUL reads as corrupt",
          mt_ota_swver_stored(&v, s, sizeof(s)) == -1);

    hearth_kv_delete("mt_cfg", "swver");
    hearth_kv_delete("mt_cfg", "swverstr");
    check("both keys gone reads as absent", mt_ota_swver_stored(&v, s, sizeof(s)) == 1);
}

static void test_mode(void)
{
    mt_ota_init();
    cap_reset();
    s_set_mode_rc = 0;
    check("boot mode is 0", mt_ota_mode() == 0);
    check("MTOTA? at boot", run("MTOTA", AT_QUERY, NULL) == AT_R_OK && last_line_is("+MTOTA:0,IDLE,0,thread"));
    check("MTOTA=2 while disabled is +MTERR:12", run("MTOTA", AT_SET, "2") == MT_ERR_OTA_STATE);
    check("MTOTA=1 -> OK", run("MTOTA", AT_SET, "1") == AT_R_OK && s_set_mode_last == 1 && mt_ota_mode() == 1);
    check("MTOTA=2 while enabled -> OK", run("MTOTA", AT_SET, "2") == AT_R_OK && s_set_mode_last == 2);
    check("MTOTA=3 is a bad parameter", run("MTOTA", AT_SET, "3") == MT_ERR_BAD_PARAM);
    check("MTOTA=0 -> OK and mode 0", run("MTOTA", AT_SET, "0") == AT_R_OK && mt_ota_mode() == 0);

    s_set_mode_rc = -1;
    check("no requestor: MTOTA=1 is +MTERR:8", run("MTOTA", AT_SET, "1") == MT_ERR_UNSUPPORTED && mt_ota_mode() == 0);
    check("MTOTA=0 with no requestor still answers OK", run("MTOTA", AT_SET, "0") == AT_R_OK && mt_ota_mode() == 0);
    s_set_mode_rc = 0;

    /* A query-now only from idle: mid-attempt the platform would decline it
     * silently, so the host must be told instead of getting a bare OK. */
    mt_ota_init();
    run("MTOTA", AT_SET, "1");
    mt_ota_on_state(MT_OTA_DOWNLOADING, "40");
    s_set_mode_last = -99;
    check("MTOTA=2 while downloading is +MTERR:12",
          run("MTOTA", AT_SET, "2") == MT_ERR_OTA_STATE && s_set_mode_last == -99);
    mt_ota_on_state(MT_OTA_DOWNLOADED, NULL);
    check("MTOTA=2 while downloaded is +MTERR:12",
          run("MTOTA", AT_SET, "2") == MT_ERR_OTA_STATE && s_set_mode_last == -99);
    mt_ota_on_state(MT_OTA_IDLE, NULL);
    check("MTOTA=2 once idle again -> OK",
          run("MTOTA", AT_SET, "2") == AT_R_OK && s_set_mode_last == 2);

    /* The variant token is whatever the platform's net_info says, and
     * "unknown" when it refuses to say. */
    cap_reset();
    s_net_info_rc = -1;
    check("MTOTA? with no net info answers the unknown variant",
          run("MTOTA", AT_QUERY, NULL) == AT_R_OK && last_line_is("+MTOTA:1,IDLE,0,unknown"));
    s_net_info_rc = 0;
    cap_reset();
    check("MTOTA? with net info answers the transport",
          run("MTOTA", AT_QUERY, NULL) == AT_R_OK && last_line_is("+MTOTA:1,IDLE,0,thread"));
}

static void test_state_urcs(void)
{
    mt_ota_init();
    cap_reset();
    s_urc_count = 0;
    mt_ota_on_state(MT_OTA_QUERYING, NULL);
    check("state URC suppressed while disabled", s_urc_count == 0);
    run("MTOTA", AT_SET, "1");
    mt_ota_on_state(MT_OTA_AVAILABLE, "65540");
    check("+MTOTA:AVAILABLE,65540", s_urc_count == 1 && last_line_is("+MTOTA:AVAILABLE,65540"));
    mt_ota_on_state(MT_OTA_DOWNLOADING, "42");
    check("+MTOTA:DOWNLOADING,42", last_line_is("+MTOTA:DOWNLOADING,42"));
    check("MTOTA? shows DOWNLOADING and 42", run("MTOTA", AT_QUERY, NULL) == AT_R_OK && last_line_is("+MTOTA:1,DOWNLOADING,42,thread"));
    mt_ota_on_state(MT_OTA_DOWNLOADED, NULL);
    check("+MTOTA:DOWNLOADED", last_line_is("+MTOTA:DOWNLOADED"));
    mt_ota_on_apply();
    check("+MTOTA:APPLY", last_line_is("+MTOTA:APPLY"));
    check("MTOTA? shows APPLYING", run("MTOTA", AT_QUERY, NULL) == AT_R_OK && last_line_is("+MTOTA:1,APPLYING,100,thread"));
    mt_ota_on_state(MT_OTA_DEFERRED, "600");
    check("+MTOTA:DEFERRED,600", last_line_is("+MTOTA:DEFERRED,600"));
}

static void test_block_relay(void)
{
    static uint8_t blk[200];
    for (int i = 0; i < 200; i++) blk[i] = (uint8_t)i;
    mt_ota_init();
    cap_reset();
    check("block refused while disabled", mt_ota_on_block(0, blk, sizeof(blk)) == -1);
    run("MTOTA", AT_SET, "1");
    check("block accepted", mt_ota_on_block(7, blk, sizeof(blk)) == 0);
    check("+MTOTA:BLOCK,7,200", last_line_is("+MTOTA:BLOCK,7,200"));
    check("second block refused while one is pending", mt_ota_on_block(8, blk, 10) == -1);

    cap_reset();
    check("MTOTAGET=7 -> OK", run("MTOTAGET", AT_SET, "7") == AT_R_OK);
    check("three chunk lines (96, 96, 8)", s_nlines == 3);
    check("first line prefix", strncmp(s_lines[0], "+MTOTABLK:7,0,000102", 20) == 0);
    check("first line carries 192 hex digits", strlen(s_lines[0]) == strlen("+MTOTABLK:7,0,") + 192);
    check("second line offset 96", strncmp(s_lines[1], "+MTOTABLK:7,96,606162", 21) == 0);
    check("third line is the 8-byte tail", strcmp(s_lines[2], "+MTOTABLK:7,192,C0C1C2C3C4C5C6C7") == 0);
    check("every line under 254 bytes", strlen(s_lines[0]) < 254 && strlen(s_lines[1]) < 254);
    check("MTOTAGET of the wrong seq is +MTERR:12", run("MTOTAGET", AT_SET, "8") == MT_ERR_OTA_STATE);
    check("a pull can be repeated", run("MTOTAGET", AT_SET, "7") == AT_R_OK);

    s_acked_rc = 0;
    check("MTOTAACK of the wrong seq is +MTERR:12", run("MTOTAACK", AT_SET, "6") == MT_ERR_OTA_STATE);
    check("MTOTAACK=7 -> OK and forwarded", run("MTOTAACK", AT_SET, "7") == AT_R_OK && s_acked_last == 7u);
    check("MTOTAGET after the ack is +MTERR:12", run("MTOTAGET", AT_SET, "7") == MT_ERR_OTA_STATE);
    check("next block accepted after the ack", mt_ota_on_block(8, blk, 96) == 0);

    mt_ota_block_timeout();
    check("timeout drops the pending block", run("MTOTAGET", AT_SET, "8") == MT_ERR_OTA_STATE);

    check("block accepted again", mt_ota_on_block(9, blk, 5) == 0);
    run("MTOTA", AT_SET, "0");
    check("MTOTA=0 drops the pending block", run("MTOTAGET", AT_SET, "9") == MT_ERR_OTA_STATE);
    run("MTOTA", AT_SET, "1");
    check("block accepted", mt_ota_on_block(10, blk, 5) == 0);
    mt_ota_on_state(MT_OTA_ERROR, "abort");
    check("ERROR drops the pending block", run("MTOTAGET", AT_SET, "10") == MT_ERR_OTA_STATE);
    check("MTOTAGET with no argument is a bad parameter", run("MTOTAGET", AT_SET, "") == MT_ERR_BAD_PARAM);
    check("MTOTAACK query form is a bare ERROR", run("MTOTAACK", AT_QUERY, NULL) == MT_R_ERROR);

    check("block accepted for the acked-refusal case", mt_ota_on_block(11, blk, 5) == 0);
    s_acked_rc = -1;
    check("acked but the platform refused is +MTERR:12", run("MTOTAACK", AT_SET, "11") == MT_ERR_OTA_STATE);
    check("the block is no longer pending after a refused ack", run("MTOTAGET", AT_SET, "11") == MT_ERR_OTA_STATE);
    s_acked_rc = 0;
}

static void test_block_chunking(void)
{
    static uint8_t blk192[192];
    static uint8_t blk1024[1024];
    for (int i = 0; i < 192; i++) blk192[i] = (uint8_t)i;
    for (int i = 0; i < 1024; i++) blk1024[i] = (uint8_t)i;

    mt_ota_init();
    run("MTOTA", AT_SET, "1");
    check("192-byte block accepted", mt_ota_on_block(1, blk192, sizeof(blk192)) == 0);
    cap_reset();
    check("MTOTAGET=1 -> OK", run("MTOTAGET", AT_SET, "1") == AT_R_OK);
    check("a 192-byte block is exactly two full lines", s_nlines == 2);
    check("second line at offset 96 with 192 hex digits",
          strncmp(s_lines[1], "+MTOTABLK:1,96,", 15) == 0 &&
          strlen(s_lines[1]) == strlen("+MTOTABLK:1,96,") + 192);

    mt_ota_init();
    run("MTOTA", AT_SET, "1");
    check("1024-byte block accepted", mt_ota_on_block(2, blk1024, sizeof(blk1024)) == 0);
    cap_reset();
    check("MTOTAGET=2 -> OK", run("MTOTAGET", AT_SET, "2") == AT_R_OK);
    check("a 1024-byte block is eleven lines, the last 64 bytes", s_nlines == 11);
    check("line 10 has offset 960 and 128 hex digits",
          strncmp(s_lines[10], "+MTOTABLK:2,960,", 16) == 0 &&
          strlen(s_lines[10]) == strlen("+MTOTABLK:2,960,") + 128);
    bool all_under_254 = true;
    for (int i = 0; i < s_nlines; i++) {
        if (strlen(s_lines[i]) >= 254) {
            all_under_254 = false;
        }
    }
    check("every line under 254 chars", all_under_254);
}

static void test_staged(void)
{
    mt_ota_init();
    run("MTOTA", AT_SET, "1");
    s_staged_rc = 0;
    check("MTOTASTAGED=1 outside DOWNLOADED is +MTERR:12", run("MTOTASTAGED", AT_SET, "1") == MT_ERR_OTA_STATE);
    mt_ota_on_state(MT_OTA_DOWNLOADED, NULL);
    check("MTOTASTAGED=1 -> OK, forwarded", run("MTOTASTAGED", AT_SET, "1") == AT_R_OK && s_staged_ok == 1);
    mt_ota_on_state(MT_OTA_DOWNLOADED, NULL);
    check("MTOTASTAGED=0,3 -> OK, reason forwarded", run("MTOTASTAGED", AT_SET, "0,3") == AT_R_OK && s_staged_ok == 0 && s_staged_reason == 3);
    mt_ota_on_state(MT_OTA_DOWNLOADED, NULL);
    check("MTOTASTAGED=0 without a reason is a bad parameter", run("MTOTASTAGED", AT_SET, "0") == MT_ERR_BAD_PARAM);
    check("MTOTASTAGED=0,7 (unknown reason) is a bad parameter", run("MTOTASTAGED", AT_SET, "0,7") == MT_ERR_BAD_PARAM);
    check("MTOTASTAGED=1,1 (a reason on an acceptance) is a bad parameter",
          run("MTOTASTAGED", AT_SET, "1,1") == MT_ERR_BAD_PARAM);
    check("MTOTASTAGED=1, (an empty trailing field) is a bad parameter",
          run("MTOTASTAGED", AT_SET, "1,") == MT_ERR_BAD_PARAM);
    check("MTOTASTAGED=-1 (a signed verdict) is a bad parameter",
          run("MTOTASTAGED", AT_SET, "-1") == MT_ERR_BAD_PARAM);
    s_staged_rc = -1;
    check("platform refusal is +MTERR:12", run("MTOTASTAGED", AT_SET, "1") == MT_ERR_OTA_STATE);
    s_staged_rc = 0;
}

int main(void)
{
    printf("\n===== mt_ota tests =====\n");
    test_table();
    test_swver();
    test_swver_corrupt();
    test_mode();
    test_state_urcs();
    test_block_relay();
    test_block_chunking();
    test_staged();
    printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
