/*
 * mt_matter_stub.c - every mt_matter.h entry point, stubbed. The linker
 * proves completeness: an unimplemented declaration fails the build.
 * Replaced by the real Nordic Matter bridge in its own round.
 *
 * Return convention (per the header's own comments, mechanically applied):
 *   - int with a 0-on-success convention: -1
 *   - mt_attr_result_t: the endpoint-lookup failure named in the comment
 *     (MT_ATTR_ERR_ENDPOINT for anything keyed by an endpoint id, since this
 *     skeleton presents no endpoints at all; MT_ATTR_ERR_CLUSTER where the
 *     comment names it explicitly, e.g. Thread info on a build that never
 *     speaks Thread)
 *   - MT_ROW_* (mt_rows.h): MT_ROW_ERR_ENDPOINT, the same "no endpoints"
 *     reasoning as mt_attr_result_t
 *   - count/size: 0
 *   - bool: false
 *   - pointer: NULL
 *   - void: empty
 *   - mt_matter_state(): MT_STATE_UNINIT
 * Every out-parameter is defensively zeroed (after a NULL check) before the
 * failure return, so a core caller that trusts a 0-filled struct on error
 * never reads uninitialized memory.
 *
 * The commissioning/state/network slice (mt_matter_state,
 * mt_matter_fabric_count, mt_matter_open_commissioning,
 * mt_matter_onboarding_codes, mt_matter_factory_reset, mt_matter_net_info,
 * mt_matter_transport_mismatch, mt_matter_thread_info, mt_thread_role_name)
 * now lives in mt_matter_zephyr.cpp against the real CHIP stack, and so
 * does the live endpoint table (mt_matter_endpoint_count,
 * mt_matter_endpoint_info, mt_matter_record_endpoint).
 *
 * The mt_devtypes.h quartet used to sit at the end of this file with
 * accept-all predicates, so the composition pipeline could be exercised
 * before any device type existed. mt_devtypes_zephyr.cpp implements them
 * for real now; AT+MTEP rejects an unknown device type again.
 *
 * mt_matter_attr_read/mt_matter_attr_write (Task 5) now live in
 * mt_matter_zephyr.cpp too, against the ember external-attribute path
 * (emberAfReadAttribute/emberAfWriteAttribute), alongside the strong
 * MatterPostAttributeChangeCallback() override that turns a changed
 * attribute into a +MTATTR URC.
 *
 * Catalogue batch 3 took the door lock and water valve slice out of here
 * for the same reason: mt_matter_lock_state_set, mt_matter_lock_source_
 * manual, mt_matter_lock_source_max, mt_matter_valve_delegate_alloc,
 * mt_matter_valve_delegate_set_endpoint and mt_matter_valve_state_set are
 * real in mt_matter_zephyr.cpp now, against DoorLockServer and the
 * ValveConfigurationAndControl delegate, next to the +MTCMD command
 * forwards those two device types need.
 */

#include <stddef.h>
#include <string.h>

#include "mt_matter.h"

/* mt_matter_switch_click() left this file in catalogue batch 5, when the
 * generic switch (0x000F) entered the registry: real in
 * mt_matter_zephyr.cpp now, on the stateless SwitchServer singleton. */

int mt_matter_temp_levels_set(uint16_t ep, const char *const *labels, uint8_t count)
{
    (void)ep;
    (void)labels;
    (void)count;
    return MT_ATTR_ERR_ENDPOINT;
}

/* mt_matter_mode_select_manager() and mt_matter_modes_set() left this file
 * in catalogue batch 4, when the mode select (0x0027) entered the
 * registry: real in mt_matter_zephyr.cpp now, on the one process-global
 * SupportedModesManager. */

void *mt_matter_modebase_delegate_alloc(uint32_t cluster_id)
{
    (void)cluster_id;
    return NULL;
}

void mt_matter_modebase_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)delegate;
    (void)ep;
}

int mt_matter_modebase_set(uint16_t ep, uint32_t cluster, const uint8_t *modes, const uint16_t *tags,
                            const char *const *labels, uint8_t count)
{
    (void)ep;
    (void)cluster;
    (void)modes;
    (void)tags;
    (void)labels;
    (void)count;
    return MT_ATTR_ERR_ENDPOINT;
}

/* The plain OperationalState slice (mt_matter_opstate_delegate_alloc,
 * mt_matter_opstate_delegate_set_endpoint, mt_matter_opstate_set) left
 * this file in catalogue batch 4, when the washer/dishwasher/dryer trio
 * entered the registry: real in mt_matter_zephyr.cpp now, on a
 * per-endpoint Instance-plus-Delegate pool. The RVC variant below is
 * still a stub. */

void *mt_matter_rvc_opstate_delegate_alloc(void) { return NULL; }

void mt_matter_rvc_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)delegate;
    (void)ep;
}

/* mt_air_quality_feature_mask() left this file in catalogue batch 2, when
 * the air quality sensor (0x002C) entered the registry: it lives in
 * mt_matter_zephyr.cpp now and returns the real bits. */

/* mt_matter_alarm_set() left this file in catalogue batch 4, when the
 * smoke/co alarm (0x0076) entered the registry: it lives in
 * mt_matter_zephyr.cpp now, cluster-dispatched on the SmokeCoAlarmServer
 * singleton with the B165 ExpressedState recompute. */

/* The chime slice (mt_matter_chime_delegate_alloc,
 * mt_matter_chime_delegate_set_endpoint, mt_matter_chime_sounds_set,
 * mt_matter_chime_set) left this file in catalogue batch 4, when the
 * chime (0x0146) entered the registry: real in mt_matter_zephyr.cpp now,
 * on a per-endpoint ChimeServer-plus-delegate pool. */

void *mt_matter_mwoc_delegate_alloc(void) { return NULL; }

void mt_matter_mwoc_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)delegate;
    (void)ep;
}

int mt_matter_meas_set(uint16_t ep, uint32_t cluster, const uint8_t *fields,
                       const int64_t *values, uint8_t count)
{
    (void)ep;
    (void)cluster;
    (void)fields;
    (void)values;
    (void)count;
    return MT_ATTR_ERR_ENDPOINT;
}

void *mt_matter_epm_delegate_alloc(void) { return NULL; }
void *mt_matter_ptop_delegate_alloc(void) { return NULL; }

void mt_matter_meas_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)delegate;
    (void)ep;
}

void *mt_matter_whm_delegate_alloc(uint16_t ep)
{
    (void)ep;
    return NULL;
}

void *mt_matter_dem_delegate_alloc(uint16_t ep)
{
    (void)ep;
    return NULL;
}

int mt_matter_demcap_set(uint16_t ep, uint8_t cause, uint8_t n, const int64_t *quads)
{
    (void)ep;
    (void)cause;
    (void)n;
    (void)quads;
    return MT_ATTR_ERR_ENDPOINT;
}

int mt_matter_rows_apply(uint16_t ep, uint8_t kind, const mt_row_stage_t *stage)
{
    (void)ep;
    (void)kind;
    (void)stage;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_rows_get(uint16_t ep, uint8_t kind, uint16_t idx,
                       mt_row_t *out, uint16_t *total)
{
    (void)ep;
    (void)kind;
    (void)idx;
    if (out) memset(out, 0, sizeof(*out));
    if (total) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_rows_total(uint16_t ep, uint8_t kind, uint16_t *total)
{
    (void)ep;
    (void)kind;
    if (total) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

uint32_t mt_meter_feature_mask(void) { return 0; }
bool mt_meter_reserve(void) { return false; }
void mt_meter_register_all(void) { }

int mt_matter_meter_set_identity(uint16_t ep, const mt_meter_identity_t *id)
{
    (void)ep;
    (void)id;
    return MT_ATTR_ERR_ENDPOINT;
}

void *mt_matter_evse_delegate_alloc(uint16_t ep)
{
    (void)ep;
    return NULL;
}

bool mt_matter_evse_reserve(void) { return false; }

int mt_matter_evse_set(uint16_t ep, uint8_t field, int64_t value)
{
    (void)ep;
    (void)field;
    (void)value;
    return MT_ATTR_ERR_ENDPOINT;
}

int mt_matter_evse_targets_apply(uint16_t ep, const mt_row_stage_t *stage)
{
    (void)ep;
    (void)stage;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_evse_targets_get(uint16_t ep, uint16_t idx, mt_row_t *out, uint16_t *total)
{
    (void)ep;
    (void)idx;
    if (out) memset(out, 0, sizeof(*out));
    if (total) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_evse_targets_total(uint16_t ep, uint16_t *total)
{
    (void)ep;
    if (total) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_evse_targets_erase_all(void) { /* Erasing schedules that cannot exist succeeds vacuously; a -1 here
     * blocked AT+MTFRESET's completion during bring-up (bench-found). */
    return 0; }
