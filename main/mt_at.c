/*
 * mt_at.c - Matter AT command handlers and registration (skeleton).
 *
 * Phase B1: this proves the shared at_core engine drives a second AT
 * personality with its own "+MTERR" code space, before any Matter code is
 * added. It mirrors the ESP-NOW firmware's en_at.c. The real AT+MT...
 * lifecycle/commissioning/data-model commands land in Phase B4.
 *
 * Handler return convention (see at_parser.h):
 *   AT_R_OK    - engine prints "OK"
 *   AT_R_DONE  - handler already printed its own terminal response
 *   1..99      - engine prints "+MTERR:<n>" then "ERROR"
 *   MT_R_ERROR - engine prints plain "ERROR"
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "at_uart.h"
#include "at_parser.h"
#include "mt_at_config.h"
#include "mt_at.h"
#include "mt_matter.h"

/* +MTERR:<n> code space (mirrors the ESP-NOW layout: 1..99 carry a code,
 * >= MT_ERR_GENERIC is a plain "ERROR"). Real codes are assigned in B4. */
#define MT_ERR_UNSUPPORTED  8   /* unknown/unsupported command */
#define MT_ERR_GENERIC      100 /* plain ERROR, no +MTERR line  */
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
 * Default 300 s; the +MTCOMMISSION:STARTED/COMPLETE/FAILED URCs report progress
 * from the platform event callback.
 */
static int cmd_mtcommission(at_type_t type, char *args)
{
    unsigned timeout = 300;
    if (type == AT_SET) {
        if (!at_parse_uint(args, &timeout) || timeout < 30 || timeout > 900) {
            return MT_R_ERROR;
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

/* AT+MTRESET -> factory reset (erase Matter data) and reboot. */
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

/*
 * AT+MTATTR=<ep>,<cluster>,<attr>       -> read  -> +MTATTR:<ep>,<cluster>,<attr>,<val>
 * AT+MTATTR=<ep>,<cluster>,<attr>,<val> -> write -> OK
 *
 * <cluster>/<attr> accept hex (0x0006) or decimal; <val> is an integer. A
 * controller-driven change to the light endpoint is reported asynchronously as
 * a +MTATTR URC (from the attribute callback in main.cpp).
 */
static int cmd_mtattr(at_type_t type, char *args)
{
    char *f[4];
    int n = at_split_args(args, f, 4);
    if (type != AT_SET || n < 3) {
        return MT_R_ERROR;
    }

    unsigned long ep, cluster, attr;
    if (!parse_u(f[0], &ep) || !parse_u(f[1], &cluster) || !parse_u(f[2], &attr)) {
        return MT_R_ERROR;
    }

    if (n == 3) {
        long v;
        if (mt_matter_attr_read((uint16_t)ep, (uint32_t)cluster, (uint32_t)attr, &v) != 0) {
            return MT_R_ERROR;
        }
        at_uart_write_line("+MTATTR:%lu,%lu,%lu,%ld", ep, cluster, attr, v);
        return AT_R_OK;
    }

    unsigned long val;
    if (!parse_u(f[3], &val)) {
        return MT_R_ERROR;
    }
    if (mt_matter_attr_write((uint16_t)ep, (uint32_t)cluster, (uint32_t)attr, (long)val) != 0) {
        return MT_R_ERROR;
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
    { "MTATTR",       cmd_mtattr      },
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
    s_at_up = true;

    /* Boot marker so the host can synchronize after a reset (mirrors +ENREADY). */
    at_uart_write_line("+MTREADY");
}

void mt_at_urc(const char *line)
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
        return;
    }
    at_uart_write_line("%s", line);
}
