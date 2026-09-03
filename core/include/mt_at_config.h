/*
 * mt_at_config.h
 *
 * Matter AT app configuration. Mirror of the ESP-NOW firmware's
 * en_at_config.h for the Matter image.
 *
 * Portable: no platform header. Platform-specific transport details (UART
 * port, pins, buffers) live in the platform's own at_core_config.h, next to
 * the port implementation that actually reads them.
 */

#pragma once

/* ------------------------------------------------------------------ */
/*  Firmware identity                                                  */
/* ------------------------------------------------------------------ */

#define MT_FW_VERSION       "1.1.0"
#define MT_MANUFACTURER     "iLabs Electronics"
/* The model string is NOT here: it names the co-processor, so it is a
 * platform fact and each port answers its own via hearth_port_model()
 * (hearth_port.h). A shared constant used to read "ESP32-C6 Hearth" on
 * every build, which was wrong on the nRF (bug fixed for 1.1.0). */

/* ------------------------------------------------------------------ */
/*  AT engine tunables                                                 */
/* ------------------------------------------------------------------ */

/* Max accepted AT command line length (bytes, incl. the "AT+" prefix). */
#define MT_AT_LINE_MAX      512

/* Command echo on boot (change at runtime with ATE0 / ATE1). */
#define MT_AT_ECHO_DEFAULT  0

/* Parser task tuning. */
#define MT_PARSER_TASK_STACK 6144
#define MT_PARSER_TASK_PRIO  10

/* ------------------------------------------------------------------ */
/*  AT+MTFLOW wire values                                              */
/* ------------------------------------------------------------------ */

/*
 * AT+MTFLOW's <mode> values: a wire-protocol enum shared with the ESP-NOW
 * firmware's AT+ENFLOW, not tied to any platform's flow-control API.
 * Formerly re-used from the ESP link's at_core_config.h; defined here
 * directly now that mt_at.c takes no platform header.
 */
#define AT_UART_FLOWCTRL_NONE     0
#define AT_UART_FLOWCTRL_RTS      1
#define AT_UART_FLOWCTRL_CTS      2
#define AT_UART_FLOWCTRL_CTS_RTS  3

/* ------------------------------------------------------------------ */
/*  AT link hardware flow control availability                         */
/* ------------------------------------------------------------------ */

/*
 * Whether this board actually wires the AT UART's RTS and CTS signals
 * between the host and the C6. The ESP link can drive them
 * (hearth_link_set_flowctrl(), AT_UART_RTS_PIN/AT_UART_CTS_PIN =
 * GPIO19/GPIO18 on C6, see the platform's at_core_config.h), and the
 * ESP-NOW firmware exposes that through AT+ENFLOW, but no C6 board built so
 * far routes the pair:
 *
 *   Challenger RP2350 WiFi6/BLE5 (V0.2) and Slice RP2350 WiFi6 both use the
 *   ESP32-C6-MINI-1-H4 with only RXD0/TXD0, IO2/IO3/IO7/IO14/IO15 (the
 *   esp-hosted SPI), IO9 (boot), IO8 (strapping), IO12/IO13 (USB D-/D+) and
 *   EN. IO18 and IO19 go nowhere. The AT link is three-wire.
 *
 * The pair IS routed on the older ESP32-C3 Connectivity board (IO18 -> CTS,
 * IO19 -> RTS), which is where the ESP link's pin assignment comes from, but
 * Hearth is C6-only.
 *
 * With this at 0, AT+MTFLOW answers a query with 0 and rejects any attempt
 * to enable a mode. Enabling CTS against an unbonded pin does not degrade
 * gracefully: it gates the C6's transmitter on a floating input and the link
 * stops until the next reset. Set to 1 on a board revision that routes them.
 */
#define MT_UART_FLOWCTRL_WIRED  0

/* ------------------------------------------------------------------ */
/*  Combined-image detection                                           */
/* ------------------------------------------------------------------ */

/*
 * True only on the combined WiFi+Thread image (sdkconfig.defaults.combined).
 * Gates AT+MTTRANSPORT: a single-transport image never registers that
 * command, so it answers the ordinary "unknown command" +MTERR:8 there
 * rather than existing as a no-op that always reports one fixed transport.
 *
 * Derived from sdkconfig, not readable from a portable header: the ESP
 * platform's build (platform/esp32c6/hearth_core/CMakeLists.txt) computes
 * it from CONFIG_ENABLE_WIFI_STATION/CONFIG_OPENTHREAD_ENABLED (see
 * task-3-report.md for why that pair) and injects it as a PUBLIC compile
 * definition, so main.cpp (a user of the macro) inherits it too. Any
 * platform that wraps core/ must define MT_COMBINED_IMAGE the same way.
 */
#ifndef MT_COMBINED_IMAGE
#error "MT_COMBINED_IMAGE must be defined by the platform build (0 or 1)"
#endif
