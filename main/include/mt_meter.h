/*
 * mt_meter.h - the C++-internal face of mt_meter.cpp (energy round C2,
 * task 9).
 *
 * NOT part of the host contract: mt_matter.h owns that, declares the
 * extern "C" mt_matter_meter_set_identity() bridge function mt_at.c calls,
 * and is the only file mt_at.c includes. This header exists for one
 * reason, the same one mt_evse.h states for its own four functions: the
 * bridge function lives in main.cpp, because ChipStackLock (the RAII guard
 * every mt_matter_* bridge function must hold, see its comment there) is
 * defined there and nowhere else, while the MeterIdentification::Instance
 * pool lives in mt_meter.cpp. mt_meter_set_identity_locked() below is the
 * seam between those two translation units.
 *
 * REQUIRES the caller to hold ChipStackLock, which is what the _locked
 * suffix records, the same convention mt_evse.h uses.
 */

#pragma once

#include <stdint.h>

#include "mt_matter.h"

/*
 * Apply *id to ep's MeterIdentification::Instance. The caller
 * (mt_matter_meter_set_identity(), main.cpp) has already confirmed ep
 * exists and carries the MeterIdentification cluster; this function does
 * its own pool lookup on top of that (defensive: cluster present but no
 * pool slot serving it cannot happen once mt_meter_register_all() has run,
 * the same "cannot happen in practice" shape mt_evse_meas_apply_locked()
 * uses for its own pool), then validates every field of *id before calling
 * any of the Instance's SetXxx() members (all-or-nothing).
 *
 * Returns an mt_attr_result_t (mt_matter.h). MT_ATTR_ERR_FAILED for the
 * defensive pool-miss case above; MT_ATTR_ERR_VALUE for an out-of-range
 * MeterTypeEnum/PowerThresholdSourceEnum, an oversized string, or both
 * power optionals absent; MT_ATTR_OK once every SetXxx() call has run.
 */
int mt_meter_set_identity_locked(uint16_t ep, const mt_meter_identity_t *id);
