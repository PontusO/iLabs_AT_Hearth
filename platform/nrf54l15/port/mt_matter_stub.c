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

/* mt_matter_temp_levels_set() left this file in catalogue batch 8, when the
 * temperature controlled cabinet (0x0071) and cook surface (0x0077) entered
 * the registry: real in mt_matter_zephyr.cpp now, on the one process-global
 * SupportedTemperatureLevelsIteratorDelegate and a block-resident label
 * store per TemperatureLevel-variant endpoint. */

/* mt_matter_mode_select_manager() and mt_matter_modes_set() left this file
 * in catalogue batch 4, when the mode select (0x0027) entered the
 * registry: real in mt_matter_zephyr.cpp now, on the one process-global
 * SupportedModesManager. */

/* The ModeBase slice (mt_matter_modebase_delegate_alloc,
 * mt_matter_modebase_delegate_set_endpoint, mt_matter_modebase_set) and
 * the RVC opstate pair (mt_matter_rvc_opstate_delegate_alloc,
 * mt_matter_rvc_opstate_delegate_set_endpoint) left this file in
 * catalogue batch 5, when the robotic vacuum cleaner (0x0074) entered the
 * registry: real in mt_matter_zephyr.cpp now, on per-(endpoint, cluster)
 * ModeBase pools and the RVC's own delegate subclass pool. */

/* The plain OperationalState slice (mt_matter_opstate_delegate_alloc,
 * mt_matter_opstate_delegate_set_endpoint, mt_matter_opstate_set) left
 * this file in catalogue batch 4, when the washer/dishwasher/dryer trio
 * entered the registry: real in mt_matter_zephyr.cpp now, on a
 * per-endpoint Instance-plus-Delegate pool. */

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

/* The MicrowaveOvenControl pair (mt_matter_mwoc_delegate_alloc,
 * mt_matter_mwoc_delegate_set_endpoint) left this file in catalogue batch 8,
 * when the microwave oven (0x0079) entered the registry: real in
 * mt_matter_zephyr.cpp now, on a HearthMwocDelegate-plus-Instance pool. The
 * second half there only stamps the endpoint; the Instance is born inside
 * mt_matter_mwoc_register(), which owns the three-way construction order its
 * constructor's two live-Instance references demand. */

/* The measurement foundation (mt_matter_meas_set, mt_matter_epm_delegate_
 * alloc, mt_matter_ptop_delegate_alloc, mt_matter_meas_delegate_set_
 * endpoint) left this file in catalogue batch 7a, when the electrical
 * sensor (0x0510) and electrical meter (0x0514) entered the registry: real
 * in mt_matter_zephyr.cpp now, on the MT_MEAS_MAX EPM/PowerTopology
 * delegate-plus-Instance pools and the ElectricalEnergyMeasurement push
 * server. mt_matter_meas_set() there still answers MT_ATTR_ERR_CLUSTER for
 * EnergyEvse (0x0099), whose device type is out of the 256 KB tier by
 * ruling DE408 (LM20 only). */

/* mt_matter_whm_delegate_alloc() left this file in catalogue batch 7b,
 * when the water heater (0x050F) entered the registry: real in
 * mt_matter_zephyr.cpp now, on the MT_WHM_MAX HearthWhmDelegate-plus-
 * Instance pool, and mt_matter_meas_set() serves the 0x0094 push family
 * with the derived Boost events. */

/* The DEM pair (mt_matter_dem_delegate_alloc, mt_matter_demcap_set) left
 * this file in catalogue batch 7a, when the device energy management
 * device type (0x050D) entered the registry: real in mt_matter_zephyr.cpp
 * now, on the MT_DEM_MAX HearthDemDelegate-plus-Instance pool. */

/* The meter quartet (mt_meter_feature_mask, mt_meter_reserve,
 * mt_meter_register_all, mt_matter_meter_set_identity) left this file in
 * catalogue batch 7a, when the electrical utility meter (0x0511) entered
 * the registry: real in mt_matter_zephyr.cpp now, on the MT_METER_MAX
 * MeterIdentification Instance pool and the post-rebuild registration
 * scan main.cpp runs. */

/* The whole Energy EVSE slice left this file in the EVSE round, when the
 * energy EVSE (0x050C) entered the registry and closed the L15 catalogue:
 * mt_matter_evse_reserve, mt_matter_evse_delegate_alloc, mt_matter_evse_set,
 * mt_matter_evse_targets_apply/get/total/erase_all and the mt_matter_rows_*
 * trio that routes AT+MTROW to them are all real in mt_matter_zephyr.cpp now,
 * on the MT_EVSE_MAX HearthEvseDelegate-plus-Instance pool and the
 * charging-target store that pool object carries. THIS FILE IS NOW EMPTY OF
 * mt_matter.h entry points: every one of them is implemented for real
 * somewhere in the port, which is what the header comment's "the linker
 * proves completeness" was always working towards. It is kept, rather than
 * deleted, because that proof is a property of the build and not of any one
 * round: a header entry point added tomorrow has a place to be stubbed while
 * its implementation is written. */
