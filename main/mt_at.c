/*
 * mt_at.c - Matter AT command handlers and registration.
 *
 * Runs on the shared at_core engine with its own "+MTERR" code space, mirroring
 * the ESP-NOW firmware's en_at.c. Covers identity, the Matter lifecycle, the
 * data model, the host-declared endpoint composition, and event subscription.
 * Anything touching esp_matter or CHIP goes through the C-linkage bridge in
 * mt_matter.h, so this stays a plain C translation unit.
 *
 * Handler return convention (see at_parser.h):
 *   AT_R_OK    - engine prints "OK"
 *   AT_R_DONE  - handler already printed its own terminal response
 *   1..99      - engine prints "+MTERR:<n>" then "ERROR"
 *   MT_R_ERROR - engine prints plain "ERROR"
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_system.h"

#include "at_uart.h"
#include "at_parser.h"
#include "mt_at_config.h"
#include "mt_at.h"
#include "mt_cmdbox.h"
#include "mt_comp_store.h"
#include "mt_composition.h"
#include "mt_devtypes.h"
#include "mt_matter.h"
#include "mt_transport.h"

/* Longest +MTEVT line: "+MTEVT:31," plus a detail. Details are "0"/"1" today
 * (bits 10, 11 and 24); 48 leaves room without putting a 512-byte
 * MT_AT_LINE_MAX buffer on the CHIP task's stack. Truncation is safe. */
#define MT_EVT_LINE_MAX 48

/* Longest +MTCMD/+MTCMDTO line: "+MTCMD:" + seq, ep, cluster, command (the
 * fixed head) plus a caller-formatted tail of up to four more fields
 * (mt_cmd_forward_fields(), the RVC/microwave batch's SetCookingParameters
 * and friends). Five numeric heads worst case (seq, ep, cluster, command,
 * 10 digits each bar ep's 5) plus a four-field comma-joined tail (10 digits
 * each) comfortably fits in 112 with room to spare; 72 was sized only for
 * the single optional payload field mt_cmd_forward_payload() added on top of
 * the four-field base. Same rationale as MT_EVT_LINE_MAX: off the CHIP
 * task's stack rather than the 512-byte MT_AT_LINE_MAX buffer. */
#define MT_CMD_LINE_MAX 112

/* +MTERR:<n> code space (mirrors the ESP-NOW layout: 1..99 carry a code,
 * >= MT_ERR_GENERIC is a plain "ERROR"). Real codes are assigned in B4. */
#define MT_ERR_BAD_PARAM    1   /* bad parameter or out of range          */
#define MT_ERR_NO_ENDPOINT  2   /* unknown endpoint                       */
#define MT_ERR_NO_CLUSTER   3   /* unknown cluster                        */
#define MT_ERR_NO_ATTRIBUTE 4   /* unknown attribute                      */
#define MT_ERR_ATTR_TYPE    5   /* attribute type unsupported here        */
#define MT_ERR_DEVTYPE      6   /* unknown or unsupported device type     */
#define MT_ERR_PERSIST      7   /* NVS persistence failure                */
#define MT_ERR_UNSUPPORTED  8   /* unknown/unsupported command            */
#define MT_ERR_NOT_READY    9   /* no composition, or stack not started   */
#define MT_ERR_COMP_REJECT  10  /* nothing staged, or endpoint cap hit    */
#define MT_ERR_GENERIC      100 /* plain ERROR, no +MTERR line            */
#define MT_R_ERROR          MT_ERR_GENERIC

/* Set once the AT UART (and its TX mutex) exist. See mt_at_urc(). Declared
 * up here, rather than next to mt_at_start() below, because mt_cmd_forward()
 * needs to read it and is defined ahead of mt_at_start() in this file. */
static volatile bool s_at_up = false;

/* ---- identity (3GPP TS 27.007 style, mirrors the ESP-NOW image) ------- */

static int cmd_cgmi(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    at_uart_write_line("%s", MT_MANUFACTURER);
    return AT_R_OK;
}

static int cmd_cgmm(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    at_uart_write_line("%s", MT_MODEL);
    return AT_R_OK;
}

static int cmd_cgmr(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    at_uart_write_line("%s", MT_FW_VERSION);
    return AT_R_OK;
}

/* ---- version query ---------------------------------------------------- */

static int cmd_ver(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return MT_R_ERROR;
    }
    at_uart_write_line("+MTVER:%s", MT_FW_VERSION);
    return AT_R_OK;
}

/* ---- Matter lifecycle & commissioning (B4.2) -------------------------- */

/* AT+MTSTATE? -> +MTSTATE:<state>,<fabric_count>  (state: 0 uninit, 1
 * commissioning, 2 operational). */
static int cmd_mtstate(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return MT_R_ERROR;
    }
    at_uart_write_line("+MTSTATE:%d,%d", mt_matter_state(), mt_matter_fabric_count());
    return AT_R_OK;
}

/* AT+MTFABRICS? -> +MTFABRICS:<count> */
static int cmd_mtfabrics(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return MT_R_ERROR;
    }
    at_uart_write_line("+MTFABRICS:%d", mt_matter_fabric_count());
    return AT_R_OK;
}

/*
 * AT+MTCOMMISSION[=<timeout_s>] -> open a basic commissioning window.
 * Default 300 s. Progress is reported by the commissioning event bits (0, 3, 5),
 * which are in the default event mask, from the platform event callback.
 *
 * The floor is 180 s because CHIP enforces the Matter 3-minute minimum
 * announcement duration (CommissioningWindowManager::MinCommissioningTimeout).
 * This check used to allow 30, so 30..179 passed here and then failed inside
 * CHIP with a bare ERROR and a spurious +MTEVT:4 from the cleanup path.
 */
static int cmd_mtcommission(at_type_t type, char *args)
{
    unsigned timeout = 300;
    if (type == AT_SET) {
        if (!at_parse_uint(args, &timeout) || timeout < 180 || timeout > 900) {
            return MT_ERR_BAD_PARAM;
        }
    } else if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    if (mt_matter_open_commissioning((int)timeout) != 0) {
        return MT_R_ERROR;
    }
    return AT_R_OK;
}

/* AT+MTCODES? -> +MTCODES:<qr_payload>,<manual_pairing_code> */
static int cmd_mtcodes(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return MT_R_ERROR;
    }
    char qr[96];
    char manual[32];
    if (mt_matter_onboarding_codes(qr, sizeof(qr), manual, sizeof(manual)) != 0) {
        return MT_R_ERROR;
    }
    at_uart_write_line("+MTCODES:%s,%s", qr, manual);
    return AT_R_OK;
}

/*
 * AT+MTRESET -> Matter reset: erase fabrics, credentials and attribute
 * persistence, then reboot. The endpoint composition SURVIVES, because it is a
 * product definition supplied by the host firmware rather than user data: a
 * board that is a dimmable light plus a temperature sensor is still that after
 * an end user unpairs it. Use AT+MTFRESET to erase everything.
 */
static int cmd_mtreset(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    /* Acknowledge, drain the UART, then reset - the device reboots and the host
     * resynchronizes on the next "+MTREADY". */
    at_uart_write_line("OK");
    vTaskDelay(pdMS_TO_TICKS(100));
    mt_matter_factory_reset();
    return AT_R_DONE;
}

/*
 * AT+MTFRESET -> full factory reset: everything AT+MTRESET erases, plus the
 * endpoint composition, leaving a blank unconfigured board. A manufacturing and
 * development operation, not an end-user one.
 *
 * The fctry partition (per-device attestation data provisioned at manufacture)
 * is deliberately NOT touched: erasing it would destroy the unit's identity,
 * which no AT command should be able to do.
 */
static int cmd_mtfreset(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    /* Erase the composition first: mt_matter_factory_reset() reboots, so
     * anything sequenced after it may never run. */
    if (mt_comp_store_erase() != 0) {
        return MT_ERR_PERSIST;
    }
    at_uart_write_line("OK");
    vTaskDelay(pdMS_TO_TICKS(100));
    mt_matter_factory_reset();
    return AT_R_DONE;
}

/* ---- data model (B4.3) ------------------------------------------------ */

/* Parse an unsigned token, hex ("0x..") or decimal. */
static bool parse_u(const char *s, unsigned long *out)
{
    if (!s || *s == '\0') {
        return false;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (*end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* Map a bridge attribute result onto this personality's +MTERR code space. */
static int attr_err_to_mterr(int r)
{
    switch (r) {
    case MT_ATTR_ERR_ENDPOINT:  return MT_ERR_NO_ENDPOINT;
    case MT_ATTR_ERR_CLUSTER:   return MT_ERR_NO_CLUSTER;
    case MT_ATTR_ERR_ATTRIBUTE: return MT_ERR_NO_ATTRIBUTE;
    case MT_ATTR_ERR_TYPE:      return MT_ERR_ATTR_TYPE;
    /* C1b/B139 fix round 1: an out-of-range value on a valid, existing
     * integer attribute is a bad parameter, not a type/lookup failure. */
    case MT_ATTR_ERR_VALUE:     return MT_ERR_BAD_PARAM;
    default:                    return MT_R_ERROR;
    }
}

/*
 * AT+MTATTR=<ep>,<cluster>,<attr>              -> read
 *     -> +MTATTR:<ep>,<cluster>,<attr>,<val>
 * AT+MTATTR=<ep>,<cluster>,<attr>,<val>[,<mode>] -> write -> OK
 *
 * <cluster>/<attr> accept hex (0x0006) or decimal; <val> is an integer.
 *
 * <mode> selects how the write is published, default 1:
 *   1  attribute::update() - subscribers and bound devices see the change
 *   0  attribute::set_val() - local only, no report
 * Mode 0 exists so a host reflecting a change that came FROM a controller does
 * not echo it back and loop.
 *
 * A controller-driven change is reported asynchronously as a +MTATTR URC (from
 * the attribute callback in main.cpp).
 */
static int cmd_mtattr(at_type_t type, char *args)
{
    char *f[5];
    int n = at_split_args(args, f, 5);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n < 3) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long ep, cluster, attr;
    if (!parse_u(f[0], &ep) || !parse_u(f[1], &cluster) || !parse_u(f[2], &attr)) {
        return MT_ERR_BAD_PARAM;
    }

    if (n == 3) {
        long v;
        int r = mt_matter_attr_read((uint16_t)ep, (uint32_t)cluster, (uint32_t)attr, &v);
        if (r != MT_ATTR_OK) {
            return attr_err_to_mterr(r);
        }
        at_uart_write_line("+MTATTR:%lu,%lu,%lu,%ld", ep, cluster, attr, v);
        return AT_R_OK;
    }

    unsigned long val;
    if (!parse_u(f[3], &val)) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long mode = 1;
    if (n == 5 && (!parse_u(f[4], &mode) || mode > 1)) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_attr_write((uint16_t)ep, (uint32_t)cluster, (uint32_t)attr,
                                 (long)val, mode != 0);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTSWITCH=<ep>[,<action>] -> emit a switch event on <ep>. Action 0 (the
 * default) is InitialPress, the upstream click(). Other actions are reserved
 * for the richer switch features and answer +MTERR:1 until they exist. The
 * first event-emission command on this surface: nothing echoes back,
 * controllers see it via their subscriptions.
 */
static int cmd_mtswitch(at_type_t type, char *args)
{
    char *f[2];
    int n = at_split_args(args, f, 2);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n < 1) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long ep, action = 0;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    if (n >= 2 && (!parse_u(f[1], &action) || action != 0)) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_switch_click((uint16_t)ep);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTTEMPLEVELS=<ep>,"<label1>"[,"<label2>",...] -> store the label list
 * backing a TemperatureLevel-variant cabinet's SupportedTemperatureLevels
 * attribute. Set-only; the grammar's quoted, comma-separated label list is
 * not at_split_args()'s job, since a comma inside a quoted label is legal
 * and at_split_args() splits on every comma unconditionally. Parsed here
 * instead, directly off the raw argument string, so at_core (shared with the
 * ESP-NOW firmware) stays untouched.
 *
 * Grammar and content rules (violating any of these is +MTERR:1, decided
 * here before mt_matter_temp_levels_set() is ever called):
 *   - <ep>: a bare unsigned token, hex or decimal, up to the first comma.
 *   - 1..MT_TEMP_LEVEL_MAX_COUNT labels, each double-quoted.
 *   - each label 1..MT_TEMP_LEVEL_MAX_LEN bytes, printable ASCII
 *     (0x20..0x7E) only.
 *   - no double-quote character inside a label: since a label is scanned
 *     up to its next raw '"', a quote inside the intended content always
 *     closes the token early, leaving trailing bytes before the following
 *     comma or end of string, which the "junk after the closing quote"
 *     check below rejects.
 */
static int cmd_mttemplevels(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (!args || *args == '\0') {
        return MT_ERR_BAD_PARAM;
    }

    char *comma = strchr(args, ',');
    if (!comma) {
        return MT_ERR_BAD_PARAM;
    }
    *comma = '\0';
    char *p = comma + 1;

    unsigned long ep;
    if (!parse_u(args, &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    const char *labels[MT_TEMP_LEVEL_MAX_COUNT];
    char label_buf[MT_TEMP_LEVEL_MAX_COUNT][MT_TEMP_LEVEL_MAX_LEN + 1];
    uint8_t count = 0;

    while (*p != '\0') {
        if (*p != '"') {
            return MT_ERR_BAD_PARAM;
        }
        p++;
        char *start = p;
        while (*p != '"') {
            if (*p == '\0') {
                return MT_ERR_BAD_PARAM;  /* unterminated quote */
            }
            p++;
        }
        size_t len = (size_t)(p - start);
        p++;  /* past the closing quote */

        if (len < 1 || len > MT_TEMP_LEVEL_MAX_LEN) {
            return MT_ERR_BAD_PARAM;
        }
        if (count >= MT_TEMP_LEVEL_MAX_COUNT) {
            return MT_ERR_BAD_PARAM;
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)start[i];
            if (c < 0x20 || c > 0x7E) {
                return MT_ERR_BAD_PARAM;
            }
        }
        memcpy(label_buf[count], start, len);
        label_buf[count][len] = '\0';
        labels[count] = label_buf[count];
        count++;

        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return MT_ERR_BAD_PARAM;  /* trailing comma, no next label */
            }
            continue;
        }
        if (*p == '\0') {
            break;
        }
        return MT_ERR_BAD_PARAM;  /* junk after the closing quote */
    }

    if (count < 1) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_temp_levels_set((uint16_t)ep, labels, count);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTLOCK=<ep>,<state>[,<source>] -> report the DoorLock cluster's LockState
 * on <ep> (typically after the host has physically actuated the lock).
 * Set-only, the AT+MTSWITCH/AT+MTTEMPLEVELS convention: bare/query forms
 * answer a plain ERROR.
 *
 * <state>: 0 NotFullyLocked, 1 Locked, 2 Unlocked (DlLockState protocol
 * values, spec 3.9/3.18). These are wire protocol values fixed by the Matter
 * spec, not SDK enum constants, so checking the 0..2 range here rather than
 * through an accessor is not a transcription of anything: the AT contract IS
 * the source of truth for this range.
 *
 * <source>: optional, defaults to mt_matter_lock_source_manual()
 * (OperationSourceEnum::kManual, read from the pinned CHIP header, never
 * transcribed into this file). Validated against
 * mt_matter_lock_source_max(): a value above it is +MTERR:1, the same as any
 * other out-of-range parameter.
 *
 * Lookup errors follow the established division: +MTERR:2 unknown endpoint,
 * +MTERR:3 the endpoint has no DoorLock cluster. A bare ERROR covers an
 * unclassified runtime failure, routed through attr_err_to_mterr() like
 * every other bridge call in this file.
 */
static int cmd_mtlock(at_type_t type, char *args)
{
    char *f[3];
    int n = at_split_args(args, f, 3);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n < 2) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long ep, state;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[1], &state) || state > 2) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long source = mt_matter_lock_source_manual();
    if (n == 3 && (!parse_u(f[2], &source) || source > mt_matter_lock_source_max())) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_lock_state_set((uint16_t)ep, (uint8_t)state, (uint8_t)source);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTVALVE=<ep>,<state>[,<level>] -> report the host's own valve
 * actuation on <ep> (design spec F1: the ValveConfigurationAndControl
 * server answers the controller Success regardless of the +MTCMD verdict
 * this firmware forwarded for Open/Close, so this is the only place the
 * real outcome reaches the fabric). Set-only, the AT+MTLOCK convention:
 * bare/query forms answer a plain ERROR.
 *
 * <state>: 0 Closed, 1 Open, 2 Transitioning (ValveStateEnum wire values,
 * spec 3.9/3.19). A wire protocol value fixed by the Matter spec, checked
 * here directly for the same reason AT+MTLOCK's <state> is.
 *
 * <level>: optional, 0..100. Absent is passed to the bridge as -1.
 *
 * Lookup errors follow the established division: +MTERR:2 unknown
 * endpoint, +MTERR:3 the endpoint has no ValveConfigurationAndControl
 * cluster. A bare ERROR covers an unclassified runtime failure, routed
 * through attr_err_to_mterr() like every other bridge call in this file.
 */
static int cmd_mtvalve(at_type_t type, char *args)
{
    char *f[3];
    int n = at_split_args(args, f, 3);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n < 2) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long ep, state;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[1], &state) || state > 2) {
        return MT_ERR_BAD_PARAM;
    }

    long level = -1;
    if (n == 3) {
        unsigned long lv;
        if (!parse_u(f[2], &lv) || lv > 100) {
            return MT_ERR_BAD_PARAM;
        }
        level = (long)lv;
    }

    int r = mt_matter_valve_state_set((uint16_t)ep, (uint8_t)state, (int)level);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTMODES has two forms, disambiguated by type (AT_MT_SPEC.md 3.20):
 *
 *   AT+MTMODES=<ep>,<mode>,"<label>"[,<mode>,"<label>",...]
 *       -> replace ep's ModeSelect SupportedModes list (the original form).
 *   AT+MTMODES=<ep>,<cluster>,<mode>,<tag>,"<label>"[,...]
 *       -> replace the (ep, cluster) ModeBase cluster's SupportedModes list
 *          (RvcRunMode, RvcCleanMode or MicrowaveOvenMode; RVC + Microwave
 *          batch, task 2).
 *
 * Both are set-only, the AT+MTTEMPLEVELS convention: bare/query forms answer
 * a plain ERROR. Neither is persisted: the host is expected to re-send its
 * list every boot, same contract as AT+MTTEMPLEVELS.
 *
 * Both are parsed here directly off the raw argument string, the same
 * reason AT+MTTEMPLEVELS is: a comma inside a quoted label is legal content,
 * and at_split_args() would split on it.
 *
 * Disambiguation, by type: after <ep> is parsed and its trailing comma
 * nulled, the next value is either the ModeSelect form's <mode> (immediately
 * followed by a quoted "<label>") or the cluster-aware form's <cluster>
 * (immediately followed by another numeric <mode>). So the character right
 * after the comma ending that first value tells the two forms apart: a `"`
 * means ModeSelect, a digit means cluster-aware. Hex "0x.." cluster ids
 * still start with the digit '0', so this still resolves correctly; any
 * other character is a malformed command either way.
 *
 * ModeSelect form grammar and content rules (violating any of these is
 * +MTERR:1, decided here before mt_matter_modes_set() is ever called):
 *   - <ep>: a bare unsigned token, hex or decimal, up to the first comma.
 *   - 1..MT_MODES_MAX_COUNT <mode>,"<label>" pairs.
 *   - <mode>: a bare unsigned token, hex or decimal, 0..255 (u8); no two
 *     pairs in the same command may carry the same mode value.
 *   - each label 1..MT_MODES_MAX_LABEL_LEN bytes, printable ASCII
 *     (0x20..0x7E) only.
 *   - no double-quote character inside a label: a label is scanned up to
 *     its next raw '"', so a quote inside the intended content always closes
 *     the token early, leaving trailing bytes the "junk after the closing
 *     quote" check below rejects.
 *
 * Cluster-aware form grammar: the same shape with a leading <cluster> and a
 * <tag> field inserted into each triple (+MTERR:1 on any violation, decided
 * here before mt_matter_modebase_set() is ever called):
 *   - <cluster>: a bare unsigned token, hex or decimal; validated against
 *     the three legal ModeBase cluster ids in the bridge (main.cpp), not
 *     here, since this C translation unit has no esp_matter/CHIP header to
 *     read those ids from.
 *   - 1..MT_MB_MAX_COUNT <mode>,<tag>,"<label>" triples, same <mode> and
 *     label rules as the ModeSelect form above, mode uniqueness included.
 *   - <tag>: a bare unsigned token, hex or decimal, 0..0xFFFF (u16); a value
 *     above that range is +MTERR:1. 0 means "cluster's conformance default",
 *     substituted by the bridge at store time (AT_MT_SPEC.md 3.20's tag-0
 *     default table); any other value passes through as-is.
 *
 * Lookup errors follow the established division for both forms: +MTERR:2
 * unknown endpoint, +MTERR:3 the endpoint has no cluster of the relevant
 * kind (ModeSelect, or the requested ModeBase cluster). A bare ERROR covers
 * an unclassified runtime failure, routed through attr_err_to_mterr() like
 * every other bridge call in this file.
 */
static int cmd_mtmodes(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (!args || *args == '\0') {
        return MT_ERR_BAD_PARAM;
    }

    char *comma = strchr(args, ',');
    if (!comma) {
        return MT_ERR_BAD_PARAM;
    }
    *comma = '\0';
    char *p = comma + 1;

    unsigned long ep;
    if (!parse_u(args, &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    /* Form disambiguation: find the comma ending the first value after <ep>
     * and look at what follows it. See the function comment above. */
    char *lookahead_comma = strchr(p, ',');
    if (!lookahead_comma) {
        return MT_ERR_BAD_PARAM;
    }
    bool cluster_aware;
    if (lookahead_comma[1] == '"') {
        cluster_aware = false;
    } else if (lookahead_comma[1] >= '0' && lookahead_comma[1] <= '9') {
        cluster_aware = true;
    } else {
        return MT_ERR_BAD_PARAM;
    }

    if (!cluster_aware) {
        uint8_t modes[MT_MODES_MAX_COUNT];
        const char *labels[MT_MODES_MAX_COUNT];
        char label_buf[MT_MODES_MAX_COUNT][MT_MODES_MAX_LABEL_LEN + 1];
        uint8_t count = 0;

        while (*p != '\0') {
            /* <mode>: a bare token up to the comma before the opening quote. */
            char *mode_comma = strchr(p, ',');
            if (!mode_comma) {
                return MT_ERR_BAD_PARAM;  /* mode with no label to follow it */
            }
            *mode_comma = '\0';
            unsigned long mode;
            if (!parse_u(p, &mode) || mode > 0xFF) {
                return MT_ERR_BAD_PARAM;
            }
            p = mode_comma + 1;

            if (*p != '"') {
                return MT_ERR_BAD_PARAM;
            }
            p++;
            char *start = p;
            while (*p != '"') {
                if (*p == '\0') {
                    return MT_ERR_BAD_PARAM;  /* unterminated quote */
                }
                p++;
            }
            size_t len = (size_t)(p - start);
            p++;  /* past the closing quote */

            if (len < 1 || len > MT_MODES_MAX_LABEL_LEN) {
                return MT_ERR_BAD_PARAM;
            }
            if (count >= MT_MODES_MAX_COUNT) {
                return MT_ERR_BAD_PARAM;
            }
            for (size_t i = 0; i < len; i++) {
                unsigned char c = (unsigned char)start[i];
                if (c < 0x20 || c > 0x7E) {
                    return MT_ERR_BAD_PARAM;
                }
            }
            for (uint8_t i = 0; i < count; i++) {
                if (modes[i] == (uint8_t)mode) {
                    return MT_ERR_BAD_PARAM;  /* duplicate mode value in this list */
                }
            }
            memcpy(label_buf[count], start, len);
            label_buf[count][len] = '\0';
            labels[count] = label_buf[count];
            modes[count] = (uint8_t)mode;
            count++;

            if (*p == ',') {
                p++;
                if (*p == '\0') {
                    return MT_ERR_BAD_PARAM;  /* trailing comma, no next pair */
                }
                continue;
            }
            if (*p == '\0') {
                break;
            }
            return MT_ERR_BAD_PARAM;  /* junk after the closing quote */
        }

        if (count < 1) {
            return MT_ERR_BAD_PARAM;
        }

        int r = mt_matter_modes_set((uint16_t)ep, modes, labels, count);
        if (r != MT_ATTR_OK) {
            return attr_err_to_mterr(r);
        }
        return AT_R_OK;
    }

    /* Cluster-aware form: <cluster>,<mode>,<tag>,"<label>"[,...]. */
    *lookahead_comma = '\0';
    unsigned long cluster;
    if (!parse_u(p, &cluster)) {
        return MT_ERR_BAD_PARAM;
    }
    p = lookahead_comma + 1;

    uint8_t modes[MT_MB_MAX_COUNT];
    uint16_t tags[MT_MB_MAX_COUNT];
    const char *labels[MT_MB_MAX_COUNT];
    char label_buf[MT_MB_MAX_COUNT][MT_MB_MAX_LABEL_LEN + 1];
    uint8_t count = 0;

    while (*p != '\0') {
        /* <mode>: a bare token up to the comma before <tag>. */
        char *mode_comma = strchr(p, ',');
        if (!mode_comma) {
            return MT_ERR_BAD_PARAM;  /* mode with no tag/label to follow it */
        }
        *mode_comma = '\0';
        unsigned long mode;
        if (!parse_u(p, &mode) || mode > 0xFF) {
            return MT_ERR_BAD_PARAM;
        }
        p = mode_comma + 1;

        /* <tag>: a bare token up to the comma before the opening quote. */
        char *tag_comma = strchr(p, ',');
        if (!tag_comma) {
            return MT_ERR_BAD_PARAM;  /* tag with no label to follow it */
        }
        *tag_comma = '\0';
        unsigned long tag;
        if (!parse_u(p, &tag) || tag > 0xFFFF) {
            return MT_ERR_BAD_PARAM;
        }
        p = tag_comma + 1;

        if (*p != '"') {
            return MT_ERR_BAD_PARAM;
        }
        p++;
        char *start = p;
        while (*p != '"') {
            if (*p == '\0') {
                return MT_ERR_BAD_PARAM;  /* unterminated quote */
            }
            p++;
        }
        size_t len = (size_t)(p - start);
        p++;  /* past the closing quote */

        if (len < 1 || len > MT_MB_MAX_LABEL_LEN) {
            return MT_ERR_BAD_PARAM;
        }
        if (count >= MT_MB_MAX_COUNT) {
            return MT_ERR_BAD_PARAM;
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)start[i];
            if (c < 0x20 || c > 0x7E) {
                return MT_ERR_BAD_PARAM;
            }
        }
        for (uint8_t i = 0; i < count; i++) {
            if (modes[i] == (uint8_t)mode) {
                return MT_ERR_BAD_PARAM;  /* duplicate mode value in this list */
            }
        }
        memcpy(label_buf[count], start, len);
        label_buf[count][len] = '\0';
        labels[count] = label_buf[count];
        modes[count] = (uint8_t)mode;
        tags[count] = (uint16_t)tag;
        count++;

        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return MT_ERR_BAD_PARAM;  /* trailing comma, no next triple */
            }
            continue;
        }
        if (*p == '\0') {
            break;
        }
        return MT_ERR_BAD_PARAM;  /* junk after the closing quote */
    }

    if (count < 1) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_modebase_set((uint16_t)ep, (uint32_t)cluster, modes, tags, labels, count);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTOPSTATE=<ep>,<state> -> report the host's own OperationalState
 * transition on <ep> (design spec F7: the washer/dishwasher/dryer trio, and
 * RVC + Microwave batch task 3: the robotic vacuum cleaner's
 * RvcOperationalState cluster), typically once the host has actually
 * finished executing an allowed Pause/Resume/Start/Stop/GoHome +MTCMD
 * verdict; unlike the water valve's F1, that verdict already fails the
 * command on the wire when denied, so this command is purely the "it
 * actually happened" report, the same split AT+MTLOCK/AT+MTVALVE use).
 * Set-only, the AT+MTLOCK/AT+MTVALVE convention: bare/query forms answer a
 * plain ERROR.
 *
 * <state>: this handler only checks membership in the UNION of both
 * clusters' legal states, since it cannot know which cluster <ep> actually
 * has (mt_at.c stays free of any esp_matter/CHIP header); the bridge
 * (mt_matter_opstate_set(), main.cpp) narrows that down per-cluster and
 * answers +MTERR:1 (MT_ATTR_ERR_VALUE) for a union member illegal on ep's
 * own cluster. The union is {0, 1, 2, 0x40, 0x41, 0x42}: 0 Stopped, 1
 * Running, 2 Paused are common to both clusters (OperationalStateEnum wire
 * values, spec F7); 0x40 kSeekingCharger, 0x41 kCharging, 0x42 kDocked are
 * RvcOperationalState's own derived-cluster-number-space states (design
 * spec section 9, RvcOperationalState::OperationalStateEnum, Enums.h). 3
 * (Error) is rejected HERE, in the handler, for both clusters: kError is
 * reserved for the error-detection path, never a state this command may set
 * directly, so the bridge never has to reason about it. This is the same
 * "wire protocol value fixed by the Matter spec, checked directly"
 * reasoning AT+MTLOCK/AT+MTVALVE's <state> follow.
 *
 * Lookup errors follow the established division: +MTERR:2 unknown endpoint,
 * +MTERR:3 the endpoint has neither OperationalState-family cluster. A bare
 * ERROR covers an unclassified runtime failure, routed through
 * attr_err_to_mterr() like every other bridge call in this file.
 */
static int cmd_mtopstate(at_type_t type, char *args)
{
    char *f[2];
    int n = at_split_args(args, f, 2);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n != 2) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long ep, state;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[1], &state)) {
        return MT_ERR_BAD_PARAM;
    }
    if (!(state <= 2 || state == 0x40 || state == 0x41 || state == 0x42)) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_opstate_set((uint16_t)ep, (uint8_t)state);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTALARM=<ep>,<field>,<value> -> report an event-emitting alarm state
 * change on <ep>'s alarm cluster (SmokeCoAlarm or RefrigeratorAlarm,
 * disambiguated by <ep>'s own cluster, never by anything in the command
 * itself, the same split AT+MTOPSTATE's two clusters use): one of the
 * cluster's own event-emitting Set* methods, chosen so the events they fire
 * (and, for SmokeCoAlarm, the critical-alarm auto-unmute) actually happen; a
 * raw AT+MTATTR write to the same attribute would skip them. Set-only, the
 * AT+MTLOCK/AT+MTVALVE/AT+MTOPSTATE convention: bare/query forms answer a
 * plain ERROR.
 *
 * <field>: this handler only checks the UNION bound, 0..11, the same split
 * AT+MTOPSTATE's <state> union check uses: it cannot know which cluster <ep>
 * carries (mt_at.c stays free of any esp_matter/CHIP header), so the
 * per-family legal set is narrowed in the bridge (mt_matter_alarm_set(),
 * main.cpp) instead.
 *   - SmokeCoAlarm: 1 SmokeState, 2 COState, 3 BatteryAlert, 4 DeviceMuted,
 *     5 TestInProgress, 6 HardwareFaultAlert, 7 EndOfServiceAlert,
 *     8 InterconnectSmokeAlarm, 9 InterconnectCOAlarm, 10 ContaminationState,
 *     11 SmokeSensitivityLevel (AT_MT_SPEC.md 3.22's table). Field 0
 *     (ExpressedState) is derived by the server from the other ten and is
 *     never settable directly: the bridge answers +MTERR:1 for it, moved
 *     down from this handler (composed appliance round, task 3) now that
 *     field 0 is a legal RefrigeratorAlarm bit (DoorOpen).
 *   - RefrigeratorAlarm: 0..7, the alarm bit number (only bit 0/DoorOpen is
 *     defined by the Matter spec in any revision through 1.5.1; the bridge
 *     narrows further against the endpoint's own Supported bitmap).
 * Anything outside 0..11 answers +MTERR:1 HERE, in the handler, the same
 * "wire-protocol-shape rejection belongs in the handler" reasoning
 * AT+MTOPSTATE's kError check follows.
 *
 * <value> is the field's own enum or bool wire value (0/1 for
 * RefrigeratorAlarm); mt_matter_alarm_set() validates it against that
 * field's/cluster's own SDK bound, since each field's Set* takes a
 * differently-typed enum and this C translation unit has no SDK access to
 * read those bounds from.
 *
 * Field 5 (TestInProgress, SmokeCoAlarm only) value 0 is the self-test
 * completion path: it fires SelfTestComplete on the SmokeCoAlarmServer
 * singleton, the far end of the +MTCMD:0,... notify a controller's
 * SelfTestRequest raises (mt_cmd_notify(), C1).
 *
 * Lookup errors follow the established division: +MTERR:2 unknown endpoint,
 * +MTERR:3 the endpoint has neither alarm cluster. A bare ERROR covers an
 * unclassified runtime failure, routed through attr_err_to_mterr() like every
 * other bridge call in this file.
 */
static int cmd_mtalarm(at_type_t type, char *args)
{
    char *f[3];
    int n = at_split_args(args, f, 3);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n != 3) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long ep, field, value;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[1], &field) || field > 11) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[2], &value) || value > 0xFF) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_alarm_set((uint16_t)ep, (uint8_t)field, (uint8_t)value);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTCHIMESOUNDS=<ep>,<id>,"<name>"[,<id>,"<name>",...] -> replace ep's
 * Chime InstalledChimeSounds list, 1..MT_CHIME_MAX_SOUNDS id/name pairs. The
 * AT+MTMODES grammar with ids: set-only, bare/query forms answer a plain
 * ERROR. Not persisted: the host is expected to re-send it every boot, the
 * same contract AT+MTMODES/AT+MTTEMPLEVELS follow.
 *
 * Parsed here directly off the raw argument string, the same reason
 * AT+MTMODES is: a comma inside a quoted name is legal content, and
 * at_split_args() would split on it. Grammar and content rules (violating any
 * of these is +MTERR:1, decided here before mt_matter_chime_sounds_set() is
 * ever called):
 *   - <ep>: a bare unsigned token, hex or decimal, up to the first comma.
 *   - 1..MT_CHIME_MAX_SOUNDS <id>,"<name>" pairs.
 *   - <id>: a bare unsigned token, hex or decimal, 0..255 (u8); no two pairs
 *     in the same command may carry the same id.
 *   - each name 1..MT_CHIME_MAX_NAME_LEN bytes, printable ASCII (0x20..0x7E)
 *     only.
 *   - no double-quote character inside a name: a name is scanned up to its
 *     next raw '"', so a quote inside the intended content always closes the
 *     token early, leaving trailing bytes the "junk after the closing quote"
 *     check below rejects.
 *
 * Lookup errors follow the established division: +MTERR:2 unknown endpoint,
 * +MTERR:3 the endpoint has no Chime cluster. A bare ERROR covers an
 * unclassified runtime failure, routed through attr_err_to_mterr() like
 * every other bridge call in this file.
 */
static int cmd_mtchimesounds(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (!args || *args == '\0') {
        return MT_ERR_BAD_PARAM;
    }

    char *comma = strchr(args, ',');
    if (!comma) {
        return MT_ERR_BAD_PARAM;
    }
    *comma = '\0';
    char *p = comma + 1;

    unsigned long ep;
    if (!parse_u(args, &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    uint8_t ids[MT_CHIME_MAX_SOUNDS];
    const char *names[MT_CHIME_MAX_SOUNDS];
    char name_buf[MT_CHIME_MAX_SOUNDS][MT_CHIME_MAX_NAME_LEN + 1];
    uint8_t count = 0;

    while (*p != '\0') {
        /* <id>: a bare token up to the comma before the opening quote. */
        char *id_comma = strchr(p, ',');
        if (!id_comma) {
            return MT_ERR_BAD_PARAM;  /* id with no name to follow it */
        }
        *id_comma = '\0';
        unsigned long id;
        if (!parse_u(p, &id) || id > 0xFF) {
            return MT_ERR_BAD_PARAM;
        }
        p = id_comma + 1;

        if (*p != '"') {
            return MT_ERR_BAD_PARAM;
        }
        p++;
        char *start = p;
        while (*p != '"') {
            if (*p == '\0') {
                return MT_ERR_BAD_PARAM;  /* unterminated quote */
            }
            p++;
        }
        size_t len = (size_t)(p - start);
        p++;  /* past the closing quote */

        if (len < 1 || len > MT_CHIME_MAX_NAME_LEN) {
            return MT_ERR_BAD_PARAM;
        }
        if (count >= MT_CHIME_MAX_SOUNDS) {
            return MT_ERR_BAD_PARAM;
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)start[i];
            if (c < 0x20 || c > 0x7E) {
                return MT_ERR_BAD_PARAM;
            }
        }
        for (uint8_t i = 0; i < count; i++) {
            if (ids[i] == (uint8_t)id) {
                return MT_ERR_BAD_PARAM;  /* duplicate id in this list */
            }
        }
        memcpy(name_buf[count], start, len);
        name_buf[count][len] = '\0';
        names[count] = name_buf[count];
        ids[count] = (uint8_t)id;
        count++;

        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return MT_ERR_BAD_PARAM;  /* trailing comma, no next pair */
            }
            continue;
        }
        if (*p == '\0') {
            break;
        }
        return MT_ERR_BAD_PARAM;  /* junk after the closing quote */
    }

    if (count < 1) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_chime_sounds_set((uint16_t)ep, ids, names, count);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTCHIME=<ep>,<what>,<value> -> set one of the Chime cluster's two plain
 * attributes on <ep> through the SDK's own ChimeCluster::SetSelectedChime()/
 * SetEnabled() (design spec F3), not a raw AT+MTATTR write: this cluster is
 * registered directly with esp_matter's data model provider, not through
 * esp_matter's generic attribute store (mt_matter.h). Set-only, the
 * AT+MTOPSTATE/AT+MTALARM convention: bare/query forms answer a plain ERROR.
 *
 * <what>: 0 SelectedChime, 1 Enabled. Anything else +MTERR:1.
 * <value>: for <what>=0, a chimeID; must already be one of the ids
 * AT+MTCHIMESOUNDS installed on this endpoint, or the bridge answers
 * +MTERR:1 (design spec F3: SetSelectedChime() itself answers
 * Status::NotFound for an unknown chimeID). For <what>=1, 0 or 1 (bool);
 * anything else +MTERR:1.
 *
 * Lookup errors follow the established division: +MTERR:2 unknown endpoint,
 * +MTERR:3 the endpoint has no Chime cluster. A bare ERROR covers an
 * unclassified runtime failure, routed through attr_err_to_mterr() like
 * every other bridge call in this file.
 */
static int cmd_mtchime(at_type_t type, char *args)
{
    char *f[3];
    int n = at_split_args(args, f, 3);
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (n != 3) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long ep, what, value;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[1], &what) || what > 1) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[2], &value) || value > 0xFF) {
        return MT_ERR_BAD_PARAM;
    }
    /* <what>=1 (Enabled) is a bool: reject here, the same "wire-protocol-
     * shape rejection belongs in the handler" reasoning AT+MTOPSTATE's kError
     * check and AT+MTALARM's two boolean fields follow. Without this,
     * SetEnabled(value != 0) (mt_matter.h) would silently coerce any value
     * 2..255 to true instead of answering +MTERR:1, since the SDK's setter
     * itself has no bool-range concept to reject on the bridge's behalf. */
    if (what == 1 && value > 1) {
        return MT_ERR_BAD_PARAM;
    }

    int r = mt_matter_chime_set((uint16_t)ep, (uint8_t)what, (uint8_t)value);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/* ---- event subscription (C3) ------------------------------------------ */

static uint32_t s_evt_mask = MT_EVT_MASK_DEFAULT;

/*
 * AT+MTEVT?          -> +MTEVTMASK:0x........
 * AT+MTEVT=<hexmask> -> subscribe to that set of event bits
 *
 * The query deliberately answers "+MTEVTMASK", not "+MTEVT": URCs may arrive
 * between a command and its terminal response, so a "+MTEVT:<n>" reply would be
 * indistinguishable from an event that happened to land at that moment.
 */
static int cmd_mtevt(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+MTEVTMASK:0x%08lX", (unsigned long)s_evt_mask);
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    unsigned long mask;
    if (!parse_u(args, &mask) || mask > 0xFFFFFFFFUL) {
        return MT_ERR_BAD_PARAM;
    }
    s_evt_mask = (uint32_t)mask;
    return AT_R_OK;
}

bool mt_at_event(int bit, const char *detail)
{
    if (bit < 0 || bit > 31) {
        return false;
    }
    if ((s_evt_mask & (1UL << bit)) == 0) {
        return false;
    }

    /*
     * Formatted, then handed to mt_at_urc() rather than written here.
     *
     * This used to call at_uart_write_line() directly, which made it a second
     * URC path with no s_at_up gate, and it panicked the device:
     *
     *   assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
     *
     * exactly the failure mt_at_urc()'s guard exists to prevent. It stayed
     * hidden because the commissioning window used to open *after*
     * mt_at_start(). Keeping BLE resident (defect D1) let CHIP advertise
     * during Server::Init instead, so kCommissioningWindowOpened started
     * arriving while at_uart's TX mutex was still NULL.
     *
     * One gate, one writer. A URC path that formats its own line and writes it
     * itself will eventually be added before the UART exists again.
     */
    char line[MT_EVT_LINE_MAX];
    if (detail) {
        snprintf(line, sizeof(line), "+MTEVT:%d,%s", bit, detail);
    } else {
        snprintf(line, sizeof(line), "+MTEVT:%d", bit);
    }
    return mt_at_urc(line);
}

/* ---- network transport query (C3) ------------------------------------- */

/* AT+MTNET? -> +MTNET:<transport>,<enabled>,<connected> */
static int cmd_mtnet(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return MT_R_ERROR;
    }
    int transport = 0, enabled = 0, connected = 0;
    if (mt_matter_net_info(&transport, &enabled, &connected) != 0) {
        return MT_R_ERROR;
    }
    /*
     * The <mismatch> field is appended, never inserted, so a host that parses
     * the first three fields and ignores extras stays correct (spec 3.12).
     * Emitted unconditionally rather than only when set: a field that appears
     * and disappears is harder to parse than one that is always there, and a
     * host cannot tell "no mismatch" from "old firmware" if it is omitted.
     */
    at_uart_write_line("+MTNET:%s,%d,%d,%d",
                       transport == MT_NET_THREAD ? "THREAD" : "WIFI",
                       enabled, connected, mt_matter_transport_mismatch());
    return AT_R_OK;
}

/* ---- endpoint composition staging (C1) -------------------------------- *
 * AT+MTEPCLEAR opens a staging session, AT+MTEP= appends to it, and
 * AT+MTEPAPPLY persists it and reboots. Staging lives in RAM only, so an
 * interrupted host leaves the stored composition untouched rather than
 * half-written.                                                            */

static mt_composition_t s_staged;
static bool             s_staging = false;

/* AT+MTEPCLEAR -> begin staging an empty composition. */
static int cmd_mtepclear(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    s_staged.count = 0;
    s_staging = true;
    return AT_R_OK;
}

/*
 * AT+MTEP?                    -> +MTEP:<index>,<endpoint_id>,<device_type>[,<variant>[,<parent_idx>]]
 *                                per endpoint. The 4th field (variant)
 *                                appears only when the variant is nonzero;
 *                                the 5th (parent_idx) appears only when the
 *                                endpoint has a parent, and never without
 *                                the 4th also present. An unparented
 *                                endpoint's line is therefore byte-identical
 *                                to before parenting existed, whatever its
 *                                variant.
 * AT+MTEP=<devtype>[,<variant>[,<parent_idx>]] -> append one endpoint to the
 *                                staged composition. <variant> defaults to 0
 *                                and must be legal for <devtype>
 *                                (mt_devtype_variant_ok()); an unknown
 *                                <devtype> answers +MTERR:6, a known
 *                                <devtype> with an illegal <variant>
 *                                answers +MTERR:1. <parent_idx> is the
 *                                already-staged index of this endpoint's
 *                                parent: it must name an earlier entry in
 *                                THIS staging session (out of range, self,
 *                                and forward references are all rejected
 *                                with +MTERR:1, since only already-staged
 *                                indexes exist and cycles are structurally
 *                                impossible under that rule). Omitting
 *                                <parent_idx> leaves the endpoint unparented;
 *                                mt_devtype_parent_ok() is consulted either
 *                                way, so a device type that REQUIRES a
 *                                parent (the cook surface, spec 2.3) is
 *                                rejected with +MTERR:1 when <parent_idx> is
 *                                absent too, and a parent of the wrong
 *                                device type (e.g. a cabinet under anything
 *                                but a fridge or oven) is rejected the same
 *                                way.
 *
 * The query always reports the LIVE composition, never the staged one.
 */
static int cmd_mtep(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        uint16_t n = mt_matter_endpoint_count();
        for (uint16_t i = 0; i < n; i++) {
            uint32_t devtype;
            uint16_t ep_id;
            uint8_t  variant;
            uint8_t  parent_idx;
            if (mt_matter_endpoint_info(i, &devtype, &ep_id, &variant, &parent_idx) == 0) {
                if (parent_idx != MT_COMP_NO_PARENT) {
                    at_uart_write_line("+MTEP:%u,%u,0x%04lX,%u,%u", i, ep_id,
                                       (unsigned long)devtype, (unsigned)variant,
                                       (unsigned)parent_idx);
                } else if (variant != 0) {
                    at_uart_write_line("+MTEP:%u,%u,0x%04lX,%u", i, ep_id,
                                       (unsigned long)devtype, (unsigned)variant);
                } else {
                    at_uart_write_line("+MTEP:%u,%u,0x%04lX", i, ep_id, (unsigned long)devtype);
                }
            }
        }
        return AT_R_OK;
    }

    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (!s_staging) {
        return MT_ERR_COMP_REJECT;
    }
    if (s_staged.count >= MT_COMP_MAX_ENDPOINTS) {
        return MT_ERR_COMP_REJECT;
    }

    char *f[3];
    int n = at_split_args(args, f, 3);
    if (n < 1) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long devtype;
    if (!parse_u(f[0], &devtype)) {
        return MT_ERR_BAD_PARAM;
    }
    if (!mt_devtype_is_known((uint32_t)devtype)) {
        return MT_ERR_DEVTYPE;
    }

    unsigned long variant = 0;
    if (n >= 2 && !parse_u(f[1], &variant)) {
        return MT_ERR_BAD_PARAM;
    }
    if (variant > 0xFF || !mt_devtype_variant_ok((uint32_t)devtype, (uint8_t)variant)) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long parent = MT_COMP_NO_PARENT;
    if (n == 3) {
        if (!parse_u(f[2], &parent)) {
            return MT_ERR_BAD_PARAM;
        }
        if (parent >= s_staged.count) {
            /* out of range, self and forward references all land here:
             * only already-staged indexes exist */
            return MT_ERR_BAD_PARAM;
        }
        if (!mt_devtype_parent_ok((uint32_t)devtype, (uint8_t)variant,
                                  s_staged.devtype[parent])) {
            return MT_ERR_BAD_PARAM;
        }
    } else {
        /* no parent given: types that REQUIRE one reject here too */
        if (!mt_devtype_parent_ok((uint32_t)devtype, (uint8_t)variant, 0)) {
            return MT_ERR_BAD_PARAM;
        }
    }

    s_staged.parent[s_staged.count] = (uint8_t)parent;
    s_staged.variant[s_staged.count] = (uint8_t)variant;
    s_staged.devtype[s_staged.count++] = (uint32_t)devtype;
    return AT_R_OK;
}

/* AT+MTEPAPPLY -> persist the staged composition, then reboot. */
static int cmd_mtepapply(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    if (!s_staging) {
        return MT_ERR_COMP_REJECT;
    }
    if (mt_comp_store_save(&s_staged) != 0) {
        return MT_ERR_PERSIST;
    }

    s_staging = false;

    /* Acknowledge, drain the UART, then reboot. The host resynchronizes on
     * the next "+MTREADY", exactly as it does after AT+MTRESET. */
    at_uart_write_line("OK");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return AT_R_DONE;
}

/* ---- AT link transport (baud, flow control) ---------------------------- */

/*
 * Standard rates only, capped at 921600. Same table as the ESP-NOW image's
 * AT+ENBAUD: the two firmwares share at_core's UART, so a host that knows
 * how to retune one link knows how to retune the other.
 *
 * Not persisted, deliberately. The rate lives in RAM, so every reset returns
 * the link to AT_UART_BAUD (115200) and a host that loses sync has a
 * guaranteed way back: pulse reset. Persisting it would make a bad switch
 * unrecoverable without a serial-download reflash.
 */
static const int s_baud_rates[] = {
    1200, 2400, 4800, 9600, 19200, 38400, 57600,
    115200, 230400, 460800, 921600,
};

/* AT+MTBAUD? -> +MTBAUD:<baud>;  AT+MTBAUD=<baud> -> OK, then switch. */
static int cmd_mtbaud(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+MTBAUD:%d", at_uart_get_baud());
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    unsigned long baud;
    if (!parse_u(args, &baud)) {
        return MT_ERR_BAD_PARAM;
    }
    bool valid = false;
    for (size_t i = 0; i < sizeof(s_baud_rates) / sizeof(s_baud_rates[0]); i++) {
        if ((unsigned long)s_baud_rates[i] == baud) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        return MT_ERR_BAD_PARAM;
    }

    /* Acknowledge at the current rate first: at_uart_set_baud() drains TX
     * before it switches, so this OK still leaves at the rate the host is
     * still listening at. The host reconfigures its own side after seeing
     * it. Any URC raised in the gap goes out at the old rate too, for the
     * same reason. */
    at_resp_ok();
    at_uart_set_baud((int)baud);
    return AT_R_DONE;
}

/*
 * AT+MTFLOW? -> +MTFLOW:<mode>;  AT+MTFLOW=<mode> -> OK, then switch.
 * Modes are at_core's: 0 none, 1 RTS, 2 CTS, 3 CTS+RTS.
 *
 * On a board that does not route the pair (MT_UART_FLOWCTRL_WIRED == 0,
 * which is every C6 board built so far) anything but 0 is rejected with
 * +MTERR:1 rather than accepted and quietly ineffective. Accepting it would
 * be worse than useless: enabling CTS gates this chip's transmitter on an
 * unbonded pin, and the link would simply stop mid-answer. See
 * mt_at_config.h for the netlist this claim rests on.
 */
static int cmd_mtflow(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+MTFLOW:%d", at_uart_get_flowctrl());
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    unsigned long mode;
    if (!parse_u(args, &mode) || mode > AT_UART_FLOWCTRL_CTS_RTS) {
        return MT_ERR_BAD_PARAM;
    }
#if !MT_UART_FLOWCTRL_WIRED
    if (mode != AT_UART_FLOWCTRL_NONE) {
        return MT_ERR_BAD_PARAM;
    }
#endif

    /* Same ordering reason as the baud switch: acknowledge at the current
     * flow-control state, then change it, because at_uart_set_flowctrl()
     * drains TX first and enabling CTS would otherwise be able to gate this
     * very response. */
    at_resp_ok();
    at_uart_set_flowctrl((int)mode);
    return AT_R_DONE;
}

/* ---- transport selection (combined image only) ------------------------ */

#if MT_COMBINED_IMAGE

/*
 * AT+MTTRANSPORT?              -> +MTTRANSPORT:<active>,<stored>
 * AT+MTTRANSPORT=<WIFI|THREAD> -> persist the choice -> OK
 *
 * Combined-image only (see MT_COMBINED_IMAGE in mt_at_config.h):
 * single-transport builds never register this command, so it answers the
 * ordinary "unknown command" +MTERR:8 there.
 *
 * <active> is the transport this boot is actually running, latched once in
 * app_main before esp_matter::start(). <stored> is whatever NVS holds right
 * now; it only differs from <active> after a set that has not yet been
 * followed by a reboot. Unlike AT+MTEP there is no apply/reboot step here:
 * setting it just becomes <active> at the next boot, whenever that is.
 */
static int cmd_mttransport(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+MTTRANSPORT:%s,%s",
                           mt_transport_name(mt_transport_active()),
                           mt_transport_name(mt_transport_stored()));
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    mt_transport_t t;
    if (mt_transport_parse(args, &t) != 0) {
        return MT_ERR_BAD_PARAM;
    }
    if (mt_transport_store(t) != 0) {
        return MT_ERR_PERSIST;
    }
    return AT_R_OK;
}

#endif /* MT_COMBINED_IMAGE */

/* ---- command forwarding: the verdict mailbox (C1) ---------------------- *
 * mt_cmdbox.c is the pure-C, host-tested slot state machine. Everything
 * that needs FreeRTOS, the AT link, or knowledge of both tasks involved
 * lives here: the critical sections around every mailbox call, the
 * semaphore mt_cmd_forward() blocks on, and the AT+MTCMDRESP handler that
 * answers it.                                                              */

/*
 * Guards every call into mt_cmdbox.c. mt_cmd_forward() runs on the CHIP
 * event loop task (called from an ember cluster callback); cmd_mtcmdresp()
 * below runs on the AT parser task. mt_cmdbox.c has no locking of its own,
 * by design (it stays host-testable with no FreeRTOS headers), so the
 * mailbox's single slot needs a critical section here around each
 * individual open/answer/take/expire call.
 */
static portMUX_TYPE s_cmdbox_mux = portMUX_INITIALIZER_UNLOCKED;

/* Given by cmd_mtcmdresp() on a successful verdict, waited on by
 * mt_cmd_forward(). Created in mt_at_start() alongside the rest of the AT
 * link bring-up, before s_at_up is set, so it exists by the time a forward
 * could possibly be called. */
static SemaphoreHandle_t s_cmd_sem = NULL;

/*
 * Shared body of mt_cmd_forward(), mt_cmd_forward_payload() and
 * mt_cmd_forward_fields(): open a mailbox slot, raise the "+MTCMD:<seq>,..."
 * URC, and block up to 1000 ms for the host's verdict. fields is the
 * already-formatted, comma-joined tail to append after <seq>,<ep>,<cluster>,
 * <command>; NULL or an empty string means no tail at all, reproducing
 * mt_cmd_forward()'s exact four-field line. The caller (mt_cmd_forward_payload()
 * for the single-field legacy form, mt_cmd_forward_fields() for the
 * multi-field RVC/microwave batch callers) owns formatting the tail,
 * including rendering an absent optional field as an empty string between
 * two commas; this function never inspects field contents, only whether
 * there is a tail at all. Everything else, the fail-closed guards, the
 * double drain around the semaphore, the timeout/+MTCMDTO path, is identical
 * across all three public entry points, so it lives here once rather than
 * three times.
 */
static bool mt_cmd_forward_common(uint16_t ep, uint32_t cluster, uint32_t command,
                                   const char *fields)
{
    /* Fail closed with no URC at all if the AT link is not up yet: there is
     * no host to forward to, and raising one before mt_at_start() has run
     * is exactly what mt_at_urc()'s s_at_up guard exists to prevent. */
    if (!s_at_up) {
        return false;
    }
    /* Fail closed if the semaphore never came up (xSemaphoreCreateBinary()
     * returned NULL, e.g. heap exhaustion). Without this, xSemaphoreTake()
     * below on a NULL handle is undefined behaviour, not a clean deny. */
    if (!s_cmd_sem) {
        return false;
    }

    taskENTER_CRITICAL(&s_cmdbox_mux);
    uint32_t seq = mt_cmdbox_open(ep, cluster, command);
    taskEXIT_CRITICAL(&s_cmdbox_mux);

    /*
     * Drain any stale give before waiting, non-blocking.
     *
     * The timeout branch below expires the slot and then drains too, but
     * there is a narrow window between this function's OWN previous call
     * timing out and that drain running: if the host's AT+MTCMDRESP for
     * that earlier seq lands in exactly that window, cmd_mtcmdresp() still
     * finds the slot PENDING (mt_cmdbox_expire() has not taken the mux
     * yet), accepts the verdict, and gives the semaphore - but the
     * forward() call that give belongs to has already committed to
     * returning false and will never consume it. Left there, THIS call's
     * xSemaphoreTake() would take that stale give immediately and deny in
     * microseconds without ever waiting for the host; the same trap would
     * then repeat for every call after this one, forever, since a stale
     * give is left behind every time an answer wins that race. One narrow
     * race at the 1000 ms boundary would otherwise cascade into permanent,
     * silent instant-deny of the whole forwarding feature until reboot.
     * This drain and the one in the timeout branch below are what make the
     * mailbox self-healing: every forward() starts from a guaranteed-empty
     * semaphore, so a stale give can survive at most until the next call.
     */
    xSemaphoreTake(s_cmd_sem, 0);

    char line[MT_CMD_LINE_MAX];
    if (fields != NULL && fields[0] != '\0') {
        snprintf(line, sizeof(line), "+MTCMD:%lu,%u,%lu,%lu,%s",
                 (unsigned long)seq, ep, (unsigned long)cluster,
                 (unsigned long)command, fields);
    } else {
        snprintf(line, sizeof(line), "+MTCMD:%lu,%u,%lu,%lu",
                 (unsigned long)seq, ep, (unsigned long)cluster,
                 (unsigned long)command);
    }
    mt_at_urc(line);

    if (xSemaphoreTake(s_cmd_sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
        taskENTER_CRITICAL(&s_cmdbox_mux);
        int verdict = mt_cmdbox_take(seq);
        taskEXIT_CRITICAL(&s_cmdbox_mux);
        return verdict == 1;
    }

    /* Timed out: drop the slot so a late answer for this seq is rejected,
     * tell the host it missed the window, and deny. */
    taskENTER_CRITICAL(&s_cmdbox_mux);
    mt_cmdbox_expire(seq);
    taskEXIT_CRITICAL(&s_cmdbox_mux);

    /*
     * Drain again, non-blocking: this is the other half of the cascade fix
     * described above. A give can slip in between the failed
     * xSemaphoreTake() a few lines up and mt_cmdbox_expire() taking the
     * mux, on THIS call's own seq. Without this drain, that give would sit
     * on the semaphore and be taken by the NEXT forward() instead of this
     * one, denying it instantly and starting the same cascade.
     */
    xSemaphoreTake(s_cmd_sem, 0);

    char to_line[MT_CMD_LINE_MAX];
    snprintf(to_line, sizeof(to_line), "+MTCMDTO:%lu", (unsigned long)seq);
    mt_at_urc(to_line);
    return false;
}

bool mt_cmd_forward(uint16_t ep, uint32_t cluster, uint32_t command)
{
    return mt_cmd_forward_common(ep, cluster, command, NULL);
}

bool mt_cmd_forward_payload(uint16_t ep, uint32_t cluster, uint32_t command, uint32_t payload)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)payload);
    return mt_cmd_forward_common(ep, cluster, command, buf);
}

/*
 * fields is the caller's own comma-joined tail, already formatted (for
 * example "1,30,80,1" for four present fields, or "1,,80," when the second
 * and fourth are absent optionals): this function does no formatting of its
 * own beyond splicing that string in after the fixed <seq>,<ep>,<cluster>,
 * <command> head. NULL or an empty string means no tail, same as
 * mt_cmd_forward(). See mt_at.h for the full contract and AT_MT_SPEC.md
 * §3.17 for the per-command arity each consumer documents.
 */
bool mt_cmd_forward_fields(uint16_t ep, uint32_t cluster, uint32_t command, const char *fields)
{
    return mt_cmd_forward_common(ep, cluster, command, fields);
}

/*
 * Raise a notify-only "+MTCMD:0,<ep>,<cluster>,<command>" URC (C1): seq 0 is
 * reserved for this form, never issued by mt_cmdbox_open(), so there is
 * nothing to answer and nothing to wait for. No mailbox slot is opened, no
 * semaphore is touched, and this returns immediately, which is what makes it
 * safe to call from the CHIP task for a command whose callback is void (e.g.
 * chime's PlayChimeSound) rather than one that reports success/failure back
 * up the ember stack the way mt_cmd_forward()'s callers do.
 *
 * Same fail-closed philosophy as mt_cmd_forward(): a silent drop, no URC at
 * all, if the AT link is not up yet. mt_at_urc()'s own s_at_up guard would
 * catch this anyway, but checking here keeps the two forwarding paths
 * reading the same way and avoids formatting a line that is just going to be
 * thrown away.
 */
void mt_cmd_notify(uint16_t ep, uint32_t cluster, uint32_t command)
{
    if (!s_at_up) {
        return;
    }

    char line[MT_CMD_LINE_MAX];
    snprintf(line, sizeof(line), "+MTCMD:0,%u,%lu,%lu",
             ep, (unsigned long)cluster, (unsigned long)command);
    mt_at_urc(line);
}

/*
 * AT+MTCMDRESP=<seq>,<verdict> -> deliver the host's verdict for a command
 * that was forwarded via a "+MTCMD:<seq>,..." URC. Set-only: bare/query
 * forms answer plain ERROR, the AT+MTSWITCH pattern, since there is no
 * "current pending command" to report, only the seq the URC already named.
 *
 * verdict is 1 (allow) or 0 (deny). A malformed line, a verdict outside
 * {0,1}, or a seq mt_cmdbox_answer() does not recognise as the one
 * currently PENDING (wrong, stale, future, or already expired) all answer
 * +MTERR:1, per the wire contract; nothing about those is distinguished
 * further.
 *
 * NO ChipStackLock here, unlike every other mt_matter_* bridge call in this
 * file, and this is deliberate rather than an oversight. mt_cmd_forward()
 * is called from an ember cluster callback running ON the CHIP event loop
 * task, which at that moment effectively holds the CHIP stack lock; it is
 * blocked inside xSemaphoreTake(), waiting for this handler. If this
 * handler (running on the AT parser task) tried to take ChipStackLock too,
 * it would block on that very same CHIP task and wait out the full 1000 ms
 * deadline on every single command: a guaranteed deadlock until
 * mt_cmd_forward()'s own timeout gives up and releases the lock. This
 * handler must only touch the mailbox and the semaphore, never the CHIP
 * stack, so any future edit here needs to preserve that.
 */
static int cmd_mtcmdresp(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    char *f[2];
    int n = at_split_args(args, f, 2);
    if (n != 2) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long seq, verdict;
    if (!parse_u(f[0], &seq) || seq > 0xFFFFFFFFUL) {
        return MT_ERR_BAD_PARAM;
    }
    if (!parse_u(f[1], &verdict) || verdict > 1) {
        return MT_ERR_BAD_PARAM;
    }

    taskENTER_CRITICAL(&s_cmdbox_mux);
    int r = mt_cmdbox_answer((uint32_t)seq, (int)verdict);
    taskEXIT_CRITICAL(&s_cmdbox_mux);

    if (r != 0) {
        return MT_ERR_BAD_PARAM;
    }

    xSemaphoreGive(s_cmd_sem);
    return AT_R_OK;
}

/* ---- dispatch table & registration ------------------------------------ */

static const at_command_t s_cmds[] = {
    { "CGMI",         cmd_cgmi        },
    { "CGMM",         cmd_cgmm        },
    { "CGMR",         cmd_cgmr        },
    { "MTVER",        cmd_ver         },
    { "MTSTATE",      cmd_mtstate     },
    { "MTFABRICS",    cmd_mtfabrics   },
    { "MTCOMMISSION", cmd_mtcommission },
    { "MTCODES",      cmd_mtcodes     },
    { "MTRESET",      cmd_mtreset     },
    { "MTFRESET",     cmd_mtfreset    },
    { "MTATTR",       cmd_mtattr      },
    { "MTSWITCH",     cmd_mtswitch    },
    { "MTTEMPLEVELS", cmd_mttemplevels },
    { "MTEP",         cmd_mtep        },
    { "MTEPCLEAR",    cmd_mtepclear   },
    { "MTEPAPPLY",    cmd_mtepapply   },
    { "MTEVT",        cmd_mtevt       },
    { "MTNET",        cmd_mtnet       },
    { "MTBAUD",       cmd_mtbaud      },
    { "MTFLOW",       cmd_mtflow      },
    { "MTCMDRESP",    cmd_mtcmdresp   },
    { "MTLOCK",       cmd_mtlock      },
    { "MTVALVE",      cmd_mtvalve     },
    { "MTMODES",      cmd_mtmodes     },
    { "MTOPSTATE",    cmd_mtopstate   },
    { "MTALARM",      cmd_mtalarm     },
    { "MTCHIMESOUNDS", cmd_mtchimesounds },
    { "MTCHIME",      cmd_mtchime     },
#if MT_COMBINED_IMAGE
    { "MTTRANSPORT",  cmd_mttransport },
#endif
};

/* Engine config for the Matter personality: "+MTERR" code space, the
 * Matter line length, and the parser task tuning. */
static const at_engine_cfg_t s_engine_cfg = {
    .line_max        = MT_AT_LINE_MAX,
    .err_prefix      = "+MTERR",
    .err_generic     = MT_ERR_GENERIC,
    .err_unsupported = MT_ERR_UNSUPPORTED,
    .echo_default    = MT_AT_ECHO_DEFAULT,
    .task_stack      = MT_PARSER_TASK_STACK,
    .task_prio       = MT_PARSER_TASK_PRIO,
};

/*
 * Bring up the AT+MT interface: install the AT UART (UART1 on the host-bridge
 * pins - the console lives on its own UART, see sdkconfig.defaults.esp32c6),
 * register the command table, start the parser engine, and emit the boot
 * marker. Called from app_main() after esp_matter::start(). C linkage so the
 * C++ entry point can call it without pulling at_core's C headers into C++.
 */
void mt_at_start(void)
{
    at_uart_init();

    /*
     * Verdict mailbox bring-up: the slot state machine and the semaphore
     * cmd_mtcmdresp() gives and mt_cmd_forward() waits on. Done before
     * at_register_commands()/at_parser_start() below, not after, so the
     * mailbox existing is structural rather than incidental: cmd_mtcmdresp()
     * cannot run until the parser task is registered and started, so doing
     * this first guarantees it is never reachable before the mailbox is
     * ready, rather than relying on BSS zero-init happening to leave the
     * slot in a safe IDLE state and on the host obeying +MTREADY to not ask
     * before the semaphore exists.
     */
    mt_cmdbox_init();
    s_cmd_sem = xSemaphoreCreateBinary();

    at_register_commands(s_cmds, sizeof(s_cmds) / sizeof(s_cmds[0]));
    at_parser_start(&s_engine_cfg);

    /*
     * Boot marker so the host can synchronize after a reset (mirrors +ENREADY).
     *
     * Write it BEFORE opening the URC gate, not after. These two lines were
     * the other way round, which left a window between s_at_up going true and
     * the marker reaching the wire. The CHIP task runs concurrently, so a URC
     * raised in that window overtook the marker: observed on hardware as
     * "+MTEVT:0" arriving before "+MTREADY" on the boot after AT+MTFRESET,
     * where an unprovisioned device auto-opens a commissioning window.
     *
     * A URC that beats the boot marker is worse than a dropped one. The host
     * discards everything ahead of +MTREADY by design (HearthLink::waitReady),
     * so it was never delivered anyway; it just made the wire lie about where
     * the boot ended. With the write first, +MTREADY is unconditionally the
     * first line of a new session, which is the whole contract of a boot
     * marker.
     *
     * at_uart_init() above has already created the TX mutex, so writing here
     * is safe; it is only mt_at_urc() that must wait for the gate.
     */
    at_uart_write_line("+MTREADY");
    s_at_up = true;
}

bool mt_at_urc(const char *line)
{
    /*
     * URCs can fire from esp_matter callbacks during esp_matter::start(),
     * which app_main runs BEFORE mt_at_start(). At that point at_uart_init()
     * has not created the TX mutex, and at_uart_write_line() would take a NULL
     * semaphore: "assert failed: xQueueSemaphoreTake queue.c:1709 ((pxQueue))",
     * then panic and reboot, forever. A commissioned device restoring its
     * OnOff state at boot hits this on every boot.
     *
     * Drop pre-init URCs. They are undeliverable by definition, the host is
     * not listening yet, and it resynchronizes on the +MTREADY that follows.
     */
    if (!s_at_up) {
        return false;
    }
    at_uart_write_line("%s", line);
    return true;
}
