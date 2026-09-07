/*
 * mt_ota.h - the AT+MT side of firmware over the air (spec
 * 2026-09-07-fota-design, sections 5 and 6). The platform owns the CHIP OTA
 * Requestor and calls down into this module from the Matter thread; the AT
 * parser task calls the command handlers. The only shared state is the one
 * pending block, guarded by HEARTH_CRIT_OTA.
 *
 * Wire: AT+MTSWVER, AT+MTOTA, AT+MTOTAGET, AT+MTOTAACK, AT+MTOTASTAGED;
 * URCs +MTOTA:<STATE>[,<detail>] and +MTOTA:BLOCK,<seq>,<len>; the block
 * itself is a command response (+MTOTABLK:<seq>,<off>,<hex>, 96 bytes a
 * line) so no line ever exceeds 254 bytes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "at_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MT_OTA_IDLE = 0,
    MT_OTA_QUERYING,
    MT_OTA_AVAILABLE,
    MT_OTA_DOWNLOADING,
    MT_OTA_DOWNLOADED,
    MT_OTA_APPLYING,
    MT_OTA_DEFERRED,
    MT_OTA_DISCONTINUED,
    MT_OTA_ERROR,
} mt_ota_state_t;

#define MT_OTA_CHUNK          96u   /* payload bytes per +MTOTABLK line       */
#define MT_OTA_SWVER_STR_MAX  32u   /* product version string incl. the NUL   */

/* Reset the module to the boot state (mode 0, IDLE, no pending block).
 * Call from mt_at_start() before at_register_commands(). */
void mt_ota_init(void);

/* The command table for at_register_commands(); *count receives its length. */
const at_command_t *mt_ota_commands(size_t *count);

/* 0 disabled (boot), 1 enabled. */
int mt_ota_mode(void);

const char *mt_ota_state_name(mt_ota_state_t s);

/* ---- platform -> core, Matter thread ------------------------------------ */

/* A requestor state change. <detail> may be NULL. Emits +MTOTA:<STATE>[,detail]
 * when the mode is 1; IDLE, ERROR and DISCONTINUED also drop a pending block. */
void mt_ota_on_state(mt_ota_state_t state, const char *detail);

/* A block arrived. <data> must stay valid and unchanged until
 * mt_matter_ota_block_acked(seq) returns or mt_ota_block_timeout() is called.
 * Returns 0 (announced as +MTOTA:BLOCK,<seq>,<len>) or -1 (mode 0, a block
 * already pending, or len 0 / data NULL): the caller must then abort. */
int mt_ota_on_block(uint32_t seq, const uint8_t *data, size_t len);

/* The platform's acknowledgement timer fired: forget the pending block. The
 * platform aborts the download after calling this. */
void mt_ota_block_timeout(void);

/* The provider said Proceed: emit +MTOTA:APPLY and enter APPLYING. */
void mt_ota_on_apply(void);

/* ---- boot-time read for the platform's ConfigurationManager ------------ */

/* The persisted product version. Returns 0 with both outputs filled, 1 when
 * the host has never declared one (outputs untouched), -1 on a storage error.
 * str_len must be at least MT_OTA_SWVER_STR_MAX. */
int mt_ota_swver_stored(uint32_t *version, char *str, size_t str_len);

#ifdef __cplusplus
}
#endif
