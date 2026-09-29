/*
 * mt_matter_stub.c - empty since catalogue batch 8.
 *
 * This file held every mt_matter.h entry point, stubbed, for the Silabs
 * skeleton: the linker proved completeness, and each stub was replaced by
 * the real implementation in the upward-port round's batches. Catalogue
 * batch 8 (2026-09-28) retired the last three (mt_matter_temp_levels_set
 * and the MicrowaveOvenControl pair), so every declaration has its one
 * definition in port/mt_matter_sl.cpp and its fragments, and the step-1b
 * helper that told a live endpoint from a dead one went with them.
 *
 * THE FILE IS KEPT RATHER THAN DELETED, mt_devtypes_stub.c's reasoning:
 * deleting it would mean editing hearth.slcp's source list,
 * test/host/Makefile's silabs-stubs target and test/host/check_decls.py's
 * pair list together, for one empty translation unit. check_decls.py goes
 * on proving that mt_matter.h's declarations have exactly one definition
 * across this file and mt_matter_sl.cpp. The banners stay so each section
 * can be found by the same name in both files.
 *
 * The FOTA quartet (`mt_matter_ota_set_mode`, `mt_matter_ota_block_acked`,
 * `mt_matter_ota_staged`, `mt_matter_swver_set`) was never stubbed here: the
 * first three live in `platform/common/hearth_ota_requestor.cpp`, the fourth
 * in `hearth_ota_sl.cpp`, and `check_decls.py` counts both files.
 */

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

/* mt_matter_temp_levels_set() left this file in catalogue batch 8
 * (2026-09-28); it lives in mt_matter_sl_b8.inc. */

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
 * it lives in mt_matter_sl_b4.inc, both arms since catalogue batch 8. */

/* ---- chime -------------------------------------------------------------------- */

/* The chime stubs (mt_matter_chime_delegate_alloc,
 * mt_matter_chime_delegate_set_endpoint, mt_matter_chime_sounds_set,
 * mt_matter_chime_set) left this file in catalogue batch 4 (2026-09-26):
 * they are the real things now, in mt_matter_sl_b4.inc. */

/* ---- Microwave Oven Control ----------------------------------------------------- */

/* The MicrowaveOvenControl pair (mt_matter_mwoc_delegate_alloc,
 * mt_matter_mwoc_delegate_set_endpoint) and the two interim link stubs of
 * its round left this file in catalogue batch 8 (2026-09-28): they are the
 * real things now, in mt_matter_sl_b8.inc. */

/* The AT+MTROW family and the energy EVSE stubs (mt_matter_rows_apply,
 * _get, _total, mt_matter_evse_delegate_alloc, mt_matter_evse_reserve,
 * mt_matter_evse_set, mt_matter_evse_targets_apply, _get, _total,
 * mt_matter_evse_targets_erase_all and the interim mt_matter_evse_register
 * link stub) left this file in the EVSE round
 * (2026-09-28): they are the real things now, in mt_matter_sl_evse.inc. */
