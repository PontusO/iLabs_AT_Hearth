/*
 * main.cpp - iLabs AT Hearth: the esp_matter runtime and the C-linkage bridge
 * the AT command handlers call into.
 *
 * Holds app_main (endpoint composition rebuild, then esp_matter::start, then
 * the AT interface), the platform and attribute callbacks that turn CHIP events
 * into URCs, and the mt_matter_* wrappers declared in mt_matter.h. Everything
 * C++ stays here; mt_at.c is plain C and never sees an esp_matter header.
 *
 * Built on IDF v5.4.1 + esp-matter release/v1.5, WiFi transport, ESP32-C6.
 */

#include <array>
#include <inttypes.h>
#include <cstring>
#include <new>
#include <stdio.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>

#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

/* C3: the SupportedTemperatureLevels delegate interface and the reporting
 * call that marks its list dirty after a host writes new labels. */
#include <app/clusters/temperature-control-server/supported-temperature-levels-manager.h>
#include <app/reporting/reporting.h>
#include <lib/support/Span.h>

/* C2: the door lock server instance (DoorLockServer::Instance()) and the
 * ember plugin-command callback signatures (LockDoor/UnlockDoor) this file
 * implements. Also pulls in OperationErrorEnum/OperationSourceEnum/
 * DlLockState/Nullable/Optional at global scope via `using` declarations
 * (door-lock-server.h:44-64), which is why the callback signatures below
 * match the SDK's prototypes without repeating those `using`s here. */
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app-common/zap-generated/callback.h>

/* C1b (bug B139): the AirQuality cluster's own attribute-access interface.
 * See the registry comment ahead of mt_air_quality_register_all() below for
 * why esp-matter's generic attribute path cannot serve this cluster. */
#include <app/clusters/air-quality-server/air-quality-server.h>

/* C2 (seven-type batch, water valve): the ValveConfigurationAndControl
 * Delegate interface (pulled in transitively via
 * valve-configuration-and-control-delegate.h) and the app-side reporting
 * calls, UpdateCurrentState()/UpdateCurrentLevel(). */
#include <app/clusters/valve-configuration-and-control-server/valve-configuration-and-control-cluster.h>

/* C3 (seven-type batch, mode select): the SupportedModesManager interface
 * (ModeOptionsProvider, ModeOptionStruct/SemanticTagStruct via cluster-
 * objects.h transitively) this file's HearthSupportedModesManager
 * implements. */
#include <app/clusters/mode-select-server/supported-modes-manager.h>

/* C4 (seven-type batch, OperationalState trio): Delegate/Instance,
 * GenericOperationalState/GenericOperationalError and ErrorStateEnum/
 * OperationalStateEnum (operational-state-cluster-objects.h, pulled in
 * transitively) this file's HearthOpStateDelegate implements. */
#include <app/clusters/operational-state-server/operational-state-server.h>

/* C5 (seven-type batch, smoke/co alarm): SmokeCoAlarmServer::Instance() and
 * its eleven event-emitting Set* methods, and the enum types (AlarmStateEnum,
 * MuteStateEnum, EndOfServiceEnum, ContaminationStateEnum, SensitivityEnum)
 * this file's mt_matter_alarm_set() validates AT+MTALARM's <value> against.
 *
 * C12 (bug B165, bench F-C10-2): also SetExpressedStateByPriority(), which
 * mt_matter_alarm_set() now calls after every setter that can move
 * ExpressedState, per the app-side recompute contract the header documents
 * at smoke-co-alarm-server.h:49-55 ("Set the highest level of Expressed
 * State according to priorityOrder") and no caller in this codebase used
 * until now. */
#include <app/clusters/smoke-co-alarm-server/smoke-co-alarm-server.h>

/* Composed appliance round, task 3: RefrigeratorAlarmServer::Instance() and
 * its Get*Value/Set*Value methods (refrigerator-alarm-server.h), the second
 * cluster mt_matter_alarm_set() now dispatches to. RefrigeratorAlarm::Id and
 * the BitMask<RefrigeratorAlarm::AlarmMap> type it takes are already visible
 * via esp_matter.h's own transitive include of the generated per-cluster ids
 * and cluster-enums.h (the same "no extra include needed for the ::Id/enum
 * types themselves" precedent mode-base-server.h's own comment below
 * documents for RvcRunMode/RvcCleanMode/MicrowaveOvenMode); only the server
 * class itself needs its own header, the same split smoke-co-alarm-server.h
 * above has from SmokeCoAlarm's own ::Id/enum visibility. */
#include <app/clusters/refrigerator-alarm-server/refrigerator-alarm-server.h>

/* C6 (seven-type batch, chime): ChimeDelegate (three pure virtuals this
 * file's HearthChimeDelegate implements) and ChimeCluster (SetSelectedChime/
 * SetEnabled, called through the data model provider's registry, see
 * mt_matter_chime_set() below). chime_integration.h declares
 * chip::app::Clusters::Chime::SetDelegate(), and app/ClusterCallbacks.h
 * declares ESPMatterChimeClusterServerInitCallback(): together the two calls
 * the F6 workaround needs (mt_chime_register_all() below). */
#include <app/clusters/chime-server/ChimeCluster.h>
#include <data_model_provider/clusters/chime_integration.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/ClusterCallbacks.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>

/* RVC + Microwave batch, task 2: ModeBase::Delegate/Instance (RvcRunMode,
 * RvcCleanMode, MicrowaveOvenMode all derive from the abstract ModeBase
 * cluster and share this one interface), StatusCode, the ChangeToModeResponse
 * command shape, and (transitively, via cluster-objects.h -> clusters/shared/
 * Structs.h) detail::Structs::ModeTagStruct::Type this file's
 * HearthModeBaseDelegate implements. RvcRunMode::Id/RvcCleanMode::Id/
 * MicrowaveOvenMode::Id and their ModeTag enums are already visible via
 * esp_matter.h's own transitive include of the generated per-cluster ids and
 * cluster-enums.h (see mt_devtypes.cpp's identical use of ::Id constants with
 * no extra include), so no separate include is needed for those. */
#include <app/clusters/mode-base-server/mode-base-server.h>

/* RVC + Microwave batch, task 4: MicrowaveOvenControl::Delegate/Instance
 * live in this dedicated header. Unlike RvcOperationalState (whose Delegate/
 * Instance are nested inside operational-state-server.h, already included
 * above, per that class's own comment in this file) no other header this
 * file already includes pulls this one in transitively, so - the same as
 * mode-base-server.h just above - it needs its own explicit include. */
#include <app/clusters/microwave-oven-control-server/microwave-oven-control-server.h>

/* Energy round A, task 2: the three measurement clusters behind AT+MTMEAS.
 * ElectricalPowerMeasurement::Delegate/Instance (pull-model, served from
 * host-pushed values), PowerTopology::Delegate/Instance (NodeTopology), the
 * ElectricalEnergyMeasurement free functions (push-model:
 * NotifyCumulativeEnergyMeasured writes the attribute store AND emits the
 * event), and ElectricalEnergyMeasurementAttrAccess, the wildcard attribute
 * access object the EEM server declares but nothing in the SDK ever
 * registers (the 8.6 disease in yet another organ; see the measurement
 * block's own comment below). SystemClock.h for the EEM timestamp policy
 * (GetClock_MatterEpochS when wall time is synced). */
#include <app/clusters/electrical-power-measurement-server/electrical-power-measurement-server.h>
#include <app/clusters/power-topology-server/power-topology-server.h>
#include <app/clusters/electrical-energy-measurement-server/electrical-energy-measurement-server.h>
#include <app/clusters/electrical-energy-measurement-server/ElectricalEnergyMeasurementCluster.h>
#include <system/SystemClock.h>

/* Energy round B, task 1: WaterHeaterManagement::Delegate/Instance behind
 * HearthWhmDelegate (Boost/CancelBoost forwarding plus the six
 * delegate-served WHM attributes). Dedicated header in the dedicated
 * water-heater-management-server directory
 * (CONFIG_SUPPORT_WATER_HEATER_MANAGEMENT_CLUSTER, sdkconfig.defaults);
 * nothing this file already includes pulls it in transitively. */
#include <app/clusters/water-heater-management-server/water-heater-management-server.h>

/* Energy round C1, task 1: DeviceEnergyManagement::Delegate/Instance behind
 * HearthDemDelegate (PowerAdjustRequest/CancelPowerAdjustRequest forwarding
 * plus the six delegate-served DEM attributes and the two owned struct
 * stores). Dedicated header in the dedicated device-energy-management-server
 * directory (CONFIG_SUPPORT_DEVICE_ENERGY_MANAGEMENT_CLUSTER,
 * sdkconfig.defaults); nothing this file already includes pulls it in
 * transitively. EventLogging.h for chip::app::LogEvent, which this round
 * calls DIRECTLY for the first time: Round A emitted through
 * NotifyCumulativeEnergyMeasured and Round B through WHM's Generate*Event
 * helpers, but the DEM cluster ships no emission helper at all (survey I.2;
 * the server emits nothing either, all four DEM events are app-emitted). */
#include <app/clusters/device-energy-management-server/device-energy-management-server.h>
#include <app/EventLogging.h>

/* 0.11.0 task 1: AT+MTTHREAD? is the first place this firmware reads a
 * string-valued attribute (ThreadNetworkDiagnostics NetworkName) off the
 * generic esp_matter::attribute::get_val() path. get_val_from_tlv_data()
 * (esp_matter_data_model.cpp) hands a non-null string back in a freshly
 * esp_matter_mem_calloc()'d buffer it documents as "now owned by the
 * caller"; esp_matter_mem_free() (this header) is the matching release,
 * not the general-purpose free(), since esp-matter does not guarantee the
 * two allocators are the same one. ThreadNetworkDiagnostics::Id and its
 * Attributes::*::Id/RoutingRoleEnum need no include of their own: they are
 * already visible via esp_matter.h's own transitive include of the
 * generated per-cluster ids and cluster-enums.h, the same precedent
 * RefrigeratorAlarm::Id's comment above documents. */
#include <esp_matter_mem.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>

/*
 * OpenThread platform defaults. These are NOT provided by ESP-IDF: every
 * esp-matter example defines its own copy (examples/light/main/app_priv.h:84
 * is the one this mirrors), so there is no header to include for them.
 * Reproduced here rather than pulling an example's private header in.
 *
 *   RADIO_MODE_NATIVE          the C6 has its own 802.15.4 radio; it is not
 *                              driving an external RCP over a serial link
 *   HOST_CONNECTION_MODE_NONE  and it is not acting as an RCP for a host
 *                              either, which is the mirror-image case
 *   storage_partition_name     "nvs", matching partitions.csv
 */
#define MT_OT_RADIO_CONFIG()  { .radio_mode = RADIO_MODE_NATIVE }
#define MT_OT_HOST_CONFIG()   { .host_connection_mode = HOST_CONNECTION_MODE_NONE }
#define MT_OT_PORT_CONFIG()   { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 }
#endif

#include "mt_at.h"
#include "mt_at_config.h"
#include "mt_comp_store.h"
#include "mt_composition.h"
#include "mt_devtypes.h"
/* Energy round C2 task 4: the lock-held internals of mt_evse.cpp, which the
 * five mt_matter_evse_* bridge functions below wrap in ChipStackLock. */
#include "mt_evse.h"
#include "mt_matter.h"
/* Energy round C2 task 9: the lock-held internals of mt_meter.cpp, which
 * mt_matter_meter_set_identity() below wraps in ChipStackLock. */
#include "mt_meter.h"
/* Pay-per-composition round task 2/3: the endpoint -> host-fed-store map and
 * the shared plain-C store shapes it points at. */
#include "mt_store_index.h"
#include "mt_stores.h"
#include "mt_transport.h"

static const char *TAG = "mt_main";

/* Free heap once the stack is up and BLE is still resident. Paired with the
 * figure logged when BLE is torn down; see kBLEDeinitialized in app_event_cb. */
static uint32_t s_heap_at_startup = 0;

/*
 * Latched at boot: the stored fabric belongs to the other transport (spec
 * 3.12.1). Latched rather than recomputed because the marker it derives from
 * is rewritten as soon as the device is commissioned here, and a live read
 * would flip to false mid-commissioning, taking away the host's explanation
 * for the window it is currently looking at.
 */
static bool s_transport_mismatch = false;

/*
 * Set by whichever path emits +MTEVT:0 for the current window, and cleared
 * when that window closes. Exactly one report per window, whichever of the two
 * paths gets there first.
 *
 * Two paths exist because CHIP may open the boot window either side of
 * mt_at_start(): before it, the native event is dropped by the s_at_up guard
 * and app_main's replay is the only delivery; after it, the native event is
 * delivered and the replay must stay quiet. An earlier attempt had the native
 * handler set this and the replay test it, which cannot work: on hardware the
 * window opened ~10 ms AFTER the replay ran, so the flag was always false when
 * the replay looked. Ownership has to sit with the emitter, not with one
 * particular emitter.
 *
 * volatile because app_main and the CHIP task touch it concurrently. A residual
 * race remains, between the replay reading the flag and mt_matter_state()
 * acquiring the CHIP lock; it is microseconds against the ~10 ms that made this
 * reproducible, and its worst outcome is the duplicate it replaces.
 */
static volatile bool s_window_evt_sent = false;

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
/*
 * Last role token reported on +MTEVT:28, and whether one has been reported at
 * all since the Thread interface last came up. Only app_event_cb() touches
 * these, and only from the CHIP event-loop task, so they need no lock and no
 * volatile.
 *
 * Why the firmware keeps this delta state after all (bench defect A, design
 * spec 2.2): CHIP's ThreadStateChange.RoleChanged tracks the OPENTHREAD role,
 * while this event carries the MATTER RoutingRole token, and that mapping is
 * many-to-one. thread-network-diagnostics-provider.cpp derives the token from
 * otThreadGetDeviceRole() combined with otThreadGetLinkMode() and
 * otThreadIsRouterEligible(), so OT_DEVICE_ROLE_CHILD alone renders as
 * SLEEPY_END_DEVICE, END_DEVICE or REED depending on link mode and router
 * eligibility. Two genuine OT role changes can therefore land on one Matter
 * token, and the bench saw exactly that: three bit-28 events inside 10 ms
 * during commissioning carrying UNASSIGNED, REED, REED. Gating on
 * RoleChanged is necessary but not sufficient; a token that did not change is
 * not a change the host can act on.
 *
 * The role byte is cached rather than the rendered string because
 * mt_thread_role_name() is injective: seven enum values, seven distinct
 * tokens, and the out-of-range fallback renders the byte itself in decimal.
 * Byte equality is therefore token equality, in two bytes instead of
 * eighteen.
 */
static uint8_t s_last_role_tok = 0;
static bool    s_last_role_tok_valid = false;
#endif

/* Window opened on a transport mismatch. The same 300 s AT+MTCOMMISSION
 * defaults to: long enough to commission unhurriedly, short enough that a
 * device nobody is attending stops advertising. */
#define MT_MISMATCH_WINDOW_S 300

using namespace esp_matter;
using namespace esp_matter::endpoint;

/*
 * Signedness of the AT+MTATTR integer family, keyed on the val type. Shared by
 * the +MTATTR URC formatter, the read bridge (which hands the flag to mt_at.c
 * so the handler formats a u64 above INT64_MAX with %llu instead of %lld) and
 * the write bridge (whose caller selects the unsigned parse on the same flag,
 * and which uses the pair to tell an out-of-width value from an unsupported
 * type). Boolean counts as unsigned: it has no negative literal.
 */
static bool attr_val_is_unsigned(esp_matter_val_type_t type)
{
    switch (type) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN:
    case ESP_MATTER_VAL_TYPE_UINT8:
    case ESP_MATTER_VAL_TYPE_ENUM8:
    case ESP_MATTER_VAL_TYPE_BITMAP8:
    case ESP_MATTER_VAL_TYPE_UINT16:
    case ESP_MATTER_VAL_TYPE_ENUM16:
    case ESP_MATTER_VAL_TYPE_BITMAP16:
    case ESP_MATTER_VAL_TYPE_UINT32:
    case ESP_MATTER_VAL_TYPE_BITMAP32:
    case ESP_MATTER_VAL_TYPE_UINT64:
    case ESP_MATTER_VAL_TYPE_NULLABLE_BOOLEAN:
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT8:
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM8:
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP8:
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT16:
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM16:
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP16:
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT32:
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP32:
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT64:
        return true;
    default:
        return false;
    }
}

/* The signed half of the same family. A type in neither set is one AT+MTATTR
 * cannot carry at all (string/array/float), which is what lets the write
 * bridge keep +MTERR:5 for those while a bad width answers +MTERR:1. */
static bool attr_val_is_signed_int(esp_matter_val_type_t type)
{
    switch (type) {
    case ESP_MATTER_VAL_TYPE_INTEGER:
    case ESP_MATTER_VAL_TYPE_INT8:
    case ESP_MATTER_VAL_TYPE_INT16:
    case ESP_MATTER_VAL_TYPE_INT32:
    case ESP_MATTER_VAL_TYPE_INT64:
    case ESP_MATTER_VAL_TYPE_NULLABLE_INTEGER:
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT8:
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT16:
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT32:
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT64:
        return true;
    default:
        return false;
    }
}

/*
 * Map an esp_matter attribute value to/from a 64-bit integer for the AT+MTATTR
 * command (host sends/receives integers; strings/floats/arrays are unsupported).
 * int64_t is the carrier for both families: an unsigned attribute's value
 * travels as the raw 64-bit pattern and the attr_val_is_unsigned() flag says
 * which reading applies, so a u64 above INT64_MAX survives the trip. The pair
 * used to go through 32-bit C long, which silently truncated i64/u64 values
 * (energy round A).
 *
 * Nullable numeric attributes (ESP_MATTER_VAL_TYPE_NULLABLE_*, esp-matter's
 * disjoint second family for every integer/bool/enum/bitmap type, offset by
 * ESP_MATTER_VAL_NULLABLE_BASE) share the same union field as their plain
 * sibling and convert the same way once a value is known to be present.
 * A NULL value is a different question: the AT grammar has no null literal,
 * and the host library's access pattern (begin() always writes an attribute
 * before any read of it) never reads a never-written nullable in practice.
 * So a null read answers exactly what an unsupported type answers today:
 * attr_val_to_i64() returns false and the caller reports +MTERR:5. Adding a
 * way to WRITE null over AT is a separate, out-of-scope feature; see
 * i64_to_attr_val() below.
 */
static bool attr_val_to_i64(const esp_matter_attr_val_t *v, int64_t *out)
{
    switch (v->type) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN:  *out = v->val.b;   return true;
    case ESP_MATTER_VAL_TYPE_INTEGER:  *out = v->val.i;   return true;
    case ESP_MATTER_VAL_TYPE_INT8:     *out = v->val.i8;  return true;
    case ESP_MATTER_VAL_TYPE_UINT8:
    case ESP_MATTER_VAL_TYPE_ENUM8:
    case ESP_MATTER_VAL_TYPE_BITMAP8:  *out = v->val.u8;  return true;
    case ESP_MATTER_VAL_TYPE_INT16:    *out = v->val.i16; return true;
    case ESP_MATTER_VAL_TYPE_UINT16:
    case ESP_MATTER_VAL_TYPE_ENUM16:
    case ESP_MATTER_VAL_TYPE_BITMAP16: *out = v->val.u16; return true;
    case ESP_MATTER_VAL_TYPE_INT32:    *out = v->val.i32; return true;
    case ESP_MATTER_VAL_TYPE_UINT32:
    case ESP_MATTER_VAL_TYPE_BITMAP32: *out = v->val.u32; return true;
    case ESP_MATTER_VAL_TYPE_INT64:    *out = v->val.i64; return true;
    /* Raw bit pattern above INT64_MAX; attr_val_is_unsigned() flags it. */
    case ESP_MATTER_VAL_TYPE_UINT64:   *out = (int64_t)v->val.u64; return true;

    /* Nullable siblings. Same union field and cast as the plain arm above;
     * chip::app::NumericAttributeTraits<T>::IsNullValue() is CHIP's own null
     * check (the same one esp_matter's internal val_is_null() uses, which
     * is not part of the public header so it is reproduced by type here). */
    case ESP_MATTER_VAL_TYPE_NULLABLE_BOOLEAN:
        if (chip::app::NumericAttributeTraits<bool>::IsNullValue(*(const uint8_t *)&v->val.b)) return false;
        *out = v->val.b; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INTEGER:
        if (chip::app::NumericAttributeTraits<int>::IsNullValue(v->val.i)) return false;
        *out = v->val.i; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT8:
        if (chip::app::NumericAttributeTraits<int8_t>::IsNullValue(v->val.i8)) return false;
        *out = v->val.i8; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT8:
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM8:
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP8:
        if (chip::app::NumericAttributeTraits<uint8_t>::IsNullValue(v->val.u8)) return false;
        *out = v->val.u8; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT16:
        if (chip::app::NumericAttributeTraits<int16_t>::IsNullValue(v->val.i16)) return false;
        *out = v->val.i16; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT16:
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM16:
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP16:
        if (chip::app::NumericAttributeTraits<uint16_t>::IsNullValue(v->val.u16)) return false;
        *out = v->val.u16; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT32:
        if (chip::app::NumericAttributeTraits<int32_t>::IsNullValue(v->val.i32)) return false;
        *out = v->val.i32; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT32:
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP32:
        if (chip::app::NumericAttributeTraits<uint32_t>::IsNullValue(v->val.u32)) return false;
        *out = v->val.u32; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT64:
        if (chip::app::NumericAttributeTraits<int64_t>::IsNullValue(v->val.i64)) return false;
        *out = v->val.i64; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT64:
        if (chip::app::NumericAttributeTraits<uint64_t>::IsNullValue(v->val.u64)) return false;
        *out = (int64_t)v->val.u64; return true;

    default: return false;
    }
}

/*
 * Same integer<->attr_val bridge, write direction. `v` arrives pre-populated
 * by attribute::get_val() with the located attribute's real type (including
 * NULLABLE_*), so this switches on that type exactly as attr_val_to_i64()
 * does; it never decides on its own whether an attribute is nullable.
 *
 * Each arm gates `in` on its own width before storing: `in` carries a full
 * 64-bit value (for an unsigned attribute, the raw bit pattern of the
 * unsigned parse, so the unsigned arms compare through uint64_t), and a
 * value that does not fit returns false instead of truncating. The 32-bit
 * predecessor of this function cast unconditionally, so 300 into a u8 wrote
 * 44 with an OK; energy round A closed that. mt_matter_attr_write() turns
 * the false into MT_ATTR_ERR_VALUE (+MTERR:1) when the type is
 * integer-carrying, and MT_ATTR_ERR_TYPE (+MTERR:5) when it is not carried
 * at all.
 *
 * A nullable arm builds the val with its esp_matter_nullable_*() constructor
 * rather than writing the union field directly, so the type tag it leaves
 * behind is unambiguously the nullable one. This never produces a null:
 * every value handed to it came from AT+MTATTR's integer grammar, which has
 * no null literal, so writing null over AT is out of scope. The one
 * exception is not a design choice but a property of Matter's own nullable
 * encoding: each type reserves one sentinel value (all-ones for unsigned,
 * the type minimum for signed) to mean null, and esp_matter_nullable_*()
 * treats a value equal to that sentinel as null on construction, same as it
 * would for a value CHIP itself produced.
 */
static bool i64_to_attr_val(esp_matter_attr_val_t *v, int64_t in)
{
    const uint64_t u = (uint64_t)in;  /* unsigned reading of the pattern */

    switch (v->type) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN:
        if (u > 1) return false;
        v->val.b = (bool)in; return true;
    case ESP_MATTER_VAL_TYPE_INTEGER:
        if (in < INT32_MIN || in > INT32_MAX) return false;
        v->val.i = (int)in; return true;
    case ESP_MATTER_VAL_TYPE_INT8:
        if (in < INT8_MIN || in > INT8_MAX) return false;
        v->val.i8 = (int8_t)in; return true;
    case ESP_MATTER_VAL_TYPE_UINT8:
    case ESP_MATTER_VAL_TYPE_ENUM8:
    case ESP_MATTER_VAL_TYPE_BITMAP8:
        if (u > UINT8_MAX) return false;
        v->val.u8 = (uint8_t)in; return true;
    case ESP_MATTER_VAL_TYPE_INT16:
        if (in < INT16_MIN || in > INT16_MAX) return false;
        v->val.i16 = (int16_t)in; return true;
    case ESP_MATTER_VAL_TYPE_UINT16:
    case ESP_MATTER_VAL_TYPE_ENUM16:
    case ESP_MATTER_VAL_TYPE_BITMAP16:
        if (u > UINT16_MAX) return false;
        v->val.u16 = (uint16_t)in; return true;
    case ESP_MATTER_VAL_TYPE_INT32:
        if (in < INT32_MIN || in > INT32_MAX) return false;
        v->val.i32 = (int32_t)in; return true;
    case ESP_MATTER_VAL_TYPE_UINT32:
    case ESP_MATTER_VAL_TYPE_BITMAP32:
        if (u > UINT32_MAX) return false;
        v->val.u32 = (uint32_t)in; return true;
    case ESP_MATTER_VAL_TYPE_INT64:
        v->val.i64 = in; return true;
    case ESP_MATTER_VAL_TYPE_UINT64:
        v->val.u64 = u; return true;

    case ESP_MATTER_VAL_TYPE_NULLABLE_BOOLEAN:
        if (u > 1) return false;
        *v = esp_matter_nullable_bool((bool)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INTEGER:
        if (in < INT32_MIN || in > INT32_MAX) return false;
        *v = esp_matter_nullable_int((int)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT8:
        if (in < INT8_MIN || in > INT8_MAX) return false;
        *v = esp_matter_nullable_int8((int8_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT8:
        if (u > UINT8_MAX) return false;
        *v = esp_matter_nullable_uint8((uint8_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM8:
        if (u > UINT8_MAX) return false;
        *v = esp_matter_nullable_enum8((uint8_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP8:
        if (u > UINT8_MAX) return false;
        *v = esp_matter_nullable_bitmap8((uint8_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT16:
        if (in < INT16_MIN || in > INT16_MAX) return false;
        *v = esp_matter_nullable_int16((int16_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT16:
        if (u > UINT16_MAX) return false;
        *v = esp_matter_nullable_uint16((uint16_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM16:
        if (u > UINT16_MAX) return false;
        *v = esp_matter_nullable_enum16((uint16_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP16:
        if (u > UINT16_MAX) return false;
        *v = esp_matter_nullable_bitmap16((uint16_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT32:
        if (in < INT32_MIN || in > INT32_MAX) return false;
        *v = esp_matter_nullable_int32((int32_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT32:
        if (u > UINT32_MAX) return false;
        *v = esp_matter_nullable_uint32((uint32_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP32:
        if (u > UINT32_MAX) return false;
        *v = esp_matter_nullable_bitmap32((uint32_t)in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT64:
        *v = esp_matter_nullable_int64(in); return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT64:
        *v = esp_matter_nullable_uint64(u); return true;

    default: return false;
    }
}

/*
 * Forward declarations: both are implemented later in this file (attr_locate()
 * with the other attribute-bridge plumbing, mt_thread_read_role() right beside
 * mt_matter_thread_info()/mt_thread_role_name(), which it shares its read path
 * with), but app_event_cb() below needs to call mt_thread_read_role() from its
 * kThreadStateChange case, which runs earlier in this translation unit than
 * either definition.
 */
static mt_attr_result_t attr_locate(uint16_t ep, uint32_t cluster, uint32_t attr,
                                    esp_matter::attribute_t **out);
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
/* Guarded like its sole call site (app_event_cb()'s kThreadStateChange case,
 * below) and its definition (near mt_matter_thread_info()): on a WiFi-only
 * image (build_b4) this whole path does not exist, and an unconditional
 * definition would be an unused static function there. noinline: this is a
 * single-call-site static function GCC otherwise folds entirely into
 * app_event_cb() even at -Os, leaving no distinct symbol; kept as a real
 * frame so it has a name in a backtrace and in `nm`. */
static bool mt_thread_read_role(uint8_t *role_out) __attribute__((noinline));
#endif

/*
 * Platform events. Each CHIP event we surface maps to a bit in the AT event
 * mask (mt_at.h); mt_at_event() drops the ones the host has not subscribed to,
 * so this switch can stay exhaustive without flooding a 115200 link.
 */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    using namespace chip::DeviceLayer;

    switch (event->Type) {
    /* Commissioning. */
    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        /* Skip if the boot replay already reported this window. */
        if (!s_window_evt_sent && mt_at_event(MT_EVT_COMMISSION_WINDOW_OPEN, nullptr)) {
            s_window_evt_sent = true;
        }
        break;
    case DeviceEventType::kCommissioningWindowClosed:
        /* CHIP raises this whenever it stops advertising for PASE, which is
         * three different moments: a commissioner established a session (the
         * window still open, only paused to new commissioners), the window
         * genuinely ended (completion, timeout, or the 20-attempt limit), and
         * a failed open cleaning up a window that never existed. The host
         * contract is one +MTEVT:4 per reported +MTEVT:0, at the moment the
         * window is really gone, so gate on both. The window manager is asked
         * directly rather than via mt_matter_state(): this callback runs on
         * the CHIP task and must not take the stack lock. */
        {
            const bool still_open = chip::Server::GetInstance()
                .GetCommissioningWindowManager().IsCommissioningWindowOpen();
            ESP_LOGI(TAG, "Commissioning window closed event (reported=%d, still_open=%d)",
                     (int)s_window_evt_sent, (int)still_open);
            if (s_window_evt_sent && !still_open) {
                /* Cleared even if the URC is masked out: the window is gone,
                 * and the next one must be reported as a fresh +MTEVT:0. */
                s_window_evt_sent = false;
                mt_at_event(MT_EVT_COMMISSION_WINDOW_CLOSED, nullptr);
            }
        }
        break;
    case DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        mt_at_event(MT_EVT_COMMISSION_SESSION_STARTED, nullptr);
        break;
    case DeviceEventType::kCommissioningSessionStopped:
        mt_at_event(MT_EVT_COMMISSION_SESSION_STOPPED, nullptr);
        break;
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        /* Whatever was unreachable is now reachable: commissioning on this
         * transport is exactly what provisions it (spec 3.12.1). Clearing the
         * latch stops AT+MTNET? reporting a mismatch that no longer exists. */
        s_transport_mismatch = false;
        mt_at_event(MT_EVT_COMMISSION_COMPLETE, nullptr);
        break;
    case DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed (fail-safe timer expired)");
        mt_at_event(MT_EVT_FAIL_SAFE_EXPIRED, nullptr);
        break;

    /* Fabric. */
    case DeviceEventType::kFabricWillBeRemoved:
        mt_at_event(MT_EVT_FABRIC_WILL_BE_REMOVED, nullptr);
        break;
    case DeviceEventType::kFabricRemoved:
        mt_at_event(MT_EVT_FABRIC_REMOVED, nullptr);
        break;
    case DeviceEventType::kFabricCommitted:
        mt_at_event(MT_EVT_FABRIC_COMMITTED, nullptr);
        break;
    case DeviceEventType::kFabricUpdated:
        mt_at_event(MT_EVT_FABRIC_UPDATED, nullptr);
        break;

    /* Connectivity. The detail field carries up/down where CHIP gives it. */
    case DeviceEventType::kWiFiConnectivityChange:
        mt_at_event(MT_EVT_WIFI_CONNECTIVITY,
                    event->WiFiConnectivityChange.Result == kConnectivity_Established ? "1" : "0");
        break;
    case DeviceEventType::kInternetConnectivityChange:
        mt_at_event(MT_EVT_INTERNET_CONNECTIVITY,
                    event->InternetConnectivityChange.IPv4 == kConnectivity_Established ? "1" : "0");
        break;
    case DeviceEventType::kInterfaceIpAddressChanged:
        mt_at_event(MT_EVT_INTERFACE_IP_CHANGED, nullptr);
        break;
    case DeviceEventType::kOperationalNetworkStarted:
        mt_at_event(MT_EVT_OPERATIONAL_NETWORK_STARTED, nullptr);
        break;
    case DeviceEventType::kDnssdInitialized:
        mt_at_event(MT_EVT_DNSSD_INITIALIZED, nullptr);
        break;
    case DeviceEventType::kServerReady:
        mt_at_event(MT_EVT_SERVER_READY, nullptr);
        break;

    /* BLE. */
    case DeviceEventType::kCHIPoBLEConnectionEstablished:
        mt_at_event(MT_EVT_BLE_CONNECTED, nullptr);
        break;
    case DeviceEventType::kCHIPoBLEConnectionClosed:
        mt_at_event(MT_EVT_BLE_DISCONNECTED, nullptr);
        break;
    case DeviceEventType::kCHIPoBLEAdvertisingChange:
        mt_at_event(MT_EVT_BLE_ADVERTISING_CHANGE, nullptr);
        break;
    case DeviceEventType::kBLEDeinitialized:
        /* Free heap here, against the figure logged at the end of app_main, is
         * the cost of CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=y: what the BLE
         * stack hands back once it is torn down, and therefore what defect D1's
         * candidate fix (keeping BLE resident so AT+MTCOMMISSION can actually
         * reopen a window) would give up permanently. Kept as a standing
         * diagnostic rather than removed after the measurement: the number
         * moves with every SDK bump, and it is the number that decision rests
         * on. */
        ESP_LOGI(TAG, "BLE deinitialized, free heap %u (was %u at startup, reclaimed %d)",
                 (unsigned)esp_get_free_heap_size(), (unsigned)s_heap_at_startup,
                 (int)esp_get_free_heap_size() - (int)s_heap_at_startup);
        mt_at_event(MT_EVT_BLE_DEINITIALIZED, nullptr);
        break;

    /* Misc. */
    case DeviceEventType::kOtaStateChanged:
        mt_at_event(MT_EVT_OTA_STATE_CHANGED, nullptr);
        break;
    case DeviceEventType::kBindingsChangedViaCluster:
        mt_at_event(MT_EVT_BINDINGS_CHANGED, nullptr);
        break;
    case DeviceEventType::kTimeSyncChange:
        mt_at_event(MT_EVT_TIME_SYNC_CHANGE, nullptr);
        break;

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    case DeviceEventType::kThreadConnectivityChange:
        mt_at_event(MT_EVT_THREAD_CONNECTIVITY,
                    event->ThreadConnectivityChange.Result == kConnectivity_Established ? "1" : "0");
        /* Thread interface down: forget the last reported role token, so a
         * genuine return to that same role after the interface comes back is
         * reported rather than swallowed as a repeat (design spec 2.2).
         *
         * This is the reset point because it is the only one CHIP actually
         * raises. kThreadInterfaceStateChange looks like the natural hook and
         * is not: CHIPDeviceEvent.h:189 declares it, and nothing anywhere in
         * connectedhomeip posts it, so a reset hung there would be dead code.
         * Ordering is safe: GenericThreadStackManagerImpl_OpenThread.hpp:201
         * posts this event from its own kThreadStateChange handling, so it is
         * queued behind the state change that caused it and the reset lands
         * after that transition's own bit-28 emit, never before it. */
        if (event->ThreadConnectivityChange.Result == kConnectivity_Lost) {
            s_last_role_tok_valid = false;
        }
        break;
    case DeviceEventType::kThreadStateChange:
        mt_at_event(MT_EVT_THREAD_STATE_CHANGE, nullptr);
        /*
         * Bit 28, 0.11.0 (design spec 2.2). CHIPDeviceEvent.h:522 declares
         * the ThreadStateChange struct's four independent bool:1 bits
         * (RoleChanged, AddressChanged, NetDataChanged, ChildNodesChanged),
         * and GenericThreadStackManagerImpl_OpenThread.hpp:114 populates
         * RoleChanged as (flags & OT_CHANGED_THREAD_ROLE) != 0. Gating on it
         * means this fires on a routing-role transition only, never on
         * address, network-data or child-table churn.
         *
         * The gate is a bench fix, not a design flourish (defect A): the
         * first cut of this block described the gate in this comment but
         * never tested the bit, and registering one test prefix on the
         * border router (pure netdata churn, no role transition possible)
         * produced four +MTEVT:25/+MTEVT:28,ROUTER pairs while the role
         * never left ROUTER.
         *
         * The RoleChanged bit alone is still not enough, hence the
         * s_last_role_tok cache: CHIP's bit tracks the OpenThread role while
         * this payload carries the Matter token, and that mapping is
         * many-to-one (see the cache's own comment near the top of this
         * file). Same-token repeats are suppressed here; the cache is reset
         * on a Thread connectivity loss above, so a genuine return to a
         * previously reported role is always reported again.
         *
         * MT_EVT_THREAD_STATE_CHANGE above stays exactly as it was, coarse
         * and unconditional; this is additive, not a replacement.
         *
         * No ChipStackLock here: this callback already runs ON the CHIP
         * event-loop task, the same reasoning the door lock adjudication
         * block states explicitly for app_event_cb() ("NO ChipStackLock in
         * this block, unlike every mt_matter_* bridge function below: these
         * callbacks already run ON the CHIP event-loop task ..., the same
         * reasoning as app_event_cb()", above mt_door_lock_adjudicate()).
         * mt_matter_thread_info() cannot be reused to read the role here for
         * that same reason: it opens its own ChipStackLock for its other
         * caller (the AT parser task, via cmd_mtthread), and CHIP's
         * PlatformManager lock is not documented reentrant, so taking it a
         * second time from a task that already (implicitly) holds it risks a
         * self-deadlock rather than a race. mt_thread_read_role() below
         * mirrors just the RoutingRole probe inside mt_matter_thread_info(),
         * with no lock of its own, since role is the only field this event's
         * payload carries.
         */
        if (event->ThreadStateChange.RoleChanged) {
            uint8_t role = 0;
            if (mt_thread_read_role(&role) &&
                (!s_last_role_tok_valid || role != s_last_role_tok)) {
                char role_tok[8];
                const char *tok = mt_thread_role_name(role);
                if (tok == nullptr) {
                    /* Same decimal-degrades-rather-than-lies fallback
                     * cmd_mtthread uses for AT+MTTHREAD? (mt_at.c, design
                     * spec 2.1): an out-of-range SDK value still reaches the
                     * host instead of silently dropping the one notification
                     * of this transition. */
                    snprintf(role_tok, sizeof(role_tok), "%u", (unsigned)role);
                    tok = role_tok;
                }
                /* Cached whether or not the host has bit 28 unmasked:
                 * mt_at_event()'s mask test decides delivery, not what the
                 * firmware believes it last told the host about the role.
                 * Keying the cache on the return value instead would make a
                 * mask flip resend a role the host already knows. */
                s_last_role_tok = role;
                s_last_role_tok_valid = true;
                mt_at_event(MT_EVT_THREAD_ROLE_CHANGED, tok);
            }
        }
        break;
    case DeviceEventType::kThreadInterfaceStateChange:
        mt_at_event(MT_EVT_THREAD_IF_STATE_CHANGE, nullptr);
        break;
#endif

    default:
        break;
    }
}

/*
 * Identify cluster (no physical indicator on this board yet). Surfaced to the
 * host as +MTIDENT so a sketch can blink whatever it likes: this backs the
 * per-endpoint onIdentify() callback in the reference API.
 */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify: type=%u effect=%u variant=%u", type, effect_id, effect_variant);

    /* START/STOP map to the on/off form; EFFECT is not carried (the reference
     * API's callback is a plain bool too). */
    if (type == identification::START || type == identification::STOP) {
        char line[32];
        snprintf(line, sizeof(line), "+MTIDENT:%u,%d", endpoint_id,
                 type == identification::START ? 1 : 0);
        mt_at_urc(line);
    }
    return ESP_OK;
}

/*
 * Every attribute update passes through here. B2 has no physical driver; it just
 * logs controller-driven writes. B4 turns POST_UPDATE into a +MTATTR URC and
 * lets AT+MTATTR sets drive attribute::update() from the host side.
 */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    /* Surface attribute changes on any endpoint we built to the host as a
     * +MTATTR URC (so a controller-driven toggle is visible over AT). The
     * root endpoint (0) is skipped to keep boot-time init noise off the link. */
    if (type == attribute::POST_UPDATE && endpoint_id != 0) {
        int64_t v;
        if (attr_val_to_i64(val, &v)) {
            /* Worst case fits: "+MTATTR:" + 5 + 10 + 10 + 20 digits + commas
             * + NUL = 57 bytes. */
            char line[64];
            if (attr_val_is_unsigned(val->type)) {
                snprintf(line, sizeof(line), "+MTATTR:%u,%lu,%lu,%llu", endpoint_id,
                         (unsigned long)cluster_id, (unsigned long)attribute_id,
                         (unsigned long long)(uint64_t)v);
            } else {
                snprintf(line, sizeof(line), "+MTATTR:%u,%lu,%lu,%lld", endpoint_id,
                         (unsigned long)cluster_id, (unsigned long)attribute_id,
                         (long long)v);
            }
            mt_at_urc(line);
        }
    }
    return ESP_OK;
}

/* ---- C-linkage bridge for the AT+MT command handlers (see mt_matter.h) ---- */

/*
 * EVERY function below runs on the AT parser task, not on the CHIP event loop,
 * and therefore MUST hold the CHIP stack lock while it touches CHIP.
 *
 * This is not defensive tidiness. Without it, AT+MTCOMMISSION=300 killed the
 * device outright on hardware:
 *
 *   E chip[DL]: Chip stack locking error at 'SystemLayerImplFreeRTOS.cpp:55'.
 *               Code is unsafe/racy
 *   E chip[-]: chipDie chipDie chipDie
 *
 * OpenBasicCommissioningWindow() arms a timer, and SystemLayerImplFreeRTOS
 * asserts that the caller holds the lock before touching the timer list. CHIP
 * does not return an error for this; it calls chipDie() and the device aborts.
 *
 * The read-only accessors were just as unlocked and did not crash, which is
 * worse rather than better: FabricCount() and IsThreadAttached() race against
 * the CHIP task mutating the same state, so they fail rarely and silently
 * instead of loudly and every time. Locking them costs a mutex per AT command,
 * against a link that carries a handful of commands a second at most.
 *
 * Do NOT use this guard in app_event_cb(): that callback already runs ON the
 * CHIP task, and taking the lock there would be at best redundant and at worst
 * a deadlock. Its only job is mt_at_event(), which touches the UART, not CHIP.
 */
namespace {
class ChipStackLock {
public:
    ChipStackLock()  { chip::DeviceLayer::PlatformMgr().LockChipStack(); }
    ~ChipStackLock() { chip::DeviceLayer::PlatformMgr().UnlockChipStack(); }
    ChipStackLock(const ChipStackLock &) = delete;
    ChipStackLock &operator=(const ChipStackLock &) = delete;
};
}  // namespace

extern "C" int mt_matter_fabric_count(void)
{
    ChipStackLock lock;
    return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

extern "C" int mt_matter_state(void)
{
    ChipStackLock lock;
    if (chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen()) {
        return MT_STATE_COMMISSIONING;
    }
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
        return MT_STATE_OPERATIONAL;
    }
    return MT_STATE_UNINIT;
}

extern "C" int mt_matter_open_commissioning(int timeout_s)
{
    ChipStackLock lock;
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager()
        .OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(timeout_s),
                                      chip::CommissioningWindowAdvertisement::kAllSupported);
    return (err == CHIP_NO_ERROR) ? 0 : -1;
}

extern "C" int mt_matter_onboarding_codes(char *qr, size_t qr_len, char *manual, size_t manual_len)
{
    ChipStackLock lock;
    chip::MutableCharSpan qrSpan(qr, qr_len);
    chip::MutableCharSpan manSpan(manual, manual_len);
    chip::RendezvousInformationFlags rendezvous(chip::RendezvousInformationFlag::kBLE);
    if (GetQRCode(qrSpan, rendezvous) != CHIP_NO_ERROR ||
        GetManualPairingCode(manSpan, rendezvous) != CHIP_NO_ERROR) {
        return -1;
    }
    /* MutableCharSpan is not guaranteed null-terminated; terminate in bounds. */
    qr[qrSpan.size() < qr_len ? qrSpan.size() : qr_len - 1] = '\0';
    manual[manSpan.size() < manual_len ? manSpan.size() : manual_len - 1] = '\0';
    return 0;
}

extern "C" void mt_matter_factory_reset(void)
{
    ChipStackLock lock;
    esp_matter::factory_reset();
}

extern "C" int mt_matter_transport_mismatch(void)
{
    /* No ChipStackLock: this reads a plain bool latched by app_main and
     * cleared on kCommissioningComplete, and touches nothing in CHIP. */
    return s_transport_mismatch ? 1 : 0;
}

extern "C" int mt_matter_net_info(int *transport, int *enabled, int *connected)
{
    if (!transport || !enabled || !connected) {
        return -1;
    }
    ChipStackLock lock;
#if MT_COMBINED_IMAGE
    /*
     * Both stacks are compiled in here, so CHIP_DEVICE_CONFIG_ENABLE_THREAD
     * alone cannot answer "which one is active": it is 1 on this image
     * regardless of the boot-time choice. Dispatch on the latch instead
     * (spec 3.12: AT+MTNET? reports the ACTIVE transport on the combined
     * image), the same value node::create()'s feature map and the dormant-
     * cluster scrub in app_main used.
     */
    if (mt_active_transport_is_thread()) {
        *transport = MT_NET_THREAD;
        *enabled   = 1;
        *connected = chip::DeviceLayer::ConnectivityMgr().IsThreadAttached() ? 1 : 0;
    } else {
        *transport = MT_NET_WIFI;
        *enabled   = 1;
        *connected = chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected() ? 1 : 0;
    }
#elif CHIP_DEVICE_CONFIG_ENABLE_THREAD
    *transport = MT_NET_THREAD;
    *enabled   = 1;
    *connected = chip::DeviceLayer::ConnectivityMgr().IsThreadAttached() ? 1 : 0;
#else
    *transport = MT_NET_WIFI;
    *enabled   = 1;
    *connected = chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected() ? 1 : 0;
#endif
    return 0;
}

/* The live composition, filled in by the boot rebuild in app_main. */
static uint32_t s_live_devtype[MT_COMP_MAX_ENDPOINTS];
static uint16_t s_live_ep_id[MT_COMP_MAX_ENDPOINTS];
static uint8_t  s_live_variant[MT_COMP_MAX_ENDPOINTS];
static uint8_t  s_live_parent[MT_COMP_MAX_ENDPOINTS];
static uint16_t s_live_count = 0;

extern "C" uint16_t mt_matter_endpoint_count(void)
{
    return s_live_count;
}

extern "C" int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id, uint8_t *variant,
                                       uint8_t *parent_idx)
{
    if (index >= s_live_count || !devtype || !ep_id || !variant || !parent_idx) {
        return -1;
    }
    *devtype    = s_live_devtype[index];
    *ep_id      = s_live_ep_id[index];
    *variant    = s_live_variant[index];
    *parent_idx = s_live_parent[index];
    return 0;
}

extern "C" void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id, uint8_t variant, uint8_t parent_idx)
{
    if (s_live_count >= MT_COMP_MAX_ENDPOINTS) {
        return;
    }
    s_live_devtype[s_live_count] = devtype;
    s_live_ep_id[s_live_count]   = ep_id;
    s_live_variant[s_live_count] = variant;
    s_live_parent[s_live_count]  = parent_idx;
    s_live_count++;
}

/*
 * ---- air quality (C1b, bug B139) -------------------------------------
 *
 * AT+MTATTR=<ep>,91,0 (AirQuality cluster, AirQuality attribute) answered
 * bare ERROR both directions. Root cause: esp-matter creates the AirQuality
 * attribute ATTRIBUTE_FLAG_MANAGED_INTERNALLY (esp_matter_attribute.cpp:2283-
 * 2287, air_quality::attribute::create_air_quality()), which only makes
 * esp_matter::attribute::get() find the attribute for a presence check;
 * nothing in esp-matter or ember ever serves its value. CHIP's
 * air-quality-server is Instance/AttributeAccessInterface based
 * (src/app/clusters/air-quality-server/air-quality-server.h): the
 * application has to construct and Init() a
 * chip::app::Clusters::AirQuality::Instance per endpoint before anything
 * answers a read, and mutate the value through Instance::UpdateAirQuality(),
 * never through esp_matter's own attribute store. This is the same shape the
 * C3 SupportedTemperatureLevels delegate above works around for the same
 * esp-matter gap. The parity census (AT_MT_SPEC.md: "AT+MTATTR therefore
 * covers essentially the whole parity surface") missed it because a census
 * of esp-matter's attribute table cannot see a cluster whose real server
 * lives entirely in CHIP with no esp-matter counterpart at all.
 *
 * Relevant CHIP signatures (air-quality-server.h):
 *
 *   Instance(EndpointId aEndpointId, BitMask<Feature> aFeature);
 *   CHIP_ERROR Init();
 *   Protocols::InteractionModel::Status UpdateAirQuality(AirQualityEnum aNewAirQuality);
 *   AirQualityEnum GetAirQuality();
 *
 * FeatureMap authority: Instance::Read() (air-quality-server.cpp) handles
 * both Attributes::AirQuality::Id and Attributes::FeatureMap::Id itself,
 * ahead of ember's attribute store. Once an Instance is registered for an
 * endpoint, ITS mFeature bitmask is what a controller reads back for
 * FeatureMap, not the ember global attribute mk_air_quality_sensor()'s
 * cluster::air_quality::feature::*::add() calls set up (5888ae1). Both are
 * kept: the ember adds still gate which AirQualityEnum values esp-matter's
 * own data model believes are legal (used nowhere at runtime once the
 * Instance exists, but harmless to leave), and the Instance below is
 * constructed from mt_air_quality_feature_mask() (mt_matter.h), the same
 * accessor mk_air_quality_sensor() (mt_devtypes.cpp) reads to decide which
 * ember adds to make, so the two cannot drift apart (fix round 1: they
 * used to be two independently-hardcoded lists of the same four features).
 *
 * Read path: GetAirQuality() is a real public accessor (unlike the
 * temperature-levels delegate above, which has none), so AT reads call it
 * directly; no shadow value is kept.
 *
 * Storage: Instance has no default constructor (BitMask<Feature> must be
 * supplied at construction), so a plain static array of Instance is not an
 * option the way a POD struct array would be. Raw aligned storage plus
 * placement-new gives static allocation with no heap churn.
 */
struct mt_air_quality_entry_t {
    bool used;
    uint16_t ep;
    chip::app::Clusters::AirQuality::Instance *instance;
};
static mt_air_quality_entry_t s_air_quality[MT_COMP_MAX_ENDPOINTS];

alignas(chip::app::Clusters::AirQuality::Instance)
    static uint8_t s_air_quality_storage[MT_COMP_MAX_ENDPOINTS][sizeof(chip::app::Clusters::AirQuality::Instance)];

static mt_air_quality_entry_t *mt_air_quality_find(uint16_t ep)
{
    for (auto &e : s_air_quality) {
        if (e.used && e.ep == ep) {
            return &e;
        }
    }
    return nullptr;
}

/*
 * Fix round 1: the single source of truth for the enabled AirQuality
 * features, declared in mt_matter.h next to mt_matter_lock_source_max()'s
 * accessor precedent. mk_air_quality_sensor() (mt_devtypes.cpp) reads the
 * same bits to decide which ember cluster::air_quality::feature::*::add()
 * calls to make, so the ember feature map and this Instance's BitMask
 * cannot drift apart the way two hardcoded lists could.
 */
extern "C" uint32_t mt_air_quality_feature_mask(void)
{
    using chip::app::Clusters::AirQuality::Feature;
    return static_cast<uint32_t>(Feature::kFair) | static_cast<uint32_t>(Feature::kModerate) |
           static_cast<uint32_t>(Feature::kVeryPoor) | static_cast<uint32_t>(Feature::kExtremelyPoor);
}

/*
 * Walk the live composition and construct an Instance for every endpoint
 * that carries an AirQuality cluster. Called from app_main after the
 * composition rebuild and before esp_matter::start(), the same window the
 * SupportedTemperatureLevels delegate registers in (see the comment there):
 * Instance::Init() calls emberAfContainsServer() and the
 * AttributeAccessInterfaceRegistry, both of which need the composition's
 * clusters to already exist, but nothing here touches the CHIP event loop
 * or arms a timer, so it is safe ahead of start() for the same reason the
 * dormant-cluster scrub and the delegate registration are.
 */
static void mt_air_quality_register_all(void)
{
    using chip::app::Clusters::AirQuality::Feature;
    using chip::app::Clusters::AirQuality::Instance;
    chip::BitMask<Feature> features(mt_air_quality_feature_mask());

    for (uint16_t i = 0; i < s_live_count && i < MT_COMP_MAX_ENDPOINTS; i++) {
        uint16_t ep = s_live_ep_id[i];
        if (esp_matter::cluster::get(ep, chip::app::Clusters::AirQuality::Id) == nullptr) {
            continue;
        }
        Instance *inst = new (&s_air_quality_storage[i]) Instance(ep, features);
        if (inst->Init() != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "AirQuality instance init failed for endpoint %u", ep);
            inst->~Instance();
            continue;
        }
        s_air_quality[i].used     = true;
        s_air_quality[i].ep       = ep;
        s_air_quality[i].instance = inst;
    }
}

/*
 * Walk endpoint -> cluster -> attribute so a failure can say WHICH level was
 * missing. esp_matter::attribute::get_val() collapses all three into one error,
 * which leaves the host unable to tell a typo in the endpoint from a typo in
 * the cluster.
 */
static mt_attr_result_t attr_locate(uint16_t ep, uint32_t cluster, uint32_t attr,
                                    esp_matter::attribute_t **out)
{
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, cluster) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    esp_matter::attribute_t *a = esp_matter::attribute::get(ep, cluster, attr);
    if (a == nullptr) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    *out = a;
    return MT_ATTR_OK;
}

extern "C" int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                                   bool *is_unsigned)
{
    /* Same reasoning as the bridge functions above, and the same lock. These
     * two never crashed on hardware, which made them look safe and is exactly
     * why they are worth fixing: esp_matter does no locking of its own (there
     * is none in its attribute.cpp), so the walk of the data model and the
     * report that update() triggers both race the CHIP task. */
    ChipStackLock lock;

    /* B139: AirQuality/AirQuality (91/0) is not on the generic path at all.
     * See the registry comment ahead of mt_air_quality_register_all() above. */
    if (cluster == chip::app::Clusters::AirQuality::Id &&
        attr == chip::app::Clusters::AirQuality::Attributes::AirQuality::Id) {
        if (esp_matter::endpoint::get(ep) == nullptr) {
            return MT_ATTR_ERR_ENDPOINT;
        }
        if (esp_matter::cluster::get(ep, chip::app::Clusters::AirQuality::Id) == nullptr) {
            return MT_ATTR_ERR_CLUSTER;
        }
        mt_air_quality_entry_t *e = mt_air_quality_find(ep);
        if (e == nullptr) {
            /* Cluster present but no registered Instance: cannot happen in
             * practice, since mt_air_quality_register_all() walks every live
             * endpoint carrying this cluster at boot. Defensive only. */
            return MT_ATTR_ERR_FAILED;
        }
        if (is_unsigned != NULL) {
            *is_unsigned = true;  /* AirQualityEnum is an enum8 */
        }
        *out = static_cast<int64_t>(e->instance->GetAirQuality());
        return MT_ATTR_OK;
    }

    esp_matter::attribute_t *a = nullptr;
    mt_attr_result_t r = attr_locate(ep, cluster, attr, &a);
    if (r != MT_ATTR_OK) {
        return r;
    }

    esp_matter_attr_val_t val;
    esp_err_t gerr = esp_matter::attribute::get_val(a, &val);
    /* B265: an attribute the SDK registered as ESP_MATTER_VAL_TYPE_ARRAY is
     * refused by get_val() BEFORE the read dispatch, so no AttributeAccess-
     * Interface behind it is ever consulted and the failure is unconditional.
     * That is exactly what +MTERR:5 means ("not an integer-valued
     * attribute"), so map it rather than let it collapse into the bare ERROR
     * MT_ATTR_ERR_FAILED renders. Found on the C2 bench reading
     * MeterIdentification PowerThreshold (a struct, which esp-matter has no
     * esp_matter_attr_val_t representation for and registers as ARRAY):
     * AT+MTATTR answered a bare ERROR while AT_MT_SPEC 3.9 claimed +MTERR:5,
     * and a bare ERROR could not be told apart from a genuinely broken read.
     *
     * ESP_ERR_NOT_SUPPORTED sources reachable on THIS leg, enumerated the way
     * the write leg's DE270 comment below enumerates its own (esp-matter
     * release/v1.5.1, this checkout's pin). mt_matter_attr_read() calls the
     * attribute-handle overload get_val(attribute_t*, val)
     * (esp_matter_data_model.cpp:986), which does exactly two things:
     *   - get_path_from_attribute_handle() (:970), whose only failure comes
     *     from find_cluster_and_endpoint_id_for_internally_managed_attribute()
     *     and is ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE /
     *     ESP_ERR_NOT_FOUND. Never ESP_ERR_NOT_SUPPORTED.
     *   - the (endpoint, cluster, attribute) overload (:927), whose ONLY
     *     textual ESP_ERR_NOT_SUPPORTED is the ARRAY guard at :931. Its other
     *     failure arms are ESP_ERR_INVALID_ARG (null val, unknown attribute),
     *     ESP_ERR_NO_MEM (scoped buffer) and ESP_FAIL (a failing
     *     provider::ReadAttribute(), and every get_val_from_tlv_data()
     *     failure). None of them is ESP_ERR_NOT_SUPPORTED.
     * get_val_internal() (:1001) also returns ESP_ERR_NOT_SUPPORTED, for a
     * managed-internally attribute, but nothing on this path calls it: it is
     * a separate entry point this bridge never uses. So the ARRAY guard is
     * the single reachable source and the mapping collapses no two meanings
     * into one code.
     *
     * This cannot alter +MTERR:11. That code is produced only by the write
     * leg's set_val() mapping below, for an attribute that is MANAGED_
     * INTERNALLY without WRITABLE; such an attribute reads through
     * ReadAttribute() perfectly well and is never ARRAY-typed, so no
     * attribute changes which of the two codes it answers. */
    if (gerr == ESP_ERR_NOT_SUPPORTED) {
        /* Keep the is_unsigned invariant below true for this arm too: the
         * caller (cmd_mtattr) lets MT_ATTR_ERR_TYPE fall through to a write
         * and parses <val> using this flag. An ARRAY attribute has no
         * signedness at all; false selects the signed parse, and the write
         * then fails on its own get_val() for the same reason. */
        if (is_unsigned != NULL) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_TYPE;
    }
    if (gerr != ESP_OK) {
        return MT_ATTR_ERR_FAILED;
    }
    /* Set before the conversion, so the flag is valid even on
     * MT_ATTR_ERR_TYPE: the write handler fetches the signedness through
     * this function BEFORE parsing its value, and a null nullable converts
     * to nothing while still having a definite signedness. */
    if (is_unsigned != NULL) {
        *is_unsigned = attr_val_is_unsigned(val.type);
    }
    return attr_val_to_i64(&val, out) ? MT_ATTR_OK : MT_ATTR_ERR_TYPE;
}

extern "C" int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t in, bool notify)
{
    ChipStackLock lock;

    /* B139: same special case as the read side above. `notify` is not
     * honored here: mode 0 (attribute::set_val(), no report) has no
     * counterpart in the Instance API. UpdateAirQuality() always calls
     * MatterReportingAttributeChangeCallback() itself (air-quality-
     * server.cpp) with no silent-set alternative. Hardware-verified
     * consequence, the OPPOSITE of what an early draft of this comment
     * guessed: that report feeds the fabric's reporting engine only. The
     * host-side +MTATTR echo URC comes from esp-matter's attribute update
     * callback, which managed-internally attributes bypass, so writes to
     * this attribute never echo a URC in ANY mode; controllers still see
     * the change through their subscriptions, and the host has the OK.
     * Documented here and in AT_MT_SPEC 3.9 rather than worked around:
     * reaching into the Instance's private state would fight the class
     * instead of using it. */
    if (cluster == chip::app::Clusters::AirQuality::Id &&
        attr == chip::app::Clusters::AirQuality::Attributes::AirQuality::Id) {
        if (esp_matter::endpoint::get(ep) == nullptr) {
            return MT_ATTR_ERR_ENDPOINT;
        }
        if (esp_matter::cluster::get(ep, chip::app::Clusters::AirQuality::Id) == nullptr) {
            return MT_ATTR_ERR_CLUSTER;
        }
        mt_air_quality_entry_t *e = mt_air_quality_find(ep);
        if (e == nullptr) {
            return MT_ATTR_ERR_FAILED;
        }
        /* AirQualityEnum runs kUnknown(0)..kExtremelyPoor(6); anything past
         * that is a bad parameter on a valid, existing attribute (+MTERR:1,
         * MT_ATTR_ERR_VALUE), not a type/lookup failure (+MTERR:5,
         * MT_ATTR_ERR_TYPE would be the wrong class here: that code means
         * "this attribute is not the kind AT+MTATTR can carry at all",
         * which is not the case for AirQuality). Fix round 1: an earlier
         * version of this returned MT_ATTR_ERR_TYPE for this check, which
         * mapped to the wrong +MTERR code; mt_attr_result_t/mt_at.c gained
         * MT_ATTR_ERR_VALUE for exactly this case rather than mt_at.c's
         * wire grammar or handlers changing at all. */
        if (in < 0 || in > 6) {
            return MT_ATTR_ERR_VALUE;
        }
        auto status = e->instance->UpdateAirQuality(
            static_cast<chip::app::Clusters::AirQuality::AirQualityEnum>(in));
        return (status == chip::Protocols::InteractionModel::Status::Success)
                   ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
    }

    esp_matter::attribute_t *a = nullptr;
    mt_attr_result_t r = attr_locate(ep, cluster, attr, &a);
    if (r != MT_ATTR_OK) {
        return r;
    }

    /* Read the current value to learn the attribute's type, then set the new
     * integer into the matching union field and push it. */
    esp_matter_attr_val_t val;
    esp_err_t gerr = esp_matter::attribute::get_val(a, &val);
    /* B265, the write leg's half. Same single reachable ESP_ERR_NOT_SUPPORTED
     * source as the read leg (get_val()'s ARRAY guard, enumerated in
     * mt_matter_attr_read() above): this is the SAME call, so the same
     * enumeration holds. Mapped for the same reason and so that AT_MT_SPEC
     * 3.8's "String/array/float attributes are not supported over this
     * command (+MTERR:5)" is true in BOTH directions: without this, reading
     * an ARRAY attribute answered +MTERR:5 while writing the identical
     * attribute answered a bare ERROR, and a host had no way to see that the
     * two failures have one cause. Distinct from the set_val() mapping at the
     * bottom of this function (+MTERR:11): that one fires for an attribute
     * that IS integer-carrying and simply not ours to write, and it is
     * unreachable for an ARRAY attribute, which never gets past this line. */
    if (gerr == ESP_ERR_NOT_SUPPORTED) {
        return MT_ATTR_ERR_TYPE;
    }
    if (gerr != ESP_OK) {
        return MT_ATTR_ERR_FAILED;
    }
    if (!i64_to_attr_val(&val, in)) {
        /* Two failures share the false: a value outside the width of an
         * integer-carrying attribute is a bad parameter (+MTERR:1 via
         * MT_ATTR_ERR_VALUE), a type this command cannot carry at all keeps
         * its historical +MTERR:5 (MT_ATTR_ERR_TYPE). */
        return (attr_val_is_unsigned(val.type) || attr_val_is_signed_int(val.type))
                   ? MT_ATTR_ERR_VALUE : MT_ATTR_ERR_TYPE;
    }

    esp_err_t err = notify ? esp_matter::attribute::update(ep, cluster, attr, &val)
                           : esp_matter::attribute::set_val(a, &val);
    if (err == ESP_OK) {
        return MT_ATTR_OK;
    }
    /* Same-value fix: ESP_ERR_NOT_FINISHED is set_val_internal()'s ONLY
     * return for it (esp_matter_data_model.cpp:741-742, val_compare()
     * against the attribute's own current value). It IS reachable from a
     * second place on this call's own path, esp_matter_data_model_provider.
     * cpp's WriteAttribute() (for a MANAGED_INTERNALLY+WRITABLE attribute,
     * via set_val()'s branch at esp_matter_data_model.cpp:1097-1098 into
     * set_val_via_write_attribute()), but that place is harmless: it treats
     * ESP_ERR_NOT_FINISHED as Status::Success internally
     * (esp_matter_data_model_provider.cpp:419-429) rather than propagating
     * the raw code, so set_val_via_write_attribute() only ever returns
     * ESP_OK or ESP_FAIL to its caller, never ESP_ERR_NOT_FINISHED itself.
     * esp_matter_ember_stubs.cpp's equivalent (the ember IM write path CHIP
     * itself uses for a controller-driven write) is a genuinely separate
     * code path, not reachable from this bridge's set_val()/update() calls
     * at all. Either way, the attribute already holds exactly the value the
     * host asked for, so by any definition that matters to a host this
     * write succeeded: answer MT_ATTR_OK rather than falling through to the
     * default bare ERROR. This is also why notify=true never showed the
     * bug: update_or_report() (esp_matter_attribute_utils.cpp:649) already
     * absorbs the identical ESP_ERR_NOT_FINISHED into ESP_OK before
     * returning, on the theory that there is nothing to report; notify=false
     * calls set_val() directly with no such absorption, so the two modes
     * disagreed on the exact same no-op until now. */
    if (err == ESP_ERR_NOT_FINISHED) {
        return MT_ATTR_OK;
    }
    /* B264 fix: ember's own min/max bounds check
     * (compare_attr_val_with_bounds() inside set_val_internal(),
     * esp_matter_data_model.cpp) is the ONLY failure reachable on this call
     * that returns ESP_ERR_INVALID_ARG, given the checks already passed
     * above: attr_locate() proved the attribute exists, and
     * i64_to_attr_val() proved val's type and width already match it, so
     * set_val()'s own "attribute not found" (ESP_ERR_NOT_FOUND) and "type
     * mismatch" (ESP_ERR_INVALID_ARG, esp_matter_data_model.cpp:1082/1087)
     * guards cannot fire here. A managed-internally (Instance-owned)
     * attribute never reaches the bounds check at all: it takes the
     * WriteAttribute() detour instead, which collapses every failure to
     * ESP_FAIL (esp_matter_data_model.cpp's set_val_via_write_attribute()),
     * or returns ESP_ERR_NOT_SUPPORTED when it is not writable at all
     * (esp_matter_data_model.cpp:1103). Neither of those is
     * ESP_ERR_INVALID_ARG, so an Instance-owned attribute is unaffected by
     * this bounds fix; the ESP_ERR_NOT_SUPPORTED case is mapped separately,
     * below, to MT_ATTR_ERR_READONLY (+MTERR:11, DE270). A second SDK
     * source of ESP_ERR_INVALID_ARG exists on the notify=true leg
     * only, update_or_report()'s own attribute-handle lookup
     * (esp_matter_attribute_utils.cpp:637-638); it is unreachable here
     * because attr_locate() above already resolved this exact ep/cluster/
     * attr triple under the same ChipStackLock, so the identical lookup
     * inside update_or_report() cannot fail. Also note:
     * compare_attr_val_with_bounds()'s default arm (an attribute value type
     * the bounds comparison does not special-case) also returns a nonzero
     * comparison, i.e. ESP_ERR_INVALID_ARG here, for ESP_MATTER_VAL_TYPE_
     * INTEGER/NULLABLE_INTEGER/NULLABLE_BOOLEAN specifically; this firmware
     * never creates a MIN_MAX-bounded attribute of any of those types
     * today, but a future device type that did would see an in-range value
     * on such an attribute wrongly answer +MTERR:1. */
    if (err == ESP_ERR_INVALID_ARG) {
        return MT_ATTR_ERR_VALUE;
    }
    /* DE270: an Instance-served (managed-internally) attribute that is not
     * itself writable. set_val() at esp_matter_data_model.cpp:1075-1103
     * (line numbers as of esp-matter release/v1.5.1, this checkout's pin)
     * has exactly two textual ESP_ERR_NOT_SUPPORTED sources reachable from
     * a call with a live attribute handle, plus one more inside the
     * function it can delegate to; all but one are proven unreachable here:
     *   - :1083 `val->type != ESP_MATTER_VAL_TYPE_ARRAY`: unreachable,
     *     because i64_to_attr_val() above already returned false (and this
     *     function already returned) for ESP_MATTER_VAL_TYPE_ARRAY and every
     *     other type its switch does not enumerate (its `default: return
     *     false;`). set_val() is called only after i64_to_attr_val()
     *     returned true, so val->type here is always one of its concrete
     *     numeric/boolean/nullable arms, never ARRAY.
     *   - set_val_internal()'s own guard (esp_matter_data_model.cpp:721,
     *     `!(flags & ATTRIBUTE_FLAG_MANAGED_INTERNALLY)`): unreachable,
     *     because set_val() only calls set_val_internal() from its own
     *     `if (!(flags & ATTRIBUTE_FLAG_MANAGED_INTERNALLY))` branch
     *     (:1091), using the same `flags` read moments earlier under the
     *     same ChipStackLock. The guard inside set_val_internal() repeats a
     *     condition its only caller already proved true; it can never fire
     *     on this path.
     *   - :1103 `return ESP_ERR_NOT_SUPPORTED;` (the branch's final line,
     *     reached when `flags & ATTRIBUTE_FLAG_MANAGED_INTERNALLY` is set
     *     and `flags & ATTRIBUTE_FLAG_WRITABLE` is not): REACHABLE, and the
     *     only reachable source. This is the Instance-served, not-writable
     *     case the C1 bench found (DEM 0x0098 ESAState, ModeBase
     *     CurrentMode): attr_locate() proved the attribute exists,
     *     i64_to_attr_val() proved the value is in range and the right
     *     type, and the attribute is simply not ours to write.
     * set_val_via_write_attribute() (the WRITABLE sibling branch, taken
     * when MANAGED_INTERNALLY is set together with WRITABLE) returns only
     * ESP_OK, ESP_FAIL or ESP_ERR_NO_MEM, never ESP_ERR_NOT_SUPPORTED, so
     * that branch's own bare-ERROR collapse is not disturbed by this
     * change.
     *
     * DE270's accompanying claim that no device type this firmware creates
     * ships such an attribute was FALSE, and is corrected here (B401,
     * caught by the batch 4 round 2 trace). The chime pair is exactly that
     * shape: create_selected_chime() and create_enabled() are both
     * ATTRIBUTE_FLAG_WRITABLE | ATTRIBUTE_FLAG_MANAGED_INTERNALLY |
     * ATTRIBUTE_FLAG_NONVOLATILE (esp_matter_attribute.cpp:4872-4884), and
     * mk_chime() puts them on a live endpoint (mt_devtypes.cpp's registry).
     * So that branch IS taken today: an AT+MTATTR write to SelectedChime or
     * Enabled live-writes through the data-model provider rather than
     * answering +MTERR:11, which is the intended behaviour and what the
     * DE397 ruling was amended to say. Nothing depended on the stale
     * claim; it only made this comment misdescribe the firmware. */
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return MT_ATTR_ERR_READONLY;
    }
    return MT_ATTR_ERR_FAILED;
}

/* ---- Thread role and mesh identity (0.11.0 task 1, AT+MTTHREAD?) --------
 *
 * Sources read before writing this bridge (task brief step 1; esp-matter
 * release/v1.5.1 pin, connectedhomeip submodule at the same pin):
 *
 *   - esp_matter_attribute.cpp:847-851 (thread_network_diagnostics::
 *     attribute::create_routing_role() and its five siblings): Channel,
 *     RoutingRole, NetworkName, PanId, ExtendedPanId and PartitionId are ALL
 *     created with ATTRIBUTE_FLAG_MANAGED_INTERNALLY | ATTRIBUTE_FLAG_
 *     NULLABLE, NetworkName via esp_matter_char_str() rather than a numeric
 *     constructor. Managed-internally is what puts every one of them,
 *     string included, on the SAME live provider-read path below, not a
 *     value cached at create() time (esp_matter_cluster.cpp's
 *     thread_network_diagnostics::create() passes each one a placeholder
 *     zero/NULL that is never read back).
 *   - esp_matter_data_model.cpp:927-999 (get_val(ep, cluster, attr, val)):
 *     builds a real AttributeReportIBs::Builder/TLVWriter and calls the data
 *     model provider's ReadAttribute() (esp_matter_data_model_provider.cpp:
 *     317-329), the same call mt_matter_attr_read() above already relies
 *     on. For a path with no ServerClusterInterface registered (Thread
 *     diagnostics has none), ReadAttribute() falls to
 *     TryReadViaAccessInterface() against the AttributeAccessInterface
 *     ThreadNetworkDiagnostics registers, so this reaches CHIP's own
 *     cluster server code, never a value this firmware wrote.
 *   - clusters/ThreadNetworkDiagnostics/AttributeIds.h: Channel = 0x0000,
 *     RoutingRole = 0x0001, NetworkName = 0x0002, PanId = 0x0003,
 *     ExtendedPanId = 0x0004. PartitionId is 0x0009, NOT the next id after
 *     ExtendedPanId as a naive reading of the cluster's attribute list order
 *     might suggest; read from the generated header, not assumed.
 *   - thread-network-diagnostics-server.cpp:68-141
 *     (ThreadDiagnosticsAttrAccess::Read()): Channel, RoutingRole,
 *     NetworkName, PanId, ExtendedPanId and PartitionId are all in the
 *     switch's WriteThreadNetworkDiagnosticAttributeToTlv() arm, confirming
 *     the AttributeAccessInterface actually serves all six, and
 *     MatterThreadNetworkDiagnosticsPluginServerInitCallback() (same file,
 *     ~line 211) is what registers it, wired in automatically by
 *     esp_matter_cluster.cpp's CLUSTER_FLAG_SERVER branch.
 *   - thread-network-diagnostics-provider.cpp:68-97
 *     (WriteThreadNetworkDiagnosticAttributeToTlv()): while the Thread
 *     dataset is not commissioned, Channel/NetworkName/PanId/ExtendedPanId/
 *     PartitionId all encode null; the comment on that switch is explicit
 *     that RoutingRole is "nullable but not listed here as thread provides
 *     valid data even when disabled or detached". This is why
 *     mt_thread_info_t (mt_matter.h) carries has_* flags for the first five
 *     and not for role: the SDK itself guarantees role is never null.
 *   - thread-network-diagnostics-provider.cpp:107-150: RoutingRole's exact
 *     derivation from otDeviceRole/otThreadGetLinkMode/
 *     otThreadIsRouterEligible, matching design spec 2.1's token table field
 *     for field; mt_thread_role_name() below reads the SAME RoutingRoleEnum
 *     the SDK derives into, not a transcribed copy (the project's "read SDK
 *     enums at call time" rule, mt_matter_lock_source_max()'s precedent).
 *   - esp_matter_attr_data_buffer.cpp (attribute_data_decode_buffer::Decode,
 *     get_val_from_tlv_data()): a null CHAR_STRING decodes with val.a.b ==
 *     nullptr; a present one is copied into a fresh esp_matter_mem_calloc()
 *     buffer "now owned by the caller" (that file's own comment), which this
 *     bridge frees with esp_matter_mem_free() after copying it, bounded,
 *     into mt_thread_info_t::name. This is the SAME get_val() mechanism the
 *     five integer fields above use, branching on val.type rather than
 *     inventing a second read path, per the task brief's instruction.
 */

/*
 * One nullable identity field, shared by Channel/PanId/ExtendedPanId/
 * PartitionId below: the same attr_locate() + get_val() + attr_val_to_i64()
 * path mt_matter_attr_read() uses for any other integer attribute, fixed to
 * endpoint 0 cluster 0x0035. False means either the attribute could not be
 * located (should not happen: the RoutingRole probe in
 * mt_matter_thread_info() already proved the cluster exists) or its current
 * value is null (attr_val_to_i64()'s meaning for a NULLABLE_* type); both
 * are answered identically here; the caller clears its has_* flag either
 * way, since a field that is missing and one that is null render the same
 * way on the wire: empty (design spec 2.1).
 */
static bool mt_thread_read_id_field(uint32_t attr_id, int64_t *out)
{
    esp_matter::attribute_t *a = nullptr;
    if (attr_locate(0, chip::app::Clusters::ThreadNetworkDiagnostics::Id, attr_id, &a) != MT_ATTR_OK) {
        return false;
    }
    esp_matter_attr_val_t val;
    if (esp_matter::attribute::get_val(a, &val) != ESP_OK) {
        return false;
    }
    return attr_val_to_i64(&val, out);
}

extern "C" int mt_matter_thread_info(mt_thread_info_t *out)
{
    if (out == nullptr) {
        return MT_ATTR_ERR_FAILED;
    }
    memset(out, 0, sizeof(*out));

    ChipStackLock lock;

    /* RoutingRole doubles as the cluster-presence probe: a WiFi image never
     * creates ThreadNetworkDiagnostics on endpoint 0, so attr_locate() fails
     * here with MT_ATTR_ERR_CLUSTER before any other field is touched.
     * cmd_mtthread (mt_at.c) maps THAT specific failure to +MTERR:8
     * (unsupported command), not the generic +MTERR:3 the attr_result-to-
     * +MTERR table would give it, because the question this command asks
     * first is "does this image speak Thread at all", not "does this one
     * endpoint carry the cluster". */
    esp_matter::attribute_t *role_attr = nullptr;
    mt_attr_result_t r = attr_locate(0, chip::app::Clusters::ThreadNetworkDiagnostics::Id,
                                     chip::app::Clusters::ThreadNetworkDiagnostics::Attributes::RoutingRole::Id,
                                     &role_attr);
    if (r != MT_ATTR_OK) {
        return r;
    }

    esp_matter_attr_val_t role_val;
    if (esp_matter::attribute::get_val(role_attr, &role_val) != ESP_OK) {
        return MT_ATTR_ERR_FAILED;
    }
    int64_t role64 = 0;
    if (!attr_val_to_i64(&role_val, &role64)) {
        /* Never observed on real firmware: thread-network-diagnostics-
         * provider.cpp deliberately excludes RoutingRole from its not-
         * commissioned null list (see this function's header comment).
         * Treated as a runtime failure rather than defaulted to a role,
         * since mt_thread_info_t::role carries no has_role flag to report
         * "unknown" honestly if this ever did happen. */
        return MT_ATTR_ERR_FAILED;
    }
    out->role = (uint8_t)role64;

    /* Same attached predicate the P2 transport-mismatch check uses
     * (mt_matter_net_info() above, spec 3.12.1). IsThreadAttached() is
     * always declared regardless of build (GenericConnectivityManagerImpl_
     * NoThread::_IsThreadAttached() answers false on a WiFi image), so this
     * needs no #if guard; the RoutingRole probe above already returned on a
     * WiFi image before execution ever reaches here anyway. */
    out->attached = chip::DeviceLayer::ConnectivityMgr().IsThreadAttached();

    int64_t v = 0;
    out->has_channel = mt_thread_read_id_field(
        chip::app::Clusters::ThreadNetworkDiagnostics::Attributes::Channel::Id, &v);
    out->channel = out->has_channel ? (uint16_t)v : 0;

    out->has_panid = mt_thread_read_id_field(
        chip::app::Clusters::ThreadNetworkDiagnostics::Attributes::PanId::Id, &v);
    out->panid = out->has_panid ? (uint16_t)v : 0;

    out->has_extpanid = mt_thread_read_id_field(
        chip::app::Clusters::ThreadNetworkDiagnostics::Attributes::ExtendedPanId::Id, &v);
    out->extpanid = out->has_extpanid ? (uint64_t)v : 0;

    out->has_partitionid = mt_thread_read_id_field(
        chip::app::Clusters::ThreadNetworkDiagnostics::Attributes::PartitionId::Id, &v);
    out->partitionid = out->has_partitionid ? (uint32_t)v : 0;

    /* NetworkName: a CHAR_STRING, not one of the NULLABLE_* integer types
     * attr_val_to_i64() understands, so it is read and branched on
     * separately, through the SAME attr_locate() + get_val() call the four
     * fields above just used (this section's header comment: same
     * mechanism, different branch on val.type, not a second read path). A
     * null name decodes with val.a.b == nullptr
     * (esp_matter_attr_data_buffer.cpp's Decode()); a present one arrives in
     * a freshly calloc'd, NUL-terminated buffer this bridge now owns and
     * must free. Copied bounded into out->name[17], Thread's own 16-byte
     * limit plus the terminator; a name at or over that width is truncated
     * rather than rejected, the same "firmware bound tighter than the far
     * end, never reject" treatment MT_MODES_MAX_LABEL_LEN and friends give
     * an over-length host-independent value this firmware did not create.
     * out->name is already "" from the memset above, so every early-exit
     * branch below leaves it correctly empty. */
    esp_matter::attribute_t *name_attr = nullptr;
    if (attr_locate(0, chip::app::Clusters::ThreadNetworkDiagnostics::Id,
                    chip::app::Clusters::ThreadNetworkDiagnostics::Attributes::NetworkName::Id,
                    &name_attr) == MT_ATTR_OK) {
        esp_matter_attr_val_t name_val;
        if (esp_matter::attribute::get_val(name_attr, &name_val) == ESP_OK) {
            /*
             * Task 2 review follow-up (robustness): the free used to be
             * gated on the SAME "type == CHAR_STRING" check as the memcpy
             * below, which is a tautology today (NetworkName only ever
             * decodes as CHAR_STRING on this SDK) but would leak the buffer
             * silently on a device expected to run for months if a future
             * SDK bump ever encoded it as LONG_CHAR_STRING instead:
             * esp_matter_attribute_utils.h's val union shares the SAME
             * { uint8_t *b; uint16_t s; ... } "a" struct across every
             * buffer-shaped type (CHAR_STRING, LONG_CHAR_STRING,
             * OCTET_STRING, LONG_OCTET_STRING, ARRAY), so val.a.b is a
             * valid, freeable pointer regardless of which of those get_val()
             * returns. The free below is keyed on the pointer alone, so it
             * is structurally leak-proof; the memcpy stays gated on
             * CHAR_STRING specifically, since that is the only shape this
             * bridge knows how to interpret as a plain byte string today.
             */
            if (name_val.type == ESP_MATTER_VAL_TYPE_CHAR_STRING && name_val.val.a.b != nullptr) {
                uint16_t len = name_val.val.a.s;
                if (len > sizeof(out->name) - 1) {
                    len = sizeof(out->name) - 1;
                }
                memcpy(out->name, name_val.val.a.b, len);
                out->name[len] = '\0';
            }
            if (name_val.val.a.b != nullptr) {
                esp_matter_mem_free(name_val.val.a.b);
            }
        }
    }

    return MT_ATTR_OK;
}

extern "C" const char *mt_thread_role_name(uint8_t role)
{
    using chip::app::Clusters::ThreadNetworkDiagnostics::RoutingRoleEnum;
    switch (static_cast<RoutingRoleEnum>(role)) {
    case RoutingRoleEnum::kUnspecified:     return "UNSPECIFIED";
    case RoutingRoleEnum::kUnassigned:      return "UNASSIGNED";
    case RoutingRoleEnum::kSleepyEndDevice: return "SLEEPY_END_DEVICE";
    case RoutingRoleEnum::kEndDevice:       return "END_DEVICE";
    case RoutingRoleEnum::kReed:            return "REED";
    case RoutingRoleEnum::kRouter:          return "ROUTER";
    case RoutingRoleEnum::kLeader:          return "LEADER";
    default:                                return nullptr;
    }
}

/*
 * Read RoutingRole with NO ChipStackLock. The sole caller is
 * app_event_cb()'s kThreadStateChange case (0.11.0, +MTEVT:28), which
 * already runs on the CHIP event-loop task, where re-entering
 * PlatformMgr().LockChipStack() would risk a self-deadlock rather than a
 * race (see that case's own comment for the full reasoning, which cites the
 * door lock adjudication block's identical rule for app_event_cb()).
 * mt_matter_thread_info() is not reused here for that same reason: it wraps
 * its whole read in a ChipStackLock for its other caller (the AT parser
 * task, via cmd_mtthread). This mirrors only that function's RoutingRole
 * probe (attr_locate() + get_val() + attr_val_to_i64()), since role is the
 * only field the role-change event's payload carries.
 *
 * Returns false when the attribute cannot be located at all (should not
 * happen: this only runs inside CHIP_DEVICE_CONFIG_ENABLE_THREAD, i.e. a
 * Thread image, which always creates ThreadNetworkDiagnostics on endpoint 0,
 * the same reasoning mt_matter_thread_info()'s own header comment gives) or
 * decodes as null (thread-network-diagnostics-provider.cpp's own
 * not-commissioned null list explicitly excludes RoutingRole, so this should
 * not happen either; see mt_matter_thread_info()'s identical check).
 *
 * Guarded by CHIP_DEVICE_CONFIG_ENABLE_THREAD, matching its sole call site
 * (app_event_cb()'s kThreadStateChange case) and its forward declaration
 * above: on a WiFi-only image this function is unreachable and would
 * otherwise be an unused static function.
 */
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
static bool mt_thread_read_role(uint8_t *role_out)
{
    esp_matter::attribute_t *role_attr = nullptr;
    if (attr_locate(0, chip::app::Clusters::ThreadNetworkDiagnostics::Id,
                    chip::app::Clusters::ThreadNetworkDiagnostics::Attributes::RoutingRole::Id,
                    &role_attr) != MT_ATTR_OK) {
        return false;
    }
    esp_matter_attr_val_t role_val;
    if (esp_matter::attribute::get_val(role_attr, &role_val) != ESP_OK) {
        return false;
    }
    int64_t role64 = 0;
    if (!attr_val_to_i64(&role_val, &role64)) {
        return false;
    }
    *role_out = (uint8_t)role64;
    return true;
}
#endif

/*
 * AT+MTSWITCH: emit the Switch cluster's InitialPress event, which is what
 * the upstream arduino-esp32 class's click() does. Events are fire-and-forget
 * toward subscribed controllers; nothing echoes on the AT link.
 *
 * EventLogging.h: "The consumer has to either lock the Matter stack lock or
 * queue the event to the Matter event queue when using LogEvent. This
 * function is not safe to call outside of the main Matter processing
 * context." ChipStackLock provides that lock, same as every other bridge
 * function here.
 */
extern "C" int mt_matter_switch_click(uint16_t ep)
{
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::Switch::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    esp_err_t err = esp_matter::cluster::switch_cluster::event::send_initial_press(ep, 1);
    return (err == ESP_OK) ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
}

/*
 * ---- temperature level labels (C3) ----------------------------------------
 *
 * esp_matter has no delegate of its own for the TemperatureControl cluster's
 * SupportedTemperatureLevels attribute: it is ATTRIBUTE_FLAG_MANAGED_INTERNALLY
 * (attribute::create_supported_temperature_levels(), esp_matter_attribute.cpp),
 * which only makes esp_matter::attribute::get() find it for a presence check;
 * the actual list content is served by CHIP itself, through an
 * AttributeAccessInterface (TemperatureControlAttrAccess in
 * temperature-control-server.cpp) that queries a globally registered
 * SupportedTemperatureLevelsIteratorDelegate. esp-matter ships no default
 * implementation, so this firmware must supply one, same as the refrigerator
 * and chef example apps in the vendored connectedhomeip tree do.
 *
 * The interface (connectedhomeip src/app/clusters/temperature-control-server/
 * supported-temperature-levels-manager.h):
 *
 *   virtual uint8_t Size() = 0;
 *   virtual CHIP_ERROR Next(MutableCharSpan & item) = 0;
 *
 * plus a non-virtual Reset(EndpointId endpoint) on the base class that sets
 * the protected mEndpoint/mIndex this override reads. One instance serves
 * every endpoint: TemperatureControlAttrAccess::Read() calls Reset(endpoint)
 * then only Next() in a loop to encode the list for whichever endpoint a
 * controller just read, so Next() below scans the per-endpoint store
 * for the entry matching mEndpoint, the same pattern
 * chef/common/clusters/temperature-control/static-supported-temperature-
 * levels.cpp's AppSupportedTemperatureLevelsDelegate uses.
 */

/*
 * Pay-per-composition round task 4: the label store is now one
 * mt_temp_levels_store_t (mt_stores.h) calloc'ed per TemperatureLevel-variant
 * endpoint at rebuild time and recorded in mt_store_index.h under
 * (ep, MT_STORE_TEMP), replacing the fixed MT_COMP_MAX_ENDPOINTS-deep pool
 * this used to be. Filled by AT+MTTEMPLEVELS (mt_matter_temp_levels_set()
 * below); starts empty (count 0) every boot, deliberately not persisted
 * (mt_matter.h).
 */
static_assert(sizeof(mt_temp_levels_store_t) == 273,
              "mt_temp_levels_store_t size changed; re-check the C6 store budget");

namespace {
class HearthTempLevelsDelegate : public chip::app::Clusters::TemperatureControl::SupportedTemperatureLevelsIteratorDelegate {
public:
    uint8_t Size() override
    {
        auto *store = (mt_temp_levels_store_t *)mt_store_index_find(mEndpoint, 0, MT_STORE_TEMP);
        if (store == nullptr) {
            return 0;
        }
        return store->count;
    }

    CHIP_ERROR Next(chip::MutableCharSpan &item) override
    {
        auto *store = (mt_temp_levels_store_t *)mt_store_index_find(mEndpoint, 0, MT_STORE_TEMP);
        if (store == nullptr || mIndex >= store->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chip::CharSpan label(store->labels[mIndex], strlen(store->labels[mIndex]));
        CHIP_ERROR err = chip::CopyCharSpanToMutableCharSpan(label, item);
        if (err != CHIP_NO_ERROR) {
            return err;
        }
        mIndex++;
        return CHIP_NO_ERROR;
    }
};
}  // namespace

static HearthTempLevelsDelegate s_temp_levels_delegate;

/*
 * The bridge for AT+MTTEMPLEVELS. Label content (1..MT_TEMP_LEVEL_MAX_COUNT
 * labels, 1..MT_TEMP_LEVEL_MAX_LEN printable-ASCII bytes each, no double
 * quote) is validated by the handler in mt_at.c before this is ever called;
 * the bounds re-checked here are defensive, not the primary gate.
 */
extern "C" int mt_matter_temp_levels_set(uint16_t ep, const char *const *labels, uint8_t count)
{
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::TemperatureControl::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (esp_matter::attribute::get(ep, chip::app::Clusters::TemperatureControl::Id,
                                   chip::app::Clusters::TemperatureControl::Attributes::SupportedTemperatureLevels::Id)
        == nullptr) {
        /* Cluster exists but this is a TemperatureNumber-variant cabinet:
         * create_supported_temperature_levels() only runs on the
         * TemperatureLevel branch of feature::temperature_level::add()
         * (esp_matter_feature.cpp). */
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    if (count < 1 || count > MT_TEMP_LEVEL_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }

    auto *store = (mt_temp_levels_store_t *)mt_store_index_find(ep, 0, MT_STORE_TEMP);
    if (store == nullptr) {
        /* Cannot happen in practice: every TemperatureLevel-variant endpoint
         * gets its store allocated at rebuild time, and the attribute check
         * above already turned away any endpoint that is not that variant.
         * Kept as a defensive return rather than an assert, the same not-
         * present verdict the exhausted pool used to return here. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_TEMP_LEVEL_MAX_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
        memcpy(store->labels[i], labels[i], len + 1);
    }
    store->count = count;

    /*
     * Mark the attribute dirty so an active subscription sees the new list.
     * MatterReportingAttributeChangeCallback(endpoint, clusterId, attributeId)
     * (src/app/reporting/reporting.h:34) is what every cluster server in the
     * SDK calls after mutating state served through a delegate or
     * AttributeAccessInterface rather than esp_matter's own attribute store
     * (e.g. service-area-server.cpp:418); there is no esp_matter equivalent
     * here, because esp_matter::attribute::update() only knows how to push
     * its own internally-managed union value, and this attribute's real
     * content lives in the per-endpoint mt_temp_levels_store_t, read out
     * through the delegate above.
     */
    MatterReportingAttributeChangeCallback(
        ep, chip::app::Clusters::TemperatureControl::Id,
        chip::app::Clusters::TemperatureControl::Attributes::SupportedTemperatureLevels::Id);
    return MT_ATTR_OK;
}

/*
 * ---- door lock (C2) --------------------------------------------------------
 *
 * The DoorLock cluster's LockDoor/UnlockDoor commands need an app-level
 * verdict the firmware cannot supply on its own (design spec F1: the
 * callback returns bool inline, no async completion path exists). Both
 * ember callbacks below forward to the host over the C1 verdict mailbox
 * (mt_cmd_forward(), mt_at.c) and return its answer; the two-second
 * controller-side invoke budget (F2) is what makes the mailbox's 1000 ms
 * deadline safe to block on here.
 *
 * NO ChipStackLock in this block, unlike every mt_matter_* bridge function
 * below: these callbacks already run ON the CHIP event-loop task (F2), the
 * same reasoning as app_event_cb(). mt_cmd_forward() takes no lock of its
 * own either, for the same reason.
 */

/*
 * One helper for both commands, differing only in the command id forwarded:
 * design spec section 5. On deny (mt_cmd_forward() returns false, which
 * covers both an explicit deny and every fail-closed path: timeout, link
 * down, or no host callback registered) sets err to OperationErrorEnum::
 * kUnspecified, which is what the server reports to the controller as
 * Status::Failure plus a LockOperationError event (F6, spec section 3).
 */
static bool mt_door_lock_adjudicate(chip::EndpointId endpointId, uint32_t command, OperationErrorEnum &err)
{
    if (mt_cmd_forward(endpointId, chip::app::Clusters::DoorLock::Id, command)) {
        return true;
    }
    err = OperationErrorEnum::kUnspecified;
    return false;
}

/*
 * Signature verbatim from door-lock-server.h:1112-1114 (connectedhomeip,
 * vendored under esp-matter release/v1.5.1).
 */
bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId endpointId, const Nullable<chip::FabricIndex> &fabricIdx,
                                            const Nullable<chip::NodeId> &nodeId, const Optional<chip::ByteSpan> &pinCode,
                                            OperationErrorEnum &err)
{
    /* fabricIdx/nodeId/pinCode: not used. F3 means a PIN-carrying command
     * never reaches this callback at all (feature map 0 registers no
     * credential validation, so the server refuses it before the app is
     * consulted); fabricIdx/nodeId are event-annotation fields the app has
     * no adjudication use for in v1. */
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    return mt_door_lock_adjudicate(endpointId, chip::app::Clusters::DoorLock::Commands::LockDoor::Id, err);
}

/*
 * Signature verbatim from door-lock-server.h:1128-1130.
 */
bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId endpointId, const Nullable<chip::FabricIndex> &fabricIdx,
                                              const Nullable<chip::NodeId> &nodeId, const Optional<chip::ByteSpan> &pinCode,
                                              OperationErrorEnum &err)
{
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    return mt_door_lock_adjudicate(endpointId, chip::app::Clusters::DoorLock::Commands::UnlockDoor::Id, err);
}

/*
 * Cluster init callback: esp_matter's door_lock::create() (esp_matter_cluster
 * .cpp:2046) registers this by address in its own CLUSTER_FLAG_INIT_FUNCTION
 * table, not through ember's generic per-endpoint dispatch, so it is called
 * once per DoorLock server instance regardless.
 *
 * No weak/default definition exists anywhere in the vendored connectedhomeip
 * tree for this symbol (checked: door-lock-server-callback.cpp supplies weak
 * defaults for the OnDoorLock/OnDoorUnlock/OnDoorUnbolt command callbacks
 * only, and grep across src/ and zzz_generated/ for
 * emberAfDoorLockClusterInitCallback finds only the zap-generated
 * declaration). Leaving it undefined is therefore a link failure, not a
 * silent no-op default; see the task report for the exact linker line.
 * Pattern verbatim from esp-matter's own example,
 * examples/door_lock/main/lock/door_lock_callbacks.cpp:20-24, minus its
 * BoltLockMgr().InitLockState() call, which belongs to that example's own
 * (unused here) lock-state simulation, not to InitServer() itself.
 */
void emberAfDoorLockClusterInitCallback(chip::EndpointId endpoint)
{
    DoorLockServer::Instance().InitServer(endpoint);
}

/*
 * AT+MTLOCK bridge: report the host's own actuation as the DoorLock cluster's
 * LockState, through the 6-arg SetLockState() so LockOperation fires (F4).
 * The firmware never calls this after an allowed AT+MTCMD verdict on its
 * own; the host decides when the physical lock has actually moved and calls
 * AT+MTLOCK itself.
 */
extern "C" int mt_matter_lock_state_set(uint16_t ep, uint8_t state, uint8_t source)
{
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::DoorLock::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    bool ok = DoorLockServer::Instance().SetLockState(ep, (DlLockState)state, (OperationSourceEnum)source);
    return ok ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
}

extern "C" uint8_t mt_matter_lock_source_manual(void)
{
    return (uint8_t)OperationSourceEnum::kManual;
}

extern "C" uint8_t mt_matter_lock_source_max(void)
{
    /* kUnknownEnumValue (DoorLock/Enums.h:336) is one past the last real
     * source and is not itself a valid source (mt_matter.h); kAliro
     * (DoorLock/Enums.h:331) is the highest one that is. */
    return (uint8_t)OperationSourceEnum::kAliro;
}

/*
 * ---- water valve (seven-type batch, task C2) -------------------------------
 *
 * ValveConfigurationAndControl::Delegate has exactly three pure virtuals
 * (valve-configuration-and-control-delegate.h:40-42, connectedhomeip
 * vendored under esp-matter release/v1.5.1):
 *
 *   virtual DataModel::Nullable<chip::Percent> HandleOpenValve(DataModel::Nullable<chip::Percent> level) = 0;
 *   virtual CHIP_ERROR HandleCloseValve()                                                                = 0;
 *   virtual void HandleRemainingDurationTick(uint32_t duration)                                          = 0;
 *
 * Design spec F1: the cluster server calls both synchronously but IGNORES
 * what they return (valve-configuration-and-control-cluster.cpp:303 wraps
 * the HandleCloseValve() call in TEMPORARY_RETURN_IGNORED; :361-365 reads
 * HandleOpenValve()'s level back but only to conditionally call
 * UpdateCurrentLevel(), never to fail the command) and answers the
 * controller Status::Success regardless, every time. The verdict this
 * firmware forwards over +MTCMD therefore gates only whether the host
 * actually opens/closes the valve; it has no effect on what the wire
 * response says, which is already fixed at Success before the delegate is
 * ever called. Documented here rather than fought: there is no return path
 * back into the ember callback for a deny to take.
 *
 * The base Delegate class carries no endpoint member (verified against the
 * header above: no field, no accessor), but HandleOpenValve()/
 * HandleCloseValve() need one to forward through
 * mt_cmd_forward(ep, cluster, command). Each pool slot stores it, set once
 * by the thunk (mt_devtypes.cpp's mk_water_valve()) right after create()
 * returns: the real id is not knowable any earlier, since esp_matter
 * assigns it inside create() itself. See mt_matter_valve_delegate_alloc()/
 * mt_matter_valve_delegate_set_endpoint() (mt_matter.h) for the handout
 * protocol this depends on.
 */
class HearthValveDelegate : public chip::app::Clusters::ValveConfigurationAndControl::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }

    /*
     * F1: the return is read by the one caller (SetValveLevel(),
     * cluster.cpp:361) only to decide whether to report a CurrentLevel; it
     * never fails the command. Forward for adjudication and report the
     * level the host was asked for on allow (the caller republishes it as
     * CurrentLevel when the Level feature is present), DataModel::
     * NullNullable on deny, i.e. "no level to report", not a failure.
     */
    chip::app::DataModel::Nullable<chip::Percent> HandleOpenValve(
        chip::app::DataModel::Nullable<chip::Percent> level) override
    {
        if (mt_cmd_forward(m_ep, chip::app::Clusters::ValveConfigurationAndControl::Id,
                            chip::app::Clusters::ValveConfigurationAndControl::Commands::Open::Id)) {
            return level;
        }
        return chip::app::DataModel::NullNullable;
    }

    /*
     * F1: TEMPORARY_RETURN_IGNORED at the only call site (cluster.cpp:303);
     * whatever this returns, CloseValve() itself always answers
     * CHIP_NO_ERROR and the controller sees Status::Success. Forward for
     * adjudication anyway: the verdict is what gates the host's own
     * actuation, which is the whole point of forwarding it.
     */
    CHIP_ERROR HandleCloseValve() override
    {
        mt_cmd_forward(m_ep, chip::app::Clusters::ValveConfigurationAndControl::Id,
                        chip::app::Clusters::ValveConfigurationAndControl::Commands::Close::Id);
        return CHIP_NO_ERROR;
    }

    /*
     * Fires once a second while a timed open counts down
     * (valve-configuration-and-control-cluster.cpp's
     * startRemainingDurationTick()). Nothing in this firmware's AT surface
     * exposes RemainingDuration's countdown as an event to forward, so
     * there is nothing to do here; the attribute itself still updates
     * (esp-matter manages it internally, see mt_matter.h's AT+MTVALVE
     * comment) and stays readable over AT+MTATTR regardless.
     */
    void HandleRemainingDurationTick(uint32_t duration) override
    {
        (void)duration;
    }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/*
 * Pool of MT_COMP_MAX_ENDPOINTS delegate objects, handed out in composition
 * order by mk_water_valve() (mt_devtypes.cpp). Static rather than heap: the
 * composition rebuild runs once at boot and the objects must outlive the
 * device, the same shape as s_temp_levels_delegate above but per-endpoint
 * instead of singleton. This is the pool-handout pattern the rest of the
 * seven-type batch (chime; washer/dishwasher/dryer's OperationalState
 * instances) copies.
 *
 * Exposed to mt_devtypes.cpp, a separate C++ translation unit that must stay
 * free of CHIP/esp_matter delegate types (this file's own header comment),
 * through the opaque void* pair declared in mt_matter.h.
 */
static HearthValveDelegate s_valve_delegates[MT_COMP_MAX_ENDPOINTS];
static size_t              s_valve_delegate_next = 0;

extern "C" void *mt_matter_valve_delegate_alloc(void)
{
    if (s_valve_delegate_next >= MT_COMP_MAX_ENDPOINTS) {
        return nullptr;
    }
    return &s_valve_delegates[s_valve_delegate_next++];
}

extern "C" void mt_matter_valve_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    static_cast<HearthValveDelegate *>(delegate)->set_endpoint(ep);
}

/*
 * AT+MTVALVE bridge: report the host's own actuation as the
 * ValveConfigurationAndControl cluster's CurrentState (and, when level is
 * present, CurrentLevel), through the SDK's own UpdateCurrentState()/
 * UpdateCurrentLevel() so the ValveStateChanged event controllers expect is
 * emitted (design spec F1). The firmware never calls this on its own after
 * an allowed +MTCMD verdict, same split as AT+MTLOCK: the host decides when
 * the physical valve has actually moved.
 *
 * level == -1 means absent (mt_at.c's cmd_mtvalve has already validated
 * 0..100 for any value it does pass through).
 */
extern "C" int mt_matter_valve_state_set(uint16_t ep, uint8_t state, int level)
{
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::ValveConfigurationAndControl::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    CHIP_ERROR err = chip::app::Clusters::ValveConfigurationAndControl::UpdateCurrentState(
        ep, (chip::app::Clusters::ValveConfigurationAndControl::ValveStateEnum)state);
    if (err != CHIP_NO_ERROR) {
        return MT_ATTR_ERR_FAILED;
    }
    if (level >= 0) {
        err = chip::app::Clusters::ValveConfigurationAndControl::UpdateCurrentLevel(ep, (chip::Percent)level);
        if (err != CHIP_NO_ERROR) {
            return MT_ATTR_ERR_FAILED;
        }
    }
    return MT_ATTR_OK;
}

/*
 * ---- mode select (seven-type batch, task C3) -------------------------------
 *
 * F2: SupportedModes is served through ONE GLOBAL SupportedModesManager
 * (chip::app::Clusters::ModeSelect::setSupportedModesManager(), called by
 * esp_matter's ModeSelectDelegateInitCB for every mode_select endpoint the
 * boot rebuild creates,
 * esp_matter_delegate_callbacks.cpp:458-462:
 *   "void ModeSelectDelegateInitCB(void *delegate, uint16_t endpoint_id)
 *    {
 *        VerifyOrReturn(delegate != nullptr);
 *        ModeSelect::SupportedModesManager *supported_modes_manager =
 *            static_cast<ModeSelect::SupportedModesManager*>(delegate);
 *        ModeSelect::setSupportedModesManager(supported_modes_manager);
 *    }"
 * note endpoint_id is accepted but never read) whose two virtuals both take
 * an EndpointId and dispatch on it internally (supported-modes-manager.h:
 * 69-80). There is no per-endpoint object for the SDK to ask for; a single
 * instance covers every mode_select endpoint in the composition.
 * mk_mode_select() (mt_devtypes.cpp) sets config.mode_select.delegate to
 * mt_matter_mode_select_manager() on every mode_select endpoint it creates,
 * so ModeSelectDelegateInitCB re-runs setSupportedModesManager() with the
 * same pointer each time a second (or third...) mode_select endpoint is
 * built: harmless, since the global is already this object
 * (esp_matter_cluster.cpp:2722-2727's mode_select::create() only calls the
 * InitCB when config->delegate is non-null, and every call installs the
 * identical pointer).
 *
 * The ModeOptionsProvider the SDK reads (supported-modes-manager.h:42-62) is
 * two raw pointers over a contiguous ModeOptionStructType array:
 *   "struct ModeOptionsProvider
 *    {
 *        using pointer = const ModeOptionStructType *;
 *        inline pointer begin() const { return mBegin; }
 *        inline pointer end() const { return mEnd; }
 *        ...
 *    };"
 * Nothing in the interface requires static or NVS-backed storage (F2), so a
 * runtime, host-fed store is legal. Backing store: one mt_mode_store_t per
 * mode_select endpoint (mt_store_index.h maps endpoint id to it), holding up
 * to MT_MODES_MAX_COUNT entries, each an mt_mode_entry_t (u8 mode value plus
 * an (MT_MODES_MAX_LABEL_LEN + 1)-byte label buffer, mt_stores.h) and a
 * ModeOptionStructType whose CharSpan.label points into that same buffer
 * (ModeSelect/Structs.h:71-81: "chip::CharSpan label; uint8_t mode; ...
 * semanticTags;").
 *
 * CharSpan lifetime: both the label bytes (mt_mode_entry_t::label) and the
 * ModeOptionStructType array (mt_mode_store_t::structs) live in one
 * calloc'ed allocation made once at endpoint create (app_main's composition
 * rebuild, before esp_matter::start()) and recorded in the index; the
 * pointer is never freed and never reallocated while the program runs. The
 * struct array is rebuilt IN PLACE on every AT+MTMODES write to the same
 * store (mt_matter_modes_set() below), never reallocated or moved, so no
 * CharSpan is ever left pointing at freed memory. The
 * two-phase rebuild (copy every entry's bytes first, THEN overwrite the
 * struct array) is still not enough on its own to rule out a reader
 * observing a struct mid-rewrite, pointing at a label only half copied: what
 * actually rules that out is mutual exclusion. mt_matter_modes_set() holds
 * ChipStackLock for the whole rebuild, and the CHIP task's own event loop
 * holds the identical lock (StackLock, PlatformMgr().LockChipStack()) for
 * its entire DispatchEvent() call (GenericPlatformManagerImpl_FreeRTOS.ipp:
 * 181,238: the lock is taken once at the top of _RunEventLoop() and only
 * dropped around the blocking dequeue, never around DispatchEvent() itself),
 * which is where ModeSelectAttrAccess::Read() and ChangeToMode() run. So a
 * fresh read can never interleave with a rebuild: it either sees the
 * pre-write state complete or the post-write state complete, never
 * something in between. The SDK also re-reads through the manager on every
 * access rather than caching a provider across calls
 * (ModeSelectAttrAccess::Read(), mode-select-server.cpp, calls
 * getModeOptionsProvider() fresh for each SupportedModes read; ChangeToMode()
 * calls getModeOptionByMode() fresh for each command), which rules out a
 * STALE provider outliving a later rebuild, but that is a secondary point:
 * the lock is what rules out a read landing mid-rebuild in the first place.
 * SemanticTags is mandatory-but-empty-legal (F2): every entry publishes a
 * default-constructed List (Span's default ctor is mDataBuf=nullptr,
 * mDataLen=0, Span.h:46), i.e. List<const SemanticTagStruct::Type>(nullptr,
 * 0).
 */
/* The mode store is the shared plain list (mt_stores.h, mt_mode_list_t) plus
 * this port's rendered ModeOptionStruct array; the SDK reads structs[] as a
 * ModeOptionsProvider, and each struct's label CharSpan aliases
 * list.entries[i].label. Both live in one heap allocation, calloc'ed once at
 * endpoint create and never freed, so the spans stay valid for the boot
 * (spec section 5), the same argument the old .bss pool made from static
 * storage. Looked up per endpoint through mt_store_index_find() rather than
 * scanned out of a fixed pool; see mt_store_index.h. The former
 * mt_mode_entry_t local definition is now mt_stores.h's. */
struct mt_mode_store_t {
    mt_mode_list_t list;
    chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type
        structs[MT_MODES_MAX_COUNT];
};
static_assert(sizeof(mt_mode_store_t) == 436,
              "mode store size moved; reconcile with mt_stores.h/mt_store_index.h");

class HearthSupportedModesManager : public chip::app::Clusters::ModeSelect::SupportedModesManager
{
private:
    /*
     * SupportedModesManager declares its own ModeOptionStructType alias
     * BEFORE its "public:" (supported-modes-manager.h:36), so it is private
     * to the base class and not nameable here even though a public base
     * virtual's signature uses it: a derived class inherits the member but
     * not access to its name. The esp32 platform's own
     * StaticSupportedModesManager (static-supported-modes-manager.h:31) hits
     * the same thing and works around it exactly this way, a private alias
     * of its own that shadows the inaccessible one for this class's override
     * signature below.
     */
    using ModeOptionStructType = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;

public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId endpointId) const override
    {
        auto *store = (mt_mode_store_t *)mt_store_index_find(endpointId, 0, MT_STORE_MODE);
        if (store == nullptr) {
            return ModeOptionsProvider();  /* begin == end == nullptr: no entry for this endpoint */
        }
        return ModeOptionsProvider(store->structs, store->structs + store->list.count);
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(
        chip::EndpointId endpointId, uint8_t mode, const ModeOptionStructType **dataPtr) const override
    {
        auto *store = (mt_mode_store_t *)mt_store_index_find(endpointId, 0, MT_STORE_MODE);
        if (store == nullptr) {
            return chip::Protocols::InteractionModel::Status::UnsupportedCluster;
        }
        for (uint8_t i = 0; i < store->list.count; i++) {
            if (store->structs[i].mode == mode) {
                *dataPtr = &store->structs[i];
                return chip::Protocols::InteractionModel::Status::Success;
            }
        }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
};
static HearthSupportedModesManager s_mode_select_manager;

extern "C" void *mt_matter_mode_select_manager(void)
{
    return &s_mode_select_manager;
}

/*
 * The bridge for AT+MTMODES. Grammar and content rules are enforced by
 * cmd_mtmodes() in mt_at.c before this is ever called; the bounds re-checked
 * here are defensive, not the primary gate (same split as
 * mt_matter_temp_levels_set() above).
 */
extern "C" int mt_matter_modes_set(uint16_t ep, const uint8_t *modes, const char *const *labels, uint8_t count)
{
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::ModeSelect::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_MODES_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }

    auto *store = (mt_mode_store_t *)mt_store_index_find(ep, 0, MT_STORE_MODE);
    if (store == nullptr) {
        /* Cannot happen in practice: every endpoint whose device type
         * carries ModeSelect gets a store at rebuild time (app_main), the
         * same cap the composition itself enforces, same reasoning as
         * mt_matter_temp_levels_set() above. Kept as a defensive return
         * rather than an assert. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_MODES_MAX_LABEL_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
        store->list.entries[i].mode = modes[i];
        memcpy(store->list.entries[i].label, labels[i], len + 1);
    }
    store->list.count = count;

    /* Rebuild the struct array in place, contiguous, each CharSpan pointing
     * into the heap-allocated label buffer above: see the CharSpan-lifetime
     * comment ahead of this class. */
    for (uint8_t i = 0; i < count; i++) {
        store->structs[i].mode  = store->list.entries[i].mode;
        store->structs[i].label = chip::CharSpan::fromCharString(store->list.entries[i].label);
        store->structs[i].semanticTags =
            chip::app::DataModel::List<const chip::app::Clusters::ModeSelect::Structs::SemanticTagStruct::Type>();
    }

    MatterReportingAttributeChangeCallback(
        ep, chip::app::Clusters::ModeSelect::Id,
        chip::app::Clusters::ModeSelect::Attributes::SupportedModes::Id);
    return MT_ATTR_OK;
}

/*
 * ---- ModeBase: RVC run/clean mode, microwave mode (RVC + Microwave batch, task 2) -----
 *
 * RvcRunMode (0x0054), RvcCleanMode (0x0055), MicrowaveOvenMode (0x005E),
 * RefrigeratorAndTemperatureControlledCabinetMode (0x0052, composed appliance
 * round task 3) and OvenMode (0x0049, task 4)
 * are all concrete derivations of the abstract ModeBase cluster
 * (mode-base-server.h): one shared Delegate interface, one shared Instance
 * class parameterised by cluster id at construction (design spec section 2).
 * Unlike ModeSelect (HearthSupportedModesManager above), which needs exactly
 * ONE manager object because SupportedModesManager dispatches on endpoint id
 * itself, ModeBase needs one delegate OBJECT per (endpoint, cluster) pair:
 * the thunk's set_delegate_and_init_callback() call (esp_matter_cluster.cpp's
 * rvc_run_mode/rvc_clean_mode/microwave_oven_mode::create(), Tasks 3-4) news a
 * ModeBase::Instance per cluster instance and its constructor calls
 * Delegate::SetInstance() on it (mode-base-server.h), the identical
 * one-Instance-per-delegate contract F7's OperationalState pool below is
 * built around (VerifyOrDie on a second Instance sharing a delegate). Same
 * pool shape as HearthOpStateDelegate below: mt_devtypes.cpp allocates a slot
 * before create(), fixes the endpoint after, per the pattern
 * mt_matter_valve_delegate_alloc()'s doc comment (mt_matter.h) explains in
 * full. The cluster id is fixed at ALLOC time here rather than by a second
 * setter: unlike the endpoint id, it is already known to the caller before
 * create() runs (it is literally which cluster::create() is about to be
 * called), and the delegate needs it immediately to answer
 * GetModeValueByIndex(0, ...) correctly the moment Instance::Init() asks
 * (below), which happens before set_endpoint() can run.
 *
 * Store: MT_MB_MAX_LISTS slots keyed by (ep, cluster), the same per-entry
 * shape as the mode select store above but with a per-entry uint16_t tag
 * alongside the mode value and label (design spec 3.1's mandatory <tag>
 * field; still the fixed pool here pending its own migration task). Full
 * replacement per AT+MTMODES cluster-aware call, RAM only, never persisted:
 * the same not-persisted contract as every other host-fed list in this file
 * (MTMODES, MTTEMPLEVELS, MTCHIMESOUNDS).
 *
 * Placeholder mode 0: ModeBase::Instance::Init() reads
 * GetModeValueByIndex(0, ...) FIRST, before any AT+MTMODES cluster-aware
 * call has ever run for that (ep, cluster) - the Instance is constructed
 * from esp_matter::start()'s init-callback pass, ahead of mt_at_start()
 * (this project's own boot-ordering rule). Checked against the SDK source
 * rather than assumed: Init() (mode-base-server.cpp) calls
 * ReturnErrorOnFailure(mDelegate->GetModeValueByIndex(0, mCurrentMode))
 * as its FIRST line, so a PROVIDER_LIST_EXHAUSTED there returns immediately,
 * before the VerifyOrDie on emberAfContainsServer a few lines later (an
 * unrelated check, for whether the cluster was zap-registered, not for this)
 * and before RegisterThisInstance()/the AttributeAccessInterface and
 * CommandHandlerInterface registrations later in the same function ever run.
 * The SDK's own init callback (esp_matter's InitModeDelegate() and its
 * per-cluster wrappers, esp_matter_delegate_callbacks.cpp) discards Init()'s
 * return value outright ("modeInstance->Init();", no check), so this failure
 * does not crash the device: the Instance object exists (its constructor's
 * own SetInstance() call already ran, so this delegate's instance()
 * passthrough would return it) but is never registered, so reads, writes and
 * commands against that cluster would go unanswered with no diagnostic at
 * all. Quieter, not louder, and correspondingly worse to debug than a boot
 * panic. So an empty slot must still answer index 0: mode 0, the cluster's
 * tag-0 default (design spec section 9's "Tag-0 defaults, exact policy"
 * table), label "Mode0". The moment the host sends the real list the
 * placeholder is superseded; there is no separate "is this the placeholder"
 * flag to clear, find_slot()/the count==0 check below simply prefer a stored
 * entry whenever one exists.
 */
/* mt_mb_entry_t now comes from mt_stores.h (identical layout: mode, tag,
 * label), pulled in for the mode select store above; the pool this struct
 * backs is unchanged and still local to this port, pending its own
 * migration task. */
struct mt_mb_slot_t {
    bool     used;
    uint16_t ep;
    uint32_t cluster;
    uint8_t  count;
    mt_mb_entry_t entries[MT_MB_MAX_COUNT];
};
static mt_mb_slot_t s_mb_slots[MT_MB_MAX_LISTS];

class HearthModeBaseDelegate : public chip::app::Clusters::ModeBase::Delegate
{
public:
    void set_cluster(chip::ClusterId cluster) { m_cluster = cluster; }
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }
    chip::ClusterId  cluster() const { return m_cluster; }

    /* GetInstance() is protected in the base Delegate; this passthrough is
     * the same mechanism HearthOpStateDelegate::instance() uses below (see
     * its class comment for the full citation trail): mt_matter_modebase_set()
     * needs the Instance to clamp CurrentMode when a replacement list drops
     * the value it currently holds. */
    chip::app::Clusters::ModeBase::Instance *instance() { return GetInstance(); }

    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }

    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, chip::MutableCharSpan &label) override
    {
        mt_mb_slot_t *slot = find_slot();
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan::fromCharString("Mode0"), label);
        }
        if (modeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan::fromCharString(slot->entries[modeIndex].label),
                                                    label);
    }

    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t &value) override
    {
        mt_mb_slot_t *slot = find_slot();
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            value = 0;
            return CHIP_NO_ERROR;
        }
        if (modeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        value = slot->entries[modeIndex].mode;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetModeTagsByIndex(
        uint8_t modeIndex,
        chip::app::DataModel::List<chip::app::Clusters::detail::Structs::ModeTagStruct::Type> &tags) override
    {
        mt_mb_slot_t *slot = find_slot();
        uint16_t tag_value;
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            tag_value = placeholder_tag();
        } else {
            if (modeIndex >= slot->count) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            tag_value = slot->entries[modeIndex].tag;
        }
        /* The SDK hands in a buffer sized for kMaxNumOfModeTags (8); this
         * delegate only ever publishes one tag per mode, so size 1 always
         * fits. Defensive check mirrors the SDK's own rvc-mode-delegates.cpp
         * reference delegate (examples/rvc-app), which guards the same way
         * before writing. */
        if (tags.size() < 1) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        tags[0].value = tag_value;
        tags.reduce_size(1);
        return CHIP_NO_ERROR;
    }

    /*
     * ChangeToMode::Id is 0x00 for RvcRunMode, RvcCleanMode, (composed
     * appliance round, task 3) RefrigeratorAndTemperatureControlledCabinetMode,
     * (task 4) OvenMode, (energy round B) WaterHeaterMode AND (energy round
     * C1) DeviceEnergyManagementMode
     * (verified against each cluster's generated CommandIds.h: all six
     * declare "inline constexpr CommandId Id = 0x00000000" for ChangeToMode),
     * so one constant covers all six. MicrowaveOvenMode never reaches this
     * override at all: its generated CommandIds.h declares
     * kAcceptedCommandsCount = 0, so the SDK wires no ChangeToMode command for
     * it at all (design spec 3.3's plan-time correction; the microwave's mode
     * is selected through SetCookingParameters' cookMode field instead).
     * UnsupportedMode is answered by the SDK's own pre-check
     * (Instance::HandleChangeToMode, mode-base-server.cpp) before this
     * delegate method is ever called, so only kSuccess/kGenericFailure are
     * this function's business, per design spec 3.3's adjudicated-ChangeToMode
     * decision.
     */
    void HandleChangeToMode(uint8_t NewMode,
                             chip::app::Clusters::ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        bool allow = mt_cmd_forward_payload(m_ep, m_cluster,
                                             chip::app::Clusters::RvcRunMode::Commands::ChangeToMode::Id, NewMode);
        response.status = chip::to_underlying(allow ? chip::app::Clusters::ModeBase::StatusCode::kSuccess
                                                      : chip::app::Clusters::ModeBase::StatusCode::kGenericFailure);
    }

private:
    mt_mb_slot_t *find_slot() const
    {
        for (auto &s : s_mb_slots) {
            if (s.used && s.ep == m_ep && s.cluster == m_cluster) {
                return &s;
            }
        }
        return nullptr;
    }

    /* Tag-0 defaults, design spec section 9's exact policy, placeholder case
     * only (index 0 of an otherwise-empty list): RvcRunMode gets kIdle,
     * RvcCleanMode gets kVacuum, MicrowaveOvenMode gets kNormal,
     * (composed appliance round, task 3)
     * RefrigeratorAndTemperatureControlledCabinetMode gets kAuto (0x00, the
     * ModeBase common tag every ModeBase-derived cluster shares,
     * Mode_Refrigerator.xml), and (task 4) OvenMode gets kBake (0x4000,
     * Mode_Oven.xml's first oven-specific tag; the cluster also inherits the
     * ModeBase common tags 0x00-0x09, but a cavity whose host has not yet
     * declared a list is more usefully described as a bake cavity than as
     * "Auto"), and (2026-08-31) EnergyEvseMode gets kManual (0x4000,
     * Mode_EVSE.xml's first cluster-specific tag), which is the number
     * the MicrowaveOvenMode fall-through already returned and is now chosen
     * rather than inherited. mt_matter_modebase_set() below applies the
     * identical table to real host-declared entries at store time.
     *
     * WaterHeaterMode takes kManual (0x4001) from its own arm below as of
     * this round (B421). It cannot be left to the fall-through: 0x4000 is
     * kOff on that cluster's enum, so an unfed water heater's placeholder
     * said Off where a host-fed one with tag 0 says Manual, disagreeing
     * with both the store-time default and AT_MT_SPEC.md 3.20.1's table
     * row. The nRF54L15 port has carried the explicit arm since batch 7b.
     *
     * DeviceEnergyManagementMode is the one cluster still served by the
     * fall-through, and it does land correctly: 0x4000 is kNoOptimization
     * on its enum, which is its store-time default too. Correct by
     * coincidence rather than by choice, the way EnergyEvseMode was until
     * the spec named it. */
    uint16_t placeholder_tag() const
    {
        using namespace chip::app::Clusters;
        if (m_cluster == RvcRunMode::Id) {
            return chip::to_underlying(RvcRunMode::ModeTag::kIdle);
        }
        if (m_cluster == RvcCleanMode::Id) {
            return chip::to_underlying(RvcCleanMode::ModeTag::kVacuum);
        }
        if (m_cluster == RefrigeratorAndTemperatureControlledCabinetMode::Id) {
            return chip::to_underlying(RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kAuto);
        }
        if (m_cluster == OvenMode::Id) {
            return chip::to_underlying(OvenMode::ModeTag::kBake);
        }
        if (m_cluster == EnergyEvseMode::Id) {
            /* Manual (0x4000), AT_MT_SPEC.md 3.20's table row (2026-08-31).
             * Same number the fall-through below already returned, spelled
             * out now that the spec names it: kNormal is 0x4000 on
             * MicrowaveOvenMode's enum and kManual is 0x4000 on this one, so
             * the old answer was right by coincidence rather than by
             * choice. */
            return chip::to_underlying(EnergyEvseMode::ModeTag::kManual);
        }
        if (m_cluster == WaterHeaterMode::Id) {
            /* B421: kManual (0x4001), AT_MT_SPEC.md 3.20.1's table row and
             * the same value mt_matter_modebase_set()'s store-time tag-0
             * substitution uses. The fall-through's 0x4000 is kOff here, so
             * this arm is load-bearing rather than spelled-out: without it
             * the placeholder contradicts the store default on the one
             * cluster where the two numbers differ. */
            return chip::to_underlying(WaterHeaterMode::ModeTag::kManual);
        }
        return chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal);
    }

    chip::EndpointId m_ep      = chip::kInvalidEndpointId;
    chip::ClusterId  m_cluster = chip::kInvalidClusterId;
};

/*
 * Pool of MT_MB_MAX_LISTS delegate objects. mt_devtypes.cpp (Tasks 3-4 of
 * this batch) hands one out per (endpoint, cluster) it creates through
 * mt_matter_modebase_delegate_alloc(cluster_id), the same alloc-before-
 * create shape as every other pool in this file; the cluster id is fixed at
 * alloc time (see the class comment above for why), the endpoint id after
 * create() returns it.
 */
static HearthModeBaseDelegate s_mb_delegates[MT_MB_MAX_LISTS];
static size_t                 s_mb_delegate_next = 0;

extern "C" void *mt_matter_modebase_delegate_alloc(uint32_t cluster_id)
{
    if (s_mb_delegate_next >= MT_MB_MAX_LISTS) {
        return nullptr;
    }
    HearthModeBaseDelegate *d = &s_mb_delegates[s_mb_delegate_next++];
    d->set_cluster(cluster_id);
    return d;
}

extern "C" void mt_matter_modebase_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    static_cast<HearthModeBaseDelegate *>(delegate)->set_endpoint(ep);
}

/*
 * Constructs the ModeBase::Instance for OvenMode (0x0049) at the SDK's usual
 * init-callback timing, the shape HearthRvcOpStateInitCB below documents in
 * full (including why this is not extern "C").
 *
 * Every OTHER ModeBase cluster this firmware creates gets its Instance from
 * an esp-matter init callback: RvcRunModeDelegateInitCB,
 * RvcCleanModeDelegateInitCB, MicrowaveOvenModeDelegateInitCB,
 * RefrigeratorAndTCCModeDelegateInitCB, each wired by that cluster's own
 * esp_matter::cluster::<name>::create(). OvenMode has no such namespace and
 * no such callback (esp_matter_delegate_callbacks.h declares neither), so
 * mt_cabinet_add_heater() (mt_devtypes.cpp) hands this function to
 * set_delegate_and_init_callback() itself.
 *
 * Feature 0: OvenMode's ONLY feature is DEPONOFF (bit 0), and Mode_Oven.xml
 * revision 2 marks it <disallowConform/> ("Set OnOff feature, StartUpMode,
 * and OnMode as disallowed"), so the feature map is 0 by conformance rather
 * than by choice - the same 0 mt_cabinet_add_heater() writes into the
 * cluster's own FeatureMap attribute, which is what keeps the Instance's
 * answer and the ember attribute agreeing (the AirQuality B139 lesson).
 * The SDK's own MicrowaveOvenModeDelegateInitCB reads the map back with
 * get_feature_map_value() instead; that accessor is esp_matter-private, and
 * with the only feature disallowed there is nothing for it to find.
 */
void HearthOvenModeInitCB(void *delegate, uint16_t endpoint_id)
{
    auto *d = static_cast<HearthModeBaseDelegate *>(delegate);
    d->set_endpoint(endpoint_id);
    auto *inst = new chip::app::Clusters::ModeBase::Instance(d, endpoint_id,
                                                              chip::app::Clusters::OvenMode::Id, 0);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "OvenMode ModeBase::Instance::Init failed on ep %u: %" CHIP_ERROR_FORMAT,
                 (unsigned)endpoint_id, err.Format());
    }
}

/*
 * The bridge for AT+MTMODES's cluster-aware form. Grammar and content rules
 * (count bounds, mode-value uniqueness, tag range, label content) are
 * enforced by cmd_mtmodes() in mt_at.c before this is ever called; the
 * bounds re-checked here are defensive, the same split as
 * mt_matter_modes_set() above. This bridge is the sole place that validates
 * cluster against the eight ModeBase ids (composed appliance round: task 3
 * adds RefrigeratorAndTemperatureControlledCabinetMode and task 4 adds
 * OvenMode to the three from the RVC + Microwave batch; energy round B adds
 * WaterHeaterMode; energy round C1 adds DeviceEnergyManagementMode; and
 * 2026-08-31 adds EnergyEvseMode): mt_at.c stays free of any
 * esp_matter/CHIP header and cannot read those ids itself.
 *
 * ENERGYEVSEMODE IS THE EIGHTH AND IT ARRIVED LATE, which is worth a line
 * because the cluster itself has shipped since energy round C2. Its
 * SupportedModes could never be fed: it was absent from this condition and
 * from AT_MT_SPEC.md 3.20.1's accept list, so an EVSE endpoint served the
 * firmware placeholder mode for ever while ChangeToMode still forwarded for
 * adjudication, a controller being asked to choose between one option. The
 * spec now names 157/0x9D with a tag-0 default of kManual (0x4000), and
 * this firmware already produced exactly that value through the
 * fall-through arms below, so nothing a host or controller could already
 * observe changes. What changes is that AT+MTMODES=<ep>,157,... answers OK
 * instead of +MTERR:3: a contract ADDITION, not an alteration.
 */
extern "C" int mt_matter_modebase_set(uint16_t ep, uint32_t cluster, const uint8_t *modes, const uint16_t *tags,
                                       const char *const *labels, uint8_t count)
{
    using namespace chip::app::Clusters;
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (cluster != RvcRunMode::Id && cluster != RvcCleanMode::Id && cluster != MicrowaveOvenMode::Id &&
        cluster != RefrigeratorAndTemperatureControlledCabinetMode::Id && cluster != OvenMode::Id &&
        cluster != WaterHeaterMode::Id && cluster != DeviceEnergyManagementMode::Id &&
        cluster != EnergyEvseMode::Id) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (esp_matter::cluster::get(ep, cluster) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_MB_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_mb_slot_t *slot = nullptr;
    for (auto &s : s_mb_slots) {
        if (s.used && s.ep == ep && s.cluster == cluster) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        for (auto &s : s_mb_slots) {
            if (!s.used) {
                slot = &s;
                break;
            }
        }
    }
    if (!slot) {
        /* MT_MB_MAX_LISTS (mt_matter.h) bounds how many distinct (endpoint,
         * cluster) ModeBase lists this firmware stores at once; it is
         * smaller than MT_COMP_MAX_ENDPOINTS (mt_composition.h) - cited by
         * name rather than value, since both have already moved once across
         * the RVC/appliance and energy rounds and a number copied into this
         * comment goes stale exactly when either next changes - so a
         * composition dense enough in RvcRunMode/RvcCleanMode/
         * MicrowaveOvenMode/EnergyEvseMode/DeviceEnergyManagementMode
         * instances CAN exhaust it well under the endpoint cap (the
         * multiple-ModeBase-clusters-per-endpoint shape several device types
         * in this firmware use makes that reachable). This return is the
         * safe fallback for that case: the call is refused with
         * MT_ATTR_ERR_FAILED rather than silently overwriting an unrelated
         * (endpoint, cluster) slot. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Label validation is its own pass, ahead of any write, for two
     * reasons. It matches the nRF54L15 port's ordering, so a doubly
     * malformed list (bad label AND the microwave rule below) returns the
     * same error on both platforms; and the loop below writes
     * slot->entries[i] as it goes while count/used are only committed
     * after it, so failing mid-loop on a RE-FED (endpoint, cluster) list
     * used to overwrite the live list's leading entries and still answer
     * an error. Validating first closes that window. */
    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_MB_MAX_LABEL_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
    }

    /*
     * THE MICROWAVE'S ONE HOST-REACHABLE WAY TO CRIPPLE ITSELF, refused
     * here (B418, ruling DE417, brought over from the nRF54L15 port's
     * batch 8 fix round, which recorded this as a deliberate divergence
     * from the C6 until the C6 caught up).
     *
     * MicrowaveOvenControl resolves an omitted cookMode by asking this
     * cluster's Instance for the mode carrying the kNormal tag, and
     * answers InvalidCommand if there is none. A host feed whose entries
     * all carry EXPLICIT non-zero tags, none of them kNormal, is accepted
     * verbatim by the substitution loop below (it only rewrites tag 0),
     * and from that moment every SetCookingParameters on that endpoint
     * answers InvalidCommand, forever and silently, with no +MTCMD raised.
     *
     * A tag of 0 satisfies the rule, because the loop below turns it into
     * kNormal for this cluster; only a list that is explicitly and
     * entirely something else is refused.
     *
     * THIS IS A FIRMWARE-IMPOSED RULE, NOT A CONFORMANCE CHECK.
     * Mode_MicrowaveOven.xml lists Normal in its ModeTag enum and
     * constrains nothing about which tags a SupportedModes list must
     * carry. The rule is this firmware's, taken because the alternative is
     * a silently dead endpoint, and AT_MT_SPEC.md says so in those terms.
     */
    if (cluster == MicrowaveOvenMode::Id) {
        bool has_normal = false;
        for (uint8_t i = 0; i < count && !has_normal; i++) {
            has_normal = (tags[i] == 0) ||
                         (tags[i] == chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal));
        }
        if (!has_normal) {
            ESP_LOGE(TAG,
                     "AT+MTMODES on endpoint %u cluster 0x%08X refused: no mode carries the "
                     "kNormal tag (0x%04X), and MicrowaveOvenControl resolves an omitted "
                     "cookMode through it, so every SetCookingParameters would answer "
                     "InvalidCommand. Send tag 0 on at least one mode, or kNormal explicitly",
                     (unsigned)ep, (unsigned)cluster,
                     (unsigned)chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal));
            return MT_ATTR_ERR_VALUE;
        }
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        /* Tag-0 defaults, design spec section 9's exact policy, substituted
         * at store time so every read (GetModeTagsByIndex above) is
         * branch-free. */
        uint16_t tag = tags[i];
        if (tag == 0) {
            if (cluster == RvcRunMode::Id) {
                tag = chip::to_underlying(i == 0 ? RvcRunMode::ModeTag::kIdle : RvcRunMode::ModeTag::kCleaning);
            } else if (cluster == RvcCleanMode::Id) {
                tag = chip::to_underlying(RvcCleanMode::ModeTag::kVacuum);
            } else if (cluster == RefrigeratorAndTemperatureControlledCabinetMode::Id) {
                /* Auto (0x00), the ModeBase common tag every mode gets,
                 * same constant-per-mode shape as RvcCleanMode/
                 * MicrowaveOvenMode above (not RvcRunMode's index-0 special
                 * case): Mode_Refrigerator.xml. */
                tag = chip::to_underlying(RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kAuto);
            } else if (cluster == OvenMode::Id) {
                /* Bake (0x4000), Mode_Oven.xml's first oven-specific tag,
                 * same constant-per-mode shape as the two clusters above:
                 * OvenMode's conformance names no single mandatory tag, so
                 * the default is the one a host that does not care about
                 * tags most plausibly means. */
                tag = chip::to_underlying(OvenMode::ModeTag::kBake);
            } else if (cluster == WaterHeaterMode::Id) {
                /* Manual (0x4001), design spec 3.4's pinned default,
                 * verified against Mode_WaterHeater.xml before this branch
                 * was written: the XML defines Manual at line 86
                 * (`<item value="0x4001" name="Manual"/>`, between Off
                 * 0x4000 and Timed 0x4002) and encodes NO mandatory-tag
                 * conformance on SupportedModes at all (its attribute rows
                 * are bare), so nothing in it contradicts kManual and the
                 * spec's default stands: the everyday operating mode a host
                 * that does not care about tags most plausibly means, the
                 * OvenMode kBake reasoning. */
                tag = chip::to_underlying(WaterHeaterMode::ModeTag::kManual);
            } else if (cluster == DeviceEnergyManagementMode::Id) {
                /* NoOptimization (0x4000), energy round C1's pinned default
                 * (design spec 3.4), verified against
                 * Mode_DeviceEnergyManagement.xml before this branch was
                 * written: the XML defines NoOptimization at line 86
                 * (`<item value="0x4000" name="NoOptimization"/>`, first of
                 * the four cluster-specific tags) and carries no
                 * mandatory-tag conformance at all (revision 2's changes
                 * are the DEPONOFF feature and StartUpMode/OnMode all
                 * disallowConform, lines 71/113/116, correctly absent from
                 * the SDK build), so nothing contradicts kNoOptimization:
                 * a mode a host has not described makes no optimization
                 * promise, the honest resting default. */
                tag = chip::to_underlying(DeviceEnergyManagementMode::ModeTag::kNoOptimization);
            } else if (cluster == EnergyEvseMode::Id) {
                /* Manual (0x4000), AT_MT_SPEC.md 3.20's table row, added
                 * 2026-08-31 with the cluster's admission above and verified
                 * against Mode_EVSE.xml before this branch was written:
                 * the XML defines Manual as the first of the four
                 * cluster-specific tags and encodes no mandatory-tag
                 * conformance on SupportedModes at all, so nothing in it
                 * contradicts kManual, which is the everyday "charge when
                 * plugged in" mode a host that does not care about tags most
                 * plausibly means, the WaterHeaterMode kManual and OvenMode
                 * kBake reasoning.
                 *
                 * Written out rather than left to the else below even though
                 * the else produces the same NUMBER: kNormal is 0x4000 on
                 * MicrowaveOvenMode's enum and kManual is 0x4000 on this
                 * one, so the old fall-through was right by coincidence.
                 * placeholder_tag() above gains the same arm for the same
                 * reason. */
                tag = chip::to_underlying(EnergyEvseMode::ModeTag::kManual);
            } else {
                tag = chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal);
            }
        }
        slot->entries[i].mode = modes[i];
        slot->entries[i].tag  = tag;
        memcpy(slot->entries[i].label, labels[i], len + 1);
    }
    slot->ep      = ep;
    slot->cluster = cluster;
    slot->count   = count;
    slot->used    = true;

    MatterReportingAttributeChangeCallback(ep, cluster, ModeBase::Attributes::SupportedModes::Id);

    /* CurrentMode clamp: if the Instance's cached CurrentMode is no longer
     * one of the new list's mode values, reset it to the new list's first
     * entry. UpdateCurrentMode() reports the CurrentMode change itself
     * (mode-base-server.cpp) when it actually changes, so nothing further is
     * needed here. Skipped rather than failed when the Instance is not
     * (yet) reachable through the pool: the SupportedModes half of this call
     * has already succeeded, and an unreachable Instance here would mean
     * esp_matter::start()'s init-callback pass has not finished, which
     * cannot happen once mt_at_start() lets any AT+MTMODES call reach this
     * bridge (the same s_at_up boot-ordering this project relies on
     * elsewhere). */
    for (auto &d : s_mb_delegates) {
        if (d.endpoint() == ep && d.cluster() == cluster) {
            ModeBase::Instance *inst = d.instance();
            if (inst != nullptr && !inst->IsSupportedMode(inst->GetCurrentMode())) {
                inst->UpdateCurrentMode(slot->entries[0].mode);
            }
            break;
        }
    }

    return MT_ATTR_OK;
}

/*
 * ---- OperationalState trio (seven-type batch, task C4) --------------------
 *
 * F7: the Delegate interface for the base OperationalState cluster (0x0060),
 * shared verbatim by all three device types this task adds (dish_washer,
 * laundry_washer, laundry_dryer wire the SAME cluster: esp_matter_endpoint.cpp's
 * dish_washer::add()/laundry_washer::add()/laundry_dryer::add() bodies are
 * byte-identical apart from the device type id, each just calling
 * operational_state::create() and event::create_operation_completion()).
 * Seven pure virtuals (operational-state-server.h:279-372):
 *
 *   virtual app::DataModel::Nullable<uint32_t> GetCountdownTime() = 0;
 *   virtual CHIP_ERROR GetOperationalStateAtIndex(size_t index, GenericOperationalState & operationalState) = 0;
 *   virtual CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, MutableCharSpan & operationalPhase) = 0;
 *   virtual void HandlePauseStateCallback(GenericOperationalError & err) = 0;
 *   virtual void HandleResumeStateCallback(GenericOperationalError & err) = 0;
 *   virtual void HandleStartStateCallback(GenericOperationalError & err) = 0;
 *   virtual void HandleStopStateCallback(GenericOperationalError & err) = 0;
 *
 * GetCountdownTime: NullNullable. Out of scope this round (design spec
 * section 8: "OperationalState phases, countdown, and
 * OnOperationalErrorDetected: v2 surface once a consumer needs them").
 *
 * GetOperationalStateAtIndex: serves the four standard states at indices
 * 0..3, CHIP_ERROR_NOT_FOUND past that. The exact list content was verified
 * against an in-tree delegate rather than assumed: the dishwasher example's
 * own list (operational-state-delegate-impl.h:90-95,
 * examples/dishwasher-app/dishwasher-common) is
 *   "const GenericOperationalState dishwasherOpStateList[4] = {
 *        GenericOperationalState(to_underlying(OperationalStateEnum::kStopped)),
 *        GenericOperationalState(to_underlying(OperationalStateEnum::kRunning)),
 *        GenericOperationalState(to_underlying(OperationalStateEnum::kPaused)),
 *        GenericOperationalState(to_underlying(OperationalStateEnum::kError)),
 *    };"
 * i.e. index 0 Stopped, 1 Running, 2 Paused, 3 Error, exactly the order this
 * class serves.
 *
 * GetOperationalPhaseAtIndex: CHIP_ERROR_NOT_FOUND at index 0 always, so
 * PhaseList reads null (no phases defined at all). Direct from the header's
 * own doc comment on the virtual (server.h:305-317): "If CHIP_ERROR_NOT_FOUND
 * is returned for index 0, that indicates that the PhaseList attribute is
 * null (there are no phases defined at all)." Out of scope this round, same
 * v2-parking note as GetCountdownTime above.
 *
 * The four Handle*StateCallback: forward to the host over mt_cmd_forward()
 * for adjudication (the door lock/valve pattern) and fill err by reference.
 * UNLIKE the valve (F1: the SDK discards the delegate's return and always
 * answers Success), here err IS the wire response: the only caller of each,
 * Instance::HandlePauseState() (operational-state-server.cpp:414-447),
 * copies err straight into the command response after calling the delegate -
 *   "mDelegate->HandlePauseStateCallback(err);
 *    ...
 *    Commands::OperationalCommandResponse::Type response;
 *    response.commandResponseState = err;
 *    ctx.mCommandHandler.AddResponse(ctx.mRequestPath, response);"
 * - and HandleStopState/HandleStartState/HandleResumeState follow the same
 * shape. A deny here genuinely fails the command on the wire, unlike the
 * valve.
 *
 * err.Set() takes an ErrorStateEnum value (Enums.h): kNoError = 0x00 on
 * allow, kUnableToCompleteOperation = 0x02 on deny, verified rather than
 * guessed -
 *   "enum class ErrorStateEnum : uint8_t
 *    {
 *        kNoError                   = 0x00,
 *        kUnableToStartOrResume     = 0x01,
 *        kUnableToCompleteOperation = 0x02,
 *        kCommandInvalidInState     = 0x03,
 *        ...
 *    };"
 * kUnableToCompleteOperation, not e.g. kUnableToStartOrResume, matches the
 * in-tree example's own choice for a generic delegate-side denial
 * (dishwasher-common's placeholder implementation uses the same code for
 * every one of its four callbacks).
 *
 * Deliberately does NOT call Instance::SetOperationalState() itself on
 * allow: same split-ownership shape as the door lock/valve (AT_MT_SPEC.md
 * 3.18/3.19). The delegate's job is only the adjudication verdict; the
 * actual OperationalState transition is reported once the host confirms it
 * really executed the transition, via AT+MTOPSTATE / mt_matter_opstate_set()
 * below. (dishwasher-common's placeholder callback calls
 * SetOperationalState() directly, which is reasonable for a self-contained
 * example but wrong for this firmware, where "the host decided" and "the
 * host actually did it" are different moments, same reasoning as
 * AT+MTLOCK/AT+MTVALVE.)
 *
 * Endpoint id: same problem the valve pool solves. HandlePauseStateCallback
 * et al. are not handed one, but mt_cmd_forward() needs it. Same fix: an
 * m_ep member set once by the thunk (mt_devtypes.cpp) right after create()
 * returns the real id.
 *
 * Instance reachability, F7's other half. The Instance object is not
 * constructed by the thunk or by create(): esp_matter defers it to its own
 * init-callback pass (OperationalStateDelegateInitCB,
 * esp_matter_delegate_callbacks.cpp:309-323), which news one
 * OperationalState::Instance per endpoint into a FILE-STATIC
 * std::unordered_map (esp_matter_delegate_callbacks.cpp:68,
 * "s_operational_state_instances") with no getter anywhere in the component:
 * esp_matter_delegate_callbacks.h declares only the *DelegateInitCB
 * functions, nothing that reads the map back. That init pass itself only
 * runs, for endpoints created before esp_matter::start() (this firmware's
 * own rule, CLAUDE.md), during provider::Startup()
 * (esp_matter_data_model_provider.cpp:297-302: "Call the init callbacks for
 * the endpoints which are created before esp_matter::start()").
 *
 * So esp-matter's own map is unusable from here, and a registry filled at
 * thunk time cannot work either: the Instance does not exist yet at thunk
 * time, only after esp_matter::start() runs. The mechanism actually used
 * needs no registry at all. The Instance constructor calls the delegate's
 * own SetInstance() on itself (operational-state-server.cpp:43-48):
 *   "Instance::Instance(Delegate * aDelegate, EndpointId aEndpointId, ClusterId aClusterId) :
 *        ...
 *    {
 *        ...
 *        mDelegate->SetInstance(this);
 *    }"
 * and Delegate::SetInstance() (server.h:349-372) stores it in the base
 * class's own mInstance, readable back through a PROTECTED GetInstance()
 * accessor:
 *   "void SetInstance(Instance * aInstance)
 *    {
 *        VerifyOrDie(mInstance == nullptr || aInstance == nullptr || mInstance == aInstance);
 *        mInstance = aInstance;
 *    }
 *    ...
 *    protected:
 *        Instance * GetInstance() { return mInstance; }"
 * (the VerifyOrDie is F7's "one delegate OBJECT per endpoint is mandatory":
 * a second Instance sharing this delegate would abort the device outright).
 * Since HearthOpStateDelegate derives from Delegate, its own member
 * functions may call the protected GetInstance() on themselves (standard
 * C++ access: a derived class reaches a base's protected member through an
 * object of its own type). A public one-line passthrough, instance() below,
 * is what mt_matter_opstate_set() calls: no parallel registry, no map, no
 * getter esp-matter does not provide. It reads null until
 * esp_matter::start() runs the init-callback pass, which always completes
 * before mt_at_start() lets any AT+MTOPSTATE reach this bridge (the same
 * s_at_up ordering CLAUDE.md documents for URCs), so a null instance() here
 * would mean something is badly out of sequence, not an expected runtime
 * state; the bridge still checks for it and answers FAILED rather than
 * assume.
 */
class HearthOpStateDelegate : public chip::app::Clusters::OperationalState::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    void set_cluster(chip::ClusterId cluster) { m_cluster = cluster; }
    chip::EndpointId endpoint() const { return m_ep; }
    chip::ClusterId  cluster() const { return m_cluster; }

    /* GetInstance() is protected in the base Delegate; this passthrough is
     * how mt_matter_opstate_set() (below) reaches the Instance the SDK
     * attached via SetInstance(), see the class comment above. */
    chip::app::Clusters::OperationalState::Instance *instance() { return GetInstance(); }

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::NullNullable;
    }

    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index, chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override
    {
        using chip::app::Clusters::OperationalState::OperationalStateEnum;
        static const OperationalStateEnum states[] = {
            OperationalStateEnum::kStopped,
            OperationalStateEnum::kRunning,
            OperationalStateEnum::kPaused,
            OperationalStateEnum::kError,
        };
        if (index >= (sizeof(states) / sizeof(states[0]))) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState.Set(chip::to_underlying(states[index]));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override
    {
        /* PhaseList ships null: no phase exists at index 0. See the class
         * comment above; index/operationalPhase are unused on this path. */
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    /*
     * Pause and Resume are disallowConform on OvenCavityOperationalState
     * (OperationalState_Oven.xml revision 2, "Set Pause and Resume commands
     * as disallowed"), so mt_cabinet_add_heater() (mt_devtypes.cpp) creates
     * no command entry for either and a controller's invoke is refused
     * UNSUPPORTED_COMMAND by the data model before it could reach here.
     * These two overrides are still answered defensively, the same shape the
     * SDK's own RvcOperationalState::Delegate uses for the Start/Stop its
     * cluster does not support (operational-state-server.h:390-406:
     * "err.Set(to_underlying(OperationalState::ErrorStateEnum::
     * kUnknownEnumValue));"). Forwarding them instead would ask the host to
     * adjudicate a command the cluster does not have.
     */
    void HandlePauseStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        if (unsupported_on_cavity(err)) {
            return;
        }
        forward(chip::app::Clusters::OperationalState::Commands::Pause::Id, err);
    }

    void HandleResumeStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        if (unsupported_on_cavity(err)) {
            return;
        }
        forward(chip::app::Clusters::OperationalState::Commands::Resume::Id, err);
    }

    void HandleStartStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Start::Id, err);
    }

    void HandleStopStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Stop::Id, err);
    }

private:
    /*
     * The command ids passed to forward() are the BASE cluster's, and they
     * are correct for the oven cavity too: OvenCavityOperationalState's own
     * generated CommandIds.h declares Stop 0x01, Start 0x02 and
     * OperationalCommandResponse 0x04, the identical numeric values the base
     * OperationalState cluster uses (checked against both generated headers,
     * not assumed from the derivation). Only the CLUSTER id differs per
     * endpoint, which is exactly what m_cluster carries: a cavity's Stop
     * forwards as +MTCMD against cluster 72/0x48 command 1, a washer's
     * against cluster 96/0x60 command 1.
     */
    void forward(uint32_t command, chip::app::Clusters::OperationalState::GenericOperationalError &err)
    {
        using chip::app::Clusters::OperationalState::ErrorStateEnum;
        bool allow = mt_cmd_forward(m_ep, m_cluster, command);
        err.Set(chip::to_underlying(allow ? ErrorStateEnum::kNoError : ErrorStateEnum::kUnableToCompleteOperation));
    }

    bool unsupported_on_cavity(chip::app::Clusters::OperationalState::GenericOperationalError &err) const
    {
        using chip::app::Clusters::OperationalState::ErrorStateEnum;
        if (m_cluster != chip::app::Clusters::OvenCavityOperationalState::Id) {
            return false;
        }
        err.Set(chip::to_underlying(ErrorStateEnum::kUnknownEnumValue));
        return true;
    }

    chip::EndpointId m_ep      = chip::kInvalidEndpointId;
    /* Defaults to the base cluster so a slot handed out before task 4's
     * cluster-aware alloc existed would still behave; every call site sets it
     * explicitly (mt_matter_opstate_delegate_alloc()). */
    chip::ClusterId  m_cluster = chip::app::Clusters::OperationalState::Id;
};

/*
 * Pool of MT_COMP_MAX_ENDPOINTS delegate objects, same shape as
 * s_valve_delegates above: dish_washer/laundry_washer/laundry_dryer, the
 * microwave, and (composed appliance round, task 4) the oven cavity each
 * hand out their own object from this SHARED pool (F7: one delegate object
 * per endpoint, never shared between two Instances - VerifyOrDie above).
 * Exposed to mt_devtypes.cpp through the same opaque void* pair pattern as
 * the valve pool, so that file never has to name HearthOpStateDelegate or
 * any CHIP delegate type. The cluster id is fixed at alloc time (task 4),
 * the same reasoning the ModeBase pool above documents; see mt_matter.h.
 */
static HearthOpStateDelegate s_opstate_delegates[MT_COMP_MAX_ENDPOINTS];
static size_t                s_opstate_delegate_next = 0;

extern "C" void *mt_matter_opstate_delegate_alloc(uint32_t cluster_id)
{
    if (s_opstate_delegate_next >= MT_COMP_MAX_ENDPOINTS) {
        return nullptr;
    }
    HearthOpStateDelegate *d = &s_opstate_delegates[s_opstate_delegate_next++];
    d->set_cluster(cluster_id);
    return d;
}

extern "C" void mt_matter_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    static_cast<HearthOpStateDelegate *>(delegate)->set_endpoint(ep);
}

/*
 * The pool lookup mt_matter_opstate_set() below uses for both clusters this
 * pool now serves. Keyed by (endpoint, cluster) rather than endpoint alone:
 * see that function's own comment.
 */
static chip::app::Clusters::OperationalState::Instance *find_opstate_instance(chip::EndpointId ep,
                                                                              chip::ClusterId cluster)
{
    for (auto &d : s_opstate_delegates) {
        if (d.endpoint() == ep && d.cluster() == cluster) {
            return d.instance();
        }
    }
    return nullptr;
}

/*
 * ---- Oven cavity OperationalState (composed appliance round, task 4) -----
 *
 * OvenCavityOperationalState (0x0048) is the RVC opstate's situation one
 * degree worse: esp-matter has no cluster::oven_cavity_operational_state
 * namespace AT ALL (checked by grepping the whole component: the only oven
 * symbols it carries are the endpoint namespace and the two empty
 * MatterOven*PluginServerInitCallback stubs), so mt_cabinet_add_heater()
 * (mt_devtypes.cpp) hand-rolls the cluster shell by mirroring
 * rvc_operational_state::create() one cluster id over, and hands this
 * function to set_delegate_and_init_callback() itself.
 *
 * CHIP, unlike esp-matter, DOES ship the server half: the generic
 * OperationalState::Instance(Delegate*, EndpointId, ClusterId) constructor
 * the brief named is PROTECTED (operational-state-server.h:177, inside the
 * "protected:" block at :166), so it cannot be called from a free function
 * like this one at all - but the header also declares a purpose-built
 * OvenCavityOperationalState::Instance (:459-478) whose public two-argument
 * constructor forwards to exactly that protected one with Id baked in. That
 * is what is constructed here; no local subclass is needed to reach the
 * protected constructor, and no derived Delegate exists for this cluster
 * either (unlike RvcOperationalState, which needs one for GoHome), so the
 * plain HearthOpStateDelegate above serves it with only its cluster id
 * changed.
 *
 * The Instance's own constructor calls mDelegate->SetInstance(this)
 * (operational-state-server.cpp), so HearthOpStateDelegate::instance()'s
 * GetInstance() passthrough reaches it afterwards with no bookkeeping here,
 * exactly as it does for the SDK-constructed base-cluster Instances. Not
 * extern "C", for the same delegate_init_callback_t linkage reason
 * HearthRvcOpStateInitCB below documents in full; mt_devtypes.cpp
 * forward-declares it by name.
 */
void HearthOvenCavityOpStateInitCB(void *delegate, uint16_t endpoint_id)
{
    auto *d = static_cast<HearthOpStateDelegate *>(delegate);
    d->set_endpoint(endpoint_id);
    auto *inst = new chip::app::Clusters::OvenCavityOperationalState::Instance(d, endpoint_id);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "OvenCavityOperationalState::Instance::Init failed on ep %u: %" CHIP_ERROR_FORMAT,
                 (unsigned)endpoint_id, err.Format());
    }
}

/*
 * ---- RVC OperationalState (RVC + Microwave batch, task 3) ----------------
 *
 * Unlike the base OperationalState cluster the trio above uses,
 * rvc_operational_state::create() (esp_matter_cluster.cpp:3103-3134,
 * verified against the pinned tree) is fully app-owned: config_t is
 * common::config_t, an EMPTY struct with no delegate field at all
 * (esp_matter_cluster.h:1007-1010), and create()'s body only creates
 * attributes/events - no set_delegate_and_init_callback() call, no
 * command::create_* call, unlike every other cluster::create() in this
 * codebase. Everything F7's OperationalState trio above gets for free from
 * esp-matter (delegate wiring, Instance construction, all four commands)
 * this cluster needs done by hand: mt_rvc_opstate_add_commands()
 * (mt_devtypes.cpp) wires the commands and calls
 * set_delegate_and_init_callback() itself, and HearthRvcOpStateInitCB below
 * constructs the Instance at that same callback pass rather than relying on
 * any SDK-provided RvcOperationalStateDelegateInitCB (none exists:
 * esp_matter_delegate_callbacks.h declares only the base
 * OperationalStateDelegateInitCB, nothing RVC-specific).
 *
 * Class hierarchy (operational-state-server.h:376-455, verified against the
 * pinned tree):
 *   RvcOperationalState::Delegate : public OperationalState::Delegate
 *     - HandleStartStateCallback/HandleStopStateCallback are already
 *       overridden in the base RvcOperationalState::Delegate itself (Start/
 *       Stop are not supported by this cluster at all), each answering
 *       kUnknownEnumValue - so this class does not implement them.
 *     - HandleGoHomeCommandCallback has a default body (same
 *       kUnknownEnumValue) but is virtual, not final: overridden below.
 *     - HandlePauseStateCallback/HandleResumeStateCallback remain pure
 *       virtual from the OperationalState::Delegate base and MUST be
 *       implemented here.
 *   RvcOperationalState::Instance : public OperationalState::Instance
 *     - Constructed with (Delegate*, EndpointId); its constructor passes
 *       Id (RvcOperationalState::Id) as the ClusterId to the protected
 *       three-argument OperationalState::Instance constructor
 *       (operational-state-server.h:421-423).
 *     - Overrides IsDerivedClusterStatePauseCompatible/
 *       IsDerivedClusterStateResumeCompatible/InvokeDerivedClusterCommand;
 *       these run INSIDE the base Instance's HandlePauseState/
 *       HandleResumeState/GoHome dispatch, entirely server-side, BEFORE
 *       this delegate is ever consulted (operational-state-server.cpp:
 *       414-570, verified line by line):
 *         - Pause is compatible with kSeekingCharger in addition to the
 *           base's own kRunning/kPaused (IsDerivedClusterStatePauseCompatible,
 *           :522-525).
 *         - Resume is compatible with kCharging/kDocked
 *           (IsDerivedClusterStateResumeCompatible, :527-531).
 *         - GoHome from kCharging/kDocked answers ErrorStateID 3
 *           (kCommandInvalidInState) WITHOUT calling this delegate at all
 *           (:556-559).
 *         - GoHome from kSeekingCharger answers success (kNoError) WITHOUT
 *           calling this delegate either (:561 guards the call on "opState
 *           != kSeekingCharger") - the cluster already treats "already
 *           seeking the charger" as a no-op success.
 *       None of these three guards are duplicated here: this delegate's
 *       three Handle*Callback methods only ever run for the cases the
 *       server has already decided are legitimate adjudication requests.
 *
 * bind_instance()/instance(): unlike HearthOpStateDelegate above (which
 * reaches its Instance through the base Delegate's protected GetInstance()
 * passthrough, since esp-matter's own generic OperationalStateDelegateInitCB
 * constructs that Instance on this class's behalf), this class stores its
 * own typed RvcOperationalState::Instance* directly, set by
 * HearthRvcOpStateInitCB right after construction: since this codebase
 * writes that init callback itself (no SDK equivalent exists), it can hand
 * the pointer over exactly this way - one plain member, rather than a
 * second protected-accessor passthrough for no behavioural difference
 * (GetInstance() would return the identical pointer, upcast to
 * OperationalState::Instance*).
 *
 * Forward/deny mapping and endpoint tracking: identical shape to
 * HearthOpStateDelegate's forward() above - mt_cmd_forward() against
 * RvcOperationalState::Id, kNoError on allow, kUnableToCompleteOperation on
 * deny (the same in-tree-example precedent HearthOpStateDelegate's comment
 * above cites). GoHome carries no forwarded payload (Task 1's
 * mt_cmd_forward, not mt_cmd_forward_payload): the GoHome command itself has
 * no fields.
 */
class HearthRvcOpStateDelegate : public chip::app::Clusters::RvcOperationalState::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    /* Set once, by HearthRvcOpStateInitCB below, right after the Instance is
     * constructed (see the class comment above for why this is a plain
     * member rather than the GetInstance() passthrough pattern). */
    void bind_instance(chip::app::Clusters::RvcOperationalState::Instance *inst) { m_instance = inst; }
    chip::app::Clusters::RvcOperationalState::Instance *instance() { return m_instance; }

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::NullNullable;
    }

    /*
     * Seven states, RVC's own enum (RvcOperationalState::OperationalStateEnum,
     * Enums.h): the four base states plus the three derived-number-space
     * ones AT+MTOPSTATE may set (design spec section 9's exact set,
     * verified against the pinned Enums.h). kEmptyingDustBin/kCleaningMop/
     * kFillingWaterTank/kUpdatingMaps (0x43-0x46) exist in the enum but are
     * out of this task's scope (not in the AT+MTOPSTATE union either), so
     * are not published here.
     */
    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index, chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override
    {
        using chip::app::Clusters::RvcOperationalState::OperationalStateEnum;
        static const OperationalStateEnum states[] = {
            OperationalStateEnum::kStopped,       OperationalStateEnum::kRunning, OperationalStateEnum::kPaused,
            OperationalStateEnum::kError,         OperationalStateEnum::kSeekingCharger,
            OperationalStateEnum::kCharging,      OperationalStateEnum::kDocked,
        };
        if (index >= (sizeof(states) / sizeof(states[0]))) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState.Set(chip::to_underlying(states[index]));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override
    {
        /* PhaseList ships null, same v2-parking note as HearthOpStateDelegate
         * above. */
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandlePauseStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::Pause::Id, err);
    }

    void HandleResumeStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::Resume::Id, err);
    }

    void HandleGoHomeCommandCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::GoHome::Id, err);
    }

private:
    void forward(uint32_t command, chip::app::Clusters::OperationalState::GenericOperationalError &err)
    {
        using chip::app::Clusters::OperationalState::ErrorStateEnum;
        bool allow = mt_cmd_forward(m_ep, chip::app::Clusters::RvcOperationalState::Id, command);
        err.Set(chip::to_underlying(allow ? ErrorStateEnum::kNoError : ErrorStateEnum::kUnableToCompleteOperation));
    }

    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    chip::app::Clusters::RvcOperationalState::Instance *m_instance = nullptr;
};

/*
 * Pool of MT_COMP_MAX_ENDPOINTS delegate objects, same sizing rationale as
 * s_opstate_delegates above: one delegate OBJECT per endpoint (F7's
 * VerifyOrDie), and an RVC endpoint carries exactly one RvcOperationalState
 * cluster, so the pool is sized by endpoint count, not by the
 * (endpoint, cluster) pair shape the ModeBase pool needs.
 */
static HearthRvcOpStateDelegate s_rvc_opstate_delegates[MT_COMP_MAX_ENDPOINTS];
static size_t                   s_rvc_opstate_delegate_next = 0;

extern "C" void *mt_matter_rvc_opstate_delegate_alloc(void)
{
    if (s_rvc_opstate_delegate_next >= MT_COMP_MAX_ENDPOINTS) {
        return nullptr;
    }
    return &s_rvc_opstate_delegates[s_rvc_opstate_delegate_next++];
}

extern "C" void mt_matter_rvc_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    /* Belt-and-braces: HearthRvcOpStateInitCB below also sets this, the
     * moment esp_matter::start()'s init-callback pass constructs the
     * Instance, since (unlike the base OperationalState trio) this codebase
     * writes that callback itself and can set the endpoint there directly.
     * Calling this from mt_devtypes.cpp right after create() keeps this
     * pool's fixup shape identical to every other delegate pool in this
     * file; setting the same value twice (this call, then the init
     * callback's own) is harmless. */
    static_cast<HearthRvcOpStateDelegate *>(delegate)->set_endpoint(ep);
}

/*
 * Constructs the RvcOperationalState::Instance at the SDK's usual
 * init-callback timing (esp_matter_data_model_provider.cpp's
 * provider::Startup(), which runs during esp_matter::start(), after the
 * boot rebuild's endpoints all exist and before mt_at_start() can let any
 * AT+MTOPSTATE/+MTCMD reach this delegate - the same s_at_up boot ordering
 * CLAUDE.md documents everywhere else). No SDK-provided
 * RvcOperationalStateDelegateInitCB exists to reuse (see the class comment
 * above), so mt_rvc_opstate_add_commands() (mt_devtypes.cpp) hands this
 * function straight to esp_matter::cluster::set_delegate_and_init_callback() itself.
 *
 * Not extern "C": esp_matter::cluster::set_delegate_and_init_callback() takes a
 * delegate_init_callback_t, a C++-linkage function pointer type
 * (esp_matter_data_model.h), so this cannot be wrapped the way every other
 * mt_devtypes.cpp <-> main.cpp bridge is (mt_matter.h's extern "C" contract
 * would give it the wrong linkage to assign). mt_devtypes.cpp forward-
 * declares this one function by name instead, matching signatures, ordinary
 * external C++ linkage; HearthRvcOpStateDelegate itself stays private to
 * this file.
 *
 * The SDK's own calling convention discards Init()'s return value (see the
 * ModeBase placeholder-mode comment above for the identical traced
 * mechanism: esp_matter's generic init-callback wrappers call Init() with
 * no check at all), so there is otherwise no diagnostic whatsoever if this
 * ever fails; logged loudly here since nothing else will.
 */
void HearthRvcOpStateInitCB(void *delegate, uint16_t endpoint_id)
{
    auto *d = static_cast<HearthRvcOpStateDelegate *>(delegate);
    d->set_endpoint(endpoint_id);
    auto *inst = new chip::app::Clusters::RvcOperationalState::Instance(d, endpoint_id);
    d->bind_instance(inst);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "RvcOperationalState::Instance::Init failed on ep %u: %" CHIP_ERROR_FORMAT, (unsigned)endpoint_id,
                 err.Format());
    }
}

/*
 * AT+MTOPSTATE bridge: find ep's delegate in whichever pool matches its
 * cluster and, through it, the Instance the SDK attached
 * (HearthOpStateDelegate::instance()/HearthRvcOpStateDelegate::instance(),
 * see each class comment above for its own mechanism), then call
 * Instance::SetOperationalState() directly - it is declared to take a plain
 * uint8_t, not an enum (operational-state-server.h:97), so state is passed
 * through unconverted. mt_matter.h's own doc comment on this function has
 * the full per-cluster state-space table; this body is its implementation.
 *
 * mt_at.c's cmd_mtopstate has already rejected anything outside the UNION
 * of both clusters' legal state sets with +MTERR:1 before this is ever
 * called (it cannot know which cluster ep actually has), so this bridge is
 * what narrows a value that is legal for the WRONG cluster (e.g. 0x40 on a
 * plain OperationalState endpoint) down to MT_ATTR_ERR_VALUE.
 * SetOperationalState() itself enforces the same per-cluster rule
 * independently (operational-state-server.cpp:101-107: "if (aOpState ==
 * to_underlying(OperationalStateEnum::kError) || !IsSupportedOperationalState(aOpState))
 * { return CHIP_ERROR_INVALID_ARGUMENT; }"), so a state that somehow slipped
 * past both checks would map to MT_ATTR_ERR_FAILED here rather than being
 * silently accepted.
 */
extern "C" int mt_matter_opstate_set(uint16_t ep, uint8_t state)
{
    using namespace chip::app::Clusters;
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }

    /*
     * The base-cluster and oven-cavity branches share one delegate pool, so
     * both look their Instance up by (endpoint, cluster), not by endpoint
     * alone: no composition this firmware builds puts two
     * OperationalState-family clusters on one endpoint, but matching the
     * cluster too costs one comparison and removes the assumption entirely.
     */
    if (esp_matter::cluster::get(ep, OperationalState::Id) != nullptr) {
        if (state > 2) {
            return MT_ATTR_ERR_VALUE;
        }
        OperationalState::Instance *inst = find_opstate_instance(ep, OperationalState::Id);
        if (inst == nullptr) {
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
    }

    /*
     * Composed appliance round, task 4: the oven cavity's cluster derives
     * from OperationalState but adds no derived-number-space states of its
     * own (OperationalState_Oven.xml constrains only the command set), so
     * the legal set is the plain {0, 1, 2} - the same test the base branch
     * above runs, deliberately not the RVC branch's widened one.
     */
    if (esp_matter::cluster::get(ep, OvenCavityOperationalState::Id) != nullptr) {
        if (state > 2) {
            return MT_ATTR_ERR_VALUE;
        }
        OperationalState::Instance *inst = find_opstate_instance(ep, OvenCavityOperationalState::Id);
        if (inst == nullptr) {
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
    }

    if (esp_matter::cluster::get(ep, RvcOperationalState::Id) != nullptr) {
        if (!(state <= 2 || state == 0x40 || state == 0x41 || state == 0x42)) {
            return MT_ATTR_ERR_VALUE;
        }
        OperationalState::Instance *inst = nullptr;
        for (auto &d : s_rvc_opstate_delegates) {
            if (d.endpoint() == ep) {
                inst = d.instance();
                break;
            }
        }
        if (inst == nullptr) {
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
    }

    return MT_ATTR_ERR_CLUSTER;
}

/*
 * ---- Microwave Oven Control (RVC + Microwave batch, task 4) --------------
 *
 * microwave_oven::add() (esp_matter_endpoint.cpp:1620-1632, verified against
 * the pinned tree) creates three clusters on the endpoint: the PLAIN
 * OperationalState cluster (0x0060, F7's pool above - the microwave has no
 * derived opstate cluster the way RVC does, so mt_opstate_add_commands()'s
 * washer-era command hand-add applies here verbatim, same landmine, same
 * fix), MicrowaveOvenMode (0x005E, task 2's ModeBase pool, keyed by
 * MicrowaveOvenMode::Id), and MicrowaveOvenControl (0x005F, this delegate).
 * There is deliberately no Identify cluster on this device type
 * (microwave_oven::config_t's base, app_with_operational_state_config, has
 * no identify field at all, esp_matter_endpoint.h:213-218, unlike e.g.
 * water_valve::config_t which adds one explicitly) - not an omission to fix.
 *
 * MicrowaveOvenControlDelegateInitCB (esp_matter_delegate_callbacks.cpp:
 * 270-308) is the one init callback in this codebase that depends on TWO
 * OTHER clusters' delegates already being registered on the same endpoint:
 * it looks up MicrowaveOvenMode's and OperationalState's delegates through
 * cluster::get()+get_delegate_impl() and requires ALL THREE (those two, plus
 * this cluster's own config.delegate) non-null -
 * "VerifyOrReturn(delegate != nullptr && microwave_oven_mode_delegate !=
 * nullptr && operational_state_delegate != nullptr);" - before constructing
 * anything. Fail that check and the callback returns having built nothing:
 * no Instance, no log line, no error path of any kind, so a microwave
 * endpoint with a missing delegate would silently answer every
 * MicrowaveOvenControl read/write/invoke as if the cluster were not
 * registered at all. mk_microwave_oven() (mt_devtypes.cpp) therefore
 * allocates and wires all three delegate pools BEFORE calling create(), and
 * this class comment is the citation trail mt_matter.h's own
 * mt_matter_mwoc_delegate_alloc() doc comment points back to.
 *
 * Three pure-virtual groups (microwave-oven-control-server.h, verified
 * against the pinned tree):
 *   - HandleSetCookingParametersCallback/HandleModifyCookTimeSecondsCallback:
 *     the two forwarded commands, implemented below.
 *   - GetMaxCookTimeSec/GetPowerSettingNum/GetMinPowerNum/GetMaxPowerNum/
 *     GetPowerStepNum: plain getters. GetMaxCookTimeSec is 86400 (24h, the
 *     design spec's fixed bound, never host-configurable this round).
 *     GetPowerSettingNum returns the last accepted power (default 100, the
 *     SDK's own kDefaultMaxPowerNum) - see the field-resolution note below
 *     for why this firmware is the sole owner of that value. GetMinPowerNum/
 *     GetMaxPowerNum/GetPowerStepNum (10/100/10, the SDK's own kDefault*
 *     constants) exist only to satisfy the pure-virtual contract: neither
 *     Instance::Read() nor Instance::HandleSetCookingParameters() ever calls
 *     them on this firmware, both read the identical kDefault* constants
 *     directly whenever PowerNumberLimits is unset (see the feature-flag
 *     note below for why it always is).
 *   - GetWattSettingByIndex/GetCurrentWattIndex/GetWattRating: the
 *     PowerInWatts side of the cluster, which this firmware never enables
 *     (see the feature-flag note below). GetWattSettingByIndex answers
 *     CHIP_ERROR_NOT_FOUND at every index (the same "index until exhausted,
 *     empty list" shape HearthChimeDelegate/HearthTempLevelsDelegate use
 *     elsewhere in this file); GetCurrentWattIndex/GetWattRating are 0/0
 *     stubs, reachable only because WattRating (unlike SelectedWattIndex) is
 *     NOT feature-gated in Instance::Read() and so is always readable
 *     regardless of which power feature is enabled.
 *
 * Field resolution, traced against Instance::HandleSetCookingParameters()
 * (microwave-oven-control-server.cpp, verified line by line against the
 * pinned tree, not assumed from the Delegate header's doc comments alone):
 * cookMode/cookTimeSec/startAfterSetting arrive at the delegate ALREADY
 * RESOLVED, never Optional at all - the callback's own C++ signature below
 * declares them uint8_t/uint32_t/bool, not Optional<...>. The server
 * defaults cookMode to whichever mode carries the kNormal tag when the
 * invoke omits it, cookTimeSec to kDefaultCookTimeSec (30 s), and
 * startAfterSetting to false, all before this delegate is ever called. Only
 * powerSettingNum/wattSettingIndex remain Optional<uint8_t> at the delegate
 * boundary, and because this firmware enables PowerAsNumber only (below),
 * the server's own PowerInWatts branch never runs: powerSettingNum ALWAYS
 * carries a value here (the server defaults it to MaxPower, 100, when the
 * invoke omits it too) and wattSettingIndex is ALWAYS NullOptional. So on
 * this firmware's actual wire traffic the four forwarded fields are never
 * empty in practice; the empty-field convention AT_MT_SPEC.md 3.17
 * documents for the general multi-field form still applies verbatim, it
 * just never triggers here (it would on a hypothetical future PowerInWatts
 * build, where wattSettingIndex could be the field left empty instead).
 *
 * Feature flags: PowerAsNumber is MANDATORY, not a choice -
 * microwave_oven_control::create() (esp_matter_cluster.cpp:3058-3100) runs
 * VALIDATE_FEATURES_EXACT_ONE against it alone and ABORTS cluster creation
 * (ABORT_CLUSTER_CREATE) on anything else, so mk_microwave_oven() below sets
 * it unconditionally. PowerNumberLimits is deliberately NOT set, and this is
 * a decision, not an oversight: its own add() (esp_matter_feature.cpp:
 * 2975-2992) only applies the limits when PowerAsNumber's bit is ABSENT from
 * the feature map - "if ((get_feature_map_value(cluster) &
 * power_as_number_feature_map) != power_as_number_feature_map) { ... } else
 * { ESP_LOGE(...); return ESP_ERR_NOT_SUPPORTED; }" - the inverse of the
 * cluster's own spec conformance (Instance::Init()'s VerifyOrReturnError
 * requires PowerNumberLimits to imply PowerAsNumber, not exclude it). With
 * PowerAsNumber mandatory on every microwave endpoint this firmware builds,
 * that add() can never reach its "apply" branch here, so enabling
 * PowerNumberLimits would buy nothing but a logged ESP_ERR_NOT_SUPPORTED:
 * dead code in this pinned tree. MinPower/MaxPower/PowerStep therefore
 * always read the SDK's own compiled-in defaults (10/100/10), not this
 * delegate's getters, exactly as the class comment above already notes.
 *
 * CookTime/PowerSetting ownership and visibility: CookTime lives in
 * MicrowaveOvenControl::Instance itself (SetCookTimeSec()/GetCookTimeSec()),
 * set here on an allowed SetCookingParameters/AddMoreTime the same way the
 * SDK's own reference implementation does it
 * (examples/microwave-oven-app/.../microwave-oven-device.cpp:
 * "mMicrowaveOvenControlInstance.SetCookTimeSec(cookTimeSec);", the pattern
 * this class's own two Handle*Callback methods below follow). PowerSetting lives
 * in this delegate object instead (m_power_setting), since the Delegate
 * interface has no SetPowerSettingNum() of any kind - GetPowerSettingNum()
 * is the only hook, so accepting a new value on allow is this firmware's own
 * bookkeeping, not an SDK-provided setter. Both attributes are served by
 * Instance::Read() (an AttributeAccessInterface, exactly like ModeBase's
 * CurrentMode and OperationalState's OperationalState attribute, §3.20.1/
 * §3.21 of AT_MT_SPEC.md), so neither write ever reaches
 * esp_matter::attribute::set_val(): no +MTATTR URC fires for either, and
 * AT+MTATTR has no path to read or write them. A host observes CookTime/
 * PowerSetting only through a commissioned controller.
 */
class HearthMwocDelegate : public chip::app::Clusters::MicrowaveOvenControl::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    /* GetInstance() is protected in the base Delegate; this passthrough is
     * the same mechanism HearthOpStateDelegate::instance() uses above: the
     * Instance is constructed by MicrowaveOvenControlDelegateInitCB (the
     * class comment above), which calls SetInstance() on this delegate
     * exactly once, before mt_at_start() lets any command reach this class
     * (the same s_at_up boot ordering CLAUDE.md documents everywhere else).
     * Used here to reach Instance::SetCookTimeSec() on an allowed forward. */
    chip::app::Clusters::MicrowaveOvenControl::Instance *instance() { return GetInstance(); }

    /*
     * SetCookingParameters: the four-field forward, field order fixed
     * (cookMode,cookTime,power,startAfter) per AT_MT_SPEC.md §3.17. cookMode/
     * cookTimeSec/startAfterSetting are already-resolved plain values (the
     * class comment above traces why); only powerSettingNum needs a
     * HasValue() check, and on this firmware's PowerAsNumber-only build it
     * always has one (same citation). wattSettingIndex is not forwarded at
     * all: it is always NullOptional here and carries no information the
     * host needs to see.
     *
     * Same raw-passthrough verdict shape as HearthChimeDelegate::
     * PlayChimeSound() above, not the OperationalState family's
     * GenericOperationalError indirection: Instance::HandleSetCookingParameters()
     * (microwave-oven-control-server.cpp) copies whatever Status this
     * returns straight into the command's InvokeResponse via
     * ctx.mCommandHandler.AddStatus(), no remapping. CookTime/PowerSetting
     * are applied only on allow: a denied command is a refusal to actually
     * accept the new cooking parameters, so nothing is written for the host
     * to have refused.
     */
    chip::Protocols::InteractionModel::Status HandleSetCookingParametersCallback(
        uint8_t cookMode, uint32_t cookTimeSec, bool startAfterSetting,
        chip::Optional<uint8_t> powerSettingNum, chip::Optional<uint8_t> wattSettingIndex) override
    {
        using chip::Protocols::InteractionModel::Status;
        char fields[40];
        int n = 0;
        n += snprintf(fields + n, sizeof(fields) - n, "%u,", (unsigned)cookMode);
        n += snprintf(fields + n, sizeof(fields) - n, "%lu,", (unsigned long)cookTimeSec);
        n += powerSettingNum.HasValue()
                 ? snprintf(fields + n, sizeof(fields) - n, "%u,", (unsigned)powerSettingNum.Value())
                 : snprintf(fields + n, sizeof(fields) - n, ",");
        n += snprintf(fields + n, sizeof(fields) - n, "%u", startAfterSetting ? 1u : 0u);
        (void)n;
        bool allow = mt_cmd_forward_fields(m_ep, chip::app::Clusters::MicrowaveOvenControl::Id,
                                            chip::app::Clusters::MicrowaveOvenControl::Commands::SetCookingParameters::Id,
                                            fields);
        if (allow) {
            if (instance() != nullptr) {
                instance()->SetCookTimeSec(cookTimeSec);
            }
            if (powerSettingNum.HasValue()) {
                m_power_setting = powerSettingNum.Value();
            }
        }
        return allow ? Status::Success : Status::Failure;
    }

    /*
     * AddMoreTime: the single-field forward, finalCookTimeSec, AT_MT_SPEC.md
     * §3.17. Same allow-applies/deny-refuses and raw-passthrough-verdict
     * shape as SetCookingParameters above.
     */
    chip::Protocols::InteractionModel::Status HandleModifyCookTimeSecondsCallback(uint32_t finalCookTimeSec) override
    {
        using chip::Protocols::InteractionModel::Status;
        char fields[16];
        snprintf(fields, sizeof(fields), "%lu", (unsigned long)finalCookTimeSec);
        bool allow = mt_cmd_forward_fields(m_ep, chip::app::Clusters::MicrowaveOvenControl::Id,
                                            chip::app::Clusters::MicrowaveOvenControl::Commands::AddMoreTime::Id, fields);
        if (allow && instance() != nullptr) {
            instance()->SetCookTimeSec(finalCookTimeSec);
        }
        return allow ? Status::Success : Status::Failure;
    }

    /* PowerAsNumber-only stubs; see the class comment above for why the
     * SDK never actually calls the three plain getters below this round. */
    CHIP_ERROR GetWattSettingByIndex(uint8_t index, uint16_t &wattSetting) override
    {
        (void)index;
        (void)wattSetting;
        return CHIP_ERROR_NOT_FOUND;
    }

    uint32_t GetMaxCookTimeSec() const override { return 86400; }
    uint8_t  GetPowerSettingNum() const override { return m_power_setting; }
    uint8_t  GetMinPowerNum() const override { return 10; }
    uint8_t  GetMaxPowerNum() const override { return 100; }
    uint8_t  GetPowerStepNum() const override { return 10; }
    uint8_t  GetCurrentWattIndex() const override { return 0; }
    uint16_t GetWattRating() const override { return 0; }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    uint8_t m_power_setting = 100; /* kDefaultMaxPowerNum, the SDK's own PowerSetting default */
};

/*
 * Pool of MT_COMP_MAX_ENDPOINTS delegate objects, the same shape as
 * s_opstate_delegates/s_chime_delegates above. Exposed to mt_devtypes.cpp
 * through the same opaque void* pair pattern, so that file never has to name
 * HearthMwocDelegate or any CHIP delegate type.
 */
static HearthMwocDelegate s_mwoc_delegates[MT_COMP_MAX_ENDPOINTS];
static size_t             s_mwoc_delegate_next = 0;

extern "C" void *mt_matter_mwoc_delegate_alloc(void)
{
    if (s_mwoc_delegate_next >= MT_COMP_MAX_ENDPOINTS) {
        return nullptr;
    }
    return &s_mwoc_delegates[s_mwoc_delegate_next++];
}

extern "C" void mt_matter_mwoc_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    static_cast<HearthMwocDelegate *>(delegate)->set_endpoint(ep);
}

/*
 * ---- electrical measurement: EPM, PowerTopology, EEM (energy round A, task 2) ----
 *
 * Three clusters, two models, one bridge (AT+MTMEAS, mt_matter_meas_set()
 * below):
 *
 *   ElectricalPowerMeasurement (0x0090) is PULL-model: the CHIP Instance is
 *   an AttributeAccessInterface that answers every read by calling the
 *   Delegate's Get*() methods (electrical-power-measurement-server.h:63-75),
 *   so the host's pushed values live in HearthEpmDelegate members and a
 *   MatterReportingAttributeChangeCallback per applied field is what makes
 *   subscriptions fire (the delegate setter precedent in the SDK's own
 *   all_device_types_app ElectricalPowerMeasurementDelegate).
 *
 *   PowerTopology (0x009C) with the NodeTopology feature needs only a
 *   constructible Delegate: its two pure virtuals are the endpoint-list
 *   iterators backing the AvailableEndpoints/ActiveEndpoints attributes,
 *   which exist only under the SET/TREE topology features
 *   (power-topology-server.cpp gates both reads on the optional attributes,
 *   which in turn require those features), so HearthPtopDelegate answers
 *   both with PROVIDER_LIST_EXHAUSTED and is never actually consulted.
 *
 *   ElectricalEnergyMeasurement (0x0091) is PUSH-model free functions:
 *   NotifyCumulativeEnergyMeasured() stores the structs into the server's
 *   own per-endpoint MeasurementData AND emits the CumulativeEnergyMeasured
 *   event in one call. That storage was verified to cover DYNAMIC endpoints
 *   before this block was written (the round's one hard unknown):
 *   gMeasurements is sized MATTER_DM_..._ENDPOINT_COUNT +
 *   CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT
 *   (ElectricalEnergyMeasurementCluster.cpp:42) and
 *   emberAfGetClusterServerEndpointIndex() maps a dynamic endpoint to
 *   fixed_count + its dynamic-table slot (attribute-storage.cpp:963), so
 *   with CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT (29) above
 *   MT_COMP_MAX_ENDPOINTS (28) every endpoint this firmware can create has
 *   a slot.
 *
 * Init routes, per cluster (the report's "which CB" question):
 *   - EPM and PowerTopology use the SDK's OWN init callbacks
 *     (ElectricalPowerMeasurementDelegateInitCB / PowerTopologyDelegateInitCB,
 *     esp_matter_delegate_callbacks.cpp:406/418): cluster::create() wires
 *     them automatically when config.delegate is set, and they construct the
 *     Instance from the endpoint's real FeatureMap attribute plus the set of
 *     optional attributes actually created on the endpoint
 *     (is_attribute_enabled), which keeps the Instance's feature answers and
 *     the ember attribute agreeing by construction (the B139 lesson) instead
 *     of by a second hardcoded list here. No custom CB exists for either.
 *   - EEM has NO SDK init callback and, worse, its wildcard attribute-access
 *     object is never registered by anything in the SDK:
 *     ESPMatterElectricalEnergyMeasurementClusterServerInitCallback is
 *     declared (ClusterCallbacks.h:124) with no implementation and no
 *     caller, the section-8.6 disease in yet another organ, so without app
 *     code every EEM attribute read would answer Failure while the ember
 *     metadata happily advertises the attributes. HearthEemInitCB below is
 *     that app code: the task-4 thunk hands it to
 *     set_delegate_and_init_callback() itself (the HearthOvenModeInitCB
 *     precedent; the delegate pointer is null and ignored, which is safe
 *     because esp_matter invokes the callback whenever it is set,
 *     esp_matter_data_model.cpp:264-266).
 *
 * Accuracy: both clusters must serve a MeasurementAccuracyStruct (EPM as the
 * mandatory Accuracy list through the delegate, EEM as its Accuracy
 * attribute via SetMeasurementAccuracy()). This firmware is a bridge; the
 * real metering hardware sits behind the host, whose accuracy this firmware
 * cannot know. The figures served (0.1% - 1%, 0.5% typical, one range
 * covering the full XML value span) are the middle band of the SDK
 * reference implementation's own table (all_device_types_app
 * electrical_measurement.cpp), a documented assumption task 3 records in
 * the AT spec rather than a measured fact.
 */

/* The XML value bounds mt_matter_meas_set() validates against
 * (electrical-power-measurement-cluster.xml /
 * electrical-energy-measurement-cluster.xml, pinned tree). Not available
 * from any generated header, so cited literals rather than transcribed
 * enum values. */
static constexpr int64_t kMeasValueAbsMax = 4611686018427387904LL; /* +-2^62 */
static constexpr int64_t kMeasFreqMax     = 1000000;   /* Frequency, mHz  */
static constexpr int64_t kMeasPfAbsMax    = 10000;     /* PowerFactor, 1/100 % */

static const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type
    s_meas_power_accuracy_ranges[] = {
    {
        .rangeMin       = -kMeasValueAbsMax,
        .rangeMax       = kMeasValueAbsMax,
        .percentMax     = chip::MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = chip::MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = chip::MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
};

/* The one mandatory EPM accuracy entry: ActivePower, the cluster's one
 * mandatory measured value. */
static const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type
    s_meas_epm_accuracy = {
    .measurementType  = chip::app::Clusters::ElectricalPowerMeasurement::MeasurementTypeEnum::kActivePower,
    .measured         = true,
    .minMeasuredValue = -kMeasValueAbsMax,
    .maxMeasuredValue = kMeasValueAbsMax,
    .accuracyRanges   = chip::app::DataModel::List<
        const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(
        s_meas_power_accuracy_ranges),
};

static const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type
    s_meas_energy_accuracy_ranges[] = {
    {
        .rangeMin       = 0,
        .rangeMax       = kMeasValueAbsMax,
        .percentMax     = chip::MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = chip::MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = chip::MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
};

/* Same tolerance figures as the EPM entry above, but typed kElectricalEnergy
 * over the energy value span: EEM's Accuracy attribute describes the energy
 * measurement itself, so serving the ActivePower-typed struct verbatim would
 * satisfy the letter of "the same accuracy" while violating the attribute's
 * meaning. */
static const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyStruct::Type
    s_meas_eem_accuracy = {
    .measurementType  = chip::app::Clusters::ElectricalEnergyMeasurement::MeasurementTypeEnum::kElectricalEnergy,
    .measured         = true,
    .minMeasuredValue = 0,
    .maxMeasuredValue = kMeasValueAbsMax,
    .accuracyRanges   = chip::app::DataModel::List<
        const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(
        s_meas_energy_accuracy_ranges),
};

/*
 * The EPM delegate: a store for host-pushed values, all null until the host
 * first pushes them, plus the fixed answers the pull-model server needs.
 * Base-class SetEndpointId()/mEndpointId (electrical-power-measurement-
 * server.h:36) carry the endpoint, set once by the thunk after create()
 * returns the id and again (idempotently) by the Instance constructor at
 * init-callback time.
 */
class HearthEpmDelegate : public chip::app::Clusters::ElectricalPowerMeasurement::Delegate
{
public:
    /* Host-pushed values, written only by mt_matter_meas_set() below. */
    chip::app::DataModel::Nullable<int64_t> m_voltage, m_active_current, m_active_power,
        m_frequency, m_power_factor, m_rms_voltage, m_rms_current;

    chip::EndpointId endpoint() const { return mEndpointId; }

    chip::app::Clusters::ElectricalPowerMeasurement::PowerModeEnum GetPowerMode() override
    {
        return chip::app::Clusters::ElectricalPowerMeasurement::PowerModeEnum::kAc;
    }
    uint8_t GetNumberOfMeasurementTypes() override { return 1; }

    /* Accuracy: exactly one entry (ActivePower, the mandatory one), backed
     * by a static const table, so the Start/End read brackets have nothing
     * to lock (the SDK reference delegate's own reasoning). */
    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(
        uint8_t index,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type &accuracy) override
    {
        if (index > 0) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        accuracy = s_meas_epm_accuracy;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    /* Ranges and harmonics: empty lists. */
    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementRangeStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    /* The seven field-table attributes, served from the pushed store. */
    chip::app::DataModel::Nullable<int64_t> GetVoltage() override { return m_voltage; }
    chip::app::DataModel::Nullable<int64_t> GetActiveCurrent() override { return m_active_current; }
    chip::app::DataModel::Nullable<int64_t> GetActivePower() override { return m_active_power; }
    chip::app::DataModel::Nullable<int64_t> GetFrequency() override { return m_frequency; }
    chip::app::DataModel::Nullable<int64_t> GetPowerFactor() override { return m_power_factor; }
    chip::app::DataModel::Nullable<int64_t> GetRMSVoltage() override { return m_rms_voltage; }
    chip::app::DataModel::Nullable<int64_t> GetRMSCurrent() override { return m_rms_current; }

    /* Everything the field table does not carry: permanently null. These
     * attributes are never created on the endpoint (task 4), so these
     * answers exist only to satisfy the pure-virtual contract. */
    chip::app::DataModel::Nullable<int64_t> GetReactiveCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetReactivePower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetNeutralCurrent() override { return {}; }
};

/*
 * The PowerTopology delegate: NodeTopology only, so both pure virtuals
 * (the SET/TREE endpoint-list iterators, see the block comment above) answer
 * "empty" and the object exists purely so the SDK init CB has something to
 * construct the Instance around. The endpoint member mirrors the other
 * pools' shape for debuggability; nothing reads it.
 */
class HearthPtopDelegate : public chip::app::Clusters::PowerTopology::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }

    CHIP_ERROR GetAvailableEndpointAtIndex(size_t, chip::EndpointId &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR GetActiveEndpointAtIndex(size_t, chip::EndpointId &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/*
 * The two pools, MT_MEAS_MAX each (mt_matter.h documents the sizing), same
 * alloc-before-create shape as every other pool in this file. One setter
 * serves both pools: the void* the thunk hands back is matched against each
 * pool's own objects, so mt_devtypes.cpp needs neither a second name nor
 * any knowledge of which class it is holding.
 */
static HearthEpmDelegate  s_meas_epm_delegates[MT_MEAS_MAX];
static size_t             s_meas_epm_next = 0;
static HearthPtopDelegate s_meas_ptop_delegates[MT_MEAS_MAX];
static size_t             s_meas_ptop_next = 0;

extern "C" void *mt_matter_epm_delegate_alloc(void)
{
    if (s_meas_epm_next >= MT_MEAS_MAX) {
        return nullptr;
    }
    return &s_meas_epm_delegates[s_meas_epm_next++];
}

extern "C" void *mt_matter_ptop_delegate_alloc(void)
{
    if (s_meas_ptop_next >= MT_MEAS_MAX) {
        return nullptr;
    }
    return &s_meas_ptop_delegates[s_meas_ptop_next++];
}

extern "C" void mt_matter_meas_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    for (auto &d : s_meas_epm_delegates) {
        if (&d == delegate) {
            d.SetEndpointId(ep);
            return;
        }
    }
    for (auto &d : s_meas_ptop_delegates) {
        if (&d == delegate) {
            d.set_endpoint(ep);
            return;
        }
    }
}

/* The pool lookup mt_matter_meas_set() uses for the EPM branch. */
static HearthEpmDelegate *meas_epm_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_meas_epm_next; i++) {
        if (s_meas_epm_delegates[i].endpoint() == ep) {
            return &s_meas_epm_delegates[i];
        }
    }
    return nullptr;
}

/*
 * Per-EEM-endpoint init at the SDK's usual init-callback timing, handed to
 * set_delegate_and_init_callback() by the task-4 thunk itself since no SDK
 * callback exists for this cluster (see the block comment above; not
 * extern "C" for the same signature reason HearthRvcOpStateInitCB
 * documents). Two jobs:
 *
 *   1. Once, on the first EEM endpoint: register the server's wildcard
 *      AttributeAccessInterface, without which every EEM attribute read
 *      answers Failure (the never-called
 *      ESPMatterElectricalEnergyMeasurementClusterServerInitCallback
 *      disease). The features passed here are what the AttrAccess serves
 *      for EVERY EEM endpoint's FeatureMap read, so the task-4 thunk must
 *      create the cluster with exactly these three feature flags
 *      (Imported + Exported + Cumulative) or FeatureMap would lie.
 *   2. Per endpoint: serve the Accuracy attribute via
 *      SetMeasurementAccuracy(), which needs the endpoint to exist in ember
 *      first, which is exactly what init-callback timing guarantees (the
 *      same ordering every Instance-constructing CB in this file relies
 *      on).
 */
void HearthEemInitCB(void *delegate, uint16_t endpoint_id)
{
    (void)delegate; /* null by contract, see mt_matter.h */
    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;

    static ElectricalEnergyMeasurementAttrAccess *s_eem_attr_access = nullptr;
    if (s_eem_attr_access == nullptr) {
        s_eem_attr_access = new ElectricalEnergyMeasurementAttrAccess(
            chip::BitMask<Feature, uint32_t>(Feature::kImportedEnergy, Feature::kExportedEnergy,
                                             Feature::kCumulativeEnergy),
            chip::BitMask<OptionalAttributes, uint32_t>());
        CHIP_ERROR err = s_eem_attr_access->Init();
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "EEM AttrAccess registration failed: %" CHIP_ERROR_FORMAT, err.Format());
        }
    }

    CHIP_ERROR err = SetMeasurementAccuracy(endpoint_id, s_meas_eem_accuracy);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "EEM SetMeasurementAccuracy failed on ep %u: %" CHIP_ERROR_FORMAT,
                 (unsigned)endpoint_id, err.Format());
    }
}

/* The WaterHeaterManagement (0x0094) branch body, energy round B: defined in
 * the water heater section below, after HearthWhmDelegate and its pool exist
 * to look into. Runs under the ChipStackLock mt_matter_meas_set() already
 * holds; see its definition for the contract. */
static int mt_meas_whm_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count);

/* The DeviceEnergyManagement (0x0098) branch body, energy round C1 task 2:
 * defined in the DEM section below, after HearthDemDelegate and its pool
 * exist to look into. Same held-lock contract as mt_meas_whm_apply(). */
static int mt_meas_dem_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count);

/*
 * The AT+MTMEAS bridge. Grammar (pair count bounds, integer parses) is
 * mt_at.c's business (task 3); this bridge owns everything that needs the
 * data model: endpoint and cluster lookup (the MTALARM dispatcher's shape),
 * per-field range validation against the cluster XML bounds, feature gating
 * (the 0x94 family, energy round B), and the atomic apply. Two passes, a
 * hard contract from the task brief: every pair is validated before any
 * pair is applied, so a bad third pair leaves the first two unapplied.
 *
 * The EnergyEvse (0x0099) branch, energy round C2 task 7, does not have a
 * body here: mt_evse_meas_apply_locked() (mt_evse.cpp, task 4) already
 * implements the same two-pass validate-then-apply and its own metadata
 * existence gate, so this bridge only needs to admit the cluster id and
 * forward. See mt_evse.h for why that function lives in its own
 * translation unit and mt_evse.cpp's file comment for why the gate reads
 * esp_matter::attribute::get() rather than a feature-bit table.
 */
extern "C" int mt_matter_meas_set(uint16_t ep, uint32_t cluster, const uint8_t *fields,
                                  const int64_t *values, uint8_t count)
{
    using namespace chip::app::Clusters;
    ChipStackLock lock;

    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (cluster != ElectricalPowerMeasurement::Id && cluster != ElectricalEnergyMeasurement::Id &&
        cluster != WaterHeaterManagement::Id && cluster != DeviceEnergyManagement::Id &&
        cluster != EnergyEvse::Id) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (esp_matter::cluster::get(ep, cluster) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1) {
        /* Defensive; mt_at.c never passes an empty list. */
        return MT_ATTR_ERR_FAILED;
    }

    if (cluster == WaterHeaterManagement::Id) {
        return mt_meas_whm_apply(ep, fields, values, count);
    }
    if (cluster == DeviceEnergyManagement::Id) {
        return mt_meas_dem_apply(ep, fields, values, count);
    }
    if (cluster == EnergyEvse::Id) {
        return mt_evse_meas_apply_locked(ep, fields, values, count);
    }

    if (cluster == ElectricalPowerMeasurement::Id) {
        HearthEpmDelegate *d = meas_epm_for(ep);
        if (d == nullptr) {
            /* Cluster present but no pool slot serves this endpoint: cannot
             * happen once the boot rebuild has run, kept as the same
             * defensive answer the other pool bridges give. */
            return MT_ATTR_ERR_FAILED;
        }

        /* Pass 1: validate everything. */
        for (uint8_t i = 0; i < count; i++) {
            switch (fields[i]) {
            case MT_MEAS_F_VOLTAGE:
            case MT_MEAS_F_ACTIVE_CURRENT:
            case MT_MEAS_F_ACTIVE_POWER:
            case MT_MEAS_F_RMS_VOLTAGE:
            case MT_MEAS_F_RMS_CURRENT:
                if (values[i] < -kMeasValueAbsMax || values[i] > kMeasValueAbsMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            case MT_MEAS_F_FREQUENCY:
                if (values[i] < 0 || values[i] > kMeasFreqMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            case MT_MEAS_F_POWER_FACTOR:
                if (values[i] < -kMeasPfAbsMax || values[i] > kMeasPfAbsMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            default:
                return MT_ATTR_ERR_VALUE;
            }
        }

        /* Pass 2: apply, one subscription report per applied field. */
        for (uint8_t i = 0; i < count; i++) {
            chip::app::DataModel::Nullable<int64_t> v = chip::app::DataModel::MakeNullable(values[i]);
            uint32_t attr_id;
            switch (fields[i]) {
            case MT_MEAS_F_VOLTAGE:
                d->m_voltage = v;
                attr_id = ElectricalPowerMeasurement::Attributes::Voltage::Id;
                break;
            case MT_MEAS_F_ACTIVE_CURRENT:
                d->m_active_current = v;
                attr_id = ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id;
                break;
            case MT_MEAS_F_ACTIVE_POWER:
                d->m_active_power = v;
                attr_id = ElectricalPowerMeasurement::Attributes::ActivePower::Id;
                break;
            case MT_MEAS_F_FREQUENCY:
                d->m_frequency = v;
                attr_id = ElectricalPowerMeasurement::Attributes::Frequency::Id;
                break;
            case MT_MEAS_F_POWER_FACTOR:
                d->m_power_factor = v;
                attr_id = ElectricalPowerMeasurement::Attributes::PowerFactor::Id;
                break;
            case MT_MEAS_F_RMS_VOLTAGE:
                d->m_rms_voltage = v;
                attr_id = ElectricalPowerMeasurement::Attributes::RMSVoltage::Id;
                break;
            default: /* MT_MEAS_F_RMS_CURRENT, pass 1 admits nothing else */
                d->m_rms_current = v;
                attr_id = ElectricalPowerMeasurement::Attributes::RMSCurrent::Id;
                break;
            }
            MatterReportingAttributeChangeCallback(ep, ElectricalPowerMeasurement::Id, attr_id);
        }
        return MT_ATTR_OK;
    }

    /* ---- ElectricalEnergyMeasurement ---- */

    /* Pass 1: validate everything. Values are cumulative mWh counters,
     * unsigned on the wire; anything negative here is either a negative
     * input or a u64 pattern above INT64_MAX, both outside the XML's
     * 0..2^62. */
    for (uint8_t i = 0; i < count; i++) {
        if (fields[i] != MT_ENERGY_F_IMPORTED && fields[i] != MT_ENERGY_F_EXPORTED) {
            return MT_ATTR_ERR_VALUE;
        }
        if (values[i] < 0 || values[i] > kMeasValueAbsMax) {
            return MT_ATTR_ERR_VALUE;
        }
    }

    ElectricalEnergyMeasurement::MeasurementData *data =
        ElectricalEnergyMeasurement::MeasurementDataForEndpoint(ep);
    if (data == nullptr) {
        /* Cluster present in esp_matter's model but ember cannot map it:
         * cannot happen once the endpoint is enabled (see the dynamic-
         * endpoint verification in the block comment above). */
        return MT_ATTR_ERR_FAILED;
    }

    bool    have_imp = false, have_exp = false;
    int64_t imp_val = 0, exp_val = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (fields[i] == MT_ENERGY_F_IMPORTED) {
            have_imp = true;
            imp_val  = values[i]; /* duplicate fields: last one wins */
        } else {
            have_exp = true;
            exp_val  = values[i];
        }
    }

    /* Timestamp policy (task brief step 4): wall time when synced, else the
     * systime fields; end = now, start = the previous push's end, carried
     * per endpoint by the server's own stored structs (the EVSE reference
     * implementation's exact shape, EVSEManufacturerImpl.cpp
     * SendCumulativeEnergyReading). */
    uint32_t now_ts = 0;
    bool     wall   = (chip::System::Clock::GetClock_MatterEpochS(now_ts) == CHIP_NO_ERROR);
    uint64_t now_ms = static_cast<uint64_t>(
        chip::System::SystemClock().GetMonotonicMilliseconds64().count());

    auto build = [&](int64_t energy,
                     const chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> &prev) {
        ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type s;
        s.energy = energy;
        if (prev.HasValue()) {
            s.startTimestamp = prev.Value().endTimestamp;
            s.startSystime   = prev.Value().endSystime;
        }
        if (wall) {
            s.endTimestamp.SetValue(now_ts);
        } else {
            s.endSystime.SetValue(now_ms);
        }
        return s;
    };

    /* A side this call does not mention is carried forward unchanged:
     * NotifyCumulativeEnergyMeasured() REPLACES both stored sides
     * (ElectricalEnergyMeasurementCluster.cpp:197-198), so passing Missing
     * for the un-pushed one would null its attribute out from under any
     * subscriber. The cost is that the event restates the carried side's
     * previous reading, which the spec's "imported, exported, or both"
     * event wording tolerates. */
    chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> imported =
        data->cumulativeImported;
    chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> exported =
        data->cumulativeExported;
    if (have_imp) {
        imported.SetValue(build(imp_val, data->cumulativeImported));
    }
    if (have_exp) {
        exported.SetValue(build(exp_val, data->cumulativeExported));
    }

    if (!ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(ep, imported, exported)) {
        return MT_ATTR_ERR_FAILED;
    }

    /* Notify writes the store and emits the event but reports no attribute
     * change (ElectricalEnergyMeasurementCluster.cpp:191-218 has no
     * MatterReportingAttributeChangeCallback), so subscriptions on the
     * attributes themselves are fired here, one per pushed side, the same
     * one-report-per-applied-field contract as the EPM branch. */
    if (have_imp) {
        MatterReportingAttributeChangeCallback(
            ep, ElectricalEnergyMeasurement::Id,
            ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id);
    }
    if (have_exp) {
        MatterReportingAttributeChangeCallback(
            ep, ElectricalEnergyMeasurement::Id,
            ElectricalEnergyMeasurement::Attributes::CumulativeEnergyExported::Id);
    }
    return MT_ATTR_OK;
}

/*
 * ---- water heater management (energy round B, task 1) ----------------------
 *
 * WaterHeaterManagement (0x0094) is FULLY delegate-served: the Instance IS
 * the cluster's AttributeAccessInterface (water-heater-management-server.
 * cpp:94-100 registers it as both command handler and attribute access), so
 * all six attributes answer from the Delegate's getters and, without a
 * delegate, every read would answer Failure while the ember metadata happily
 * advertises them (the EEM 8.10 disease; here the fix is a real delegate).
 * The delegate contract, verbatim from water-heater-management-server.h
 * (lines 93-113, the eight pure virtuals):
 *
 *   virtual Protocols::InteractionModel::Status HandleBoost(uint32_t duration, Optional<bool> oneShot,
 *                                                           Optional<bool> emergencyBoost, Optional<int16_t> temporarySetpoint,
 *                                                           Optional<Percent> targetPercentage, Optional<Percent> targetReheat) = 0;
 *   virtual Protocols::InteractionModel::Status HandleCancelBoost() = 0;
 *   virtual BitMask<WaterHeaterHeatSourceBitmap> GetHeaterTypes() = 0;
 *   virtual BitMask<WaterHeaterHeatSourceBitmap> GetHeatDemand()  = 0;
 *   virtual uint16_t GetTankVolume()                              = 0;
 *   virtual Energy_mWh GetEstimatedHeatRequired()                 = 0;
 *   virtual Percent GetTankPercentage()                           = 0;
 *   virtual BoostStateEnum GetBoostState()                        = 0;
 *
 * plus the two non-virtual event helpers (header lines 124-133), the
 * NotifyCumulativeEnergyMeasured one-call shape, which task 3's BoostState
 * transition derivation calls:
 *
 *   CHIP_ERROR GenerateBoostStartedEvent(uint32_t durationSecs, Optional<bool> oneShot, Optional<bool> emergencyBoost,
 *                                        Optional<int16_t> temporarySetpoint, Optional<Percent> targetPercentage,
 *                                        Optional<Percent> targetReheat);
 *   CHIP_ERROR GenerateBoostEndedEvent();
 *
 * and the Instance constructor (header line 142):
 *
 *   Instance(EndpointId aEndpointId, Delegate & aDelegate, Feature aFeature)
 *
 * which WaterHeaterManagementDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp:487-498) news around the delegate with
 * a feature bitmask SNAPSHOTTED from the endpoint's FeatureMap attribute at
 * delegate-init time (get_feature_map_value(), then Init()): the thunk must
 * therefore have hand-set the WHM feature bits on the cluster BEFORE the
 * init callback runs (task 2; both SDK feature helpers are broken, survey
 * A.2). The CB keeps a static single-Instance pointer overwritten per
 * endpoint, which leaks nothing this firmware does twice: endpoints are
 * built once per boot.
 *
 * Endpoint id: the Instance constructor calls mDelegate.SetEndpointId()
 * (water-heater-management-server.h:142-148), the same as EPM, so the SDK
 * does eventually stamp it at init-callback time. This pool still takes the
 * endpoint id at alloc time, for two honest reasons: the task brief pins the
 * alloc(ep) interface for tasks 2/3, and setting it at handout makes task
 * 3's by-endpoint slot lookup independent of init-CB timing; the Instance
 * ctor later overwrites it with the identical value. See
 * mt_matter_whm_delegate_alloc() below and its mt_matter.h doc comment for
 * the handout protocol.
 *
 * Command forwarding: HandleBoost/HandleCancelBoost run on the CHIP event
 * loop task (the Instance's CommandHandlerInterface calls them synchronously
 * from the invoke path), so NO ChipStackLock is taken here, the same
 * reasoning as the door lock block above (main.cpp's own rule: delegate
 * methods invoked BY CHIP already hold the stack) and the same shape as
 * HearthMwocDelegate's two Handle*Callback forwards. The verdict is a raw
 * passthrough: Instance::HandleBoost()/HandleCancelBoost()
 * (water-heater-management-server.cpp:224-241) copy whatever Status the
 * delegate returns straight into the command response via AddStatus(), no
 * remapping, so a deny genuinely fails the command on the wire (the
 * opstate/microwave side of the valve-vs-opstate split, AT_MT_SPEC.md 3.19
 * vs 3.21).
 *
 * Split ownership, the AT+MTLOCK/AT+MTVALVE reasoning: an allowed Boost does
 * NOT touch m_boost_state here. The host decides when the boost is really
 * running and pushes BoostState Active over AT+MTMEAS (task 3), whose
 * Inactive-to-Active transition derives the BoostStarted event from m_boost
 * below; "the host decided" and "the host actually did it" are different
 * moments.
 */
class HearthWhmDelegate : public chip::app::Clusters::WaterHeaterManagement::Delegate
{
public:
    /* Host-pushed cached state, written only by task 3's
     * mt_matter_meas_set() 0x94 branch. The defaults are the pre-first-push
     * answers the design spec 2.4 pins: everything 0, BoostState Inactive. */
    uint8_t  m_heater_types = 0;
    uint8_t  m_heat_demand  = 0;
    chip::app::Clusters::WaterHeaterManagement::BoostStateEnum m_boost_state =
        chip::app::Clusters::WaterHeaterManagement::BoostStateEnum::kInactive;
    uint16_t m_tank_volume  = 0;
    int64_t  m_est_heat_req = 0;
    uint8_t  m_tank_percent = 0;

    /* The parameters of the last host-ACCEPTED Boost command, cached for the
     * derived BoostStarted event (design spec 3.3: task 3 hands these to
     * GenerateBoostStartedEvent() on the Inactive-to-Active push). valid
     * false with duration 0 means none: a host-initiated boost with no prior
     * controller command (physical button) emits duration 0, no optionals. */
    struct BoostParams {
        uint32_t                      duration = 0;
        chip::Optional<bool>          one_shot;
        chip::Optional<bool>          emergency;
        chip::Optional<int16_t>       setpoint;
        chip::Optional<chip::Percent> target_pct;
        chip::Optional<chip::Percent> reheat;
        bool                          valid = false;
    } m_boost;

    chip::EndpointId endpoint() const { return mEndpointId; }

    /*
     * Boost: pack the five Optionals into the design spec 3.2 wire form,
     * "<duration>,<mask>[,<v1>[,<v2>[,<v3>]]]": MT_BOOST_P_* presence bits
     * in canonical order, MT_BOOST_V_* carrying the two bools' values when
     * present, then ONLY the present numeric optionals appended in canonical
     * order (worked example: duration 3600, oneShot true, targetPercentage
     * 80 forwards as "3600,265,80"). The Instance has already range-checked
     * targetPercentage/targetReheat and their feature conformance before
     * calling this (water-heater-management-server.cpp:171-221), so the
     * packing needs no validation of its own. Accepted parameters are cached
     * on allow only: a denied Boost never started, so there is nothing for a
     * later BoostStarted event to describe.
     */
    chip::Protocols::InteractionModel::Status HandleBoost(
        uint32_t duration, chip::Optional<bool> oneShot, chip::Optional<bool> emergencyBoost,
        chip::Optional<int16_t> temporarySetpoint, chip::Optional<chip::Percent> targetPercentage,
        chip::Optional<chip::Percent> targetReheat) override
    {
        using chip::Protocols::InteractionModel::Status;

        unsigned mask = 0;
        if (oneShot.HasValue()) {
            mask |= MT_BOOST_P_ONESHOT;
            if (oneShot.Value()) {
                mask |= MT_BOOST_V_ONESHOT;
            }
        }
        if (emergencyBoost.HasValue()) {
            mask |= MT_BOOST_P_EMERGENCY;
            if (emergencyBoost.Value()) {
                mask |= MT_BOOST_V_EMERGENCY;
            }
        }
        if (temporarySetpoint.HasValue()) {
            mask |= MT_BOOST_P_SETPOINT;
        }
        if (targetPercentage.HasValue()) {
            mask |= MT_BOOST_P_TARGET_PCT;
        }
        if (targetReheat.HasValue()) {
            mask |= MT_BOOST_P_REHEAT;
        }

        char fields[48];
        int n = snprintf(fields, sizeof(fields), "%lu,%u", (unsigned long)duration, mask);
        if (temporarySetpoint.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%d", (int)temporarySetpoint.Value());
        }
        if (targetPercentage.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%u", (unsigned)targetPercentage.Value());
        }
        if (targetReheat.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%u", (unsigned)targetReheat.Value());
        }
        (void)n;

        bool allow = mt_cmd_forward_fields(mEndpointId, chip::app::Clusters::WaterHeaterManagement::Id,
                                            chip::app::Clusters::WaterHeaterManagement::Commands::Boost::Id,
                                            fields);
        if (!allow) {
            return Status::Failure;
        }
        m_boost.duration   = duration;
        m_boost.one_shot   = oneShot;
        m_boost.emergency  = emergencyBoost;
        m_boost.setpoint   = temporarySetpoint;
        m_boost.target_pct = targetPercentage;
        m_boost.reheat     = targetReheat;
        m_boost.valid      = true;
        return Status::Success;
    }

    /*
     * CancelBoost: in-state guard FIRST, without waking the host, since
     * there is nothing for the host to adjudicate. The guarded answer is
     * SUCCESS-and-silence, not a failure: the cluster test plan's
     * TC_EWATERHTR_2_2 step 26 (python_testing/TC_EWATERHTR_2_2.py:199-200)
     * sends CancelBoost immediately after verifying BoostState Inactive and
     * requires "status SUCCESS(0x00) and no event sent", and the SDK's own
     * reference delegate does the same (WhmDelegateImpl::HandleCancelBoost,
     * examples/energy-management-app/.../WhmDelegateImpl.cpp:264-295, skips
     * the whole cancel body when not Active and returns Status::Success).
     * The "no event" half holds by construction here: no forward means no
     * host actuation, no BoostState push, no Active-to-Inactive transition,
     * so task 3's derivation never emits BoostEnded. The guard reads the
     * host-pushed BoostState cache, the same value a controller reads.
     *
     * The guard also CONSUMES the parameter cache (B410, ruled 2026-08-30).
     * A Boost the host accepted and a controller then cancelled while
     * BoostState was still Inactive used to leave m_boost armed, because
     * only emission consumes it; the next host-initiated Active transition
     * then emitted BoostStarted carrying the cancelled command's duration
     * and optionals. The spec's own denied-boost rationale says a boost
     * that never took effect describes nothing, so the cancel clears it and
     * a later physical-button boost emits duration 0 with no optionals,
     * which is what the no-prior-command case means. The nRF54L15 port
     * carries the identical line; the SDK's reference delegate does not
     * clear its cache here either.
     * Otherwise forward command 1 payload-less (NULL fields reproduces
     * mt_cmd_forward()'s exact four-field +MTCMD line) and pass the verdict
     * through raw, same as Boost above.
     */
    chip::Protocols::InteractionModel::Status HandleCancelBoost() override
    {
        using chip::Protocols::InteractionModel::Status;
        using chip::app::Clusters::WaterHeaterManagement::BoostStateEnum;

        if (m_boost_state == BoostStateEnum::kInactive) {
            m_boost = BoostParams{};
            return Status::Success;
        }
        bool allow = mt_cmd_forward_fields(mEndpointId, chip::app::Clusters::WaterHeaterManagement::Id,
                                            chip::app::Clusters::WaterHeaterManagement::Commands::CancelBoost::Id,
                                            NULL);
        return allow ? Status::Success : Status::Failure;
    }

    /* The six getters, serving the host-pushed cache. */
    chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap> GetHeaterTypes() override
    {
        return chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(m_heater_types);
    }
    chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap> GetHeatDemand() override
    {
        return chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(m_heat_demand);
    }
    uint16_t GetTankVolume() override { return m_tank_volume; }
    chip::Energy_mWh GetEstimatedHeatRequired() override { return m_est_heat_req; }
    chip::Percent GetTankPercentage() override { return m_tank_percent; }
    chip::app::Clusters::WaterHeaterManagement::BoostStateEnum GetBoostState() override { return m_boost_state; }
};

/*
 * The pool, MT_WHM_MAX slots (mt_matter.h documents the sizing), same
 * array-plus-next-counter shape as s_meas_epm_delegates above. The one
 * difference from the measurement handout is that the endpoint id is a
 * parameter of the alloc itself: the task brief pins this interface for
 * tasks 2/3, and stamping the id at handout keeps task 3's by-endpoint
 * lookup independent of init-CB timing. The Instance constructor later
 * calls SetEndpointId() with the identical value (see the class comment
 * above); task 2's thunk calls this after create() has returned the real
 * id.
 */
static HearthWhmDelegate s_whm_delegates[MT_WHM_MAX];
static size_t            s_whm_next = 0;

extern "C" void *mt_matter_whm_delegate_alloc(uint16_t ep)
{
    if (s_whm_next >= MT_WHM_MAX) {
        return nullptr;
    }
    HearthWhmDelegate *d = &s_whm_delegates[s_whm_next++];
    d->SetEndpointId(ep);
    return d;
}

/* The pool lookup mt_meas_whm_apply() uses, the meas_epm_for() shape. */
static HearthWhmDelegate *whm_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_whm_next; i++) {
        if (s_whm_delegates[i].endpoint() == ep) {
            return &s_whm_delegates[i];
        }
    }
    return nullptr;
}

/*
 * AT+MTMEAS's WaterHeaterManagement (0x0094) branch, energy round B: the
 * host pushes water heater state the same way it pushes electrical
 * measurements, and the firmware derives the cluster's two events from
 * BoostState transitions (design spec 3.1/3.3). Called only from
 * mt_matter_meas_set(), which has already taken the ChipStackLock and
 * resolved the endpoint and cluster lookups (so LogEvent() inside the
 * Generate*Event helpers runs lock-held, the same guarantee every other
 * bridge gives the system layer); this function owns the field table.
 *
 * Feature gate, bridge-side by design (mt_at.c only classifies signedness):
 * TankVolume and EstimatedHeatRequired exist only under EnergyManagement
 * (0x1), TankPercentage only under TankPercent (0x2), and a push to a
 * gated field on an endpoint without the feature answers MT_ATTR_ERR_CLUSTER,
 * the same data-model code an energy push gets on a power-only electrical
 * endpoint: "this endpoint does not serve that data" is one condition, not
 * two. The bits are read from the cluster's own FeatureMap attribute, the
 * SAME source WaterHeaterManagementDelegateInitCB snapshots the Instance's
 * feature answers from (esp_matter_delegate_callbacks.cpp:487-498), so the
 * gate and the served FeatureMap cannot disagree.
 *
 * Value bounds, the cluster XML (WaterHeaterManagement.xml, pinned tree):
 * HeaterTypes/HeatDemand are WaterHeaterHeatSourceBitmap (bits 0x01..0x10,
 * so 0..0x1F); BoostState is enum8 0/1; TankVolume is uint16;
 * EstimatedHeatRequired is energy_mWh with min 0 (int64 on the wire for
 * pipeline symmetry, so the negative half is cut here, answering +MTERR:1
 * through MT_ATTR_ERR_VALUE per the design spec 3.1); TankPercentage is
 * percent 0..100. Two passes, the family's hard contract: every pair is
 * validated (range AND feature gate) before any pair is applied.
 *
 * Derived events (design spec 3.3), evaluated per applied BoostState pair
 * against the cache value it is about to replace:
 *   - Inactive to Active: GenerateBoostStartedEvent() with the parameters of
 *     the last host-ACCEPTED Boost command (HandleBoost() above caches them
 *     on allow). The cache is then RESET to its duration-0/no-optionals
 *     default: the parameters describe the boost that just started, and a
 *     later host-initiated boost with no fresh controller command (a
 *     physical button) must emit duration 0 rather than restate a finished
 *     boost's numbers, the disclosure AT_MT_SPEC.md 3.25 carries.
 *   - Active to Inactive: GenerateBoostEndedEvent().
 *   - A push repeating the current state emits nothing, which is also what
 *     makes HandleCancelBoost()'s in-state guard "no event sent" claim hold
 *     by construction.
 * A Generate*Event failure (event buffer exhaustion) is logged by the helper
 * itself and deliberately does NOT fail the push: the pushed state is
 * applied and served either way, so answering an error would tell the host
 * "nothing changed" about a change that took, the EEM branch's own
 * error-after-apply caveat without the misleading answer.
 * Every applied field, BoostState included and same-state pushes included,
 * is still reported dirty (one MatterReportingAttributeChangeCallback per
 * applied pair) so subscriptions fire per sample, the EPM rule.
 */
static int mt_meas_whm_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count)
{
    using namespace chip::app::Clusters::WaterHeaterManagement;

    HearthWhmDelegate *d = whm_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run, kept as the same defensive
         * answer the EPM branch gives. */
        return MT_ATTR_ERR_FAILED;
    }

    /* The endpoint's WHM feature bits, from the cluster's own FeatureMap
     * attribute (see the block comment). A read failure is defensive: the
     * global attribute exists on every created cluster. */
    uint32_t feature_map = 0;
    {
        esp_matter::attribute_t *a =
            esp_matter::attribute::get(ep, Id, Attributes::FeatureMap::Id);
        esp_matter_attr_val_t val;
        if (a == nullptr || esp_matter::attribute::get_val(a, &val) != ESP_OK) {
            return MT_ATTR_ERR_FAILED;
        }
        feature_map = val.val.u32;
    }

    /* Pass 1: validate everything, range and feature gate both. */
    for (uint8_t i = 0; i < count; i++) {
        switch (fields[i]) {
        case MT_WHM_F_HEATER_TYPES:
        case MT_WHM_F_HEAT_DEMAND:
            if (values[i] < 0 || values[i] > 0x1F) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_BOOST_STATE:
            if (values[i] < 0 || values[i] > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_TANK_VOLUME:
            if ((feature_map & chip::to_underlying(Feature::kEnergyManagement)) == 0) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > 0xFFFF) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_EST_HEAT_REQ:
            if ((feature_map & chip::to_underlying(Feature::kEnergyManagement)) == 0) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > kMeasValueAbsMax) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_TANK_PERCENT:
            if ((feature_map & chip::to_underlying(Feature::kTankPercent)) == 0) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > 100) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Pass 2: apply, one subscription report per applied field, deriving
     * the boost events on BoostState transitions as they are applied (a
     * push carrying 2,1,2,0 legitimately emits Started then Ended, the
     * same sequential last-writer semantics the EPM branch has). */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t attr_id;
        switch (fields[i]) {
        case MT_WHM_F_HEATER_TYPES:
            d->m_heater_types = (uint8_t)values[i];
            attr_id = Attributes::HeaterTypes::Id;
            break;
        case MT_WHM_F_HEAT_DEMAND:
            d->m_heat_demand = (uint8_t)values[i];
            attr_id = Attributes::HeatDemand::Id;
            break;
        case MT_WHM_F_BOOST_STATE: {
            BoostStateEnum next = (values[i] != 0) ? BoostStateEnum::kActive
                                                   : BoostStateEnum::kInactive;
            BoostStateEnum prev = d->m_boost_state;
            d->m_boost_state    = next;
            if (prev != next) {
                if (next == BoostStateEnum::kActive) {
                    (void)d->GenerateBoostStartedEvent(d->m_boost.duration, d->m_boost.one_shot,
                                                       d->m_boost.emergency, d->m_boost.setpoint,
                                                       d->m_boost.target_pct, d->m_boost.reheat);
                    d->m_boost = HearthWhmDelegate::BoostParams{};
                } else {
                    (void)d->GenerateBoostEndedEvent();
                }
            }
            attr_id = Attributes::BoostState::Id;
            break;
        }
        case MT_WHM_F_TANK_VOLUME:
            d->m_tank_volume = (uint16_t)values[i];
            attr_id = Attributes::TankVolume::Id;
            break;
        case MT_WHM_F_EST_HEAT_REQ:
            d->m_est_heat_req = values[i];
            attr_id = Attributes::EstimatedHeatRequired::Id;
            break;
        default: /* MT_WHM_F_TANK_PERCENT, pass 1 admits nothing else */
            d->m_tank_percent = (uint8_t)values[i];
            attr_id = Attributes::TankPercentage::Id;
            break;
        }
        MatterReportingAttributeChangeCallback(ep, Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * ---- device energy management (energy round C1, task 1) --------------------
 *
 * DeviceEnergyManagement (0x0098) is FULLY delegate-served, the WHM shape:
 * the Instance is the cluster's AttributeAccessInterface AND its
 * CommandHandlerInterface (device-energy-management-server.h:207-252), so
 * all five attributes answer from the Delegate's getters and every command
 * reaches the Delegate only after the server's own pre-validation. The
 * delegate contract, verbatim from device-energy-management-server.h
 * (lines 55-201, SEVENTEEN pure virtuals; the Round B record said 16 and
 * was corrected by this round's survey):
 *
 *   virtual Status PowerAdjustRequest(const int64_t power, const uint32_t duration, AdjustmentCauseEnum cause) = 0;
 *   virtual Status CancelPowerAdjustRequest() = 0;
 *   virtual Status StartTimeAdjustRequest(const uint32_t requestedStartTime, AdjustmentCauseEnum cause) = 0;
 *   virtual Status PauseRequest(const uint32_t duration, AdjustmentCauseEnum cause) = 0;
 *   virtual Status ResumeRequest() = 0;
 *   virtual Status ModifyForecastRequest(const uint32_t forecastID,
 *                                        const DataModel::DecodableList<Structs::SlotAdjustmentStruct::Type> & slotAdjustments,
 *                                        AdjustmentCauseEnum cause) = 0;
 *   virtual Status RequestConstraintBasedForecast(const DataModel::DecodableList<Structs::ConstraintsStruct::Type> & constraints,
 *                                                 AdjustmentCauseEnum cause) = 0;
 *   virtual Status CancelRequest() = 0;
 *   virtual ESATypeEnum GetESAType() = 0;
 *   virtual bool GetESACanGenerate() = 0;
 *   virtual ESAStateEnum GetESAState() = 0;
 *   virtual int64_t GetAbsMinPower() = 0;
 *   virtual int64_t GetAbsMaxPower() = 0;
 *   virtual OptOutStateEnum GetOptOutState() = 0;
 *   virtual const DataModel::Nullable<Structs::PowerAdjustCapabilityStruct::Type> & GetPowerAdjustmentCapability() = 0;
 *   virtual const DataModel::Nullable<Structs::ForecastStruct::Type> & GetForecast() = 0;
 *   virtual CHIP_ERROR SetESAState(ESAStateEnum) = 0;
 *
 * THE IRON RULE (design spec 2.5, stated at every DEM site): DEM feature
 * bits are set ONLY via cluster::device_energy_management::feature::X::add(),
 * never by writing FeatureMap. Instance::RetrieveAcceptedCommands derives
 * the advertised command list from the FeatureMap snapshot, while
 * esp-matter's dispatcher looks the invoked command up as an ember
 * command_t (esp_matter_command.cpp:42-44: get(cluster, command_id,
 * COMMAND_FLAG_ACCEPTED), then VerifyOrReturn, so a missing entry means the
 * invoke returns NO status at all, not an error); only feature::add()
 * creates those entries, so a hand-set bit diverges the two surfaces. This
 * is the exact inverse of Round B's WHM hand-set workaround, and this
 * delegate touches no feature bit at all: that is the thunk's job (task 3),
 * through feature::power_adjustment::add().
 *
 * Server pre-validation, the reason several guards WHM needed do not exist
 * here (device-energy-management-server.cpp:254-388): the OptOut x cause
 * matrix (CheckOptOutAllowsRequest, lines 254-300) and the range walk over
 * every PowerAdjustmentCapability entry (lines 326-359) both run BEFORE
 * PowerAdjustRequest() is called, and a null capability answers
 * ConstraintError without waking the host (lines 326-340). So an
 * uncapability'd endpoint (AT+MTDEMCAP not yet sent, task 2) never forwards
 * anything.
 *
 * Struct-store lifetime contract (header lines 173-196): the two struct
 * getters hand out references valid only until the next Matter event is
 * processed, so both stores are OWNED members with fixed backing storage
 * (PowerAdjustStruct backing[MT_DEM_CAP_MAX_ENTRIES]); nothing is
 * re-allocated per read. GetForecast() returns a permanently null Nullable
 * in C1: PFR/SFR are out of scope (SlotStruct's 13 Optionals plus a nested
 * cost list are an order of magnitude past Boost's mask, survey I.8), and
 * without those feature bits the server never reads it anyway (Read()
 * feature-gates Forecast behind PFR|SFR).
 *
 * Event emission is chip::app::LogEvent DIRECTLY, the NEW pattern for this
 * project: Round A emitted through NotifyCumulativeEnergyMeasured, Round B
 * through WHM's Generate*Event helpers; the DEM cluster ships no helper and
 * the server emits nothing itself (all four DEM events are app-emitted,
 * survey I.2, the reference delegate's GeneratePowerAdjustEndEvent shape,
 * DeviceEnergyManagementDelegateImpl.cpp:291-326). Two lock disciplines,
 * one per path, commented at each emission site below.
 *
 * Duration measurement: the accept timestamp is
 * chip::System::SystemClock().GetMonotonicMilliseconds64(), the same source
 * the EEM branch above uses for its systime fields; monotonic rather than
 * the reference impl's GetClock_MatterEpochS because wall time may never
 * sync on this device and a duration must not fail with it. The event's
 * duration field is computed at emission.
 *
 * Endpoint id and init callback: mt_matter_dem_delegate_alloc() takes the
 * endpoint id up front (the WHM alloc(ep) reasoning, see mt_matter.h); the
 * Instance constructor later calls mDelegate.SetEndpointId() with the
 * identical value (device-energy-management-server.h:210-216) when
 * DeviceEnergyManagementDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp:368-376) news the Instance with a
 * feature bitmask SNAPSHOTTED from the endpoint's FeatureMap attribute at
 * delegate-init time, which is why the thunk's feature::add() calls must
 * precede the init callback. The CB keeps a static single-Instance pointer
 * overwritten per endpoint with no Shutdown of the previous one; benign,
 * delegate lifetimes are boot-long and endpoints are built once per boot.
 *
 * Command forwarding: PowerAdjustRequest/CancelPowerAdjustRequest run on
 * the CHIP event loop task (the Instance's CommandHandlerInterface calls
 * them synchronously from the invoke path), so NO ChipStackLock is taken
 * in them, main.cpp's own rule (delegate methods invoked BY CHIP already
 * hold the stack), the HearthWhmDelegate::HandleBoost shape. The verdict is
 * a raw passthrough: HandlePowerAdjustRequest/HandleCancelPowerAdjustRequest
 * (server .cpp:363-364/390-391) copy whatever Status the delegate returns
 * straight into the command response via AddStatus(), so a deny genuinely
 * fails the command on the wire.
 */
class HearthDemDelegate : public chip::app::Clusters::DeviceEnergyManagement::Delegate
{
public:
    /* Host-pushed cached state, written only by task 2's
     * mt_matter_meas_set() 0x98 branch. The defaults are the pre-first-push
     * answers the design spec 2.6 pins: ESAType 0 (kEvse, the enum's zero),
     * canGenerate false, ESAState Online, powers 0, OptOutState NoOptOut. */
    uint8_t m_esa_type     = 0;
    bool    m_can_generate = false;
    chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum m_esa_state =
        chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum::kOnline;
    int64_t m_abs_min_power = 0;
    int64_t m_abs_max_power = 0;
    chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum m_opt_out =
        chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum::kNoOptOut;

    /* The session's approximate energy use (mWh), the PowerAdjustEnd event's
     * energyUse field: MT_DEM_F_ADJ_ENERGY_USE, an event carrier and not an
     * attribute (mt_matter.h), the reference delegate's
     * GetApproxEnergyDuringSession() shape. Host-pushed while an adjustment
     * runs, consumed and reset by every PowerAdjustEnd emission. */
    int64_t m_energy_use = 0;

    /* The owned PowerAdjustmentCapability store, task 2's AT+MTDEMCAP
     * surface: fixed backing array (the struct-store lifetime contract in
     * the class comment above), entry count, and the Nullable the getter
     * hands out, null until the host installs entries.
     *
     * Cause contract (task review F2, for task 2's demcap bridge): the
     * struct's LIVE cause (m_pa_capability.Value().cause) is owned by the
     * firmware while an adjustment runs (stamped on accept, see
     * PowerAdjustRequest below); m_pa_cause_baseline is the host-pushed
     * resting value every PowerAdjustEnd restores. Task 2's bridge must
     * write the baseline member (and the live cause together with it when
     * no adjustment is active), never the live cause alone, or the restore
     * on PowerAdjustEnd will resurrect a stale value. Defaults to
     * kNoAdjustment (0x00, PowerAdjustReasonEnum read at source), the
     * reference impl's resting value. */
    chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustStruct::Type
        m_pa_entries[MT_DEM_CAP_MAX_ENTRIES];
    uint8_t m_pa_count = 0;
    chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustCapabilityStruct::Type>
        m_pa_capability;
    chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum m_pa_cause_baseline =
        chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum::kNoAdjustment;

    chip::EndpointId endpoint() const { return mEndpointId; }

    /* Task 2's field-6 cache setter (consumed-by-task-2, mt_matter.h). */
    void SetAdjEnergyUse(int64_t mwh) { m_energy_use = mwh; }

    /*
     * PowerAdjustRequest: forward the three flat scalars adjudicated over
     * +MTCMD (design spec 3.3, "<power>,<duration>,<cause>"); the server
     * has already validated the request against every capability entry and
     * the OptOut matrix (class comment above), so the forward needs no
     * validation of its own. On allow: ESAState kPowerAdjustActive (the
     * firmware owns this transition, the contrast to Round B's Boost where
     * the host pushed BoostState: PowerAdjustStart is fieldless and the
     * state change is unconditional on accept), start the duration clock,
     * emit PowerAdjustStart, Success.
     *
     * Re-adjust rule (task review F1): a second request accepted while
     * ESAState is ALREADY kPowerAdjustActive emits NO second
     * PowerAdjustStart and does NOT re-arm the duration clock. That is
     * certification behaviour, not taste: TC_DEM_2_2 step 14
     * (src/python_testing/TC_DEM_2_2.py:122-123, code 291-297) requires
     * SUCCESS "and no event sent" (wait_for_event_expect_no_report) for
     * exactly this re-adjust, and the eventual PowerAdjustEnd's duration
     * is asserted against the FIRST accept (start recorded at step 13,
     * line 279; elapsed check at 328-329). The reference delegate
     * implements the same via its in-progress guard, which also preserves
     * the original start timestamp
     * (DeviceEnergyManagementDelegateImpl.cpp:93-142). The forward still
     * happens and the verdict still maps to the response either way: the
     * re-adjust must reach the sketch, only the event and the clock are
     * suppressed.
     *
     * Capability cause tracking (task review F2): an ACCEPTED request also
     * stamps the owned PowerAdjustmentCapability struct's cause with the
     * adjustment-reason mapping of the command's cause
     * (kLocalOptimization -> kLocalOptimizationAdjustment,
     * kGridOptimization -> kGridOptimizationAdjustment, read from
     * PowerAdjustReasonEnum at source) and reports the attribute dirty,
     * asserted by TC_DEM_2_2 steps 13a/14b (lines 288-289/302-305) and
     * mirrored from the reference impl
     * (DeviceEnergyManagementDelegateImpl.cpp:125-138, setter at 929-939).
     * Every PowerAdjustEnd restores the host-pushed baseline cause; see
     * EmitPowerAdjustEnd() below.
     */
    chip::Protocols::InteractionModel::Status PowerAdjustRequest(
        const int64_t power, const uint32_t duration,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        using chip::Protocols::InteractionModel::Status;

        char fields[48];
        snprintf(fields, sizeof(fields), "%lld,%lu,%u", (long long)power, (unsigned long)duration,
                 (unsigned)chip::to_underlying(cause));

        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::PowerAdjustRequest::Id, fields);
        if (!allow) {
            return Status::Failure;
        }

        /* Whether this accept STARTS an adjustment or RE-ADJUSTS a running
         * one, decided before the state write (the F1 rule in the method
         * comment above). */
        bool in_progress = (m_esa_state == ESAStateEnum::kPowerAdjustActive);

        /* F2: stamp the capability struct's cause with the accepted
         * request's adjustment reason (method comment above). The server's
         * CheckOptOutAllowsRequest has already rejected kUnknownEnumValue
         * (device-energy-management-server.cpp:258-262), so the default
         * arm is defensive only. */
        switch (cause) {
        case AdjustmentCauseEnum::kLocalOptimization:
            SetCapabilityCause(PowerAdjustReasonEnum::kLocalOptimizationAdjustment);
            break;
        case AdjustmentCauseEnum::kGridOptimization:
            SetCapabilityCause(PowerAdjustReasonEnum::kGridOptimizationAdjustment);
            break;
        default:
            break;
        }

        SetStateRaw(ESAStateEnum::kPowerAdjustActive);
        if (!in_progress) {
            m_pa_start_ms = static_cast<uint64_t>(
                chip::System::SystemClock().GetMonotonicMilliseconds64().count());

            /* Emission discipline: COMMAND PATH. Runs on the CHIP event
             * loop task from the Instance's invoke path, so NO
             * ChipStackLock is taken (main.cpp's own rule, the
             * HearthWhmDelegate::HandleBoost precedent); the LogEvent
             * shape is what Round B's Generate*Event calls wrapped one
             * helper down. A LogEvent failure (event buffer exhaustion) is
             * logged and deliberately does NOT fail the command: the host
             * has already accepted the adjustment and the state is applied
             * and served, the WHM error-after-apply caveat. */
            Events::PowerAdjustStart::Type event;
            chip::EventNumber n;
            CHIP_ERROR err = chip::app::LogEvent(event, mEndpointId, n);
            if (err != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "DEM ep %u: PowerAdjustStart event failed: %" CHIP_ERROR_FORMAT,
                         mEndpointId, err.Format());
            }
        }
        /* else: re-adjust while active, no second Start and the clock
         * keeps measuring from the first accept (TC_DEM_2_2 step 14, the
         * F1 rule above). */
        return Status::Success;
    }

    /*
     * CancelPowerAdjustRequest: forward payload-less (NULL fields
     * reproduces mt_cmd_forward()'s exact four-field +MTCMD line). NO
     * firmware in-state guard, the deliberate contrast to Round B's
     * HandleCancelBoost: the CHIP server refuses the command with
     * InvalidInState unless ESAState is kPowerAdjustActive BEFORE this
     * delegate runs (device-energy-management-server.cpp:381-388), so
     * unlike WHM there is nothing left to guard, and the harness pins that
     * no +MTCMD is raised in the wrong state. On allow: emit
     * PowerAdjustEnd(Cancelled, measured duration, cached energyUse), then
     * reset to Online through the raw setter, NOT SetESAState(): the
     * transition derivation there would emit a second PowerAdjustEnd with
     * the wrong cause (NormalCompletion) for a session this method has
     * already ended as Cancelled.
     */
    chip::Protocols::InteractionModel::Status CancelPowerAdjustRequest() override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        using chip::Protocols::InteractionModel::Status;

        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::CancelPowerAdjustRequest::Id,
                                            NULL);
        if (!allow) {
            return Status::Failure;
        }

        /* Emission discipline: COMMAND PATH, same reasoning and precedent
         * as the PowerAdjustStart emission above: CHIP task, no
         * ChipStackLock. */
        EmitPowerAdjustEnd(CauseEnum::kCancelled);
        SetStateRaw(ESAStateEnum::kOnline);
        return Status::Success;
    }

    /*
     * The six non-PowerAdjustment command handlers. Unreachable while this
     * firmware is PA-only: the server's InvokeCommand dispatch answers
     * UnsupportedCommand for every one of them unless the matching feature
     * bit (STA, PAU, FA, CON) is in the Instance's FeatureMap snapshot
     * (device-energy-management-server.cpp:181-251), and task 3's thunk
     * only ever calls feature::power_adjustment::add(). Failure rather
     * than Success so that if a future round adds a feature bit without
     * implementing its handler, the miss is a visible command failure on
     * the wire, not a silent lie.
     */
    chip::Protocols::InteractionModel::Status StartTimeAdjustRequest(
        const uint32_t requestedStartTime,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)requestedStartTime;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status PauseRequest(
        const uint32_t duration,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)duration;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status ResumeRequest() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status ModifyForecastRequest(
        const uint32_t forecastID,
        const chip::app::DataModel::DecodableList<
            chip::app::Clusters::DeviceEnergyManagement::Structs::SlotAdjustmentStruct::Type> &slotAdjustments,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)forecastID;
        (void)slotAdjustments;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status RequestConstraintBasedForecast(
        const chip::app::DataModel::DecodableList<
            chip::app::Clusters::DeviceEnergyManagement::Structs::ConstraintsStruct::Type> &constraints,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)constraints;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status CancelRequest() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }

    /* The eight getters, serving the host-pushed cache. */
    chip::app::Clusters::DeviceEnergyManagement::ESATypeEnum GetESAType() override
    {
        return static_cast<chip::app::Clusters::DeviceEnergyManagement::ESATypeEnum>(m_esa_type);
    }
    bool GetESACanGenerate() override { return m_can_generate; }
    chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum GetESAState() override
    {
        return m_esa_state;
    }
    int64_t GetAbsMinPower() override { return m_abs_min_power; }
    int64_t GetAbsMaxPower() override { return m_abs_max_power; }
    chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum GetOptOutState() override
    {
        return m_opt_out;
    }
    const chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustCapabilityStruct::Type> &
    GetPowerAdjustmentCapability() override
    {
        return m_pa_capability;
    }
    /* Permanently null in C1: PFR/SFR out of scope (class comment above). */
    const chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::ForecastStruct::Type> &
    GetForecast() override
    {
        return m_forecast;
    }

    /*
     * SetESAState: the push-path transition entry (consumed-by-task-2,
     * mt_matter.h): task 2's mt_matter_meas_set() 0x98 branch calls this
     * for every MT_DEM_F_ESA_STATE pair it applies, and the derivation
     * lives here (design spec 3.3): a transition LEAVING kPowerAdjustActive
     * emits PowerAdjustEnd(NormalCompletion, measured duration, cached
     * energyUse); a same-state push emits nothing and reports nothing (the
     * Round B BoostState rule). The reference impl's shape otherwise
     * (DeviceEnergyManagementDelegateImpl.cpp:869-886): reject
     * kUnknownEnumValue and up, report the attribute dirty on change, so
     * callers do not report ESAState again themselves.
     *
     * The one SDK caller of this virtual is the server's Pause path
     * (device-energy-management-server.cpp:586, SetESAState(kPaused)),
     * unreachable while PA-only (see the non-PA handlers above), so in
     * practice every call site is a bridge holding the ChipStackLock.
     */
    CHIP_ERROR SetESAState(chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum next) override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        if (next >= ESAStateEnum::kUnknownEnumValue) {
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        if (next == m_esa_state) {
            return CHIP_NO_ERROR;
        }
        ESAStateEnum prev = m_esa_state;
        if (prev == ESAStateEnum::kPowerAdjustActive) {
            /* Emission discipline: PUSH PATH. Reached from
             * mt_matter_meas_set(), which has already taken the
             * ChipStackLock, so LogEvent runs lock-held: the Round A
             * precedent (NotifyCumulativeEnergyMeasured from the EEM
             * branch) and Round B's Generate*Event calls from
             * mt_meas_whm_apply(), both the same bridge-held-lock
             * guarantee. */
            EmitPowerAdjustEnd(CauseEnum::kNormalCompletion);
        }
        if (next == ESAStateEnum::kPowerAdjustActive) {
            /* Entering via a push is off-design (3.3: the firmware owns the
             * entry transition on an accepted PowerAdjustRequest, which
             * arms this clock itself), but nothing stops a host doing it;
             * arm the clock here too so a later PowerAdjustEnd measures
             * from this moment instead of from boot. */
            m_pa_start_ms = static_cast<uint64_t>(
                chip::System::SystemClock().GetMonotonicMilliseconds64().count());
        }
        SetStateRaw(next);
        return CHIP_NO_ERROR;
    }

private:
    /* Forecast: permanently null, never written (class comment above). */
    chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::ForecastStruct::Type>
        m_forecast;

    /* Monotonic ms at the last accepted PowerAdjustRequest, the duration
     * clock (class comment above). */
    uint64_t m_pa_start_ms = 0;

    /* F2's live-cause write: stamp the owned capability struct's cause and
     * report PowerAdjustmentCapability dirty, the reference impl's
     * SetPowerAdjustmentCapabilityPowerAdjustReason shape
     * (DeviceEnergyManagementDelegateImpl.cpp:929-939). A null capability
     * (AT+MTDEMCAP never sent) has no cause to stamp; that state cannot
     * carry a running adjustment anyway, since the server refuses
     * PowerAdjustRequest on a null capability (class comment above), so
     * the restore path can only see it after a host emptied the store
     * mid-adjustment, and skipping is the right answer there too. */
    void SetCapabilityCause(chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum r)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        if (m_pa_capability.IsNull() || m_pa_capability.Value().cause == r) {
            return;
        }
        m_pa_capability.Value().cause = r;
        MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                               Attributes::PowerAdjustmentCapability::Id);
    }

    /* Cache write + dirty report, no event derivation: the command paths
     * above use this directly because they emit their own event (or none)
     * before transitioning; SetESAState() is the derived-emission entry.
     * Reports on change only, the reference impl's shape (class comment on
     * SetESAState above). */
    void SetStateRaw(chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum next)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        if (m_esa_state != next) {
            m_esa_state = next;
            MatterReportingAttributeChangeCallback(mEndpointId, Id, Attributes::ESAState::Id);
        }
    }

    /* Fill and log PowerAdjustEnd: cause per caller, duration measured
     * against the accept timestamp, energyUse from the host-pushed field-6
     * cache, which is consumed (reset to 0) by the emission, both-ends
     * rule of design spec 3.3. Also restores the capability struct's cause
     * to the host-pushed baseline (task review F2, the reference impl's
     * kNoAdjustment restore on both end paths,
     * DeviceEnergyManagementDelegateImpl.cpp:218/272). Lock discipline is
     * the CALLER's and is commented at each call site; a LogEvent failure
     * is logged and does not propagate (the WHM error-after-apply caveat:
     * the transition the event describes has already been decided). */
    void EmitPowerAdjustEnd(chip::app::Clusters::DeviceEnergyManagement::CauseEnum cause)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        uint64_t now_ms = static_cast<uint64_t>(
            chip::System::SystemClock().GetMonotonicMilliseconds64().count());
        Events::PowerAdjustEnd::Type event;
        event.cause     = cause;
        event.duration  = static_cast<uint32_t>((now_ms - m_pa_start_ms) / 1000);
        event.energyUse = m_energy_use;

        chip::EventNumber n;
        CHIP_ERROR err = chip::app::LogEvent(event, mEndpointId, n);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "DEM ep %u: PowerAdjustEnd event failed: %" CHIP_ERROR_FORMAT,
                     mEndpointId, err.Format());
        }
        m_energy_use = 0;
        SetCapabilityCause(m_pa_cause_baseline);
    }
};

/*
 * The pool, MT_DEM_MAX slots (mt_matter.h documents the sizing), the
 * HearthWhmDelegate pool's exact shape: array plus next-counter, endpoint
 * id stamped at handout (the alloc(ep) reasoning in mt_matter.h; the
 * Instance constructor later calls SetEndpointId() with the identical
 * value, see the class comment above). The SDK init CB's static
 * single-Instance pointer (esp_matter_delegate_callbacks.cpp:368-376) is
 * overwritten per endpoint with no Shutdown handle for earlier Instances:
 * benign, delegate lifetimes are boot-long. Task 3's thunk consumes the
 * alloc; task 2's push bridge finds the slot again with dem_for() below.
 */
static HearthDemDelegate s_dem_delegates[MT_DEM_MAX];
static size_t            s_dem_next = 0;

extern "C" void *mt_matter_dem_delegate_alloc(uint16_t ep)
{
    if (s_dem_next >= MT_DEM_MAX) {
        return nullptr;
    }
    HearthDemDelegate *d = &s_dem_delegates[s_dem_next++];
    d->SetEndpointId(ep);
    return d;
}

/* The pool lookup the 0x98 branch and the AT+MTDEMCAP bridge use, the
 * whm_for() shape. */
static HearthDemDelegate *dem_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_dem_next; i++) {
        if (s_dem_delegates[i].endpoint() == ep) {
            return &s_dem_delegates[i];
        }
    }
    return nullptr;
}

/*
 * AT+MTMEAS's DeviceEnergyManagement (0x0098) branch, energy round C1 task 2:
 * the host pushes ESA state the same way it pushes electrical and water
 * heater state (design spec 3.1). Called only from mt_matter_meas_set(),
 * which has already taken the ChipStackLock and resolved the endpoint and
 * cluster lookups, so the SetESAState() derivation below runs under the
 * bridge-held lock its own emission-discipline comment requires; this
 * function owns the field table.
 *
 * Value bounds, the cluster XML (DeviceEnergyManagement.xml, pinned tree)
 * and the generated enums (DeviceEnergyManagement/Enums.h): ESAType is
 * ESATypeEnum (contiguous 0x00..0x0D plus kOther 0xFF; kUnknownEnumValue 14
 * is the generated helper, not a wire value); ESACanGenerate is bool 0/1;
 * ESAState is ESAStateEnum 0x00..0x04; OptOutState is OptOutStateEnum
 * 0x00..0x03; AbsMinPower/AbsMaxPower are power-mW (int64, no XML range
 * constraint beyond the type; the XML's AbsMaxPower>=AbsMinPower relation
 * is a cross-field data-model property the controller reads, deliberately
 * not enforced across independent single-field pushes). Field 6 is int64
 * mWh, full width. Two passes, the family's hard contract: every pair is
 * validated before any pair is applied.
 *
 * Apply-side departures from the WHM branch, both pinned by the design spec
 * and Task 1's delegate contract (the SetESAState/SetAdjEnergyUse comments
 * above):
 *   - MT_DEM_F_ESA_STATE routes through SetESAState(), which owns the
 *     whole transition protocol: a push LEAVING kPowerAdjustActive emits
 *     PowerAdjustEnd(NormalCompletion, measured duration, cached field-6
 *     energyUse, cache reset by the emission), a same-state push emits
 *     nothing AND reports nothing (the reference impl reports on change
 *     only, unlike WHM's report-per-sample BoostState), and a push INTO
 *     kPowerAdjustActive is legal but emits no PowerAdjustStart: only a
 *     command-path accept emits Start (SetESAState's comment explains; it
 *     still arms the duration clock so a later End measures honestly). So
 *     no MatterReportingAttributeChangeCallback is issued here for field 2.
 *   - MT_DEM_F_ADJ_ENERGY_USE caches only and never marks anything dirty:
 *     it is an EVENT CARRIER, not an attribute (design spec 3.1's field-6
 *     row; AT_MT_SPEC.md 3.25), the reference delegate's
 *     GetApproxEnergyDuringSession() shape, consumed by the next
 *     PowerAdjustEnd emission.
 * Fields 0/1/3/4/5 are the plain cache-update-plus-dirty-report shape the
 * EPM and WHM branches established.
 */
static int mt_meas_dem_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;

    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run, kept as the same defensive
         * answer the EPM and WHM branches give. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Pass 1: validate everything. */
    for (uint8_t i = 0; i < count; i++) {
        switch (fields[i]) {
        case MT_DEM_F_ESA_TYPE:
            if (!((values[i] >= 0 && values[i] <= 0x0D) || values[i] == 0xFF)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ESA_CAN_GEN:
            if (values[i] < 0 || values[i] > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ESA_STATE:
            if (values[i] < 0 ||
                values[i] >= chip::to_underlying(ESAStateEnum::kUnknownEnumValue)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ABS_MIN_POWER:
        case MT_DEM_F_ABS_MAX_POWER:
        case MT_DEM_F_ADJ_ENERGY_USE:
            /* int64 full width, no XML bound beyond the type. */
            break;
        case MT_DEM_F_OPT_OUT_STATE:
            if (values[i] < 0 ||
                values[i] >= chip::to_underlying(OptOutStateEnum::kUnknownEnumValue)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Pass 2: apply, in order (sequential last-writer semantics, the family
     * rule). ESAState transitions derive events as they are applied. */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t attr_id;
        switch (fields[i]) {
        case MT_DEM_F_ESA_TYPE:
            d->m_esa_type = (uint8_t)values[i];
            attr_id = Attributes::ESAType::Id;
            break;
        case MT_DEM_F_ESA_CAN_GEN:
            d->m_can_generate = (values[i] != 0);
            attr_id = Attributes::ESACanGenerate::Id;
            break;
        case MT_DEM_F_ESA_STATE:
            /* SetESAState owns derivation AND the on-change dirty report
             * (block comment above); pass 1 already cut the enum range, so
             * its ConstraintError arm is unreachable here. */
            (void)d->SetESAState(static_cast<ESAStateEnum>(values[i]));
            continue;
        case MT_DEM_F_ABS_MIN_POWER:
            d->m_abs_min_power = values[i];
            attr_id = Attributes::AbsMinPower::Id;
            break;
        case MT_DEM_F_ABS_MAX_POWER:
            d->m_abs_max_power = values[i];
            attr_id = Attributes::AbsMaxPower::Id;
            break;
        case MT_DEM_F_OPT_OUT_STATE:
            d->m_opt_out = static_cast<OptOutStateEnum>(values[i]);
            attr_id = Attributes::OptOutState::Id;
            break;
        default: /* MT_DEM_F_ADJ_ENERGY_USE, pass 1 admits nothing else */
            /* Event carrier: cache only, never dirty (block comment above,
             * design spec 3.1). */
            d->SetAdjEnergyUse(values[i]);
            continue;
        }
        MatterReportingAttributeChangeCallback(ep, Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * The AT+MTDEMCAP bridge (energy round C1 task 2, design spec 3.2). Grammar
 * (arity against <n>, integer parses, the u32 duration bound) is
 * cmd_mtdemcap()'s business; this bridge owns everything semantic, in the
 * established lookup-then-value order:
 *
 *   - endpoint and cluster lookups (the mt_matter_meas_set() shape);
 *   - the PowerAdjustment feature gate, read from the cluster's own
 *     FeatureMap attribute (the mt_meas_whm_apply() precedent, the same
 *     source the DEM delegate-init CB snapshots): the XML marks
 *     PowerAdjustmentCapability mandatoryConform on feature PA only, so a
 *     variant-1 (FeatureMap 0) endpoint has no such attribute to set and
 *     answers MT_ATTR_ERR_ATTRIBUTE, the design spec's staged variant-1 row
 *     (section 5: "DEM v1 MTDEMCAP answers the attribute-missing code");
 *   - the cause enum range against PowerAdjustReasonEnum;
 *   - per-entry minPower<=maxPower and minDuration<=maxDuration ordering:
 *     the CHIP server's PowerAdjustRequest range walk
 *     (device-energy-management-server.cpp:326-359) takes each entry's
 *     min/max as an interval, so a mis-ordered entry would make the server
 *     validate against an empty interval; rejected as MT_ATTR_ERR_VALUE
 *     before anything is applied.
 *
 * The apply replaces the WHOLE owned capability (full-replacement, the
 * host-fed-list convention): the fixed backing array is rewritten, the
 * Nullable rebuilt (null when n is 0), and PowerAdjustmentCapability
 * reported dirty. Baseline cause contract (task review F2, the store
 * comment in HearthDemDelegate above and mt_matter.h): the baseline member
 * ALWAYS takes the pushed cause; the live struct cause takes it only when
 * no adjustment is running. While one runs, the live cause is
 * firmware-owned (stamped by the accept) and is carried into the rebuilt
 * struct unchanged, so the eventual PowerAdjustEnd restores the NEW
 * baseline: replacing the capability mid-adjustment must not overwrite the
 * stamped cause TC_DEM_2_2 steps 13a/14b assert. (A host that emptied the
 * store mid-adjustment and re-installs has no stamped value left to carry;
 * the baseline is the only honest choice, and the End's restore is then a
 * no-op.)
 */
extern "C" int mt_matter_demcap_set(uint16_t ep, uint8_t cause, uint8_t n, const int64_t *quads)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;
    ChipStackLock lock;

    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: the same
         * defensive answer as the push branches. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Feature gate (block comment above). A read failure is defensive: the
     * global attribute exists on every created cluster. */
    {
        esp_matter::attribute_t *a =
            esp_matter::attribute::get(ep, Id, Attributes::FeatureMap::Id);
        esp_matter_attr_val_t val;
        if (a == nullptr || esp_matter::attribute::get_val(a, &val) != ESP_OK) {
            return MT_ATTR_ERR_FAILED;
        }
        if ((val.val.u32 & chip::to_underlying(Feature::kPowerAdjustment)) == 0) {
            return MT_ATTR_ERR_ATTRIBUTE;
        }
    }

    if (n > MT_DEM_CAP_MAX_ENTRIES) {
        /* Defensive; cmd_mtdemcap's <n> range check answers first. */
        return MT_ATTR_ERR_VALUE;
    }
    if (cause >= chip::to_underlying(PowerAdjustReasonEnum::kUnknownEnumValue)) {
        return MT_ATTR_ERR_VALUE;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (quads[4 * i] > quads[4 * i + 1] ||           /* minPower > maxPower */
            quads[4 * i + 2] > quads[4 * i + 3]) {       /* minDuration > maxDuration */
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Apply. The live cause carried into the rebuilt struct: the pushed
     * baseline normally, the firmware-stamped value while an adjustment is
     * running (F2, block comment above). */
    PowerAdjustReasonEnum baseline = static_cast<PowerAdjustReasonEnum>(cause);
    PowerAdjustReasonEnum live     = baseline;
    if (d->m_esa_state == ESAStateEnum::kPowerAdjustActive && !d->m_pa_capability.IsNull()) {
        live = d->m_pa_capability.Value().cause;
    }
    d->m_pa_cause_baseline = baseline;

    if (n == 0) {
        d->m_pa_count = 0;
        d->m_pa_capability.SetNull();
    } else {
        for (uint8_t i = 0; i < n; i++) {
            d->m_pa_entries[i].minPower    = quads[4 * i];
            d->m_pa_entries[i].maxPower    = quads[4 * i + 1];
            d->m_pa_entries[i].minDuration = static_cast<uint32_t>(quads[4 * i + 2]);
            d->m_pa_entries[i].maxDuration = static_cast<uint32_t>(quads[4 * i + 3]);
        }
        d->m_pa_count = n;
        Structs::PowerAdjustCapabilityStruct::Type cap;
        cap.powerAdjustCapability.SetNonNull(
            chip::app::DataModel::List<const Structs::PowerAdjustStruct::Type>(d->m_pa_entries, n));
        cap.cause = live;
        d->m_pa_capability.SetNonNull(cap);
    }
    MatterReportingAttributeChangeCallback(ep, Id, Attributes::PowerAdjustmentCapability::Id);
    return MT_ATTR_OK;
}

/*
 * ---- smoke/co alarm (seven-type batch, task C5) ----------------------------
 *
 * Signature verbatim from smoke-co-alarm-server.h:178 (connectedhomeip,
 * vendored under esp-matter release/v1.5.1). Design spec F4: this is the
 * cluster's one mandatory link symbol, no weak default exists anywhere in the
 * vendored tree, so leaving it undefined is a link failure, not a silent
 * no-op, the same shape emberAfDoorLockClusterInitCallback's comment above
 * documents for its own cluster.
 *
 * SmokeCoAlarmServer::HandleRemoteSelfTestRequest (smoke-co-alarm-server.cpp)
 * has already set TestInProgress true, ExpressedState Testing, and answered
 * the controller Status::Success BEFORE calling this: there is no verdict
 * left to give back, only a fire-and-forget notice that a test was
 * requested. This is the C1 notify-only +MTCMD form's first consumer
 * (mt_cmd_notify(), mt_at.h): seq 0, no mailbox slot, never blocks. Runs on
 * the CHIP task (the same reasoning the door lock/valve ember callbacks
 * above give for their own lack of ChipStackLock), so none is taken here
 * either.
 *
 * The host is expected to run its own test and report completion with
 * AT+MTALARM=<ep>,5,0 (TestInProgress false), which fires SelfTestComplete
 * on the SmokeCoAlarmServer singleton; see mt_matter_alarm_set() below, which
 * also recomputes ExpressedState so the endpoint is not left wedged at
 * Testing (bug B165, bench F-C10-2).
 */
void emberAfPluginSmokeCoAlarmSelfTestRequestCommand(chip::EndpointId endpointId)
{
    mt_cmd_notify(endpointId, chip::app::Clusters::SmokeCoAlarm::Id,
                  chip::app::Clusters::SmokeCoAlarm::Commands::SelfTestRequest::Id);
}

/*
 * ExpressedState priority order (bug B165, bench F-C10-2): the SDK's own
 * app-side recompute contract (smoke-co-alarm-server.h:49-55,
 * SetExpressedStateByPriority) leaves the order up to the application and
 * has no default. Nothing in this codebase called it before C12, which is
 * why ExpressedState only ever reached kTesting and never came back down:
 * SetTestInProgress() (smoke-co-alarm-server.cpp:223-237) fires
 * SelfTestComplete but never touches ExpressedState, and every reference
 * app in the vendored tree pairs its state-changing setters with a
 * recompute call. Order copied verbatim from the SDK's own dedicated
 * example (examples/smoke-co-alarm-app/silabs/src/SmokeCoAlarmManager.cpp:
 * 32-36, and identically at examples/all-clusters-app/all-clusters-common/
 * src/smco-stub.cpp:33-36): smoke alarm and its interconnect echo outrank
 * CO alarm and its echo, then hardware fault, then an in-progress self
 * test, then end-of-service, then low battery last.
 */
static const std::array<SmokeCoAlarmServer::ExpressedStateEnum, SmokeCoAlarmServer::kPriorityOrderLength>
    s_alarm_expressed_state_priority = {
        SmokeCoAlarmServer::ExpressedStateEnum::kSmokeAlarm,
        SmokeCoAlarmServer::ExpressedStateEnum::kInterconnectSmoke,
        SmokeCoAlarmServer::ExpressedStateEnum::kCOAlarm,
        SmokeCoAlarmServer::ExpressedStateEnum::kInterconnectCO,
        SmokeCoAlarmServer::ExpressedStateEnum::kHardwareFault,
        SmokeCoAlarmServer::ExpressedStateEnum::kTesting,
        SmokeCoAlarmServer::ExpressedStateEnum::kEndOfService,
        SmokeCoAlarmServer::ExpressedStateEnum::kBatteryAlert,
    };

/*
 * AT+MTALARM bridge (composed appliance round, task 3: restructured into a
 * per-cluster dispatcher). <field> is checked only against the UNION bound
 * 0..11 by mt_at.c's cmd_mtalarm (0..7 fridge, 1..11 smoke; mt_at.c stays
 * free of any esp_matter/CHIP header and cannot know which cluster <ep>
 * carries), so this bridge looks up <ep>'s alarm cluster first and dispatches
 * to the matching family: SmokeCoAlarm checked before RefrigeratorAlarm,
 * MT_ATTR_ERR_CLUSTER if <ep> carries neither. No device type this firmware
 * creates wires both clusters on one endpoint, so the check order is not
 * itself a behaviour decision, only a fixed order to test in.
 *
 * The smoke branch is the pre-existing body, UNCHANGED, with one addition at
 * its top: field 0 (ExpressedState) is derived and never settable, so it now
 * answers MT_ATTR_ERR_VALUE here (moved down from mt_at.c's cmd_mtalarm,
 * which used to reject field 0 for every cluster before this bridge was ever
 * reached; now that field 0 is a legal DoorOpen bit for the fridge branch,
 * the rejection has to be per-family, so it lives here instead). Every other
 * smoke field dispatches to the matching SmokeCoAlarmServer setter, after
 * range-checking <value> against THAT field's own SDK enum
 * (smoke-co-alarm-server.h:40-46 aliases the five enum types off
 * chip::app::Clusters::SmokeCoAlarm::*Enum). Every AlarmStateEnum field
 * (SmokeState/COState/BatteryAlert/InterconnectSmokeAlarm/
 * InterconnectCOAlarm) shares one bound: kUnknownEnumValue = 3
 * (SmokeCoAlarm/Enums.h). MuteStateEnum's bound is 2, EndOfServiceEnum's is
 * 2, ContaminationStateEnum's is 4, SensitivityEnum's is 3 (same file); the
 * two boolean fields (TestInProgress, HardwareFaultAlert) are checked
 * against 0/1 directly, since bool has no kUnknownEnumValue to read. The
 * cast to each field's enum type happens only AFTER its own bound check
 * passes, never before.
 *
 * needs_recompute (bug B165): set for every field that feeds
 * s_alarm_expressed_state_priority, matching the SDK reference apps'
 * pairing of setter + SetExpressedStateByPriority() exactly (cited above).
 * DeviceMuted, ContaminationState and SmokeSensitivityLevel are left out on
 * the same evidence: no ExpressedStateEnum value corresponds to any of the
 * three, and the reference apps never recompute after touching them
 * (smco-stub.cpp:127-130, :161-163, :91-105, :170-176). No double emit:
 * SetExpressedState() (smoke-co-alarm-server.cpp:417-429) is idempotent
 * against the current value, so field 5's SetTestInProgress(ep, false) has
 * already raised SelfTestComplete by the time the recompute below runs, and
 * the recompute raises AllClear only if the new priority verdict actually
 * differs from the wedged kTesting, a distinct fact, not a duplicate of it.
 *
 * The fridge branch (RefrigeratorAlarm, 87/0x0057; see AT_MT_SPEC.md 3.22)
 * treats <field> as the alarm BIT number, <value> as
 * 0/1. RefrigeratorAlarmServer::Instance()'s Get*Value/Set*Value methods
 * (refrigerator-alarm-server.h, verified against the pinned tree) are the
 * exact shape used below: GetSupportedValue/GetStateValue/SetStateValue,
 * each taking a BitMask<RefrigeratorAlarm::AlarmMap>. AlarmMap is a compat
 * alias for AlarmBitmap (CompatEnumNames.h: "using AlarmMap = AlarmBitmap",
 * PR 31517 renamed the type), a uint32_t-backed enum
 * (RefrigeratorAlarm/Enums.h); the Matter spec (checked 1.2 through 1.5.1's
 * RefrigeratorAlarm.xml) defines exactly ONE bit in every revision,
 * DoorOpen at bit 0 - not the eight-bit-wide bitmap this task's brief
 * assumed. field > 7 is kept anyway as a defensive union-shaped bound (it
 * also happens to be a full byte), not because AlarmMap's own width demands
 * it: the real gate is GetSupportedValue()'s bit test just below, which
 * already rejects any field this device's config (mk_refrigerator(),
 * mt_devtypes.cpp: mask=1 state=0 supported=1) does not advertise.
 * SetStateValue() writes the State attribute AND emits the cluster's Notify
 * event itself (refrigerator-alarm-server.cpp:115-149, "When State changes
 * we are generating Notify event"), so - like the smoke branch's own
 * setters - a raw AT+MTATTR write to the same attribute would reach the
 * wire but skip the event a subscribed controller expects; this bridge
 * exists for exactly that reason on both clusters.
 */
extern "C" int mt_matter_alarm_set(uint16_t ep, uint8_t field, uint8_t value)
{
    using namespace chip::app::Clusters;
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }

    if (esp_matter::cluster::get(ep, SmokeCoAlarm::Id) != nullptr) {
        if (field == 0) {
            /* ExpressedState is derived by the server from the other ten
             * fields and is never settable directly (the rejection that
             * used to live in mt_at.c's cmd_mtalarm before field 0 became a
             * legal fridge value too). */
            return MT_ATTR_ERR_VALUE;
        }

        SmokeCoAlarmServer &srv = SmokeCoAlarmServer::Instance();
        bool ok;
        bool needs_recompute = false;
        switch (field) {
        case 1: /* SmokeState */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetSmokeState(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 2: /* COState */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetCOState(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 3: /* BatteryAlert */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetBatteryAlert(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 4: /* DeviceMuted: not an ExpressedState priority, no recompute */
            if (value >= (uint8_t)SmokeCoAlarmServer::MuteStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetDeviceMuted(ep, (SmokeCoAlarmServer::MuteStateEnum)value);
            break;
        case 5: /* TestInProgress: bool, the self-test completion path at 0 */
            if (value > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetTestInProgress(ep, value != 0);
            needs_recompute = true;
            break;
        case 6: /* HardwareFaultAlert: bool */
            if (value > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetHardwareFaultAlert(ep, value != 0);
            needs_recompute = true;
            break;
        case 7: /* EndOfServiceAlert */
            if (value >= (uint8_t)SmokeCoAlarmServer::EndOfServiceEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetEndOfServiceAlert(ep, (SmokeCoAlarmServer::EndOfServiceEnum)value);
            needs_recompute = true;
            break;
        case 8: /* InterconnectSmokeAlarm */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetInterconnectSmokeAlarm(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 9: /* InterconnectCOAlarm */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetInterconnectCOAlarm(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 10: /* ContaminationState: not an ExpressedState priority */
            if (value >= (uint8_t)SmokeCoAlarmServer::ContaminationStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetContaminationState(ep, (SmokeCoAlarmServer::ContaminationStateEnum)value);
            break;
        case 11: /* SmokeSensitivityLevel: not an ExpressedState priority */
            if (value >= (uint8_t)SmokeCoAlarmServer::SensitivityEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetSmokeSensitivityLevel(ep, (SmokeCoAlarmServer::SensitivityEnum)value);
            break;
        default:
            /* mt_at.c's cmd_mtalarm caps field at 11 (the union bound) with
             * +MTERR:1; kept for defensiveness, unreachable in practice. */
            return MT_ATTR_ERR_VALUE;
        }
        if (ok && needs_recompute) {
            srv.SetExpressedStateByPriority(ep, s_alarm_expressed_state_priority);
        }
        return ok ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
    }

    if (esp_matter::cluster::get(ep, RefrigeratorAlarm::Id) != nullptr) {
        /* field is the alarm bit number, value 0/1 */
        if (value > 1) {
            return MT_ATTR_ERR_VALUE;
        }
        if (field > 7) {
            /* defensive union-shaped bound; see the class comment above for
             * why AlarmMap's own width does not actually demand this */
            return MT_ATTR_ERR_VALUE;
        }

        auto &server = RefrigeratorAlarmServer::Instance();
        chip::BitMask<RefrigeratorAlarm::AlarmMap> supported, state;
        auto bit = static_cast<RefrigeratorAlarm::AlarmMap>(1u << field);
        if (server.GetSupportedValue(ep, &supported) != chip::Protocols::InteractionModel::Status::Success ||
            !supported.Has(bit)) {
            return MT_ATTR_ERR_VALUE; /* bit not supported on this device */
        }
        if (server.GetStateValue(ep, &state) != chip::Protocols::InteractionModel::Status::Success) {
            return MT_ATTR_ERR_FAILED;
        }
        if (value) {
            state.Set(bit);
        } else {
            state.Clear(bit);
        }
        /* SetStateValue writes the attribute AND emits the Notify event
         * (refrigerator-alarm-server.cpp:115-149); one call, both effects. */
        return server.SetStateValue(ep, state) == chip::Protocols::InteractionModel::Status::Success
                   ? MT_ATTR_OK
                   : MT_ATTR_ERR_FAILED;
    }

    return MT_ATTR_ERR_CLUSTER;
}

/*
 * ---- chime (seven-type batch, task C6) -------------------------------------
 *
 * F3: create() aborts on a null delegate (cluster::chime::create(),
 * esp_matter_cluster.cpp: "VerifyOrReturnValue(config != NULL &&
 * config->delegate != nullptr, NULL, ...)") - a trap by pointer, not a
 * VALIDATE_FEATURES macro like the smoke/co alarm's or power source's. The
 * thunk (mt_devtypes.cpp's mk_chime()) hands out a pool slot before create()
 * and fixes its endpoint after, the same before/after shape mk_water_valve()/
 * the OperationalState trio use above; see mt_matter_chime_delegate_alloc()'s
 * doc comment in mt_matter.h.
 *
 * ChimeDelegate has three pure virtuals (ChimeCluster.h, connectedhomeip
 * vendored under esp-matter release/v1.5.1):
 *
 *   virtual CHIP_ERROR GetChimeSoundByIndex(uint8_t chimeIndex, uint8_t &chimeID, MutableCharSpan &name) = 0;
 *   virtual CHIP_ERROR GetChimeIDByIndex(uint8_t chimeIndex, uint8_t &chimeID) = 0;
 *   virtual Protocols::InteractionModel::Status PlayChimeSound(uint8_t chimeID) = 0;
 *
 * The first two feed InstalledChimeSounds, the same "index until exhausted"
 * shape as HearthTempLevelsDelegate above: CHIP_ERROR_PROVIDER_LIST_EXHAUSTED
 * past the end of the per-endpoint store AT+MTCHIMESOUNDS fills
 * (mt_matter_chime_sounds_set() below).
 *
 * PlayChimeSound is a REAL wire verdict, the one command on this firmware's
 * whole +MTCMD surface where the host's answer reaches the controller exactly
 * as given: ChimeCluster::HandlePlayChimeSound() takes whatever Status this
 * returns and copies it straight into the command's InvokeResponse, with no
 * SDK-side override or remapping (contrast the water valve, F1, whose verdict
 * the SDK discards outright and always answers Success; and the
 * OperationalState trio, F7, whose verdict is remapped through
 * GenericOperationalError's own ErrorStateEnum rather than passed through
 * raw). Forward the chimeID through the single trailing +MTCMD payload
 * field (mt_cmd_forward_payload(), C1, AT_MT_SPEC.md 3.17's arity table)
 * so the host sees WHICH chime was requested,
 * and return the verdict directly: Status::Success on allow, Status::Failure
 * on deny.
 *
 * This delegate call is not unconditional, though: ChimeCluster::
 * HandlePlayChimeSound() (ChimeCluster.cpp) answers the controller itself,
 * without ever calling PlayChimeSound() below, in two cases: Enabled is
 * false (answers Status::Success with no delegate call and no event), or an
 * explicit chimeID in the command is not one AT+MTCHIMESOUNDS installed
 * (answers Status::NotFound). Neither case reaches mt_cmd_forward_payload(),
 * so no +MTCMD is raised for either; a host that has set Enabled false will
 * see PlayChimeSound invokes stop reaching +MTCMD entirely, not fail it.
 */
/* mt_chime_entry_t now comes from mt_stores.h (identical layout: id, name),
 * pulled in for the mode select store above; the pool this struct backs is
 * unchanged and still local to this port, pending its own migration task. */
struct mt_chime_slot_t {
    bool    used;
    uint16_t ep;
    uint8_t  count;
    mt_chime_entry_t entries[MT_CHIME_MAX_SOUNDS];
};
static mt_chime_slot_t s_chime_slots[MT_COMP_MAX_ENDPOINTS];

static mt_chime_slot_t *mt_chime_find_slot(uint16_t ep)
{
    for (auto &s : s_chime_slots) {
        if (s.used && s.ep == ep) {
            return &s;
        }
    }
    return nullptr;
}

class HearthChimeDelegate : public chip::app::Clusters::ChimeDelegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    CHIP_ERROR GetChimeSoundByIndex(uint8_t chimeIndex, uint8_t &chimeID, chip::MutableCharSpan &name) override
    {
        mt_chime_slot_t *slot = mt_chime_find_slot(m_ep);
        if (slot == nullptr || chimeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = slot->entries[chimeIndex].id;
        chip::CharSpan src(slot->entries[chimeIndex].name, strlen(slot->entries[chimeIndex].name));
        return chip::CopyCharSpanToMutableCharSpan(src, name);
    }

    CHIP_ERROR GetChimeIDByIndex(uint8_t chimeIndex, uint8_t &chimeID) override
    {
        mt_chime_slot_t *slot = mt_chime_find_slot(m_ep);
        if (slot == nullptr || chimeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = slot->entries[chimeIndex].id;
        return CHIP_NO_ERROR;
    }

    /* The one verdict on this firmware's +MTCMD surface passed straight
     * through to the wire: see the class comment above. Only reached when
     * Enabled is true and any explicit chimeID is one this endpoint
     * installed; the SDK answers the controller itself, without calling
     * here, for a disabled chime or an unknown chimeID (class comment). */
    chip::Protocols::InteractionModel::Status PlayChimeSound(uint8_t chimeID) override
    {
        using chip::Protocols::InteractionModel::Status;
        bool allow = mt_cmd_forward_payload(m_ep, chip::app::Clusters::Chime::Id,
                                            chip::app::Clusters::Chime::Commands::PlayChimeSound::Id, chimeID);
        return allow ? Status::Success : Status::Failure;
    }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/*
 * Pool of MT_COMP_MAX_ENDPOINTS delegate objects, the same shape as
 * s_valve_delegates/s_opstate_delegates above. Exposed to mt_devtypes.cpp
 * through the same opaque void* pair pattern, so that file never has to name
 * HearthChimeDelegate or any CHIP delegate type.
 */
static HearthChimeDelegate s_chime_delegates[MT_COMP_MAX_ENDPOINTS];
static size_t              s_chime_delegate_next = 0;

extern "C" void *mt_matter_chime_delegate_alloc(void)
{
    if (s_chime_delegate_next >= MT_COMP_MAX_ENDPOINTS) {
        return nullptr;
    }
    return &s_chime_delegates[s_chime_delegate_next++];
}

extern "C" void mt_matter_chime_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    static_cast<HearthChimeDelegate *>(delegate)->set_endpoint(ep);
}

/*
 * F6 workaround: ESPMatterChimeClusterServerInitCallback (the only function
 * that actually constructs a ChimeCluster and registers it with esp_matter's
 * data model provider) has no call site anywhere in the pinned esp-matter/
 * connectedhomeip tree - grep evidence: the only two hits for its name in the
 * whole SDK tree are its own definition
 * (data_model_provider/clusters/chime_integration.cpp:36) and its declaration
 * (zap_common/app/ClusterCallbacks.h:67). cluster::chime::create()
 * (esp_matter_cluster.cpp) wires only the delegate stash (ChimeDelegateInitCB
 * -> chip::app::Clusters::Chime::SetDelegate()) and a no-op legacy
 * plugin-server stub (MatterChimePluginServerInitCallback, also defined as an
 * empty body in chime_integration.cpp); without a manual call the Chime
 * cluster's ember-managed attributes exist (cluster::chime::create() still
 * creates them) but nothing in the data model actually serves a read, a
 * write, or PlayChimeSound. Recorded for the I90 upstream ride-along (design
 * spec section 8: "Chime upstream fix: the missing init call site rides the
 * I90 list").
 *
 * Called from the same pre-start registration window as the
 * SupportedTemperatureLevels delegate and the AirQuality Instances above
 * (mt_air_quality_register_all()'s doc comment has the full "why pre-start is
 * safe" reasoning: nothing here touches the CHIP event loop or arms a timer).
 *
 * This function calls chip::app::Clusters::Chime::SetDelegate() itself,
 * rather than relying on esp_matter's own ChimeDelegateInitCB to have done
 * it first, and the ordering is load-bearing, not stylistic:
 * ESPMatterChimeClusterServerInitCallback() dereferences
 * *gDelegates[endpointId] unconditionally (chime_integration.cpp:41), so the
 * delegate must already be in Chime's internal map before this runs.
 * ChimeDelegateInitCB is the callback esp_matter's OWN chime::create() wires
 * automatically (set_delegate_and_init_callback(), esp_matter_cluster.cpp),
 * but it does not fire until esp_matter::start() -> chip::Server::Init() ->
 * esp_matter::data_model::provider::Startup()
 * (data_model_provider/esp_matter_data_model_provider.cpp), which walks every
 * endpoint that already exists and invokes each cluster's
 * delegate_init_callback - strictly AFTER this pre-start window closes. A
 * version of this function that skipped the explicit SetDelegate() call and
 * trusted ChimeDelegateInitCB to have already run would dereference a
 * default-constructed (null) map entry and crash on the very first boot with
 * a chime endpoint. (ChimeDelegateInitCB does still fire later, during
 * start(); it only re-sets the same pointer to the same delegate, which is
 * harmless.)
 *
 * Registering the cluster before esp_matter::start() is separately safe, on
 * its own terms: ServerClusterInterfaceRegistry::Register()
 * (src/app/server-cluster/ServerClusterInterfaceRegistry.cpp) only calls
 * Startup() on the newly-registered interface if a context is already set;
 * with none yet (true here, since chip::Server::Init() has not run) it just
 * links the registration into the registry's list and returns success.
 * esp_matter::data_model::provider::Startup(), later inside
 * esp_matter::start(), calls ServerClusterInterfaceRegistry::SetContext(),
 * which walks every already-registered interface (including the ChimeCluster
 * this function just registered) and calls Startup() on each - that is what
 * actually wires it to the interaction model.
 */
static void mt_chime_register_all(void)
{
    for (uint16_t i = 0; i < s_live_count && i < MT_COMP_MAX_ENDPOINTS; i++) {
        uint16_t ep = s_live_ep_id[i];
        if (esp_matter::cluster::get(ep, chip::app::Clusters::Chime::Id) == nullptr) {
            continue;
        }
        HearthChimeDelegate *delegate = nullptr;
        for (auto &d : s_chime_delegates) {
            if (d.endpoint() == ep) {
                delegate = &d;
                break;
            }
        }
        if (delegate == nullptr) {
            ESP_LOGE(TAG, "chime delegate missing for endpoint %u", ep);
            continue;
        }
        chip::app::Clusters::Chime::SetDelegate(ep, delegate);
        ESPMatterChimeClusterServerInitCallback(ep);
    }
}

/*
 * AT+MTCHIMESOUNDS bridge: grammar and content rules are enforced by
 * cmd_mtchimesounds() in mt_at.c before this is ever called; the bounds
 * re-checked here are defensive, not the primary gate (same split as
 * mt_matter_modes_set() above).
 */
extern "C" int mt_matter_chime_sounds_set(uint16_t ep, const uint8_t *ids, const char *const *names, uint8_t count)
{
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::Chime::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_CHIME_MAX_SOUNDS) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_chime_slot_t *slot = mt_chime_find_slot(ep);
    if (!slot) {
        for (auto &s : s_chime_slots) {
            if (!s.used) {
                slot = &s;
                break;
            }
        }
    }
    if (!slot) {
        /* Cannot happen in practice: one slot per MT_COMP_MAX_ENDPOINTS, the
         * same cap the composition itself enforces, same reasoning as
         * mt_matter_temp_levels_set()/mt_matter_modes_set() above. Kept as a
         * defensive return rather than an assert. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(names[i]);
        if (len < 1 || len > MT_CHIME_MAX_NAME_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
        slot->entries[i].id = ids[i];
        memcpy(slot->entries[i].name, names[i], len + 1);
    }
    slot->ep    = ep;
    slot->count = count;
    slot->used  = true;

    /*
     * Mark InstalledChimeSounds dirty so an active subscription sees the new
     * list. ChimeDelegate's own doc comment on GetChimeSoundByIndex()/
     * GetChimeIDByIndex() (ChimeCluster.h) says "If the contents of this list
     * change, the device SHALL call the Instance's
     * ReportInstalledChimeSoundsChange method to report that this attribute
     * has changed" - but no such method exists anywhere on ChimeCluster or
     * its base DefaultServerCluster (grep evidence against the whole
     * chime-server directory finds zero definitions, only that doc comment,
     * repeated twice). The header wins over the plan here (project
     * convention): this cluster was rewritten onto the DefaultServerCluster/
     * ServerClusterInterfaceRegistry architecture, and the doc comment is a
     * leftover reference to a pre-rewrite Instance-based API that no longer
     * exists on this SDK revision. The mechanism ChimeCluster::
     * SetSelectedChime()/SetEnabled() actually use for the same purpose,
     * NotifyAttributeChanged(), is `protected` on DefaultServerCluster and
     * this translation unit is not a subclass, so it cannot be called from
     * here either.
     *
     * MatterReportingAttributeChangeCallback() is the same substitute
     * mt_matter_temp_levels_set()/mt_matter_modes_set() above already use for
     * an identical problem (a delegate-served list attribute with no
     * esp_matter::attribute::update() equivalent): it marks a path dirty
     * through DataModel::Provider::Temporary_ReportAttributeChanged()
     * directly (src/app/reporting/reporting.cpp), which is not ember-specific
     * and works identically for a DefaultServerCluster-registered attribute
     * (Chime) as it does for an ember-managed one (TemperatureControl,
     * ModeSelect).
     */
    MatterReportingAttributeChangeCallback(
        ep, chip::app::Clusters::Chime::Id,
        chip::app::Clusters::Chime::Attributes::InstalledChimeSounds::Id);
    return MT_ATTR_OK;
}

/*
 * AT+MTCHIME bridge: mt_at.c's cmd_mtchime has already checked <what> is 0 or
 * 1 and <value> fits a u8 before this is ever called.
 *
 * The Chime cluster is registered directly with esp_matter's data model
 * provider (mt_chime_register_all() above), not through esp_matter's generic
 * attribute store, so there is no esp_matter::attribute::get()/cluster::get()
 * accessor for its instance the way every other bridge in this file uses;
 * fetch it back from the same registry it was registered into.
 * ServerClusterInterfaceRegistry::Get() (src/app/server-cluster/
 * ServerClusterInterfaceRegistry.h) is public, and the ChimeCluster instance
 * registered at (ep, Chime::Id) is always exactly a ChimeCluster (this file
 * is the only thing that ever registers one there, via
 * ESPMatterChimeClusterServerInitCallback()), so the static_cast back from
 * ServerClusterInterface* is safe: ChimeCluster -> DefaultServerCluster ->
 * ServerClusterInterface is a single, non-virtual inheritance chain
 * (ChimeCluster.h, DefaultServerCluster.h).
 */
extern "C" int mt_matter_chime_set(uint16_t ep, uint8_t what, uint8_t value)
{
    ChipStackLock lock;
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::Chime::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }

    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(
        chip::app::ConcreteClusterPath(ep, chip::app::Clusters::Chime::Id));
    if (iface == nullptr) {
        /* Cannot happen in practice once esp_matter::start() has run: every
         * endpoint with a Chime cluster was registered by
         * mt_chime_register_all() before start(). Kept as a defensive
         * return rather than an assert, the same shape the OperationalState
         * bridge above uses for its own "should not happen" instance lookup. */
        return MT_ATTR_ERR_FAILED;
    }
    auto *chime = static_cast<chip::app::Clusters::ChimeCluster *>(iface);

    using chip::Protocols::InteractionModel::Status;
    Status st;
    switch (what) {
    case 0: /* SelectedChime */
        st = chime->SetSelectedChime(value);
        break;
    case 1: /* Enabled */
        st = chime->SetEnabled(value != 0);
        break;
    default:
        /* mt_at.c's cmd_mtchime already rejects <what> outside 0..1 with
         * +MTERR:1; kept for defensiveness, unreachable in practice. */
        return MT_ATTR_ERR_VALUE;
    }
    return (st == Status::Success) ? MT_ATTR_OK : MT_ATTR_ERR_VALUE;
}

/*
 * ---- Energy EVSE (energy round C2, task 4) ---------------------------------
 *
 * The delegate, its charging-target store and that store's NVS persistence
 * all live in mt_evse.cpp; only these five bridge functions live here, and
 * they exist here for exactly one reason: ChipStackLock is defined in this
 * file and nowhere else. Each is a thin lock-and-forward to the matching
 * mt_evse_*_locked() (mt_evse.h), which is where the _locked suffix comes
 * from and what it means.
 *
 * The lock is not bookkeeping here, it is the load-bearing half of the
 * "GetTargets() returns a non-owning view" contract: the CHIP task calls
 * GetTargets() and the response encoder back to back while holding the
 * stack, so taking the same lock is what stops an AT-side apply from
 * rewriting the schedule between the view being handed out and the encoder
 * reading it. See mt_evse.cpp's file comment.
 */
extern "C" int mt_matter_evse_set(uint16_t ep, uint8_t field, int64_t value)
{
    ChipStackLock lock;

    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::EnergyEvse::Id) == nullptr) {
        return MT_ATTR_ERR_CLUSTER;
    }
    return mt_evse_meas_apply_locked(ep, &field, &value, 1);
}

extern "C" int mt_matter_evse_targets_apply(uint16_t ep, const mt_row_stage_t *stage)
{
    ChipStackLock lock;
    /* clear_days 0: this is the AT path, where a day the host did not send
     * rows for is simply a day it did not mention. Only the fabric path has
     * a way to say "this day now has no targets" (mt_evse.h). */
    return mt_evse_targets_apply_locked(ep, stage, 0);
}

extern "C" int mt_matter_evse_targets_get(uint16_t ep, uint16_t idx, mt_row_t *out,
                                          uint16_t *total)
{
    ChipStackLock lock;
    return mt_evse_targets_get_locked(ep, idx, out, total);
}

extern "C" int mt_matter_evse_targets_total(uint16_t ep, uint16_t *total)
{
    ChipStackLock lock;
    return mt_evse_targets_total_locked(ep, total);
}

extern "C" int mt_matter_evse_targets_erase_all(void)
{
    ChipStackLock lock;
    return mt_evse_targets_erase_all_locked();
}

/*
 * ---- nested row payloads (AT+MTROW family, energy round C2 task 5) --------
 *
 * mt_matter_rows_apply/get/total are the single entry points mt_at.c calls
 * (declared and documented in mt_matter.h); they dispatch on <kind> to
 * whichever store actually owns that row shape. Kind 1
 * (MT_ROW_KIND_EVSE_TARGET) is the only kind this firmware knows today: it
 * forwards straight into the mt_matter_evse_targets_* bridge functions
 * above, which already take ChipStackLock and forward again into
 * mt_evse.cpp's _locked store. This dispatcher therefore takes no lock of
 * its own and never touches the CHIP stack directly, only through the
 * callee, so there is no recursive-lock question to answer.
 *
 * mt_at.c's cmd_mtrow/cmd_mtrowapply/cmd_mtrowget already reject any kind
 * mt_rows_field_count() does not know (an unknown row shape, +MTERR:3)
 * before ever reaching here, so the default arm below is only reachable if
 * a future kind is added to mt_rows.c's field table without a matching
 * store being wired in here yet. MT_ROW_ERR_KIND is exactly the "no such
 * kind" answer mt_at.c's row_err_to_mterr() maps to +MTERR:3, the same code
 * the earlier guard already gives, so the two layers agree even though this
 * arm should never fire in practice.
 *
 * Task 2 shipped weak, fail-closed stub definitions of these three symbols
 * in mt_at.c purely so the firmware could link before this store existed;
 * this task removes those stubs, and this strong definition is what the
 * linker now resolves to.
 */
extern "C" int mt_matter_rows_apply(uint16_t ep, uint8_t kind, const mt_row_stage_t *stage)
{
    switch (kind) {
    case MT_ROW_KIND_EVSE_TARGET:
        return mt_matter_evse_targets_apply(ep, stage);
    default:
        return MT_ROW_ERR_KIND;
    }
}

extern "C" int mt_matter_rows_get(uint16_t ep, uint8_t kind, uint16_t idx, mt_row_t *out,
                                  uint16_t *total)
{
    switch (kind) {
    case MT_ROW_KIND_EVSE_TARGET:
        return mt_matter_evse_targets_get(ep, idx, out, total);
    default:
        if (total) {
            *total = 0;
        }
        return MT_ROW_ERR_KIND;
    }
}

extern "C" int mt_matter_rows_total(uint16_t ep, uint8_t kind, uint16_t *total)
{
    switch (kind) {
    case MT_ROW_KIND_EVSE_TARGET:
        return mt_matter_evse_targets_total(ep, total);
    default:
        if (total) {
            *total = 0;
        }
        return MT_ROW_ERR_KIND;
    }
}

/*
 * AT+MTMETERID (energy round C2, task 9). The pool, the field validation
 * and the apply all live in mt_meter.cpp (mt_meter_set_identity_locked(),
 * mt_meter.h); this is here, the mt_matter_evse_* shape, for exactly one
 * reason: ChipStackLock is defined in this file and nowhere else.
 *
 * The cluster lookup below answers MT_ATTR_ERR_ATTRIBUTE, not the generic
 * MT_ATTR_ERR_CLUSTER every other AT+MTATTR-shaped bridge in this file uses
 * for "endpoint exists, wrong cluster". That is deliberate, not a copy-paste
 * slip: this command's own error table (cmd_mtmeterid, mt_at.c) maps "ep
 * exists but has no MeterIdentification cluster" to +MTERR:4, following the
 * AT+MTROW family's "endpoint exists but carries no payload of that kind"
 * code (design spec 2.5) rather than AT+MTATTR's own lookup-error ladder
 * (+MTERR:2/3/4/5). attr_err_to_mterr() (mt_at.c) maps MT_ATTR_ERR_ATTRIBUTE
 * to +MTERR:4 unchanged, so returning it here needs no new mapping code:
 * only the choice of which existing mt_attr_result_t member to return had
 * to change.
 */
extern "C" int mt_matter_meter_set_identity(uint16_t ep, const mt_meter_identity_t *id)
{
    ChipStackLock lock;

    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, chip::app::Clusters::MeterIdentification::Id) == nullptr) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    return mt_meter_set_identity_locked(ep, id);
}

/* --------------------------------------------------------------------------- */

/*
 * Boot commissioning-window policy. Isolated here because open question P2
 * (design spec section 12.1) is unresolved: an unconfigured device is
 * specified not to advertise, but CHIP auto-opens a window at boot when the
 * node is uncommissioned, and whether that is cleanly suppressible on this
 * esp-matter revision is not yet established.
 *
 * Until P2 concludes this only logs what it would do. Do not scatter window
 * decisions elsewhere: this function is the single place that changes when
 * P2 lands.
 */
/*
 * Can this device actually be reached on the transport this image provides?
 *
 * Asked of CHIP rather than of a stored marker, and that distinction was
 * settled on hardware. An earlier version recorded which transport a fabric
 * was commissioned on and compared it at boot, which sounds equivalent and is
 * not: it answers "where did this fabric come from" when the question is "can
 * this device reach a network". A device commissioned over Thread, reflashed
 * to the WiFi image, joined WiFi perfectly well on credentials left by an
 * earlier WiFi commissioning and served its Thread-era fabric over mDNS. The
 * marker would have called that a mismatch and opened a pointless
 * commissioning window on a device that was working.
 *
 * IsWiFiStationProvisioned() / IsThreadProvisioned() answer the real question,
 * need no NVS of their own, and cannot drift out of date.
 *
 * Call only after esp_matter::start(): both read state the stack populates,
 * and both want the CHIP lock.
 *
 * On the combined image CHIP_DEVICE_CONFIG_ENABLE_THREAD is 1 regardless of
 * which stack actually booted (both are compiled in), so it cannot be the
 * dispatch key here: asking IsThreadProvisioned() unconditionally would flag
 * a healthy WiFi-active, WiFi-commissioned device as mismatched, open an
 * unwanted boot window, and raise a spurious +MTEVT:27. Dispatch on the
 * latch instead, the same one node::create()'s feature map and the
 * dormant-cluster scrub in app_main use.
 */
static bool mt_transport_is_provisioned()
{
    ChipStackLock lock;
#if MT_COMBINED_IMAGE
    return mt_active_transport_is_thread()
        ? chip::DeviceLayer::ConnectivityMgr().IsThreadProvisioned()
        : chip::DeviceLayer::ConnectivityMgr().IsWiFiStationProvisioned();
#elif CHIP_DEVICE_CONFIG_ENABLE_THREAD
    return chip::DeviceLayer::ConnectivityMgr().IsThreadProvisioned();
#else
    return chip::DeviceLayer::ConnectivityMgr().IsWiFiStationProvisioned();
#endif
}

static void mt_boot_window_policy(bool configured)
{
    /*
     * P2, resolved. The question used to be "can the boot window be suppressed
     * on an unconfigured device?" and it was the wrong way round: the bug was
     * that "configured" was the wrong predicate. It has to mean *has a fabric
     * usable on this transport*, not merely *has a fabric*.
     *
     * A device reflashed between the WiFi and Thread images keeps its fabric,
     * because NVS survives a reflash by design. CHIP then finds it, logs
     * "Fabric already commissioned. Disabling BLE advertisement", and the
     * device sits there reporting itself commissioned while reachable by no
     * route at all: no network, because it has no credentials for this
     * transport, and no BLE, because CHIP just switched it off.
     *
     * Nothing is erased to fix that. The fabric is still valid and becomes
     * useful again the moment the original image is reflashed, which is a
     * routine thing to do while developing. What is wrong is the device's
     * claim about itself, so the claim is what gets corrected: open a window
     * and let it be commissioned onto the transport it actually has.
     */
    /*
     * A fabric with no way to be reached on it. Note this asks for the FABRIC
     * count, not the `configured` argument, which reports whether the host has
     * declared an endpoint composition. Those are independent: a device can
     * hold a composition and no fabric (declared but never commissioned) or a
     * fabric and no composition (commissioned, then AT+MTEPCLEAR). Only a
     * fabric can be stranded by a transport it cannot reach.
     */
    s_transport_mismatch = (mt_matter_fabric_count() > 0) && !mt_transport_is_provisioned();

    if (s_transport_mismatch) {
        ESP_LOGW(TAG, "boot window policy: holds a fabric but this transport is not "
                      "provisioned, so it is unreachable; opening a commissioning "
                      "window (spec 3.12.1)");
        /* The +MTEVT:27 for this is raised in app_main AFTER mt_at_start(), not
         * here. This runs before the AT interface exists, so mt_at_urc() would
         * drop it on the s_at_up guard, and even if it did not, a URC ahead of
         * +MTREADY is a protocol violation (spec section 1). */
        if (mt_matter_open_commissioning(MT_MISMATCH_WINDOW_S) != 0) {
            ESP_LOGE(TAG, "failed to open the commissioning window; the device holds a "
                          "fabric it cannot reach and cannot be recommissioned without "
                          "AT+MTCOMMISSION or AT+MTFRESET");
        }
        return;
    }

    if (configured) {
        ESP_LOGI(TAG, "boot window policy: configured device, default CHIP behaviour");
    } else {
        ESP_LOGI(TAG, "boot window policy: unconfigured, CHIP opens its own window");
    }
}

extern "C" void app_main(void)
{
    /* Platform NVS (fabric/credentials/attribute persistence live here). */
    nvs_flash_init();

#if MT_COMBINED_IMAGE
    /*
     * One choice, read once, before node::create() and esp_matter::start():
     * both the patched esp-matter (which driver registers, which stack
     * launches) and the app-level scrub below key on this single latch for
     * the whole boot. mt_transport_active()/_stored() differ only when
     * AT+MTTRANSPORT= staged a switch that has not been rebooted into yet.
     */
    mt_transport_latch_active();
    ESP_LOGI(TAG, "active transport: %s (stored: %s)",
             mt_transport_name(mt_transport_active()),
             mt_transport_name(mt_transport_stored()));
#endif

    /* Matter node with the mandatory Root Node device type on endpoint 0. */
    node::config_t node_config;
#if MT_COMBINED_IMAGE
    /*
     * Set the root node's NetworkCommissioning feature map to match the
     * active transport. Verified settable pre-create: esp_matter's
     * root_node::create() forwards config->network_commissioning straight
     * into cluster::network_commissioning::create() (esp_matter_endpoint.cpp),
     * whose feature_map gate decides which attributes (e.g.
     * supported_wifi_bands) get created at all (esp_matter_cluster.cpp). The
     * struct's own default (esp_matter_cluster.h) picks WiFi over Thread by
     * #if/#elif on the compile-time CHIP_DEVICE_CONFIG_ENABLE_* macros, which
     * is wrong on a combined image where both are defined and the real
     * answer is a runtime choice; this overrides that default explicitly.
     */
    node_config.root_node.network_commissioning.feature_map = mt_active_transport_is_thread()
        ? chip::to_underlying(chip::app::Clusters::NetworkCommissioning::Feature::kThreadNetworkInterface)
        : chip::to_underlying(chip::app::Clusters::NetworkCommissioning::Feature::kWiFiNetworkInterface);
#endif
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }
    (void)node; /* mt_devtype_create() reaches the node via node::get() */

#if MT_COMBINED_IMAGE
    /*
     * Both WiFiNetworkDiagnostics and ThreadNetworkDiagnostics clusters
     * attach to endpoint 0 at compile time with no mutual exclusion (design
     * spec section 2, item 3), because on a single-stack image only one of
     * them is ever compiled in. On the combined image both are, so the one
     * belonging to the transport this boot did NOT choose has to be removed
     * by hand or a controller reading endpoint 0's descriptor sees a stack
     * that is not actually running. No ChipStackLock here: this runs before
     * esp_matter::start(), same as the composition rebuild below, so there is
     * no CHIP event loop yet to race.
     */
    {
        endpoint_t *root = esp_matter::endpoint::get(node, 0);
        uint32_t dormant = mt_active_transport_is_thread()
            ? chip::app::Clusters::WiFiNetworkDiagnostics::Id
            : chip::app::Clusters::ThreadNetworkDiagnostics::Id;
        cluster_t *c = esp_matter::cluster::get(root, dormant);
        if (c) {
            esp_matter::cluster::destroy(c);
            ESP_LOGI(TAG, "dormant %s diagnostics cluster removed",
                     mt_active_transport_is_thread() ? "WiFi" : "Thread");
        }
    }
#endif

    /*
     * Rebuild the endpoint composition the host declared over AT+MTEP. The
     * device does this unaided so it rejoins its fabric after a power cut
     * without waiting on the host (design spec section 5.3).
     *
     * This must happen BEFORE esp_matter::start(): esp_matter persists its
     * endpoint-id counter only for endpoints created after start, so building
     * here is what makes the ids reproducible on every boot.
     */
    mt_composition_t comp;
    int rc = mt_comp_store_load(&comp);
    if (rc < 0) {
        ESP_LOGE(TAG, "composition load failed, starting unconfigured");
        comp.count = 0;
    } else if (rc == 1) {
        ESP_LOGI(TAG, "no stored composition, starting unconfigured");
        comp.count = 0;
    }

    /*
     * The endpoint -> host-fed-store index (mt_store_index.h), sized 2 per
     * endpoint: an RVC endpoint carries two ModeBase stores, every other
     * device type carries at most one store of any kind. Initialised once
     * here, before any endpoint is created, so every per-endpoint store
     * allocated below (starting with mode select) has somewhere to go.
     */
    if (!mt_store_index_init(2u * comp.count)) {
        ESP_LOGE(TAG, "store index init failed, aborting rebuild");
        comp.count = 0;
    }

    for (uint16_t i = 0; i < comp.count; i++) {
        uint16_t ep_id = 0;
        uint32_t parent_devtype = 0;
        uint16_t parent_ep_id = 0;
        if (comp.parent[i] != MT_COMP_NO_PARENT) {
            parent_devtype = s_live_devtype[comp.parent[i]];
            parent_ep_id   = s_live_ep_id[comp.parent[i]];
        }
        if (mt_devtype_create(comp.devtype[i], comp.variant[i], parent_devtype, parent_ep_id,
                              &ep_id) != 0) {
            /*
             * Abort the whole rebuild rather than skipping the failed entry.
             * endpoint::create() increments the id counter only after every
             * failure path has returned, so a failed create consumes no id and
             * every endpoint after it shifts down by one, silently handing a
             * commissioned device the wrong data model. See design spec 12.1.
             * The same abort covers a parenting failure (mt_devtype_create()
             * returns -1 for that too): a live endpoint with the wrong parent
             * is the identical kind of silently-wrong data model.
             */
            ESP_LOGE(TAG, "endpoint %u (0x%04X) failed, aborting rebuild",
                     i, (unsigned)comp.devtype[i]);
            comp.count = 0;
            break;
        }

        /*
         * Pay-per-composition round task 3: an endpoint whose device type
         * attached a ModeSelect cluster gets its host-fed store allocated
         * right here, calloc'ed once and never freed (see the CharSpan-
         * lifetime comment ahead of HearthSupportedModesManager above), and
         * recorded in the index under (ep, MT_STORE_MODE). A failed alloc or
         * a full index is the same class of failure as a failed endpoint
         * create: abort the whole rebuild rather than leave a commissioned
         * device with a ModeSelect cluster and no backing store.
         */
        if (esp_matter::cluster::get(ep_id, chip::app::Clusters::ModeSelect::Id) != nullptr) {
            auto *store = (mt_mode_store_t *)calloc(1, sizeof(mt_mode_store_t));
            if (store == nullptr || !mt_store_index_add(ep_id, 0, MT_STORE_MODE, store)) {
                ESP_LOGE(TAG, "mode store alloc failed at endpoint %u", ep_id);
                comp.count = 0;
                break;
            }
        }

        /*
         * Pay-per-composition round task 4: same pattern, for the
         * TemperatureLevel-variant TemperatureControl cabinets (0x0071
         * variant 1, 0x0077 variant 1). The SupportedTemperatureLevels
         * attribute only exists on that branch (attribute::
         * create_supported_temperature_levels() runs only under
         * feature::temperature_level::add(), esp_matter_feature.cpp; a
         * TemperatureNumber-variant cabinet has the cluster but not this
         * attribute), so checking for the attribute rather than the cluster
         * is what mt_matter_temp_levels_set() already relies on above.
         */
        if (esp_matter::attribute::get(ep_id, chip::app::Clusters::TemperatureControl::Id,
                chip::app::Clusters::TemperatureControl::Attributes::SupportedTemperatureLevels::Id)
            != nullptr) {
            auto *store = (mt_temp_levels_store_t *)calloc(1, sizeof(mt_temp_levels_store_t));
            if (store == nullptr || !mt_store_index_add(ep_id, 0, MT_STORE_TEMP, store)) {
                ESP_LOGE(TAG, "temp levels store alloc failed at endpoint %u", ep_id);
                comp.count = 0;
                break;
            }
        }

        mt_matter_record_endpoint(comp.devtype[i], ep_id, comp.variant[i], comp.parent[i]);
    }

    ESP_LOGI(TAG, "composition rebuilt: %u endpoint(s)", mt_matter_endpoint_count());

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /*
     * Hand OpenThread its platform config BEFORE esp_matter::start().
     *
     * start() calls openthread_init_stack(), which does
     * `assert(s_platform_config)` (OpenthreadLauncher.cpp:263) and nothing
     * else sets it. Omit this and the Thread image boot-loops on that assert
     * with no hint as to what is missing: everything up to and including BLE
     * init logs normally first, so it reads like a BLE fault rather than a
     * missing initialisation.
     *
     * This is why "the Thread variant builds" was not the same claim as "the
     * Thread variant runs". The config guards in this file cover the AT
     * surface (the AT+MTNET? transport report, event bits 24 to 26); they do
     * not bring up the stack that surface describes.
     *
     * On the combined image both stacks are compiled in, so this must ALSO be
     * conditioned on the latch (mt_transport_latch_active(), read above): the
     * patched esp-matter only launches OpenThread when the latch says Thread
     * (sdk-patches/esp-matter/0001-hearth-runtime-transport-selection.patch),
     * and handing over a platform config for a stack that never launches is
     * harmless but pointless. The single-stack Thread build has no latch to
     * consult (MT_COMBINED_IMAGE is 0 there) and keeps this unconditional,
     * byte-identical to before this task.
     */
#if MT_COMBINED_IMAGE
    if (mt_active_transport_is_thread())
#endif
    {
        esp_openthread_platform_config_t ot_config = {
            .radio_config = MT_OT_RADIO_CONFIG(),
            .host_config = MT_OT_HOST_CONFIG(),
            .port_config = MT_OT_PORT_CONFIG(),
        };
        set_openthread_platform_config(&ot_config);
    }
#endif

    /*
     * Register the SupportedTemperatureLevels delegate once, before start().
     * CHIP's temperature-control-server queries this global singleton for
     * every endpoint (TemperatureControlAttrAccess::Read(), see the delegate
     * class above); a TemperatureLevel-variant cabinet in the composition
     * just rebuilt would otherwise serve an empty list forever, since no
     * later CHIP event goes back and re-checks whether an instance exists.
     */
    chip::app::Clusters::TemperatureControl::SetInstance(&s_temp_levels_delegate);

    /*
     * C1b (bug B139): construct the AirQuality server Instance for every
     * live endpoint that carries the cluster, same window as the delegate
     * registration just above and for the same reason: this needs the
     * composition's clusters to already exist and must land before
     * esp_matter::start() so nothing races the CHIP event loop.
     */
    mt_air_quality_register_all();

    /*
     * C6 (seven-type batch): the F6 SDK-gap workaround. Same window, same
     * "must land before esp_matter::start()" reasoning as the AirQuality
     * registration just above, plus its own ordering constraint against
     * esp_matter's own delegate init callback; see mt_chime_register_all()'s
     * doc comment for the full trail.
     */
    mt_chime_register_all();

    /*
     * Energy round C2, task 8: the MeterIdentification cluster's own
     * disease, same window and same "must land before esp_matter::start()"
     * reasoning as AirQuality and Chime just above. Full citation trail
     * (esp-matter never constructs the Instance this cluster needs, and why
     * that is only discoverable by reading the SDK, not by testing this
     * firmware) is mt_meter.cpp's file comment.
     */
    mt_meter_register_all();

    /* Bring up the Matter stack (BLE commissioning + WiFi or Thread transport). */
    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    mt_boot_window_policy(mt_matter_endpoint_count() > 0);
    ESP_LOGI(TAG, "Matter started with %u endpoint(s)", mt_matter_endpoint_count());

    /* Bring up the AT+MT host interface (AT UART + parser). Runs alongside
     * Matter; the host drives Matter over AT from B4.2 onward. */
    mt_at_start();
    ESP_LOGI(TAG, "iLabs AT Hearth: AT+MT interface up");

    s_heap_at_startup = esp_get_free_heap_size();
    ESP_LOGI(TAG, "free heap at startup: %u (BLE resident)", (unsigned)s_heap_at_startup);
#if MT_COMBINED_IMAGE
    /* Same figure, annotated: the dormant stack's BSS/heap tax (Task 2's
     * measured tens-of-KB deltas, ARCHITECTURE.md section 3.1) is baked into
     * the number just logged above, so an SDK bump that changes it shows up
     * here rather than needing a diff against build_b4/build_thread. */
    ESP_LOGI(TAG, "combined image: active transport %s, dormant stack scrubbed",
             mt_transport_name(mt_transport_active()));
#endif

    /*
     * Boot-state events, replayed now that the AT interface exists.
     *
     * Everything raised before mt_at_start() is dropped by design (the host is
     * not listening and resynchronizes on +MTREADY), but "a commissioning
     * window is open" is the one piece of boot state a host genuinely acts on,
     * and whether CHIP opens it during esp_matter::start() or shortly after is
     * not something this firmware controls. It moved once already: keeping BLE
     * resident for defect D1 made CHIP advertise during Server::Init, which is
     * early enough to be dropped, where before it landed just late enough to
     * be delivered.
     *
     * Replaying it here makes the sequence deterministic instead of a race:
     * +MTREADY, then exactly one +MTEVT:0 on every boot that has a window open,
     * whichever side of mt_at_start() CHIP happened to open it. Worth pinning
     * precisely because C5 will assert on it, and an assertion against a race
     * is worse than no assertion.
     *
     * s_window_evt_delivered is what makes it exactly one rather than one or
     * two: the replay only fires for an event the host did not already get.
     */
    if (!s_window_evt_sent && mt_matter_state() == MT_STATE_COMMISSIONING
        && !s_window_evt_sent) {
        /* Re-tested after mt_matter_state(), which takes and releases the CHIP
         * lock: any event dispatch in flight when the first test ran has
         * completed by then, so the second test sees its flag. */
        if (mt_at_event(MT_EVT_COMMISSION_WINDOW_OPEN, nullptr)) {
            s_window_evt_sent = true;
        }
    }

    /* Same reasoning, and the same placement requirement: raised from
     * mt_boot_window_policy() this would be dropped by the s_at_up guard and
     * would breach the rule that +MTREADY is the first line of a session. */
    if (s_transport_mismatch) {
        mt_at_event(MT_EVT_TRANSPORT_MISMATCH, nullptr);
    }
}
