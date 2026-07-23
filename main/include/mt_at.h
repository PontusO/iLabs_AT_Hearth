/*
 * mt_at.h - Matter AT command surface for the shared at_core engine.
 */

#pragma once

#include "at_parser.h"

/* Engine config for the Matter personality (error prefix "+MTERR", code
 * space, line length, parser task tuning). Passed to at_parser_start(). */
extern const at_engine_cfg_t mt_at_engine_cfg;

/* Register the AT+MT... command table with the engine. Call before
 * at_parser_start(). */
void mt_at_register(void);
