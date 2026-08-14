/*
 * mt_evse.h - the C++-internal face of mt_evse.cpp (energy round C2, task 4).
 *
 * NOT part of the host contract: mt_matter.h owns that, declares the
 * extern "C" mt_matter_evse_* bridge functions mt_at.c calls, and is the
 * only file mt_at.c includes. This header exists for one reason: the bridge
 * functions live in main.cpp, because ChipStackLock (the RAII guard every
 * mt_matter_* bridge function must hold, see its comment there) is defined
 * there and nowhere else, while the delegate, the charging-target store and
 * its NVS persistence live in mt_evse.cpp. The four functions below are the
 * seam between those two translation units.
 *
 * EVERY function declared here REQUIRES the caller to hold ChipStackLock,
 * which is what the _locked suffix records. They read and write delegate
 * state that the CHIP event loop also reads (GetTargets hands the encoder a
 * non-owning view of exactly this storage, see mt_evse.cpp), so an unlocked
 * caller races the fabric.
 */

#pragma once

#include <stdint.h>

#include "mt_rows.h"

/*
 * Push count (field, value) pairs onto ep's EnergyEvse cluster, the
 * AT+MTMEAS 0x0099 branch (task 7 wires the wire side; mt_matter_evse_set()
 * in main.cpp is the single-pair form task 4 ships). Two-pass
 * validate-then-apply, the mt_matter_meas_set() contract: nothing is applied
 * unless every pair passes.
 *
 * Returns an mt_attr_result_t (mt_matter.h), NOT an MT_ROW_* code: this is
 * the measurement-push family, and the field ids are MT_EVSE_F_*.
 */
int mt_evse_meas_apply_locked(uint16_t ep, const uint8_t *fields, const int64_t *values,
                              uint8_t count);

/*
 * Commit a staged row set as ep's charging schedule, MERGING by day
 * (mt_matter_rows_apply()'s contract in mt_matter.h; the merge rule and the
 * count-0 exception are documented at the implementation).
 *
 * stage points at mt_at.c's file-static staging buffer and is consumed
 * before this call returns; the pointer is never retained.
 *
 * Returns an MT_ROW_* code.
 */
int mt_evse_targets_apply_locked(uint16_t ep, const mt_row_stage_t *stage);

/*
 * Read stored row idx of ep's charging schedule into *out, and ep's total
 * stored row count into *total. Rows are the flattened (schedule, target)
 * sequence in stored order, which is what makes AT+MTROWGET's index space
 * stable between two reads with no write in between.
 *
 * Returns an MT_ROW_* code.
 */
int mt_evse_targets_get_locked(uint16_t ep, uint16_t idx, mt_row_t *out, uint16_t *total);

/* Number of stored rows for ep, AT+MTROWGET's bulk-loop bound. MT_ROW_* code. */
int mt_evse_targets_total_locked(uint16_t ep, uint16_t *total);

/*
 * Erase every stored schedule, in RAM and in NVS, for AT+MTFRESET. Returns 0
 * on success, -1 if the NVS erase failed (the RAM stores are cleared either
 * way).
 */
int mt_evse_targets_erase_all_locked(void);
