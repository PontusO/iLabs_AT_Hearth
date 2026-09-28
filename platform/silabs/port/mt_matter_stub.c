/*
 * mt_matter_stub.c - every mt_matter.h entry point, stubbed, for the Silabs
 * skeleton. The linker proves completeness: an unimplemented declaration
 * fails the build. Each stub is replaced by the real implementation in the
 * upward-port round's batches; a stub that survives a batch is a stub the
 * batch forgot, which is what the "linker proves it" rule is for.
 *
 * Return convention, mechanically from the header comments: int with a
 * 0-on-success convention -> -1; mt_attr_result_t and MT_ROW_* -> the
 * endpoint code when the endpoint id is dead, the family's "the endpoint
 * exists but does not carry this" code when it is live (see
 * stub_endpoint_live() below); counts 0; bool false; pointer NULL; void
 * empty. Every out-parameter is zeroed (after a NULL check) before the
 * failure return.
 *
 * Round 2 task 4 retired the first nine: the commissioning state, network
 * and Thread answers come from the running stack in port/mt_matter_sl.cpp
 * now. Round 2 task 5 retired the live-composition trio, and round 2 task 6
 * the attribute read/write pair. Catalogue batch 3 (2026-09-25) retired the
 * door lock and water valve pair, and catalogue batch 4 (2026-09-26) the
 * mode select pair. What is left below is everything the data model has to
 * exist for.
 */

#include <stddef.h>
#include <string.h>

#include "mt_matter.h"

/*
 * ---- step 1b (round 2 task 6): a stub that knows the live endpoints ------
 *
 * A stub for a family this image has not ported. The endpoint may still be
 * live (round 2 task 5's dynamic endpoints), and the honest answer then is
 * "no such cluster on that endpoint", the code the family's real port gives
 * for a light. Only a dead endpoint id answers "no such endpoint". No
 * data-model access: this is still a stub, replaced by the batch that ports
 * the family.
 *
 * Before this, every one of these answered "no such endpoint"
 * unconditionally, which was right only while no endpoint existed at all. It
 * stopped being right the moment task 5 stood up the rig's light: eighteen
 * harness Phase 1 rows exist precisely to prove a host can tell "that
 * endpoint is not there" from "that endpoint is there and does not do this",
 * and a stub that collapses the two answers the wrong one for a live light.
 *
 * WHICH code "no such cluster" is varies by family and is taken from that
 * family's own entry in core/include/mt_matter.h, never assumed: most answer
 * MT_ATTR_ERR_CLUSTER (the AT+MTROW family answered MT_ROW_ERR_NO_PAYLOAD
 * until the EVSE round made it real), and AT+MTMETERID answers MT_ATTR_ERR_ATTRIBUTE because its
 * header entry says "deliberately not MT_ATTR_ERR_CLUSTER" and gives the
 * reason. Each call site below names the code it uses for that reason.
 *
 * Stubs whose family takes no endpoint id are unchanged, and so are the
 * delegate allocators: a pointer return has no error code to divide.
 *
 * This reads the same table AT+MTEP? reports (port/mt_matter_sl.cpp), which
 * the boot rebuild fills before mt_at_start() lets any command run, so there
 * is nothing to lock and no CHIP call to make from here.
 */
static bool stub_endpoint_live(uint16_t ep)
{
    uint16_t n = mt_matter_endpoint_count();
    for (uint16_t i = 0; i < n; i++) {
        uint32_t dt;
        uint16_t id;
        uint8_t var, pidx;
        if (mt_matter_endpoint_info(i, &dt, &id, &var, &pidx) == 0 && id == ep) {
            return true;
        }
    }
    return false;
}

/* The shape the step-1b stubs answer with. */
#define STUB_ATTR_MISS(ep) (stub_endpoint_live(ep) ? MT_ATTR_ERR_CLUSTER : MT_ATTR_ERR_ENDPOINT)

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

/* Round 2 task 5 moved this section to port/mt_matter_sl.cpp, which answers
 * it from the table the boot rebuild fills: mt_matter_endpoint_count,
 * mt_matter_endpoint_info and mt_matter_record_endpoint. The banner stays so
 * the section can be found by the same name in both files. */

/* ---- attribute read/write ------------------------------------------------ */

/* Round 2 task 6 moved this section to port/mt_matter_sl.cpp, which answers
 * it from the ember attribute store: mt_matter_attr_read and
 * mt_matter_attr_write, plus the strong MatterPostAttributeChangeCallback
 * that raises the +MTATTR URC. The banner stays so the section can be found
 * by the same name in both files. */

/* ---- temperature level labels (C3) --------------------------------------- */

int mt_matter_temp_levels_set(uint16_t ep, const char *const *labels, uint8_t count)
{
    (void)labels;
    (void)count;
    /* no TemperatureControl cluster: MT_ATTR_ERR_CLUSTER (mt_matter.h:248) */
    return STUB_ATTR_MISS(ep);
}

/* The door lock and water valve bridge functions left this file in
 * catalogue batch 3 (2026-09-25); they live in mt_matter_sl.cpp. */

/* The mode select bridge functions left this file in catalogue batch 4
 * (2026-09-26); they live in mt_matter_sl.cpp. */

/* The RVC's three ModeBase stubs (mt_matter_modebase_delegate_alloc,
 * mt_matter_modebase_delegate_set_endpoint, mt_matter_modebase_set) left this
 * file in catalogue batch 5b (2026-09-27): they are the real things now, in
 * mt_matter_sl_b5.inc. */

/* The OperationalState trio's stubs (mt_matter_opstate_delegate_alloc,
 * mt_matter_opstate_delegate_set_endpoint, mt_matter_opstate_set) are the real
 * things now, in mt_matter_sl_b4.inc. */

/* ---- air quality (C1b, bug B139) --------------------------------------------- */

/* mt_air_quality_feature_mask() left this file in catalogue batch 2, when
 * the air quality sensor (0x002C) entered the registry: it lives in
 * mt_matter_sl.cpp now and returns the real bits. */

/* ---- smoke/co alarm + refrigerator alarm ------------------------------------- */

/* mt_matter_alarm_set() left this file in catalogue batch 4 (2026-09-26);
 * it lives in mt_matter_sl.cpp (the smoke/co arm only; the refrigerator
 * alarm arm is catalogue batch 8). */

/* ---- chime -------------------------------------------------------------------- */

/* The chime stubs (mt_matter_chime_delegate_alloc,
 * mt_matter_chime_delegate_set_endpoint, mt_matter_chime_sounds_set,
 * mt_matter_chime_set) left this file in catalogue batch 4 (2026-09-26):
 * they are the real things now, in mt_matter_sl_b4.inc. */

/* ---- Microwave Oven Control ----------------------------------------------------- */

void *mt_matter_mwoc_delegate_alloc(void) { return NULL; }
void mt_matter_mwoc_delegate_set_endpoint(void *delegate, uint16_t ep) { (void)delegate; (void)ep; }

/* The AT+MTROW family and the energy EVSE stubs (mt_matter_rows_apply,
 * _get, _total, mt_matter_evse_delegate_alloc, mt_matter_evse_reserve,
 * mt_matter_evse_set, mt_matter_evse_targets_apply, _get, _total,
 * mt_matter_evse_targets_erase_all and the interim mt_matter_evse_register
 * link stub) left this file in the EVSE round
 * (2026-09-28): they are the real things now, in mt_matter_sl_evse.inc. */
