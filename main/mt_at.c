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

/* Longest +MTCMD/+MTCMDTO line: "+MTCMD:" + up to three u32 fields (10
 * digits each) plus the u16 ep (5 digits) and three commas: 7 + 10 + 1 + 5 +
 * 1 + 10 + 1 + 10 = 45, plus the terminating NUL. 56 leaves the same kind of
 * headroom MT_EVT_LINE_MAX does for +MTEVT, again off the CHIP task's stack
 * rather than the 512-byte MT_AT_LINE_MAX buffer. */
#define MT_CMD_LINE_MAX 56

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
 * AT+MTEP?                    -> +MTEP:<index>,<endpoint_id>,<device_type>[,<variant>]
 *                                per endpoint; the 4th field appears only
 *                                when the variant is nonzero, so an existing
 *                                host that has never seen a variant reads
 *                                byte-identical output.
 * AT+MTEP=<devtype>[,<variant>] -> append one endpoint to the staged
 *                                composition. <variant> defaults to 0 and
 *                                must be legal for <devtype>
 *                                (mt_devtype_variant_ok()); an unknown
 *                                <devtype> answers +MTERR:6, a known
 *                                <devtype> with an illegal <variant>
 *                                answers +MTERR:1.
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
            if (mt_matter_endpoint_info(i, &devtype, &ep_id, &variant) == 0) {
                if (variant != 0) {
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

    char *f[2];
    int n = at_split_args(args, f, 2);
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
    if (n == 2 && !parse_u(f[1], &variant)) {
        return MT_ERR_BAD_PARAM;
    }
    if (variant > 0xFF || !mt_devtype_variant_ok((uint32_t)devtype, (uint8_t)variant)) {
        return MT_ERR_BAD_PARAM;
    }

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

bool mt_cmd_forward(uint16_t ep, uint32_t cluster, uint32_t command)
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
    snprintf(line, sizeof(line), "+MTCMD:%lu,%u,%lu,%lu",
             (unsigned long)seq, ep, (unsigned long)cluster, (unsigned long)command);
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
