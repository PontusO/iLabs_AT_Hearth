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

#include <errno.h>
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
#include "mt_rows.h"
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

/*
 * The verdict window mt_cmd_forward_common() blocks for, in milliseconds.
 *
 * MT_CMD_VERDICT_MS is EVERY scalar forward's window and is 1000 ms exactly,
 * unchanged since C1. Every shipped +MTCMD consumer, and every host sketch
 * written against them, was validated against this number; it is named here
 * rather than raised so that the row-bearing window below cannot be
 * mistaken for a general relaxation of the deadline.
 *
 * MT_CMD_VERDICT_ROWS_MS is used by nothing but mt_rows_inbound_forward()
 * below. It is longer because a row-bearing forward is the only one whose
 * verdict requires a round trip before the host can even see what it is
 * judging: the +MTCMD carries a row count, and the rows themselves have to
 * be pulled with AT+MTROWGET. See mt_at.h for what that costs a sketch that
 * blocks in loop().
 *
 * WHAT THE LONGER WINDOW COSTS THE CHIP TASK, and it is not the same on the
 * two images. A forward blocks the CHIP event loop for its whole duration.
 * The WiFi build sets CONFIG_USE_MINIMAL_MDNS=y, so the mDNS responder runs
 * ON THAT TASK: for the whole window it answers nothing, and both
 * operational discovery and commissionable advertising pause. The Thread
 * build sets it =n, and is unaffected. That is worth knowing before this
 * constant is raised again, because this project already carries a WiFi
 * operational-discovery transient on its watch list and a 3 s responder gap
 * is exactly the shape of thing that would feed it.
 */
#define MT_CMD_VERDICT_MS      1000
#define MT_CMD_VERDICT_ROWS_MS 3000

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
#define MT_ERR_ATTR_READONLY 11 /* attribute exists but is served by a
                                  * cluster Instance and is not writable
                                  * over AT (DE270)                        */
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
    /* Same reason, and the deliberate asymmetry with the composition: a
     * charging schedule is USER data, so it survives AT+MTRESET and only a
     * factory reset removes it, where the composition is a product
     * definition and survives this command's Matter-reset half. */
    if (mt_matter_evse_targets_erase_all() != 0) {
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

/* 64-bit value parse: optional leading minus only when the target
 * attribute's type is signed (B-round-A: the pipeline was silently
 * truncating i64/u64 through 32-bit long). Rejects empty, sign-only,
 * trailing garbage and out-of-range exactly like parse_u does. */
static bool parse_i64(const char *s, bool allow_negative, int64_t *out)
{
    char *end;
    errno = 0;
    if (s[0] == '-' && !allow_negative) {
        return false;
    }
    long long v = strtoll(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

/* Unsigned companion: strtoull so a u64 attribute carries values above
 * INT64_MAX (18446744073709551615 round-trips). The leading minus is
 * rejected up front because strtoull negates it silently instead of
 * failing, which is exactly the class of accident this pair replaces. */
static bool parse_u64(const char *s, uint64_t *out)
{
    char *end;
    errno = 0;
    if (s[0] == '-') {
        return false;
    }
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

/*
 * THE FORM/VALUE BOUNDARY for a hand-parsed command's own tokens (as
 * distinct from attr_err_to_mterr()/row_err_to_mterr() below it, which map
 * a BRIDGE result onto +MTERR once a command has already decided its own
 * wire shape is fine). Every command that parses fields off the raw line
 * itself, rather than relying entirely on a downstream codec, answers:
 *
 *   - a bare ERROR when the SHAPE of the line is wrong: the wrong number of
 *     fields, an unterminated quote, a required token that is
 *     structurally MISSING (as distinct from a token that IS present but
 *     empty, which in AT+MTROW/AT+MTMETERID means an absent optional and is
 *     a value-level concept, not a shape one), or the wrong command form
 *     (e.g. a query where only a set exists);
 *   - +MTERR:1 when the shape is right and a VALUE within it cannot be
 *     accepted, whether because it fails to parse as the type it should be
 *     (a present, correctly delimited token that is not a number) or
 *     because it parses but is out of range.
 *
 * This was NOT applied consistently when first written: cmd_mtrow (and the
 * rest of the AT+MTROW family it shares a grammar with) answered a bare
 * ERROR for an unparseable-but-present field, on the reasoning that "not a
 * number" is itself a shape problem; AT+MTMETERID copied that choice
 * verbatim. cmd_mtdemcap and cmd_mtmodes/cmd_mtchimesounds had already
 * shipped answering +MTERR:1 for the identical case. A review of all three
 * shapes together (fix round 1, task 9 report,
 * .superpowers/sdd/2026-08-14-energy-round-c2/task-9-report.md) found the
 * split and ruled in favour of the two ALREADY-SHIPPED commands, both
 * because a host may already depend on their answer and because "a token
 * that cannot be parsed as a number" is, on balance, a value judgement
 * (this specific piece of content is unacceptable) rather than a shape one
 * (the command's structure is wrong), the same way an out-of-range number
 * is: only the missing/unterminated/wrong-arity cases genuinely prevent the
 * parser from even establishing where a token's content is.
 *
 * Governs cmd_mtrow/cmd_mtrowclear/cmd_mtrowapply/cmd_mtrowget (the
 * AT+MTROW family) and cmd_mtmeterid, all normalised to this rule in fix
 * round 1. cmd_mtdemcap and cmd_mtmodes/cmd_mtchimesounds already followed
 * it and are unchanged. The next hand-parsed command should follow it from
 * the start rather than re-deriving the split.
 */

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
    /* DE270: the attribute exists and the value is fine, but it is served
     * by a cluster Instance and cannot be written over AT. Distinct from
     * MT_ERR_BAD_PARAM (the value was rejected) and MT_ERR_NO_ATTRIBUTE (no
     * such attribute). */
    case MT_ATTR_ERR_READONLY:  return MT_ERR_ATTR_READONLY;
    default:                    return MT_R_ERROR;
    }
}

/*
 * AT+MTATTR=<ep>,<cluster>,<attr>              -> read
 *     -> +MTATTR:<ep>,<cluster>,<attr>,<val>
 * AT+MTATTR=<ep>,<cluster>,<attr>,<val>[,<mode>] -> write -> OK
 *
 * <cluster>/<attr> accept hex (0x0006) or decimal; <val> is a full-width
 * 64-bit integer (hex or decimal), signed per the attribute's own type: a
 * leading minus is accepted only for signed attributes, and an unsigned
 * attribute carries 0 up to its type's maximum (a u64 takes
 * 18446744073709551615). The signedness is fetched from the bridge before
 * <val> is parsed, which is also why a write's lookup errors (+MTERR:2..5)
 * are decided before a bad <val> (+MTERR:1).
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

    /* Read first in both directions: for a read it is the answer itself, and
     * a write needs the attribute's signedness before parsing <val> (a
     * leading minus is only legal on a signed attribute, and a u64 value
     * above INT64_MAX needs the unsigned parse). */
    int64_t v;
    bool is_unsigned = false;
    int r = mt_matter_attr_read((uint16_t)ep, (uint32_t)cluster, (uint32_t)attr,
                                &v, &is_unsigned);

    if (n == 3) {
        if (r != MT_ATTR_OK) {
            return attr_err_to_mterr(r);
        }
        if (is_unsigned) {
            at_uart_write_line("+MTATTR:%lu,%lu,%lu,%llu", ep, cluster, attr,
                               (unsigned long long)(uint64_t)v);
        } else {
            at_uart_write_line("+MTATTR:%lu,%lu,%lu,%lld", ep, cluster, attr,
                               (long long)v);
        }
        return AT_R_OK;
    }

    /* MT_ATTR_ERR_TYPE passes through to the write: the signedness flag is
     * valid for it (a null nullable reads as ERR_TYPE yet writes fine), and
     * a genuinely non-integer attribute fails the write below with the same
     * +MTERR:5 it always answered. */
    if (r != MT_ATTR_OK && r != MT_ATTR_ERR_TYPE) {
        return attr_err_to_mterr(r);
    }

    int64_t val;
    if (is_unsigned) {
        uint64_t u;
        if (!parse_u64(f[3], &u)) {
            return MT_ERR_BAD_PARAM;
        }
        val = (int64_t)u;  /* raw pattern; the bridge stores it per type */
    } else {
        if (!parse_i64(f[3], true, &val)) {
            return MT_ERR_BAD_PARAM;
        }
    }

    unsigned long mode = 1;
    if (n == 5 && (!parse_u(f[4], &mode) || mode > 1)) {
        return MT_ERR_BAD_PARAM;
    }

    int rw = mt_matter_attr_write((uint16_t)ep, (uint32_t)cluster, (uint32_t)attr,
                                  val, mode != 0);
    if (rw != MT_ATTR_OK) {
        return attr_err_to_mterr(rw);
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
 *          (RvcRunMode, RvcCleanMode or MicrowaveOvenMode from the RVC +
 *          Microwave batch, task 2; later rounds grow the accept list
 *          bridge-side, see mt_matter_modebase_set()).
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
 *     the legal ModeBase cluster ids in the bridge (main.cpp), not here,
 *     since this C translation unit has no esp_matter/CHIP header to read
 *     those ids from.
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

/* Signedness per (cluster, field) for AT+MTMEAS value parsing, mirroring the
 * MT_MEAS_F_* / MT_ENERGY_F_* / MT_WHM_F_* / MT_DEM_F_* / MT_EVSE_F_* unit
 * comments in mt_matter.h: every ElectricalPowerMeasurement (0x0090) field is
 * signed, both ElectricalEnergyMeasurement (0x0091) cumulative counters are
 * unsigned, of the WaterHeaterManagement (0x0094) fields only
 * EstimatedHeatRequired (field 4, int64 mWh) is signed, kept signed for
 * 64-bit pipeline symmetry even though the XML pins its minimum at 0: the
 * min-0 range check is the bridge's business (feature gating likewise; this
 * C side only classifies signedness), so "-1" on field 4 parses here and
 * answers +MTERR:1 from the bridge's validate pass instead of the parse
 * gate. Of the DeviceEnergyManagement (0x0098, energy round C1) fields the
 * two int64 mW powers (3-4) and the int64 mWh event carrier (6) are signed
 * and the three enums/bool (0-2, 5) are unsigned. Of the EnergyEvse (0x0099,
 * energy round C2) fields, seven are int64 currents or energies and signed
 * for the same 64-bit pipeline symmetry, every one of them XML-pinned to a
 * minimum of 0 and enforced there rather than here: CircuitCapacity (4),
 * MinimumChargeCurrent (5), MaximumChargeCurrent (6),
 * UserMaximumChargeCurrent (7), NextChargeRequiredEnergy (11),
 * BatteryCapacity (15), SessionEnergyCharged (18). The other twelve fields
 * (0-3, 8-10, 12-14, 16-17: the three enums, the u32 timestamps/window/
 * session fields and the u8/u16 percentages) are unsigned. Unknown fields
 * (and unknown clusters) default to signed: the accept set only shapes the
 * parse, and the bridge rejects an unknown field with MT_ATTR_ERR_VALUE in
 * its validate pass regardless of how the value was parsed, so nothing
 * wrongly admitted here is ever applied. */
static bool mt_meas_field_signed(uint32_t cluster, uint8_t field)
{
    if (cluster == 0x0091 &&
        (field == MT_ENERGY_F_IMPORTED || field == MT_ENERGY_F_EXPORTED)) {
        return false;
    }
    if (cluster == 0x0094 && field <= MT_WHM_F_TANK_PERCENT &&
        field != MT_WHM_F_EST_HEAT_REQ) {
        return false;
    }
    if (cluster == 0x0098 && field <= MT_DEM_F_OPT_OUT_STATE &&
        field != MT_DEM_F_ABS_MIN_POWER && field != MT_DEM_F_ABS_MAX_POWER) {
        return false;
    }
    if (cluster == 0x0099 && field <= MT_EVSE_F_SESSION_ENERGY_CHARGED &&
        field != MT_EVSE_F_CIRCUIT_CAPACITY && field != MT_EVSE_F_MIN_CHARGE_CURRENT &&
        field != MT_EVSE_F_MAX_CHARGE_CURRENT && field != MT_EVSE_F_USER_MAX_CHARGE_CURRENT &&
        field != MT_EVSE_F_NEXT_CHARGE_ENERGY && field != MT_EVSE_F_BATTERY_CAPACITY &&
        field != MT_EVSE_F_SESSION_ENERGY_CHARGED) {
        return false;
    }
    return true;
}

/*
 * AT+MTMEAS=<ep>,<cluster>,<field>,<value>[,<field>,<value>,...] -> push
 * 1..MT_MEAS_MAX_PAIRS (field, value) measurement/state pairs onto <ep>'s
 * push-served cluster in one call (energy round A; round B adds 0x0094,
 * round C1 adds 0x0098, round C2 task 7 adds 0x0099). <cluster> selects the
 * family, 0x0090 ElectricalPowerMeasurement, 0x0091
 * ElectricalEnergyMeasurement, 0x0094 WaterHeaterManagement, 0x0098
 * DeviceEnergyManagement or 0x0099 EnergyEvse; the field ids (MT_MEAS_F_* /
 * MT_ENERGY_F_* / MT_WHM_F_* / MT_DEM_F_* / MT_EVSE_F_*, mt_matter.h) are
 * per-cluster spaces that may overlap numerically. Set-only, the
 * AT+MTLOCK/AT+MTVALVE/AT+MTOPSTATE/AT+MTALARM convention: bare/query
 * forms answer a plain ERROR.
 *
 * EnergyEvse is entirely delegate-served (all its scalar attributes, unlike
 * every earlier family here, so this is the ONLY way the host can set any
 * of them) and is the one family where AT+MTMEAS is also an avoidance: three
 * of its attributes (UserMaximumChargeCurrent, RandomizationDelayWindow,
 * ApproximateEVEfficiency) are additionally writable over the fabric, which
 * would make them this project's first writable delegate-served attributes
 * reachable over AT+MTATTR too. DE270 (firmware 0.10.1) resolved that write
 * path's read-only case but left the writable one collapsing to a bare
 * ERROR (the SDK's WriteAttribute() detour discards the provider's real
 * status there); routing these three through AT+MTMEAS instead sidesteps
 * that gap rather than closing it. See mt_evse.cpp's
 * mt_evse_meas_apply_locked() for the field table and the metadata
 * existence gate (esp_matter::attribute::get()) that answers
 * +MTERR:4 for a field esp-matter never creates or a variant that lacks it,
 * without a second table to keep in sync.
 *
 * <value> is a full-width 64-bit integer (hex or decimal), signed per the
 * field's own signedness (mt_meas_field_signed() above): a leading minus is
 * accepted only for the signed power fields and rejected at parse for the
 * unsigned energy counters, the same signedness-first parse AT+MTATTR's
 * write path uses. This handler only checks the wire shape (1..7 pairs, an
 * even pair tail, numeric tokens, <field> in u8); the bridge
 * (mt_matter_meas_set(), main.cpp) validates every field id and value
 * against the cluster XML bounds BEFORE applying anything, so a bad third
 * pair leaves the first two unapplied.
 *
 * Lookup errors follow the established division: +MTERR:2 unknown endpoint,
 * +MTERR:3 <cluster> is not one of the five push-served ids, <ep> does not
 * carry it, or (0x0094 only) a pushed field's backing feature is absent on
 * that endpoint (the bridge-side gate; see mt_matter.h's MT_WHM_F_* note).
 * +MTERR:4 (0x0099 only) a pushed field names an attribute esp-matter never
 * created on that endpoint at all, whether because the field is one of the
 * three esp-matter ships no caller for or because it is SoC-gated and this
 * is a variant-1 endpoint (mt_matter.h's MT_EVSE_F_* note; the two cases
 * share one metadata-driven gate and one error code, deliberately not the
 * same code 0x0094's feature gate uses, since 0x0099 has no feature-bit
 * table to consult in mt_at.c). A bare ERROR covers an unclassified runtime
 * failure, routed through attr_err_to_mterr() like every other bridge call
 * in this file.
 */
#define MT_MEAS_MAX_PAIRS 7
static int cmd_mtmeas(at_type_t type, char *args)
{
    char *f[2 + 2 * MT_MEAS_MAX_PAIRS];
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    int n = at_split_args(args, f, 2 + 2 * MT_MEAS_MAX_PAIRS);
    if (n < 4 || (n - 2) % 2 != 0) {
        return MT_ERR_BAD_PARAM;
    }
    unsigned long ep, cluster;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF || !parse_u(f[1], &cluster)) {
        return MT_ERR_BAD_PARAM;
    }
    uint8_t fields[MT_MEAS_MAX_PAIRS];
    int64_t values[MT_MEAS_MAX_PAIRS];
    uint8_t count = (uint8_t)((n - 2) / 2);
    for (uint8_t i = 0; i < count; i++) {
        unsigned long field;
        if (!parse_u(f[2 + 2 * i], &field) || field > 0xFF) {
            return MT_ERR_BAD_PARAM;
        }
        /* signedness per (cluster, field): energy fields are unsigned,
         * power fields signed; the bridge re-validates, this parse only
         * needs the right accept set */
        if (!parse_i64(f[3 + 2 * i], mt_meas_field_signed((uint32_t)cluster, (uint8_t)field), &values[i])) {
            return MT_ERR_BAD_PARAM;
        }
        fields[i] = (uint8_t)field;
    }
    int r = mt_matter_meas_set((uint16_t)ep, (uint32_t)cluster, fields, values, count);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
    return AT_R_OK;
}

/*
 * AT+MTDEMCAP=<ep>,<cause>,<n>[,<minPower>,<maxPower>,<minDuration>,<maxDuration>]{n}
 *   -> replace <ep>'s whole DeviceEnergyManagement PowerAdjustmentCapability
 * (energy round C1, design spec 3.2): <cause> is the PowerAdjustReasonEnum,
 * <n> counts PowerAdjustStruct entries, 0..MT_DEM_CAP_MAX_ENTRIES where 0
 * means the capability reads null. Set-only, no query form (the Instance-
 * owned rule): bare/query forms answer a plain ERROR. A struct list
 * genuinely does not fit AT+MTMEAS's field=value grammar, hence the round's
 * one new command family.
 *
 * The error division, per the design spec's own split for this command
 * ("bare ERROR (form) / +MTERR:1 (values)"):
 *   - exactly 3 + 4n parameters or bare ERROR: <n> declares the command's
 *     own shape, so a token count contradicting it is a malformed FORM, not
 *     a bad value (contrast cmd_mtmeas above, whose pair grammar is
 *     self-delimiting and answers +MTERR:1 on arity);
 *   - +MTERR:1 for values: a non-numeric token, <n> above
 *     MT_DEM_CAP_MAX_ENTRIES (a well-formed count carrying an out-of-range
 *     value), a duration above u32, or a signed-parse failure on a power.
 * This C side checks arity and signedness only (the standing split): powers
 * parse through the signed 64-bit pipeline, durations through the unsigned
 * one with an explicit u32 bound. parse_u64 rather than parse_u for the
 * durations, deliberately: parse_u's unsigned long is 32-bit on the target
 * and strtoul clamps an out-of-range token to ULONG_MAX without parse_u
 * noticing (it never checks errno), so "4294967296" would silently become
 * 0xFFFFFFFF there while the 64-bit host suite rejected it; parse_u64
 * checks ERANGE, so the u32 bound below actually holds on both. Everything
 * semantic (the cause enum range, per-entry
 * minPower<=maxPower / minDuration<=maxDuration ordering, the
 * PowerAdjustment feature gate) is the bridge's business
 * (mt_matter_demcap_set(), main.cpp) and answers through
 * attr_err_to_mterr() like every other bridge call in this file.
 *
 * The token array carries one entry of headroom past the n=4 maximum so a
 * full n=5 line still splits far enough for the <n> range check to answer
 * +MTERR:1; anything longer overflows at_split_args() and lands in the
 * arity mismatch's bare ERROR (the <n> token itself is always terminated,
 * so the range check still outranks the count check).
 */
#define MT_DEMCAP_MAX_TOKENS (3 + 4 * (MT_DEM_CAP_MAX_ENTRIES + 1))
static int cmd_mtdemcap(at_type_t type, char *args)
{
    char *f[MT_DEMCAP_MAX_TOKENS];
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    int ntok = at_split_args(args, f, MT_DEMCAP_MAX_TOKENS);
    if (ntok >= 0 && ntok < 3) {
        /* fewer than the three mandatory parameters: form */
        return MT_R_ERROR;
    }

    unsigned long ep, cause, n;
    if (!parse_u(f[0], &ep) || ep > 0xFFFF ||
        !parse_u(f[1], &cause) || cause > 0xFF ||
        !parse_u(f[2], &n)) {
        return MT_ERR_BAD_PARAM;
    }
    if (n > MT_DEM_CAP_MAX_ENTRIES) {
        return MT_ERR_BAD_PARAM;
    }
    if (ntok != 3 + 4 * (int)n) {
        /* the token count contradicts <n>: form */
        return MT_R_ERROR;
    }

    int64_t quads[4 * MT_DEM_CAP_MAX_ENTRIES];
    for (unsigned long i = 0; i < n; i++) {
        /* minPower, maxPower: int64 mW through the 64-bit pipeline */
        if (!parse_i64(f[3 + 4 * i], true, &quads[4 * i]) ||
            !parse_i64(f[4 + 4 * i], true, &quads[4 * i + 1])) {
            return MT_ERR_BAD_PARAM;
        }
        /* minDuration, maxDuration: u32 seconds (parse_u64 for the
         * ERANGE-checked bound, see the block comment above) */
        uint64_t mind, maxd;
        if (!parse_u64(f[5 + 4 * i], &mind) || mind > 0xFFFFFFFFULL ||
            !parse_u64(f[6 + 4 * i], &maxd) || maxd > 0xFFFFFFFFULL) {
            return MT_ERR_BAD_PARAM;
        }
        quads[4 * i + 2] = (int64_t)mind;
        quads[4 * i + 3] = (int64_t)maxd;
    }

    int r = mt_matter_demcap_set((uint16_t)ep, (uint8_t)cause, (uint8_t)n, quads);
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

/*
 * AT+MTTHREAD? -> +MTTHREAD:<role>,<attached>,<channel>,<panid>,<extpanid>,
 *                            <partitionid>,"<name>"
 * (design spec 2.1, task 1). Query form only; a set form is a wrong command
 * FORM, so it answers a bare ERROR, not a +MTERR line (this project's own
 * grammar division). Exists on both images: on a WiFi image
 * mt_matter_thread_info() answers MT_ATTR_ERR_CLUSTER (Thread's diagnostics
 * cluster is never created there) and this handler maps that specifically
 * to +MTERR:8 (MT_ERR_UNSUPPORTED), not the generic +MTERR:3 attr_err_to_
 * mterr() would give a plain missing cluster, since the question here is
 * "does this image speak Thread at all", not "does this one endpoint carry
 * the cluster".
 */
static int cmd_mtthread(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return MT_R_ERROR;
    }

    mt_thread_info_t info;
    int r = mt_matter_thread_info(&info);
    if (r != MT_ATTR_OK) {
        /* Any failure other than "no cluster" should not happen (endpoint 0
         * always exists): no more specific +MTERR code applies, so it falls
         * back to a bare ERROR rather than borrowing attr_err_to_mterr()'s
         * generic attribute-lookup mapping. */
        return (r == MT_ATTR_ERR_CLUSTER) ? MT_ERR_UNSUPPORTED : MT_R_ERROR;
    }

    /* <role>: a decoded RoutingRoleEnum token, or the raw decimal when the
     * value is outside the known set (spec 2.1: "a future SDK addition
     * degrades to a number rather than a lie"). */
    char role_tok[8];
    const char *role = mt_thread_role_name(info.role);
    if (role == NULL) {
        snprintf(role_tok, sizeof(role_tok), "%u", (unsigned)info.role);
        role = role_tok;
    }

    /*
     * Null renders as an EMPTY field, never as 0 (spec 2.1): channel decimal,
     * the three ids 0x-prefixed hex at their natural byte width, matching
     * how AT+MTEP/AT+MTEVTMASK already render ids in this file. Each stays
     * "" (its already-initialised value) when the matching has_* flag is
     * clear.
     */
    char channel_f[8]   = "";
    char panid_f[8]     = "";
    char extpanid_f[20] = "";
    char partid_f[12]   = "";
    if (info.has_channel) {
        snprintf(channel_f, sizeof(channel_f), "%u", (unsigned)info.channel);
    }
    if (info.has_panid) {
        snprintf(panid_f, sizeof(panid_f), "0x%04lX", (unsigned long)info.panid);
    }
    if (info.has_extpanid) {
        snprintf(extpanid_f, sizeof(extpanid_f), "0x%016llX",
                 (unsigned long long)info.extpanid);
    }
    if (info.has_partitionid) {
        snprintf(partid_f, sizeof(partid_f), "0x%08lX", (unsigned long)info.partitionid);
    }

    /*
     * <name>: always double-quoted, `"` escaped as `\"` and `\` as `\\`
     * (spec 2.1, normative for any future string field in this command
     * family). name_esc is sized for the worst case, every one of
     * info.name's up to 16 characters needing a 2-byte escape, plus the
     * terminator.
     */
    char name_esc[2 * sizeof(info.name)];
    size_t o = 0;
    for (size_t i = 0; info.name[i] != '\0'; i++) {
        char c = info.name[i];
        if (o + 3 > sizeof(name_esc)) {
            break;  /* defensive: cannot happen given the sizing above */
        }
        if (c == '"' || c == '\\') {
            name_esc[o++] = '\\';
        }
        name_esc[o++] = c;
    }
    name_esc[o] = '\0';

    at_uart_write_line("+MTTHREAD:%s,%d,%s,%s,%s,%s,\"%s\"",
                       role, info.attached ? 1 : 0,
                       channel_f, panid_f, extpanid_f, partid_f, name_esc);
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
 * Shared body of mt_cmd_forward(), mt_cmd_forward_payload(),
 * mt_cmd_forward_fields() and mt_rows_inbound_forward(): open a mailbox
 * slot, raise the "+MTCMD:<seq>,..." URC, and block up to wait_ms for the
 * host's verdict.
 *
 * wait_ms is a parameter rather than a constant for exactly one caller:
 * mt_rows_inbound_forward() passes MT_CMD_VERDICT_ROWS_MS because its host
 * has to fetch the rows before it can judge them. Every other caller passes
 * MT_CMD_VERDICT_MS, which is the same 1000 ms they have always used; the
 * parameter exists so that stays visibly true at each call site instead of
 * depending on a shared constant nobody edits by accident.
 *
 * seq_out, when non-NULL, receives the sequence number IMMEDIATELY after the
 * mailbox slot is opened and BEFORE the URC is written. The ordering is the
 * whole point of the parameter: AT+MTROWGET's seq-qualified form compares a
 * host-supplied seq against the outstanding forward's, and the host can only
 * learn that seq from the URC, so recording it first makes "the host knows
 * the seq" imply "the firmware has published it". Written before the wire,
 * never after. NULL for every scalar forward, none of which has anything to
 * qualify. fields is the
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
                                   const char *fields, uint32_t wait_ms, uint32_t *seq_out)
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

    /* Publish the seq before the URC, never after: see the header comment. */
    if (seq_out != NULL) {
        *seq_out = seq;
    }

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
     * race at the deadline would otherwise cascade into permanent,
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

    if (xSemaphoreTake(s_cmd_sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
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
    return mt_cmd_forward_common(ep, cluster, command, NULL, MT_CMD_VERDICT_MS, NULL);
}

bool mt_cmd_forward_payload(uint16_t ep, uint32_t cluster, uint32_t command, uint32_t payload)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)payload);
    return mt_cmd_forward_common(ep, cluster, command, buf, MT_CMD_VERDICT_MS, NULL);
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
    return mt_cmd_forward_common(ep, cluster, command, fields, MT_CMD_VERDICT_MS, NULL);
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

/* ---- nested row payloads (AT+MTROW family, energy round C2 task 2) ---- *
 * AT+MTROW stages one row at a time into a single global buffer (matching
 * the composition's AT+MTEP / AT+MTEPCLEAR / AT+MTEPAPPLY staging idiom,
 * design spec 2.1); AT+MTROWCLEAR discards the pending set; AT+MTROWAPPLY
 * validates the whole set and commits it through the mt_matter_rows_*
 * bridge (implemented later, in C++, by the task that wires each row kind
 * to a live store); AT+MTROWGET reads back, either one row or every row,
 * straight from the live store, with no cursor state kept here (design
 * spec 2.3, "reads are stateless").                                       */

/* Fixed leading tokens (<ep>,<kind>,<idx>) plus the widest row shape any
 * kind defines. */
#define MT_ROW_MAX_TOKENS (3 + MT_ROW_MAX_FIELDS)

/* One global staging buffer for the HOST's row sets, matching
 * s_staged/s_staging above: a host stages one (ep, kind) set at a time, and
 * mt_rows_stage() itself discards a set staged for a different (ep, kind)
 * before starting a new one.
 *
 * WRITTEN ONLY BY THE AT PARSER TASK. The fabric-originated sets a
 * controller's SetTargets brings in live in s_row_inbound below, a
 * separate buffer written only by the CHIP event loop task; see that
 * block's comment for why the two are not one buffer, and mt_at.h for the
 * host-facing contract. */
static mt_row_stage_t s_row_stage;

/*
 * Guards every call into mt_rows.c's mutators (mt_rows_stage(),
 * mt_rows_clear(), mt_rows_validate()) against s_row_stage, and every
 * access to the inbound stage's ownership state below: same reason and the
 * same pattern as s_cmdbox_mux above. mt_rows.c has no locking of its own,
 * by design (it stays host-testable with no FreeRTOS headers, see
 * mt_rows.h's file comment), so the critical section lives here, never in
 * mt_rows.c.
 *
 * The mux is deliberately NOT held across mt_matter_rows_apply(): that call
 * may block (it takes ChipStackLock, the standing mt_matter_* bridge
 * convention) and a portMUX_TYPE is a spinlock, which must never wrap a
 * call that can block or take another lock. cmd_mtrowapply() below reads
 * s_row_stage under the mux only long enough to validate and decide
 * whether to proceed, then passes the struct to the bridge by pointer,
 * unguarded: a full copy does not fit the AT parser task's 6144-byte
 * stack (mt_row_stage_t holds MT_ROW_MAX_ROWS rows and runs to several KB,
 * see mt_rows.h).
 *
 * That unguarded pointer pass was flagged when it shipped (task 2) as
 * something the inbound SetTargets path would have to close, because it is
 * only safe while the AT parser task is s_row_stage's ONLY writer. Task 6
 * closed it by keeping that true: the inbound path never touches this
 * buffer. Anything added here that writes s_row_stage from another task
 * re-opens the window, and the failure mode is a host's AT+MTROWAPPLY
 * committing rows it never staged.
 */
static portMUX_TYPE s_row_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- the inbound row stage (energy round C2, task 6) ------------------- *
 *
 * The buffer a fabric-originated multi-row command (today only EnergyEvse
 * SetTargets) stages its payload into so the host can PULL it with
 * AT+MTROWGET during the verdict window. mt_at.h carries the caller's
 * contract and the reasoning for the pull; this block is the concurrency
 * argument, which is the part that cannot be seen from the header.
 *
 * ---- WHO WRITES WHAT, AND WHEN ----
 *
 * s_row_stage      written only by the AT parser task (cmd_mtrow,
 *                  cmd_mtrowclear, cmd_mtrowapply).
 * s_row_inbound    written only by the CHIP event loop task, and only
 *                  while it owns the claim.
 * the state below  written by both, always inside s_row_mux.
 *
 * There is no buffer either task can write while the other reads. That is
 * the whole design, and it is why the ~5.6 KB of a second mt_row_stage_t is
 * spent rather than sharing one: with one buffer, a controller's SetTargets
 * lands on top of whatever set the host is half way through uploading for
 * the same (ep, kind), and when the two happen to hold the same number of
 * rows the host's next AT+MTROWAPPLY validates cleanly and commits THE
 * CONTROLLER'S rows as its own. No unit test can see that, no build can see
 * that, and on the bench it looks like a schedule the user never wrote.
 *
 * ---- THE CLAIM STATES ----
 *
 * IDLE     nobody owns the buffer.
 * STAGING  the CHIP task is filling it. NOT readable: a reader would see a
 *          half-built set.
 * PENDING  fully staged, the +MTCMD is on the wire, the CHIP task is
 *          BLOCKED in xSemaphoreTake and touches nothing. This is the only
 *          state cmd_mtrowget() serves from, and the CHIP task's quiescence
 *          for the whole of it is what makes that read safe with no lock
 *          around the row data itself.
 * CLOSED   the verdict is in and the CHIP task is merging (a read of the
 *          stage, never a write). No NEW reader is admitted, because the
 *          set is no longer what a host would be asking about, but a reader
 *          already streaming keeps going: mt_rows_inbound_release() below
 *          deliberately does NOT clear row[], so the bytes under an
 *          in-flight reader stay intact.
 *
 * s_inbound_reading is the one flag that guards the other direction: a
 * bulk AT+MTROWGET of 70 rows takes on the order of 300 ms at 115200, long
 * enough for a forward to time out, a second SetTargets to arrive, and its
 * mt_rows_init() to memset the array under the reader's feet. A claim is
 * therefore refused while a read is in flight, and the controller gets
 * Busy, which it can retry.
 *
 * ---- WHY THE READ IS QUALIFIED BY SEQUENCE NUMBER ----
 *
 * s_inbound_seq is the outstanding forward's seq, and AT+MTROWGET only
 * serves the pending set when the host names it. Without that, the pending
 * read is silently ambiguous: the response is byte-identical to a live-store
 * read, and the override goes live before the URC is even written, so a host
 * command already in flight could be answered from a PROPOSAL it has never
 * heard of. A host that polls for its own reasons and caches the answer
 * would then hold a schedule this device may never store, with nothing on
 * the wire to tell it apart from the truth.
 *
 * Making that a rule in the spec and obeying it in the library would leave
 * the protocol able to express the mistake. Requiring the seq makes it
 * unrepresentable: the seq is only knowable from the +MTCMD line, so asking
 * for a proposal is now an act a host cannot perform by accident, and an
 * unqualified read is always the live store.
 */
static mt_row_stage_t s_row_inbound;

typedef enum {
    MT_INBOUND_IDLE = 0,
    MT_INBOUND_STAGING,
    MT_INBOUND_PENDING,
    MT_INBOUND_CLOSED,
} mt_inbound_state_t;

static mt_inbound_state_t s_inbound_state   = MT_INBOUND_IDLE;
static bool               s_inbound_reading = false;
/* 0 when nothing is outstanding. mt_cmdbox_open() never issues 0, so the
 * idle value cannot be matched by any seq a host could legitimately hold. */
static uint32_t           s_inbound_seq     = 0;

mt_row_stage_t *mt_rows_inbound_claim(uint16_t ep, uint8_t kind)
{
    taskENTER_CRITICAL(&s_row_mux);
    bool got = (s_inbound_state == MT_INBOUND_IDLE && !s_inbound_reading);
    if (got) {
        s_inbound_state = MT_INBOUND_STAGING;
    }
    taskEXIT_CRITICAL(&s_row_mux);

    if (!got) {
        return NULL;
    }

    /* Outside the critical section on purpose: this memsets several KB, and
     * a portMUX_TYPE critical section disables interrupts. The STAGING
     * state is what keeps everyone else out meanwhile, which is exactly
     * what a state machine buys over holding a lock. */
    mt_rows_init(&s_row_inbound);

    /*
     * Stamp the identity HERE rather than leaving it to the first
     * mt_rows_stage() call, so that a set of ZERO rows is still identified
     * as belonging to (ep, kind). Without this, an empty inbound set would
     * fail cmd_mtrowget()'s match, fall through to the live-store read, and
     * block on ChipStackLock for the whole verdict window: the one
     * unanswerable-forward trap this whole branch exists to avoid, hiding
     * in the one case with no rows in it. An empty set must answer "total
     * 0, no rows" exactly as promptly as a full one.
     */
    s_row_inbound.active = true;
    s_row_inbound.ep     = ep;
    s_row_inbound.kind   = kind;
    return &s_row_inbound;
}

bool mt_rows_inbound_forward(uint16_t ep, uint32_t cluster, uint32_t command, uint32_t aux)
{
    /* The count comes off the stage, never from the caller: the number on
     * the wire and the number of rows AT+MTROWGET will hand back are then
     * the same fact read once, not two values to keep in step. */
    taskENTER_CRITICAL(&s_row_mux);
    uint16_t count = s_row_inbound.count;
    if (s_inbound_state == MT_INBOUND_STAGING) {
        s_inbound_state = MT_INBOUND_PENDING;
    }
    taskEXIT_CRITICAL(&s_row_mux);

    /*
     * "<rowcount>,<aux>". Worst case, with every head at its maximum:
     * "+MTCMD:" 7 + seq 10 (u32) + 1 + ep 5 + 1 + cluster 10 + 1 +
     * command 10 + 1 + rowcount 5 (u16) + 1 + aux 10 (u32) = 62, plus a NUL
     * = 63, against MT_CMD_LINE_MAX 112. Today's only caller is far smaller
     * (cluster 153, command 5, rowcount <= 70, aux <= 127), and the widest
     * line this buffer must hold is still the generic four-field tail of
     * mt_cmd_forward_fields(), unchanged by this task.
     */
    char fields[24];
    snprintf(fields, sizeof(fields), "%u,%lu", (unsigned)count, (unsigned long)aux);

    bool allow = mt_cmd_forward_common(ep, cluster, command, fields, MT_CMD_VERDICT_ROWS_MS,
                                       &s_inbound_seq);

    /* Verdict in (or timed out): stop admitting new readers before the
     * caller starts merging. A reader already streaming is unaffected, see
     * the state comment above. */
    taskENTER_CRITICAL(&s_row_mux);
    if (s_inbound_state == MT_INBOUND_PENDING) {
        s_inbound_state = MT_INBOUND_CLOSED;
    }
    taskEXIT_CRITICAL(&s_row_mux);

    return allow;
}

void mt_rows_inbound_release(void)
{
    taskENTER_CRITICAL(&s_row_mux);
    /* active/count only. row[] is deliberately left alone: a reader that
     * snapshotted the count under this same mux is still walking it, and
     * the next claim's mt_rows_init() is the thing that actually clears the
     * array, which cannot run while that reader holds s_inbound_reading. */
    s_row_inbound.active = false;
    s_row_inbound.count  = 0;
    s_inbound_state      = MT_INBOUND_IDLE;
    s_inbound_seq        = 0;
    taskEXIT_CRITICAL(&s_row_mux);
}

/*
 * Map every mt_rows.h / mt_matter.h MT_ROW_* code onto this personality's
 * +MTERR space (design spec 2.5's error table, reproduced in the task
 * brief): MT_ROW_OK -> OK, MT_ROW_ERR_FORM -> a bare ERROR (the command
 * form was wrong), MT_ROW_ERR_VALUE -> +MTERR:1, MT_ROW_ERR_KIND ->
 * +MTERR:3, MT_ROW_ERR_ENDPOINT -> +MTERR:2, MT_ROW_ERR_NO_PAYLOAD ->
 * +MTERR:4. No new error codes this round.
 */
static int row_err_to_mterr(int r)
{
    switch (r) {
    case MT_ROW_OK:             return AT_R_OK;
    case MT_ROW_ERR_VALUE:      return MT_ERR_BAD_PARAM;
    case MT_ROW_ERR_KIND:       return MT_ERR_NO_CLUSTER;
    case MT_ROW_ERR_ENDPOINT:   return MT_ERR_NO_ENDPOINT;
    case MT_ROW_ERR_NO_PAYLOAD: return MT_ERR_NO_ATTRIBUTE;
    /* Applied to the live model but not written to NVS (energy round C2
     * task 4): the same +MTERR:7 every other persistence failure in this
     * file answers, so a host can tell "it will not survive a reboot" from
     * "it was rejected". */
    case MT_ROW_ERR_PERSIST:    return MT_ERR_PERSIST;
    case MT_ROW_ERR_FORM:       return MT_R_ERROR;
    default:                    return MT_R_ERROR;
    }
}

/*
 * AT+MTROW=<ep>,<kind>,<idx>,<field>[,<field>...] -> stage one row of a
 * nested payload (design spec 2.1) into the single global row stage above.
 * Set-only: bare/query forms answer a plain ERROR.
 *
 * <kind> selects the field table (mt_rows_field_count()); the number of
 * trailing field tokens must equal it EXACTLY, always, including a
 * present-but-empty token for an absent optional: "AT+MTROW=5,1,0,64,480,80"
 * for a 4-field kind is five tokens, one short, a form error (bare ERROR),
 * while "AT+MTROW=5,1,0,64,480,80," (trailing comma) is a complete,
 * well-formed row whose last field is an absent optional. That is what
 * keeps "the line was too short" (form) distinguishable from "the host
 * chose not to supply this optional" (a value-level concept): the token
 * must still be present, only its content may be empty.
 *
 * <field> tokens parse through the generic signed 64-bit pipeline
 * (parse_i64(), sign always accepted): mt_at.c does not know any kind's
 * per-field range or signedness, only mt_rows.c's field table does (kept
 * that way deliberately, mt_rows.h's file comment), so range and
 * signedness rejection both happen inside mt_rows_stage() and both answer
 * +MTERR:1 (MT_ROW_ERR_VALUE) the same way.
 *
 * This handler never touches the live composition (staging is pure RAM),
 * so it can only answer a bare ERROR, +MTERR:1 or +MTERR:3, never
 * +MTERR:2 or +MTERR:4; those two are reserved for AT+MTROWAPPLY and
 * AT+MTROWGET, which do consult the live model through the
 * mt_matter_rows_* bridge.
 */
static int cmd_mtrow(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    char *f[MT_ROW_MAX_TOKENS];
    int ntok = at_split_args(args, f, MT_ROW_MAX_TOKENS);
    if (ntok < 3) {
        /* too few tokens for even <ep>,<kind>,<idx>, or at_split_args()
         * overflowed (-1, more tokens than any kind could ever need):
         * either way, form */
        return MT_R_ERROR;
    }

    unsigned long ep;
    if (!parse_u(f[0], &ep)) {
        return MT_ERR_BAD_PARAM;
    }
    if (ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long kind;
    if (!parse_u(f[1], &kind)) {
        return MT_ERR_BAD_PARAM;
    }
    if (kind > 0xFF) {
        return MT_ERR_NO_CLUSTER;
    }
    int nfields = mt_rows_field_count((uint8_t)kind);
    if (nfields < 0) {
        return MT_ERR_NO_CLUSTER;
    }

    if (ntok - 3 != nfields) {
        return MT_R_ERROR;
    }

    unsigned long idx;
    if (!parse_u(f[2], &idx)) {
        return MT_ERR_BAD_PARAM;
    }
    if (idx > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    mt_row_t row;
    memset(&row, 0, sizeof(row));
    row.nfields = (uint8_t)nfields;
    for (int i = 0; i < nfields; i++) {
        const char *tok = f[3 + i];
        if (tok[0] == '\0') {
            /* an empty token: absent optional (design spec 2.2), the one
             * place in the whole protocol an empty field is not a form
             * error */
            continue;
        }
        int64_t v;
        if (!parse_i64(tok, true, &v)) {
            return MT_ERR_BAD_PARAM;
        }
        row.present[i] = true;
        row.value[i] = v;
    }

    taskENTER_CRITICAL(&s_row_mux);
    int r = mt_rows_stage(&s_row_stage, (uint16_t)ep, (uint8_t)kind, (uint16_t)idx, &row);
    taskEXIT_CRITICAL(&s_row_mux);

    return row_err_to_mterr(r);
}

/*
 * AT+MTROWCLEAR=<ep>,<kind> -> discard the PENDING (staged) set for
 * (ep, kind); never touches the stored/live payload (design spec 2.6,
 * "the two are deliberately different verbs because confusing them
 * destroys a live schedule"). Set-only: bare/query forms answer ERROR.
 *
 * mt_rows_clear() itself refuses (MT_ROW_ERR_VALUE, +MTERR:1) rather than
 * silently no-op when nothing is staged for (ep, kind): see its comment in
 * mt_rows.c. A <kind> mt_rows.c has never heard of answers +MTERR:3 here,
 * the same as every other command in the family, checked before calling
 * into the codec so a truly unknown kind is never confused with "nothing
 * staged".
 */
static int cmd_mtrowclear(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    char *f[2];
    int n = at_split_args(args, f, 2);
    if (n != 2) {
        return MT_R_ERROR;
    }

    unsigned long ep;
    if (!parse_u(f[0], &ep)) {
        return MT_ERR_BAD_PARAM;
    }
    if (ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long kind;
    if (!parse_u(f[1], &kind)) {
        return MT_ERR_BAD_PARAM;
    }
    if (kind > 0xFF || mt_rows_field_count((uint8_t)kind) < 0) {
        return MT_ERR_NO_CLUSTER;
    }

    taskENTER_CRITICAL(&s_row_mux);
    int r = mt_rows_clear(&s_row_stage, (uint16_t)ep, (uint8_t)kind);
    taskEXIT_CRITICAL(&s_row_mux);

    return row_err_to_mterr(r);
}

/*
 * AT+MTROWAPPLY=<ep>,<kind>,<count> -> validate the whole staged set for
 * (ep, kind), then commit it through mt_matter_rows_apply() (design spec
 * 2.6). <count> must equal the number of rows staged for (ep, kind), or 0,
 * which is the documented clear request: it empties the entire stored
 * payload for (ep, kind), whether or not anything happens to be staged
 * (mt_rows_validate()'s own comment; do NOT add an "is anything staged"
 * precondition here, it would reintroduce the exact bug Task 1's review
 * caught). Set-only: bare/query forms answer ERROR.
 *
 * mt_rows_validate() checks the set structurally (dense indices, kind-
 * specific whole-set rules) but, by design, takes no <ep>/<kind> of its
 * own to compare against: it only sees whatever is CURRENTLY staged. So
 * this handler checks the (ep, kind) match itself before trusting
 * mt_rows_validate()'s count comparison: a nonzero <count> against a stage
 * that belongs to a different (ep, kind), or nothing at all, is the same
 * "count disagrees with what is actually staged for this target" value
 * error mt_rows_validate() gives when the match DOES hold but the count is
 * wrong, so both answer +MTERR:1 identically.
 *
 * On a successful commit the pending set is retired (reset to inactive),
 * mirroring AT+MTEPAPPLY ending its own staging session; on any failure
 * the stage is left untouched so the host can fix and retry without
 * restaging everything.
 */
static int cmd_mtrowapply(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    char *f[3];
    int n = at_split_args(args, f, 3);
    if (n != 3) {
        return MT_R_ERROR;
    }

    unsigned long ep;
    if (!parse_u(f[0], &ep)) {
        return MT_ERR_BAD_PARAM;
    }
    if (ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long kind;
    if (!parse_u(f[1], &kind)) {
        return MT_ERR_BAD_PARAM;
    }
    if (kind > 0xFF || mt_rows_field_count((uint8_t)kind) < 0) {
        return MT_ERR_NO_CLUSTER;
    }

    unsigned long count;
    if (!parse_u(f[2], &count)) {
        return MT_ERR_BAD_PARAM;
    }
    if (count > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    taskENTER_CRITICAL(&s_row_mux);
    bool matches = s_row_stage.active && s_row_stage.ep == (uint16_t)ep &&
                   s_row_stage.kind == (uint8_t)kind;
    int vr;
    if (count == 0) {
        /*
         * The documented clear request: a count of 0 means the host wants
         * the STORED payload emptied, full stop, and any rows it happened
         * to stage first for this (ep, kind) are ABANDONED, not committed.
         * Neutralize the stage HERE, under the mux, before the unlocked
         * bridge call below, and do it UNCONDITIONALLY: whether or not
         * s_row_stage currently matches (ep, kind).
         *
         * This must not be gated on "!matches". mt_matter_rows_apply()'s
         * documented contract (mt_matter.h) is: commit stage->row[] only
         * when stage->active && stage->ep == ep && stage->kind == kind,
         * otherwise apply zero rows. If matches is true here (a set IS
         * staged for exactly this ep/kind) and we skip the reset, the
         * stage sails into the bridge still active with its real,
         * nonzero count, matching perfectly, so the bridge commits it.
         * Apply MERGES rather than replaces (design spec 2.6), so the
         * host's "give me an empty schedule" request would silently
         * commit whatever it had staged instead, answered with a plain
         * OK: exactly the bug the review caught (AT+MTROW=5,1,0,64,480,
         * 80, then AT+MTROW=5,1,1,32,600,90, then AT+MTROWAPPLY=5,1,0
         * must empty ep 5's schedule, not merge those two rows into it).
         * Resetting unconditionally means the bridge always sees a
         * non-matching (inactive) stage for a count-0 call, so its own
         * "no match -> zero rows" rule is what actually clears, with no
         * special case to keep in sync on either side.
         *
         * This is the SECOND time count == 0's semantics have needed a
         * deliberate, commented carve-out in this function: the first was
         * Task 1's review catching a guard that rejected this same path
         * outright (a count of 0 with nothing staged used to answer
         * +MTERR:1 instead of clearing). Read both defects before
         * touching this branch again.
         */
        mt_rows_init(&s_row_stage);
        vr = MT_ROW_OK;
    } else if (!matches) {
        /* nothing staged for THIS (ep, kind): a nonzero count cannot
         * possibly match it, the same value error mt_rows_validate() gives
         * when a matching stage's count is wrong */
        vr = MT_ROW_ERR_VALUE;
    } else {
        vr = mt_rows_validate(&s_row_stage, (uint16_t)count);
    }
    taskEXIT_CRITICAL(&s_row_mux);

    if (vr != MT_ROW_OK) {
        return row_err_to_mterr(vr);
    }

    /* Outside the critical section: mt_matter_rows_apply() may block (it
     * takes ChipStackLock), which a portMUX_TYPE spinlock must never wrap.
     * It is passed the file-static stage directly (see s_row_mux's comment
     * above for why this is not a full copy) and is responsible for
     * treating a stage that does not currently match (ep, kind) as an
     * empty set, exactly the convention just used above. */
    int r = mt_matter_rows_apply((uint16_t)ep, (uint8_t)kind, &s_row_stage);
    if (r != MT_ROW_OK) {
        return row_err_to_mterr(r);
    }

    taskENTER_CRITICAL(&s_row_mux);
    if (s_row_stage.active && s_row_stage.ep == (uint16_t)ep && s_row_stage.kind == (uint8_t)kind) {
        mt_rows_init(&s_row_stage);
    }
    taskEXIT_CRITICAL(&s_row_mux);

    return AT_R_OK;
}

/*
 * Worst-case rendered "+MTROW:" line, for the assertion the task brief asks
 * for: "+MTROW:" (7 bytes) + a 5-digit <idx> (uint16_t max 65535) + ","
 * (1) + a 5-digit <total> (5) + MT_ROW_MAX_FIELDS (8) fields, each a ","
 * (1) plus a full-width negative int64 (INT64_MIN, "-9223372036854775808",
 * 20 characters). 7 + 5 + 1 + 5 + 8 * (1 + 20) = 186 bytes, plus a NUL =
 * 187; 200 leaves headroom. This is the bound for the GENERIC
 * MT_ROW_MAX_FIELDS shape any future kind might use, not this round's
 * actual one: kind 1 (EVSE target) has 4 fields, all non-negative, idx
 * 0..69 and total <= 70, so its worst line ("+MTROW:69,70,127,1439,100,"
 * plus a 19-digit added-energy value) is under 50 bytes, matching the task
 * brief's "roughly 40 bytes" estimate. Either way this sits far inside the
 * 250-byte budget the host's 255-byte usable RX line leaves for a response
 * (design spec 1.1 / 2.5): the bulk read is the one place in this round a
 * line-length mistake would be invisible to firmware and silently dropped
 * whole by the host (design spec 1.1's overflow-behaviour row).
 */
#define MT_ROW_LINE_MAX 200

static void emit_row_line(uint16_t idx, uint16_t total, const mt_row_t *row, int nfields)
{
    char line[MT_ROW_LINE_MAX];
    int off = snprintf(line, sizeof(line), "+MTROW:%u,%u", (unsigned)idx, (unsigned)total);
    for (int i = 0; i < nfields && off >= 0 && (size_t)off < sizeof(line); i++) {
        if (row->present[i]) {
            off += snprintf(line + off, sizeof(line) - (size_t)off, ",%lld",
                             (long long)row->value[i]);
        } else {
            off += snprintf(line + off, sizeof(line) - (size_t)off, ",");
        }
    }
    at_uart_write_line("%s", line);
}

/*
 * AT+MTROWGET=<ep>,<kind>,<idx> -> one line, "+MTROW:<idx>,<total>,<field>
 * [,<field>...]", then OK; <idx> at or past <total> answers OK with no
 * +MTROW line (design spec 2.3, how a host detects an empty payload).
 * AT+MTROWGET=<ep>,<kind>       -> every row, 0..<total>-1, as consecutive
 * +MTROW: lines, then OK; this bulk form is only safe because the host is
 * inside a command dedicated to draining it (design spec 2.3).
 *
 * Reads are stateless: no cursor is kept here, every call re-derives
 * <total> from mt_matter_rows_total() before touching a single row, which
 * is also where +MTERR:2 (unknown ep) and +MTERR:4 (ep exists, no payload
 * of this kind) are answered, before any row read is attempted. Set-only:
 * bare/query forms answer ERROR.
 *
 * ---- THE ONE CASE THIS DOES NOT READ THE LIVE STORE (task 6) ----
 *
 * AT+MTROWGET=<ep>,<kind>,<idx>,<seq>  one proposed row
 * AT+MTROWGET=<ep>,<kind>,,<seq>       every proposed row
 *
 * With a <seq> naming the forward that is currently awaiting a verdict for
 * this (ep, kind), the command serves THAT set: the rows a controller is
 * proposing, which is what the host has just been asked to judge. The empty
 * <idx> token is the family's own absent-optional spelling, the same one
 * AT+MTROW uses, so the bulk and single forms stay one grammar.
 *
 * WITHOUT a <seq> this command ALWAYS reads the live store, and that is the
 * point of requiring one. The two responses are byte-identical, and the
 * pending set becomes servable before the +MTCMD line is even written, so
 * an unqualified override would let a host command already in flight be
 * answered from a proposal the host has never heard of; a host that polls
 * for its own reasons would cache a schedule this device may never store.
 * Since a <seq> can only be learned from the +MTCMD line, asking for a
 * proposal is now something a host cannot do by accident rather than
 * something a specification asks it not to do. A <seq> that does not match
 * the outstanding forward, or one supplied when nothing is pending, answers
 * +MTERR:1: it is a value that is wrong, not a command form that is.
 *
 * The pending branch is not merely a convenience, it is a REQUIREMENT, and
 * the reason is
 * the same one cmd_mtcmdresp() carries: mt_matter_rows_total() and
 * mt_matter_rows_get() take ChipStackLock, and the CHIP event loop task is
 * sitting inside its command dispatch (holding the stack) blocked on the
 * verdict semaphore. An AT+MTROWGET that went through the bridge during
 * that window would block on the stack lock for the whole deadline, so the
 * host could never fetch the rows, could never answer, and every schedule a
 * controller ever sent would time out and be denied. The inbound set is
 * therefore read straight out of RAM here, with no bridge call and no lock.
 *
 * The corollary, which belongs in the host's notes: during a verdict window
 * the ONLY two commands that make progress are this one and AT+MTCMDRESP.
 * Anything else that touches the data model blocks on the stack lock, and a
 * host that issues one before answering has queued its own verdict behind a
 * command that cannot complete until the verdict times out.
 */
static int cmd_mtrowget(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }

    char *f[4];
    int n = at_split_args(args, f, 4);
    if (n < 2 || n > 4) {
        /* Two tokens minimum, four maximum. A short line is still a form
         * error, exactly as before the seq form existed: the arity widened
         * at the tail, it did not become elastic. */
        return MT_R_ERROR;
    }

    unsigned long ep;
    if (!parse_u(f[0], &ep)) {
        return MT_ERR_BAD_PARAM;
    }
    if (ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }

    unsigned long kind;
    if (!parse_u(f[1], &kind)) {
        return MT_ERR_BAD_PARAM;
    }
    if (kind > 0xFF) {
        return MT_ERR_NO_CLUSTER;
    }
    int nfields = mt_rows_field_count((uint8_t)kind);
    if (nfields < 0) {
        return MT_ERR_NO_CLUSTER;
    }

    /*
     * <idx> is present for a single-row read and an EMPTY TOKEN for a bulk
     * read, but only when a <seq> follows it: with no <seq> there is no
     * later token to hold the position open, so a two-token line is the
     * bulk form and an empty third token is a form error, not a bulk
     * request spelled the long way. One spelling per shape.
     */
    bool single;
    unsigned long idx = 0;
    if (n == 2) {
        single = false;
    } else {
        bool idx_empty = (f[2][0] == '\0');
        if (idx_empty && n != 4) {
            return MT_R_ERROR;
        }
        single = !idx_empty;
        if (single) {
            if (!parse_u(f[2], &idx)) {
                return MT_ERR_BAD_PARAM;
            }
            if (idx > 0xFFFF) {
                return MT_ERR_BAD_PARAM;
            }
        }
    }

    bool          qualified = (n == 4);
    unsigned long seq       = 0;
    if (qualified) {
        if (!parse_u(f[3], &seq)) {
            return MT_ERR_BAD_PARAM;
        }
        if (seq > 0xFFFFFFFFUL || seq == 0) {
            /* 0 is never issued by mt_cmdbox_open() and is the idle marker
             * here, so naming it can never be a legitimate request. */
            return MT_ERR_BAD_PARAM;
        }
    }

    /*
     * The qualified read, before any bridge call: see the header comment for
     * why going near ChipStackLock during a verdict window would make the
     * whole adjudication unanswerable. Everything needed to serve the read
     * is snapshotted here under the mux; the rows themselves are then read
     * outside it, which is safe because the CHIP task is blocked for the
     * whole of MT_INBOUND_PENDING and no claim can start while
     * s_inbound_reading is set.
     */
    bool     inbound = false;
    uint16_t itotal  = 0;
    if (qualified) {
        taskENTER_CRITICAL(&s_row_mux);
        inbound = (s_inbound_state == MT_INBOUND_PENDING && s_inbound_seq == (uint32_t)seq &&
                   s_row_inbound.active && s_row_inbound.ep == (uint16_t)ep &&
                   s_row_inbound.kind == (uint8_t)kind);
        if (inbound) {
            itotal            = s_row_inbound.count;
            s_inbound_reading = true;
        }
        taskEXIT_CRITICAL(&s_row_mux);

        if (!inbound) {
            /* Named a seq that is not the outstanding one, or named one when
             * nothing is pending for this (ep, kind). NOT a silent fallback
             * to the live store: the host asked for a specific proposal, and
             * answering with something else is the confusion this form
             * exists to prevent. */
            return MT_ERR_BAD_PARAM;
        }
    }

    if (inbound) {
        if (single) {
            /* Same "at or past total answers OK with no line" rule as the
             * live read below, so a host parses one response shape. */
            if (idx < itotal) {
                emit_row_line((uint16_t)idx, itotal, &s_row_inbound.row[idx], nfields);
            }
        } else {
            for (uint16_t i = 0; i < itotal; i++) {
                emit_row_line(i, itotal, &s_row_inbound.row[i], nfields);
            }
        }
        taskENTER_CRITICAL(&s_row_mux);
        s_inbound_reading = false;
        taskEXIT_CRITICAL(&s_row_mux);
        return AT_R_OK;
    }

    uint16_t total;
    int r = mt_matter_rows_total((uint16_t)ep, (uint8_t)kind, &total);
    if (r != MT_ROW_OK) {
        return row_err_to_mterr(r);
    }

    if (single) {
        if (idx >= total) {
            /* at or past total: OK, no +MTROW line */
            return AT_R_OK;
        }
        mt_row_t row;
        uint16_t t2;
        int rr = mt_matter_rows_get((uint16_t)ep, (uint8_t)kind, (uint16_t)idx, &row, &t2);
        if (rr != MT_ROW_OK) {
            return row_err_to_mterr(rr);
        }
        emit_row_line((uint16_t)idx, t2, &row, nfields);
        return AT_R_OK;
    }

    for (uint16_t i = 0; i < total; i++) {
        mt_row_t row;
        uint16_t t2;
        int rr = mt_matter_rows_get((uint16_t)ep, (uint8_t)kind, i, &row, &t2);
        if (rr != MT_ROW_OK) {
            return row_err_to_mterr(rr);
        }
        emit_row_line(i, t2, &row, nfields);
    }
    return AT_R_OK;
}

/*
 * Scan one quoted string field of AT+MTMETERID starting at **pp, which must
 * point at the opening '"'. On success, *start and *len describe the
 * content (still inside the caller's line buffer, not copied) and *pp is
 * advanced past the closing quote.
 *
 * MT_R_ERROR: no opening quote, or the closing quote is never found before
 * the end of the line. Both are FORM errors (see the FORM/VALUE rule above
 * attr_err_to_mterr()), and for the same reason as each other: neither lets
 * this function establish where the quoted field's content is supposed to
 * start or end at all, which is a different failure from a token whose
 * boundary IS known (already isolated by a comma) but whose content will
 * not parse. "No opening quote" is this command's own required-token-
 * missing case (the position calls for a quoted string and nothing shaped
 * like one is there); "unterminated quote" is named explicitly in the rule
 * above as staying FORM even though AT+MTMODES/AT+MTCHIMESOUNDS answer
 * +MTERR:1 for the identical case, which is exactly the inconsistency fix
 * round 1 (task 9 report) normalised everything else to avoid repeating.
 * MT_ERR_BAD_PARAM: content over MT_METERID_MAX_STR bytes, or a
 * non-printable byte (0x20..0x7E only accepted). An embedded raw quote is
 * not caught here directly: it always ends the scan early, and the
 * unintended remainder shows up at the call site as junk before the next
 * required separator, which is where it is rejected.
 */
static int mtmeterid_scan_string(char **pp, const char **start, size_t *len)
{
    char *p = *pp;
    if (*p != '"') {
        return MT_R_ERROR;
    }
    p++;
    char *s = p;
    while (*p != '"') {
        if (*p == '\0') {
            return MT_R_ERROR;  /* unterminated quote: form */
        }
        p++;
    }
    size_t n = (size_t)(p - s);
    p++;  /* past the closing quote */

    if (n > MT_METERID_MAX_STR) {
        return MT_ERR_BAD_PARAM;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) {
            return MT_ERR_BAD_PARAM;
        }
    }

    *start = s;
    *len = n;
    *pp = p;
    return 0;
}

/*
 * AT+MTMETERID=<ep>,<type>,"<pod>","<serial>","<protocol>",<pwr>,<apparent>,<src>
 *   -> push a full MeterIdentification identity onto <ep> in one call
 * (energy round C2, design spec 6.2): MeterType, PointOfDelivery,
 * MeterSerialNumber, ProtocolVersion and PowerThreshold. Set-only: bare/query
 * forms answer a plain ERROR. This is the ONLY way any of these five
 * attributes ever gets a value; see mt_meter_set_identity_locked()'s comment
 * (mt_meter.cpp) for why there is deliberately no matching read-back verb.
 *
 * Hand-parsed off the raw argument string, the AT+MTMODES/AT+MTCHIMESOUNDS
 * reason: a comma inside a quoted field is legal content and at_split_args()
 * would split on it. Its error-code split follows the FORM/VALUE rule
 * documented above attr_err_to_mterr(), which this command helped establish
 * (fix round 1, task 9 report, .superpowers/sdd/2026-08-14-energy-round-c2/
 * task-9-report.md):
 *
 *   - a token genuinely missing (the line ends before a structurally
 *     required comma or quote is found), or an unterminated quote, is a
 *     FORM error (bare ERROR): the shape of the command itself is wrong,
 *     not any one value in it. (Fix round 1 corrected an earlier draft of
 *     this comment, and this function's own behaviour, which had also
 *     treated a present-but-unparseable numeric token as FORM; that was the
 *     exact inconsistency the round-level ruling normalised away, see the
 *     rule comment above attr_err_to_mterr().)
 *   - a token that is present, is correctly delimited, and either fails to
 *     parse as the numeric type it should be OR parses but violates a
 *     range (ep > 0xFFFF, type > 2, src > 2), a string over
 *     MT_METERID_MAX_STR bytes, a non-printable byte, junk after a quoted
 *     field's closing quote (an embedded quote is exactly this: the raw
 *     quote ends the scan early and the intended remainder becomes junk
 *     before the next comma), or both <pwr> and <apparent> absent, is a
 *     VALUE error (+MTERR:1).
 *
 * <type>: MeterTypeEnum, 0 Utility, 1 Private, 2 Generic.
 * "<pod>"/"<serial>"/"<protocol>": quoted strings, printable ASCII
 * (0x20..0x7E) only, 0..MT_METERID_MAX_STR (64) bytes; no raw double-quote
 * character may appear inside one (see "junk after the closing quote"
 * above).
 * <pwr>/<apparent>: the PowerThresholdStruct's powerThreshold/
 * apparentPowerThreshold, signed 64-bit milliwatts. Optional: an empty
 * token (two adjacent commas) means absent, the AT+MTROW empty-field
 * convention (design spec 2.2, "an empty field means absent") extended to
 * this command. At least one of the two must be present (the XML's
 * PowerThresholdStruct "choice b").
 * <src>: PowerThresholdSourceEnum, 0 Contract, 1 Regulator, 2 Equipment, or
 * an empty token for null. The last field: an empty token here is simply
 * nothing left on the line after <apparent>'s comma.
 *
 * Lookup errors, from mt_matter_meter_set_identity() (mt_matter.h):
 * +MTERR:2 unknown endpoint, +MTERR:4 the endpoint has no
 * MeterIdentification cluster (this command's own table, not the generic
 * +MTERR:3 lookup failures elsewhere in this file use: see
 * mt_matter_meter_set_identity()'s comment in main.cpp for why). A bare
 * ERROR covers an unclassified runtime failure, routed through
 * attr_err_to_mterr() like every other bridge call in this file.
 */
static int cmd_mtmeterid(at_type_t type, char *args)
{
    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (!args || *args == '\0') {
        return MT_R_ERROR;  /* every field missing: form */
    }

    char *p = args;

    /* <ep>: bare token up to the comma before <type>. */
    char *comma = strchr(p, ',');
    if (!comma) {
        return MT_R_ERROR;
    }
    *comma = '\0';
    unsigned long ep;
    if (!parse_u(p, &ep)) {
        return MT_ERR_BAD_PARAM;
    }
    if (ep > 0xFFFF) {
        return MT_ERR_BAD_PARAM;
    }
    p = comma + 1;

    /* <type>: bare token up to the comma before "<pod>". */
    comma = strchr(p, ',');
    if (!comma) {
        return MT_R_ERROR;
    }
    *comma = '\0';
    unsigned long mtype;
    if (!parse_u(p, &mtype)) {
        return MT_ERR_BAD_PARAM;
    }
    if (mtype > 2) {
        return MT_ERR_BAD_PARAM;
    }
    p = comma + 1;

    /*
     * "<pod>", "<serial>", "<protocol>": three quoted strings, each
     * followed by a comma except the last, which is followed by <pwr>'s
     * own leading digit (or comma, if <pwr> is absent). Copied into
     * NUL-terminated local buffers rather than passed as raw spans: the
     * bridge's mt_meter_identity_t (mt_matter.h) takes plain C strings, the
     * same shape mt_matter_modes_set()'s labels use.
     */
    char pod_buf[MT_METERID_MAX_STR + 1];
    char serial_buf[MT_METERID_MAX_STR + 1];
    char protocol_buf[MT_METERID_MAX_STR + 1];
    char *bufs[3] = { pod_buf, serial_buf, protocol_buf };

    for (int i = 0; i < 3; i++) {
        const char *s;
        size_t n;
        int r = mtmeterid_scan_string(&p, &s, &n);
        if (r != 0) {
            return r;
        }
        memcpy(bufs[i], s, n);
        bufs[i][n] = '\0';

        if (*p == '\0') {
            return MT_R_ERROR;  /* the remaining fields are missing: form */
        }
        if (*p != ',') {
            return MT_ERR_BAD_PARAM;  /* junk after the closing quote, e.g. an embedded quote */
        }
        p++;
    }

    /* <pwr>: optional int64, an empty token means absent. */
    comma = strchr(p, ',');
    if (!comma) {
        return MT_R_ERROR;  /* <apparent>/<src> missing entirely: form */
    }
    *comma = '\0';
    bool pwr_present = (*p != '\0');
    int64_t pwr = 0;
    if (pwr_present) {
        if (!parse_i64(p, true, &pwr)) {
            return MT_ERR_BAD_PARAM;
        }
    }
    p = comma + 1;

    /* <apparent>: same shape. */
    comma = strchr(p, ',');
    if (!comma) {
        return MT_R_ERROR;  /* <src> missing entirely: form */
    }
    *comma = '\0';
    bool apparent_present = (*p != '\0');
    int64_t apparent = 0;
    if (apparent_present) {
        if (!parse_i64(p, true, &apparent)) {
            return MT_ERR_BAD_PARAM;
        }
    }
    p = comma + 1;

    if (!pwr_present && !apparent_present) {
        return MT_ERR_BAD_PARAM;  /* choice b: at least one is required */
    }

    /* <src>: the last field, no trailing comma. An empty token means null. */
    bool src_present = (*p != '\0');
    unsigned long src = 0;
    if (src_present) {
        if (!parse_u(p, &src)) {
            return MT_ERR_BAD_PARAM;
        }
        if (src > 2) {
            return MT_ERR_BAD_PARAM;
        }
    }

    mt_meter_identity_t id = {
        .meter_type      = (uint8_t)mtype,
        .pod             = pod_buf,
        .serial          = serial_buf,
        .protocol        = protocol_buf,
        .pwr_present     = pwr_present,
        .pwr             = pwr,
        .apparent_present = apparent_present,
        .apparent        = apparent,
        .src_present     = src_present,
        .src             = (uint8_t)src,
    };

    int r = mt_matter_meter_set_identity((uint16_t)ep, &id);
    if (r != MT_ATTR_OK) {
        return attr_err_to_mterr(r);
    }
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
    { "MTTHREAD",     cmd_mtthread    },
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
    { "MTMEAS",       cmd_mtmeas      },
    { "MTDEMCAP",     cmd_mtdemcap    },
    { "MTROW",        cmd_mtrow       },
    { "MTROWCLEAR",   cmd_mtrowclear  },
    { "MTROWAPPLY",   cmd_mtrowapply  },
    { "MTROWGET",     cmd_mtrowget    },
    { "MTMETERID",    cmd_mtmeterid   },
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
