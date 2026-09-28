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
 * MT_ATTR_ERR_CLUSTER, the AT+MTROW family answers MT_ROW_ERR_NO_PAYLOAD
 * (+MTERR:4 through mt_at.c's row code mapping, where MT_ROW_ERR_ENDPOINT is
 * +MTERR:2), and AT+MTMETERID answers MT_ATTR_ERR_ATTRIBUTE because its
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

/* The two shapes the step-1b stubs answer with. */
#define STUB_ATTR_MISS(ep) (stub_endpoint_live(ep) ? MT_ATTR_ERR_CLUSTER : MT_ATTR_ERR_ENDPOINT)
#define STUB_ROW_MISS(ep)  (stub_endpoint_live(ep) ? MT_ROW_ERR_NO_PAYLOAD : MT_ROW_ERR_ENDPOINT)

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

/* ---- nested row payloads (AT+MTROW family, energy round C2) ------------------------ */

/* The MT_ROW_* family's "the endpoint exists but no cluster stores this row
 * kind" code is MT_ROW_ERR_NO_PAYLOAD, which mt_at.c renders as +MTERR:4, not
 * MT_ROW_ERR_CLUSTER: mt_rows.h's codec owns the MT_ROW_ERR_* space and has
 * no such name (mt_matter.h:1152-1163, :1207-1208). */

int mt_matter_rows_apply(uint16_t ep, uint8_t kind, const mt_row_stage_t *stage)
{
    (void)kind; (void)stage;
    return STUB_ROW_MISS(ep);
}

int mt_matter_rows_get(uint16_t ep, uint8_t kind, uint16_t idx,
                       mt_row_t *out, uint16_t *total)
{
    (void)kind;
    (void)idx;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (total != NULL) *total = 0;
    return STUB_ROW_MISS(ep);
}

int mt_matter_rows_total(uint16_t ep, uint8_t kind, uint16_t *total)
{
    (void)kind;
    if (total != NULL) *total = 0;
    return STUB_ROW_MISS(ep);
}

/* ---- Energy EVSE delegate and targets store (energy round C2) --------------------- */

void *mt_matter_evse_delegate_alloc(uint16_t ep) { (void)ep; return NULL; }

bool mt_matter_evse_reserve(void) { return false; }

/* The port-local second half mt_devtypes_sl.cpp calls (it is not in
 * mt_matter.h, hence the prototype here). Unreachable while
 * mt_matter_evse_reserve() above refuses every EVSE; it exists only so the
 * tree links until the EVSE bridge (mt_matter_sl_evse.inc) brings the real
 * one and this leaves with the other EVSE stubs. */
void mt_matter_evse_register(void *delegate, uint16_t ep, bool with_soc);
void mt_matter_evse_register(void *delegate, uint16_t ep, bool with_soc)
{
    (void)delegate;
    (void)ep;
    (void)with_soc;
}

int mt_matter_evse_set(uint16_t ep, uint8_t field, int64_t value)
{
    (void)field;
    (void)value;
    /* ep carries no EnergyEvse cluster: MT_ATTR_ERR_CLUSTER
     * (mt_matter.h:1552-1553) */
    return STUB_ATTR_MISS(ep);
}

/* The three below are the kind-1 arm mt_matter_rows_*() routes to, so they
 * carry the same MT_ROW_* contract by reference (mt_matter.h:1586-1606; the
 * _total entry names +MTERR:2 / +MTERR:4 outright). No AT command reaches
 * them while the rows_* trio above is itself a stub; they answer the same way
 * so the pair cannot disagree the day one of them stops being a stub. */

int mt_matter_evse_targets_apply(uint16_t ep, const mt_row_stage_t *stage)
{
    (void)stage;
    return STUB_ROW_MISS(ep);
}

int mt_matter_evse_targets_get(uint16_t ep, uint16_t idx, mt_row_t *out, uint16_t *total)
{
    (void)idx;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (total != NULL) *total = 0;
    return STUB_ROW_MISS(ep);
}

int mt_matter_evse_targets_total(uint16_t ep, uint16_t *total)
{
    if (total != NULL) *total = 0;
    return STUB_ROW_MISS(ep);
}

/*
 * Ruling B493: this ANSWERS rather than refuses, and the 0 is the honest
 * answer rather than a convenient one.
 *
 * core/mt/mt_at.c's cmd_mtfreset() calls this before mt_matter_factory_reset()
 * so that a factory reset leaves no EVSE charging schedule behind. This image
 * has no EVSE device type and therefore no targets store at all, so there is
 * nothing to erase and erasing nothing SUCCEEDED. Returning -1 would make
 * AT+MTFRESET fail on a device whose state is already exactly what the command
 * asks for, which is the transport stub's precedent one command over: a stub
 * for a capability the image does not have answers for the state it is in
 * instead of refusing the question.
 *
 * The EVSE round's real implementation replaces this; until then AT+MTFRESET
 * is exercised on the bench (round 2 task 5) and must answer OK, clear the
 * composition and reboot.
 */
int mt_matter_evse_targets_erase_all(void) { return 0; }
