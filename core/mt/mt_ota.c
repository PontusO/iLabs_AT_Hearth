/*
 * mt_ota.c - the AT+MT side of firmware over the air. See mt_ota.h.
 *
 * Threads: the command handlers run on the AT parser task; mt_ota_on_*
 * run on the Matter thread. The pending block (pointer, length, sequence)
 * is the only state both touch, under HEARTH_CRIT_OTA; everything else is
 * written by one side and read by the other as plain ints, which the
 * contract tolerates (a stale mode or state costs one extra line, never a
 * wrong pointer).
 */
#include "mt_ota.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hearth_log.h"
#include "hearth_port.h"
#include "mt_at.h"
#include "mt_at_config.h"
#include "mt_err.h"
#include "mt_matter.h"

static const char *TAG = "mt_ota";

#define MT_OTA_NVS_NAMESPACE "mt_cfg"
#define MT_OTA_NVS_KEY_VER   "swver"
#define MT_OTA_NVS_KEY_STR   "swverstr"

static int s_mode;
static mt_ota_state_t s_state;
static uint8_t s_percent;

/* the pending block, HEARTH_CRIT_OTA */
static bool s_blk_pending;
static uint32_t s_blk_seq;
static const uint8_t *s_blk_data;
static size_t s_blk_len;

static const char *const s_state_names[] = {
    "IDLE", "QUERYING", "AVAILABLE", "DOWNLOADING", "DOWNLOADED",
    "APPLYING", "DEFERRED", "DISCONTINUED", "ERROR",
};

const char *mt_ota_state_name(mt_ota_state_t s)
{
    if ((unsigned)s >= sizeof(s_state_names) / sizeof(s_state_names[0])) {
        return "ERROR";
    }
    return s_state_names[s];
}

/* One gate, one writer (mt_at.c's rule): every URC goes through mt_at_urc. */
static void urc(const char *fmt, ...)
{
    char line[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    mt_at_urc(line);
}

static void drop_pending(void)
{
    hearth_crit_enter(HEARTH_CRIT_OTA);
    s_blk_pending = false;
    s_blk_data = NULL;
    s_blk_len = 0;
    hearth_crit_exit(HEARTH_CRIT_OTA);
}

void mt_ota_init(void)
{
    s_mode = 0;
    s_state = MT_OTA_IDLE;
    s_percent = 0;
    drop_pending();
}

int mt_ota_mode(void)
{
    return s_mode;
}

/* ---- persisted product version ---------------------------------------- */

int mt_ota_swver_stored(uint32_t *version, char *str, size_t str_len)
{
    uint8_t raw[4];
    size_t n = sizeof(raw);
    int r = hearth_kv_get_blob(MT_OTA_NVS_NAMESPACE, MT_OTA_NVS_KEY_VER, raw, &n);
    if (r != 0) {
        return r;              /* 1 absent, -1 error */
    }
    if (n != sizeof(raw) || str_len < MT_OTA_SWVER_STR_MAX) {
        return -1;
    }
    char s[MT_OTA_SWVER_STR_MAX];
    size_t sn = sizeof(s);
    r = hearth_kv_get_blob(MT_OTA_NVS_NAMESPACE, MT_OTA_NVS_KEY_STR, s, &sn);
    if (r != 0) {
        return r == 1 ? -1 : r; /* a version without its string is corrupt */
    }
    if (sn == 0 || sn > sizeof(s) || s[sn - 1] != '\0') {
        return -1;
    }
    *version = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
               ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    memcpy(str, s, sn);
    return 0;
}

static int swver_store(uint32_t version, const char *str)
{
    uint32_t cur;
    char curs[MT_OTA_SWVER_STR_MAX];
    if (mt_ota_swver_stored(&cur, curs, sizeof(curs)) == 0 &&
        cur == version && strcmp(curs, str) == 0) {
        return 0;               /* unchanged: no flash write */
    }
    uint8_t raw[4] = { (uint8_t)version, (uint8_t)(version >> 8),
                       (uint8_t)(version >> 16), (uint8_t)(version >> 24) };
    /* The two writes below are not atomic: a failure between them leaves a
     * stored version with no matching string, which mt_ota_swver_stored()
     * then reports as -1 (corrupt) rather than 0 or 1; the host's next
     * AT+MTSWVER=... overwrites both keys and repairs it. */
    if (hearth_kv_set_blob(MT_OTA_NVS_NAMESPACE, MT_OTA_NVS_KEY_VER, raw, sizeof(raw)) != 0 ||
        hearth_kv_set_blob(MT_OTA_NVS_NAMESPACE, MT_OTA_NVS_KEY_STR, str, strlen(str) + 1) != 0) {
        HEARTH_LOGE(TAG, "storing the product version failed");
        return -1;
    }
    HEARTH_LOGI(TAG, "product version stored: %lu \"%s\"", (unsigned long)version, str);
    return 0;
}

/* ---- helpers ------------------------------------------------------------ */

/* "<u32>" -> 0 ok, -1 malformed */
static int parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }
    /* strtoul accepts a sign and wraps a negative one round to a huge
     * unsigned, so "-1" would parse as 4294967295 and the range check below
     * would not catch it: unsigned long is 32 bits on both targets, which
     * makes `v > 0xFFFFFFFFul` a no-op there. A sign is not part of the
     * grammar for any of these fields, so refuse it outright. */
    if (*s == '-' || *s == '+') {
        return -1;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v > 0xFFFFFFFFul) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* Strip one pair of surrounding double quotes in place. */
static char *unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

static const char *variant_token(void)
{
#if MT_COMBINED_IMAGE
    return "combined";
#else
    int transport = 0, enabled = 0, connected = 0;
    if (mt_matter_net_info(&transport, &enabled, &connected) != 0) {
        return "unknown";
    }
    return transport == MT_NET_THREAD ? "thread" : "wifi";
#endif
}

/* ---- platform -> core --------------------------------------------------- */

void mt_ota_on_state(mt_ota_state_t state, const char *detail)
{
    s_state = state;
    if (state == MT_OTA_DOWNLOADING && detail != NULL) {
        s_percent = (uint8_t)strtoul(detail, NULL, 10);
    } else if (state == MT_OTA_DOWNLOADED || state == MT_OTA_APPLYING) {
        s_percent = 100;
    } else if (state != MT_OTA_DOWNLOADING) {
        s_percent = 0;
    }
    if (state == MT_OTA_IDLE || state == MT_OTA_ERROR || state == MT_OTA_DISCONTINUED) {
        drop_pending();
    }
    if (!s_mode) {
        return;
    }
    if (detail != NULL && *detail != '\0') {
        urc("+MTOTA:%s,%s", mt_ota_state_name(state), detail);
    } else {
        urc("+MTOTA:%s", mt_ota_state_name(state));
    }
}

int mt_ota_on_block(uint32_t seq, const uint8_t *data, size_t len)
{
    if (!s_mode || data == NULL || len == 0) {
        return -1;
    }
    hearth_crit_enter(HEARTH_CRIT_OTA);
    if (s_blk_pending) {
        hearth_crit_exit(HEARTH_CRIT_OTA);
        return -1;
    }
    s_blk_pending = true;
    s_blk_seq = seq;
    s_blk_data = data;
    s_blk_len = len;
    hearth_crit_exit(HEARTH_CRIT_OTA);
    s_state = MT_OTA_DOWNLOADING;
    urc("+MTOTA:BLOCK,%lu,%u", (unsigned long)seq, (unsigned)len);
    return 0;
}

void mt_ota_block_timeout(void)
{
    drop_pending();
}

void mt_ota_on_apply(void)
{
    s_state = MT_OTA_APPLYING;
    s_percent = 100;
    if (s_mode) {
        urc("+MTOTA:APPLY");
    }
}

/* ---- commands ----------------------------------------------------------- */

/*
 * AT+MTSWVER=<version>,"<string>"  -> OK      persisted, written only on change
 * AT+MTSWVER?                      -> +MTSWVER:<version>,"<string>"
 * Never declared: +MTSWVER:0,"" (Hearth's own version is in force).
 */
static int cmd_mtswver(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        uint32_t v = 0;
        char s[MT_OTA_SWVER_STR_MAX] = "";
        int r = mt_ota_swver_stored(&v, s, sizeof(s));
        if (r < 0) {
            return MT_ERR_PERSIST;
        }
        hearth_link_write_line("+MTSWVER:%lu,\"%s\"", (unsigned long)v, r == 0 ? s : "");
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    char *comma = strchr(args, ',');
    if (comma == NULL) {
        return MT_ERR_BAD_PARAM;
    }
    *comma = '\0';
    uint32_t v;
    if (parse_u32(args, &v) != 0) {
        return MT_ERR_BAD_PARAM;
    }
    char *s = unquote(comma + 1);
    size_t n = strlen(s);
    if (n == 0 || n >= MT_OTA_SWVER_STR_MAX || strchr(s, '"') != NULL) {
        return MT_ERR_BAD_PARAM;
    }
    if (swver_store(v, s) != 0) {
        return MT_ERR_PERSIST;
    }
    if (mt_matter_swver_set(v, s) != 0) {
        HEARTH_LOGW(TAG, "live version update refused; in force from the next boot");
    }
    return AT_R_OK;
}

/*
 * AT+MTOTA=0|1|2  -> OK   (spec 5.2)
 * AT+MTOTA?       -> +MTOTA:<mode>,<STATE>,<percent>,<variant>
 */
static int cmd_mtota(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        hearth_link_write_line("+MTOTA:%d,%s,%u,%s", s_mode, mt_ota_state_name(s_state),
                               (unsigned)s_percent, variant_token());
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    uint32_t m;
    if (parse_u32(args, &m) != 0 || m > 2) {
        return MT_ERR_BAD_PARAM;
    }
    if (m == 2) {
        /* A query-now is only meaningful from idle. Mid-attempt the platform's
         * driver would decline it silently (its own "already in progress"
         * path), so the host would get an OK for a query that never left, and
         * a state line ordering it could not explain. Refuse it here instead,
         * where the answer can say so. */
        if (!s_mode || s_state != MT_OTA_IDLE) {
            return MT_ERR_OTA_STATE;
        }
        return mt_matter_ota_set_mode(2) == 0 ? AT_R_OK : MT_ERR_OTA_STATE;
    }
    if (m == 0) {
        /* Disabling never fails from the host's point of view: call the
         * platform for its side effect (aborting anything in flight) but
         * always collapse to the disabled state regardless of what it
         * returns, since a platform with no requestor at all (mode 1's
         * +MTERR:8 case) still has nothing to abort and nothing to refuse. */
        (void)mt_matter_ota_set_mode(0);
        s_mode = 0;
        s_state = MT_OTA_IDLE;
        s_percent = 0;
        drop_pending();
        return AT_R_OK;
    }
    if (mt_matter_ota_set_mode((int)m) != 0) {
        return MT_ERR_UNSUPPORTED;
    }
    s_mode = (int)m;
    return AT_R_OK;
}

/*
 * AT+MTOTAGET=<seq> -> +MTOTABLK:<seq>,<off>,<hex> per 96-byte chunk -> OK
 * +MTERR:12 when no block <seq> is pending. A pull may be repeated. A pull
 * interrupted by a drop ends with +MTERR:12 after the lines already sent;
 * the host discards them.
 */
static int cmd_mtotaget(at_type_t type, char *args)
{
    static const char hexd[] = "0123456789ABCDEF";
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    uint32_t seq;
    if (parse_u32(args, &seq) != 0) {
        return MT_ERR_BAD_PARAM;
    }
    /*
     * The Matter thread can drop the pending block mid-loop (a timeout, an
     * abort, or MTOTA=0 landing between two iterations of this loop): the
     * pending check below then fails on the NEXT chunk and this returns
     * +MTERR:12 after some +MTOTABLK lines already reached the host. That is
     * intended, not a defect: the pointer is only ever read here under
     * HEARTH_CRIT_OTA with the pending check right beside it, so a drop can
     * never be observed as a stale or dangling pointer, only as "no longer
     * pending"; holding the section across hearth_link_write_line() to make
     * the whole pull atomic would violate hearth_port.h's rule that a
     * critical section never wraps a blocking call; and a mid-pull ERROR is
     * itself the useful signal: the block is gone, so the host discards
     * whatever partial lines it already has and waits for the next +MTOTA
     * state line to say what happened instead.
     */
    for (size_t off = 0;; off += MT_OTA_CHUNK) {
        uint8_t chunk[MT_OTA_CHUNK];
        size_t n;
        hearth_crit_enter(HEARTH_CRIT_OTA);
        if (!s_blk_pending || s_blk_seq != seq) {
            hearth_crit_exit(HEARTH_CRIT_OTA);
            return MT_ERR_OTA_STATE;
        }
        if (off >= s_blk_len) {
            hearth_crit_exit(HEARTH_CRIT_OTA);
            return AT_R_OK;
        }
        n = s_blk_len - off;
        if (n > MT_OTA_CHUNK) {
            n = MT_OTA_CHUNK;
        }
        memcpy(chunk, s_blk_data + off, n);
        hearth_crit_exit(HEARTH_CRIT_OTA);

        char hex[2 * MT_OTA_CHUNK + 1];
        for (size_t i = 0; i < n; i++) {
            hex[2 * i] = hexd[chunk[i] >> 4];
            hex[2 * i + 1] = hexd[chunk[i] & 0x0F];
        }
        hex[2 * n] = '\0';
        hearth_link_write_line("+MTOTABLK:%lu,%u,%s", (unsigned long)seq, (unsigned)off, hex);
    }
}

/* AT+MTOTAACK=<seq> -> OK; +MTERR:12 when no block <seq> is pending. */
static int cmd_mtotaack(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    uint32_t seq;
    if (parse_u32(args, &seq) != 0) {
        return MT_ERR_BAD_PARAM;
    }
    hearth_crit_enter(HEARTH_CRIT_OTA);
    if (!s_blk_pending || s_blk_seq != seq) {
        hearth_crit_exit(HEARTH_CRIT_OTA);
        return MT_ERR_OTA_STATE;
    }
    s_blk_pending = false;
    s_blk_data = NULL;
    s_blk_len = 0;
    hearth_crit_exit(HEARTH_CRIT_OTA);
    if (mt_matter_ota_block_acked(seq) != 0) {
        return MT_ERR_OTA_STATE;
    }
    return AT_R_OK;
}

/*
 * AT+MTOTASTAGED=1          -> OK   proceed to ApplyUpdateRequest
 * AT+MTOTASTAGED=0,<reason> -> OK   refuse; reason 1..6 (spec 5.4)
 * Only in DOWNLOADED, otherwise +MTERR:12.
 */
static int cmd_mtotastaged(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    char *comma = strchr(args, ',');
    if (comma != NULL) {
        *comma = '\0';
    }
    uint32_t ok, reason = 0;
    if (parse_u32(args, &ok) != 0 || ok > 1) {
        return MT_ERR_BAD_PARAM;
    }
    if (ok == 0) {
        if (comma == NULL || parse_u32(comma + 1, &reason) != 0 || reason < 1 || reason > 6) {
            return MT_ERR_BAD_PARAM;
        }
    } else if (comma != NULL) {
        return MT_ERR_BAD_PARAM;
    }
    if (s_state != MT_OTA_DOWNLOADED) {
        return MT_ERR_OTA_STATE;
    }
    if (mt_matter_ota_staged((int)ok, (int)reason) != 0) {
        return MT_ERR_OTA_STATE;
    }
    return AT_R_OK;
}

static const at_command_t s_ota_cmds[] = {
    { "MTSWVER",     cmd_mtswver     },
    { "MTOTA",       cmd_mtota       },
    { "MTOTAGET",    cmd_mtotaget    },
    { "MTOTAACK",    cmd_mtotaack    },
    { "MTOTASTAGED", cmd_mtotastaged },
};

const at_command_t *mt_ota_commands(size_t *count)
{
    *count = sizeof(s_ota_cmds) / sizeof(s_ota_cmds[0]);
    return s_ota_cmds;
}
