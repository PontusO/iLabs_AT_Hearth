/*
 * mt_at_config.h
 *
 * Matter AT app configuration, layered on the shared AT-core transport
 * config (at_core_config.h: UART port, pins, baud, flow control, buffers).
 * Mirror of the ESP-NOW firmware's en_at_config.h for the Matter image.
 *
 * Target: ESP32-C6 only.
 */

#pragma once

/* Shared transport config (AT_UART_*, AT_TARGET_ESP8266, ...). */
#include "at_core_config.h"

/* ------------------------------------------------------------------ */
/*  Firmware identity                                                  */
/* ------------------------------------------------------------------ */

#define MT_FW_VERSION       "0.9.0"
#define MT_MANUFACTURER     "iLabs Electronics"
#define MT_MODEL            "ESP32-C6 Hearth"

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
/*  AT link hardware flow control availability                         */
/* ------------------------------------------------------------------ */

/*
 * Whether this board actually wires the AT UART's RTS and CTS signals
 * between the host and the C6. at_core can drive them (at_uart_set_flowctrl,
 * AT_UART_RTS_PIN/AT_UART_CTS_PIN = GPIO19/GPIO18 on C6), and the ESP-NOW
 * firmware exposes that through AT+ENFLOW, but no C6 board built so far
 * routes the pair:
 *
 *   Challenger RP2350 WiFi6/BLE5 (V0.2) and Slice RP2350 WiFi6 both use the
 *   ESP32-C6-MINI-1-H4 with only RXD0/TXD0, IO2/IO3/IO7/IO14/IO15 (the
 *   esp-hosted SPI), IO9 (boot), IO8 (strapping), IO12/IO13 (USB D-/D+) and
 *   EN. IO18 and IO19 go nowhere. The AT link is three-wire.
 *
 * The pair IS routed on the older ESP32-C3 Connectivity board (IO18 -> CTS,
 * IO19 -> RTS), which is where at_core's pin assignment comes from, but
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

#include "sdkconfig.h"

/*
 * True only on the combined WiFi+Thread image (sdkconfig.defaults.combined).
 * Gates AT+MTTRANSPORT: a single-transport image never registers that
 * command, so it answers the ordinary "unknown command" +MTERR:8 there
 * rather than existing as a no-op that always reports one fixed transport.
 *
 * CONFIG_ENABLE_WIFI_STATION and CONFIG_OPENTHREAD_ENABLED were checked
 * against the three generated sdkconfigs before picking this pair (see
 * task-3-report.md): the WiFi-only build sets ENABLE_WIFI_STATION alone,
 * the Thread-only build sets OPENTHREAD_ENABLED alone (sdkconfig.defaults.
 * thread explicitly turns ENABLE_WIFI_STATION off), and the combined build
 * is the only one of the three carrying both.
 */
#if defined(CONFIG_ENABLE_WIFI_STATION) && defined(CONFIG_OPENTHREAD_ENABLED)
#define MT_COMBINED_IMAGE 1
#else
#define MT_COMBINED_IMAGE 0
#endif
