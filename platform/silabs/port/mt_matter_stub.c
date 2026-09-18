/*
 * mt_matter_stub.c - every mt_matter.h entry point, stubbed, for the Silabs
 * skeleton. The linker proves completeness: an unimplemented declaration
 * fails the build. Each stub is replaced by the real implementation in the
 * upward-port round's batches; a stub that survives a batch is a stub the
 * batch forgot, which is what the "linker proves it" rule is for.
 *
 * Return convention, mechanically from the header comments: int with a
 * 0-on-success convention -> -1; mt_attr_result_t -> MT_ATTR_ERR_ENDPOINT
 * (no endpoints exist here) or MT_ATTR_ERR_CLUSTER where the comment names
 * it; MT_ROW_* -> MT_ROW_ERR_ENDPOINT; counts 0; bool false; pointer NULL;
 * void empty. Every out-parameter is zeroed (after a NULL check) before the
 * failure return.
 *
 * Round 2 task 4 retired the first nine: the commissioning state, network
 * and Thread answers come from the running stack in port/mt_matter_sl.cpp
 * now. What is left below is everything the data model has to exist for.
 */

#include <stddef.h>
#include <string.h>

#include "mt_matter.h"

/* ---- commissioning state and identity ----------------------------------- */
/* ---- network transport (C3) -------------------------------------------- */
/* ---- Thread role and mesh identity -------------------------------------- */

/* Round 2 task 4 moved all three sections to port/mt_matter_sl.cpp, which
 * answers them from the running Matter stack: mt_matter_state,
 * mt_matter_fabric_count, mt_matter_open_commissioning,
 * mt_matter_onboarding_codes, mt_matter_factory_reset, mt_matter_net_info,
 * mt_matter_transport_mismatch, mt_matter_thread_info and
 * mt_thread_role_name. The banners stay so the next section to move can be
 * found by the same name in both files. */

/* ---- live composition ---------------------------------------------------- */

uint16_t mt_matter_endpoint_count(void) { return 0; }

int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id, uint8_t *variant,
                            uint8_t *parent_idx)
{
    (void)index;
    if (devtype != NULL) *devtype = 0;
    if (ep_id != NULL) *ep_id = 0;
    if (variant != NULL) *variant = 0;
    if (parent_idx != NULL) *parent_idx = 0;
    return -1;
}

void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id, uint8_t variant, uint8_t parent_idx)
{
    (void)devtype;
    (void)ep_id;
    (void)variant;
    (void)parent_idx;
}

/* ---- attribute read/write ------------------------------------------------ */

int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                        bool *is_unsigned)
{
    (void)ep; (void)cluster; (void)attr;
    if (out != NULL) *out = 0;
    if (is_unsigned != NULL) *is_unsigned = false;
    return MT_ATTR_ERR_ENDPOINT;
}

int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t val, bool notify)
{
    (void)ep;
    (void)cluster;
    (void)attr;
    (void)val;
    (void)notify;
    return MT_ATTR_ERR_ENDPOINT;
}

int mt_matter_switch_click(uint16_t ep) { (void)ep; return MT_ATTR_ERR_ENDPOINT; }

/* ---- temperature level labels (C3) --------------------------------------- */

int mt_matter_temp_levels_set(uint16_t ep, const char *const *labels, uint8_t count)
{
    (void)ep;
    (void)labels;
    (void)count;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- door lock (C2) -------------------------------------------------------- */

int mt_matter_lock_state_set(uint16_t ep, uint8_t state, uint8_t source)
{
    (void)ep;
    (void)state;
    (void)source;
    return MT_ATTR_ERR_ENDPOINT;
}

uint8_t mt_matter_lock_source_manual(void) { return 0; }

uint8_t mt_matter_lock_source_max(void) { return 0; }

/* ---- water valve ----------------------------------------------------------- */

void *mt_matter_valve_delegate_alloc(void) { return NULL; }
void mt_matter_valve_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

int mt_matter_valve_state_set(uint16_t ep, uint8_t state, int level)
{
    (void)ep;
    (void)state;
    (void)level;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- mode select ------------------------------------------------------------ */

void *mt_matter_mode_select_manager(void) { return NULL; }

int mt_matter_modes_set(uint16_t ep, const uint8_t *modes, const char *const *labels, uint8_t count)
{
    (void)ep;
    (void)modes;
    (void)labels;
    (void)count;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- ModeBase: RVC run/clean mode, microwave mode, and the rest ------------- */

void *mt_matter_modebase_delegate_alloc(uint32_t cluster_id) { (void)cluster_id; return NULL; }
void mt_matter_modebase_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

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

/* ---- OperationalState trio -------------------------------------------------- */

void *mt_matter_opstate_delegate_alloc(uint32_t cluster_id) { (void)cluster_id; return NULL; }
void mt_matter_opstate_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

int mt_matter_opstate_set(uint16_t ep, uint8_t state)
{
    (void)ep;
    (void)state;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- RVC OperationalState ---------------------------------------------------- */

void *mt_matter_rvc_opstate_delegate_alloc(void) { return NULL; }
void mt_matter_rvc_opstate_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

/* ---- air quality (C1b, bug B139) --------------------------------------------- */

uint32_t mt_air_quality_feature_mask(void) { return 0; }

/* ---- smoke/co alarm + refrigerator alarm ------------------------------------- */

int mt_matter_alarm_set(uint16_t ep, uint8_t field, uint8_t value)
{
    (void)ep;
    (void)field;
    (void)value;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- chime -------------------------------------------------------------------- */

void *mt_matter_chime_delegate_alloc(void) { return NULL; }
void mt_matter_chime_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

int mt_matter_chime_sounds_set(uint16_t ep, const uint8_t *ids, const char *const *names, uint8_t count)
{
    (void)ep;
    (void)ids;
    (void)names;
    (void)count;
    return MT_ATTR_ERR_ENDPOINT;
}

int mt_matter_chime_set(uint16_t ep, uint8_t what, uint8_t value)
{
    (void)ep;
    (void)what;
    (void)value;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- Microwave Oven Control ----------------------------------------------------- */

void *mt_matter_mwoc_delegate_alloc(void) { return NULL; }
void mt_matter_mwoc_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

/* ---- electrical measurement (energy round A) ------------------------------------ */

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
void mt_matter_meas_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

/* ---- water heater management (energy round B) ------------------------------------ */

void *mt_matter_whm_delegate_alloc(uint16_t ep) { (void)ep; return NULL; }

/* ---- device energy management (energy round C1) ----------------------------------- */

void *mt_matter_dem_delegate_alloc(uint16_t ep) { (void)ep; return NULL; }

int mt_matter_demcap_set(uint16_t ep, uint8_t cause, uint8_t n, const int64_t *quads)
{
    (void)ep;
    (void)cause;
    (void)n;
    (void)quads;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- nested row payloads (AT+MTROW family, energy round C2) ------------------------ */

int mt_matter_rows_apply(uint16_t ep, uint8_t kind, const mt_row_stage_t *stage)
{
    (void)ep; (void)kind; (void)stage;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_rows_get(uint16_t ep, uint8_t kind, uint16_t idx,
                       mt_row_t *out, uint16_t *total)
{
    (void)ep;
    (void)kind;
    (void)idx;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (total != NULL) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_rows_total(uint16_t ep, uint8_t kind, uint16_t *total)
{
    (void)ep;
    (void)kind;
    if (total != NULL) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

/* ---- Meter Identification Instance pool (energy round C2) -------------------------- */

uint32_t mt_meter_feature_mask(void) { return 0; }

bool mt_meter_reserve(void) { return false; }

void mt_meter_register_all(void) {}

int mt_matter_meter_set_identity(uint16_t ep, const mt_meter_identity_t *id)
{
    (void)ep;
    (void)id;
    return MT_ATTR_ERR_ENDPOINT;
}

/* ---- Energy EVSE delegate and targets store (energy round C2) --------------------- */

void *mt_matter_evse_delegate_alloc(uint16_t ep) { (void)ep; return NULL; }

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
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (total != NULL) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_evse_targets_total(uint16_t ep, uint16_t *total)
{
    (void)ep;
    if (total != NULL) *total = 0;
    return MT_ROW_ERR_ENDPOINT;
}

int mt_matter_evse_targets_erase_all(void) { return -1; }
