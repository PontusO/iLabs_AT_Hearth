/*
 * mt_err.h - the +MTERR:<n> code space (AT_MT_SPEC.md section 5), shared by
 * every command module that returns one. mt_at.c owned these privately until
 * the OTA module (mt_ota.c) needed the same numbers; the mapping onto the
 * engine's err_generic / err_unsupported config stays in mt_at.c.
 */
#pragma once

#define MT_ERR_BAD_PARAM     1   /* bad parameter or out of range          */
#define MT_ERR_NO_ENDPOINT   2   /* unknown endpoint                       */
#define MT_ERR_NO_CLUSTER    3   /* unknown cluster                        */
#define MT_ERR_NO_ATTRIBUTE  4   /* unknown attribute                      */
#define MT_ERR_ATTR_TYPE     5   /* attribute type unsupported here        */
#define MT_ERR_DEVTYPE       6   /* unknown or unsupported device type     */
#define MT_ERR_PERSIST       7   /* NVS persistence failure                */
#define MT_ERR_UNSUPPORTED   8   /* unknown/unsupported command            */
#define MT_ERR_NOT_READY     9   /* no composition, or stack not started   */
#define MT_ERR_COMP_REJECT   10  /* nothing staged, or endpoint cap hit    */
#define MT_ERR_ATTR_READONLY 11  /* served by a cluster Instance (DE270)   */
#define MT_ERR_OTA_STATE     12  /* OTA command not valid in this OTA state */
#define MT_ERR_GENERIC       100 /* plain ERROR, no +MTERR line            */
#define MT_R_ERROR           MT_ERR_GENERIC
