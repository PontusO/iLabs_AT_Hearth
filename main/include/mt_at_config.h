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

#define MT_FW_VERSION       "0.1.0"
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
