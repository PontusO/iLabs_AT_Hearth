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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"

#include "at_uart.h"
#include "at_parser.h"
#include "mt_at_config.h"
#include "mt_at.h"
#include "mt_comp_store.h"
#include "mt_composition.h"
#include "mt_devtypes.h"
#include "mt_matter.h"

/* Longest +MTEVT line: "+MTEVT:31," plus a detail. Details are "0"/"1" today
 * (bits 10, 11 and 24); 48 leaves room without putting a 512-byte
 * MT_AT_LINE_MAX buffer on the CHIP task's stack. Truncation is safe. */
#define MT_EVT_LINE_MAX 48

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
 * AT+MTEP?            -> +MTEP:<index>,<endpoint_id>,<device_type> per endpoint
 * AT+MTEP=<devtype>   -> append one endpoint to the staged composition
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
            if (mt_matter_endpoint_info(i, &devtype, &ep_id) == 0) {
                at_uart_write_line("+MTEP:%u,%u,0x%04lX", i, ep_id, (unsigned long)devtype);
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

    unsigned long devtype;
    if (!parse_u(args, &devtype)) {
        return MT_ERR_BAD_PARAM;
    }
    if (!mt_devtype_is_known((uint32_t)devtype)) {
        return MT_ERR_DEVTYPE;
    }

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
    { "MTEP",         cmd_mtep        },
    { "MTEPCLEAR",    cmd_mtepclear   },
    { "MTEPAPPLY",    cmd_mtepapply   },
    { "MTEVT",        cmd_mtevt       },
    { "MTNET",        cmd_mtnet       },
    { "MTBAUD",       cmd_mtbaud      },
    { "MTFLOW",       cmd_mtflow      },
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
/* Set once the AT UART (and its TX mutex) exist. See mt_at_urc(). */
static volatile bool s_at_up = false;

void mt_at_start(void)
{
    at_uart_init();
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
