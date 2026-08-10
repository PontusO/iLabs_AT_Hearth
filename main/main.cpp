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
#include "mt_matter.h"
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

/* Window opened on a transport mismatch. The same 300 s AT+MTCOMMISSION
 * defaults to: long enough to commission unhurriedly, short enough that a
 * device nobody is attending stops advertising. */
#define MT_MISMATCH_WINDOW_S 300

using namespace esp_matter;
using namespace esp_matter::endpoint;

/*
 * Map an esp_matter attribute value to/from a plain integer for the AT+MTATTR
 * command (host sends/receives integers; strings/floats/arrays are unsupported).
 *
 * Nullable numeric attributes (ESP_MATTER_VAL_TYPE_NULLABLE_*, esp-matter's
 * disjoint second family for every integer/bool/enum/bitmap type, offset by
 * ESP_MATTER_VAL_NULLABLE_BASE) share the same union field as their plain
 * sibling and convert the same way once a value is known to be present.
 * A NULL value is a different question: the AT grammar has no null literal,
 * and the host library's access pattern (begin() always writes an attribute
 * before any read of it) never reads a never-written nullable in practice.
 * So a null read answers exactly what an unsupported type answers today:
 * attr_val_to_long() returns false and the caller reports +MTERR:5. Adding a
 * way to WRITE null over AT is a separate, out-of-scope feature; see
 * long_to_attr_val() below.
 */
static bool attr_val_to_long(const esp_matter_attr_val_t *v, long *out)
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
    case ESP_MATTER_VAL_TYPE_BITMAP32: *out = (long)v->val.u32; return true;
    case ESP_MATTER_VAL_TYPE_INT64:    *out = (long)v->val.i64; return true;
    case ESP_MATTER_VAL_TYPE_UINT64:   *out = (long)v->val.u64; return true;

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
        *out = (long)v->val.u32; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT64:
        if (chip::app::NumericAttributeTraits<int64_t>::IsNullValue(v->val.i64)) return false;
        *out = (long)v->val.i64; return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT64:
        if (chip::app::NumericAttributeTraits<uint64_t>::IsNullValue(v->val.u64)) return false;
        *out = (long)v->val.u64; return true;

    default: return false;
    }
}

/*
 * Same integer<->attr_val bridge, write direction. `v` arrives pre-populated
 * by attribute::get_val() with the located attribute's real type (including
 * NULLABLE_*), so this switches on that type exactly as attr_val_to_long()
 * does; it never decides on its own whether an attribute is nullable.
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
static bool long_to_attr_val(esp_matter_attr_val_t *v, long in)
{
    switch (v->type) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN:  v->val.b   = (bool)in;     return true;
    case ESP_MATTER_VAL_TYPE_INTEGER:  v->val.i   = (int)in;      return true;
    case ESP_MATTER_VAL_TYPE_INT8:     v->val.i8  = (int8_t)in;   return true;
    case ESP_MATTER_VAL_TYPE_UINT8:
    case ESP_MATTER_VAL_TYPE_ENUM8:
    case ESP_MATTER_VAL_TYPE_BITMAP8:  v->val.u8  = (uint8_t)in;  return true;
    case ESP_MATTER_VAL_TYPE_INT16:    v->val.i16 = (int16_t)in;  return true;
    case ESP_MATTER_VAL_TYPE_UINT16:
    case ESP_MATTER_VAL_TYPE_ENUM16:
    case ESP_MATTER_VAL_TYPE_BITMAP16: v->val.u16 = (uint16_t)in; return true;
    case ESP_MATTER_VAL_TYPE_INT32:    v->val.i32 = (int32_t)in;  return true;
    case ESP_MATTER_VAL_TYPE_UINT32:
    case ESP_MATTER_VAL_TYPE_BITMAP32: v->val.u32 = (uint32_t)in; return true;
    case ESP_MATTER_VAL_TYPE_INT64:    v->val.i64 = (int64_t)in;  return true;
    case ESP_MATTER_VAL_TYPE_UINT64:   v->val.u64 = (uint64_t)in; return true;

    case ESP_MATTER_VAL_TYPE_NULLABLE_BOOLEAN:  *v = esp_matter_nullable_bool((bool)in);          return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INTEGER:  *v = esp_matter_nullable_int((int)in);            return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT8:     *v = esp_matter_nullable_int8((int8_t)in);        return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT8:    *v = esp_matter_nullable_uint8((uint8_t)in);      return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM8:    *v = esp_matter_nullable_enum8((uint8_t)in);      return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP8:  *v = esp_matter_nullable_bitmap8((uint8_t)in);    return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT16:    *v = esp_matter_nullable_int16((int16_t)in);      return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT16:   *v = esp_matter_nullable_uint16((uint16_t)in);    return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM16:   *v = esp_matter_nullable_enum16((uint16_t)in);    return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP16: *v = esp_matter_nullable_bitmap16((uint16_t)in);  return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT32:    *v = esp_matter_nullable_int32((int32_t)in);      return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT32:   *v = esp_matter_nullable_uint32((uint32_t)in);    return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP32: *v = esp_matter_nullable_bitmap32((uint32_t)in);  return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_INT64:    *v = esp_matter_nullable_int64((int64_t)in);      return true;
    case ESP_MATTER_VAL_TYPE_NULLABLE_UINT64:   *v = esp_matter_nullable_uint64((uint64_t)in);    return true;

    default: return false;
    }
}

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
        break;
    case DeviceEventType::kThreadStateChange:
        mt_at_event(MT_EVT_THREAD_STATE_CHANGE, nullptr);
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
        long v;
        if (attr_val_to_long(val, &v)) {
            char line[64];
            snprintf(line, sizeof(line), "+MTATTR:%u,%lu,%lu,%ld", endpoint_id,
                     (unsigned long)cluster_id, (unsigned long)attribute_id, v);
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
 * cluster::air_quality::feature::*::add() calls set up (81451a7). Both are
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
 * option the way s_temp_levels above is. Raw aligned storage plus
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

extern "C" int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, long *out)
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
        *out = static_cast<long>(e->instance->GetAirQuality());
        return MT_ATTR_OK;
    }

    esp_matter::attribute_t *a = nullptr;
    mt_attr_result_t r = attr_locate(ep, cluster, attr, &a);
    if (r != MT_ATTR_OK) {
        return r;
    }

    esp_matter_attr_val_t val;
    if (esp_matter::attribute::get_val(a, &val) != ESP_OK) {
        return MT_ATTR_ERR_FAILED;
    }
    return attr_val_to_long(&val, out) ? MT_ATTR_OK : MT_ATTR_ERR_TYPE;
}

extern "C" int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, long in, bool notify)
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
    if (esp_matter::attribute::get_val(a, &val) != ESP_OK) {
        return MT_ATTR_ERR_FAILED;
    }
    if (!long_to_attr_val(&val, in)) {
        return MT_ATTR_ERR_TYPE;
    }

    esp_err_t err = notify ? esp_matter::attribute::update(ep, cluster, attr, &val)
                           : esp_matter::attribute::set_val(a, &val);
    return (err == ESP_OK) ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
}

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
 * Label store, one slot per live endpoint. Filled by AT+MTTEMPLEVELS
 * (mt_matter_temp_levels_set() below); starts empty every boot, deliberately
 * not persisted (mt_matter.h). "used" rather than a sentinel endpoint id
 * because 0 is a legal-looking value to compare against by accident.
 */
struct mt_temp_level_entry_t {
    bool    used;
    uint16_t ep;
    uint8_t  count;
    char     labels[MT_TEMP_LEVEL_MAX_COUNT][MT_TEMP_LEVEL_MAX_LEN + 1];
};
static mt_temp_level_entry_t s_temp_levels[MT_COMP_MAX_ENDPOINTS];

namespace {
class HearthTempLevelsDelegate : public chip::app::Clusters::TemperatureControl::SupportedTemperatureLevelsIteratorDelegate {
public:
    uint8_t Size() override
    {
        for (auto &e : s_temp_levels) {
            if (e.used && e.ep == mEndpoint) {
                return e.count;
            }
        }
        return 0;
    }

    CHIP_ERROR Next(chip::MutableCharSpan &item) override
    {
        for (auto &e : s_temp_levels) {
            if (e.used && e.ep == mEndpoint) {
                if (mIndex >= e.count) {
                    return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
                }
                chip::CharSpan label(e.labels[mIndex], strlen(e.labels[mIndex]));
                CHIP_ERROR err = chip::CopyCharSpanToMutableCharSpan(label, item);
                if (err != CHIP_NO_ERROR) {
                    return err;
                }
                mIndex++;
                return CHIP_NO_ERROR;
            }
        }
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
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

    mt_temp_level_entry_t *slot = nullptr;
    for (auto &e : s_temp_levels) {
        if (e.used && e.ep == ep) {
            slot = &e;
            break;
        }
    }
    if (!slot) {
        for (auto &e : s_temp_levels) {
            if (!e.used) {
                slot = &e;
                break;
            }
        }
    }
    if (!slot) {
        /* Cannot happen in practice: the store has one slot per
         * MT_COMP_MAX_ENDPOINTS, the same cap the composition itself
         * enforces, so there is always a free or matching slot for a live
         * endpoint. Kept as a defensive return rather than an assert. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_TEMP_LEVEL_MAX_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
        memcpy(slot->labels[i], labels[i], len + 1);
    }
    slot->ep    = ep;
    slot->count = count;
    slot->used  = true;

    /*
     * Mark the attribute dirty so an active subscription sees the new list.
     * MatterReportingAttributeChangeCallback(endpoint, clusterId, attributeId)
     * (src/app/reporting/reporting.h:34) is what every cluster server in the
     * SDK calls after mutating state served through a delegate or
     * AttributeAccessInterface rather than esp_matter's own attribute store
     * (e.g. service-area-server.cpp:418); there is no esp_matter equivalent
     * here, because esp_matter::attribute::update() only knows how to push
     * its own internally-managed union value, and this attribute's real
     * content lives in s_temp_levels, read out through the delegate above.
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
 * runtime, host-fed store is legal. Backing store: MT_COMP_MAX_ENDPOINTS
 * slots x MT_MODES_MAX_COUNT entries, each holding the u8 mode value, a
 * (MT_MODES_MAX_LABEL_LEN + 1)-byte label buffer, and a ModeOptionStructType
 * whose CharSpan.label points into that same buffer
 * (ModeSelect/Structs.h:71-81: "chip::CharSpan label; uint8_t mode; ...
 * semanticTags;").
 *
 * CharSpan lifetime: both the label bytes (mt_mode_entry_t::label) and the
 * ModeOptionStructType array (mt_mode_slot_t::structs) are static storage
 * (s_mode_slots below), never heap or stack, so neither is freed while the
 * program runs. The struct array is rebuilt IN PLACE on every AT+MTMODES
 * write to the same slot (mt_matter_modes_set() below), never reallocated or
 * moved, so no CharSpan is ever left pointing at freed memory. The
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
struct mt_mode_entry_t {
    uint8_t mode;
    char    label[MT_MODES_MAX_LABEL_LEN + 1];
};

struct mt_mode_slot_t {
    bool     used;
    uint16_t ep;
    uint8_t  count;
    mt_mode_entry_t entries[MT_MODES_MAX_COUNT];
    chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type structs[MT_MODES_MAX_COUNT];
};
static mt_mode_slot_t s_mode_slots[MT_COMP_MAX_ENDPOINTS];

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
        for (auto &slot : s_mode_slots) {
            if (slot.used && slot.ep == endpointId) {
                return ModeOptionsProvider(slot.structs, slot.structs + slot.count);
            }
        }
        return ModeOptionsProvider();  /* begin == end == nullptr: no entry for this endpoint */
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(
        chip::EndpointId endpointId, uint8_t mode, const ModeOptionStructType **dataPtr) const override
    {
        for (auto &slot : s_mode_slots) {
            if (slot.used && slot.ep == endpointId) {
                for (uint8_t i = 0; i < slot.count; i++) {
                    if (slot.structs[i].mode == mode) {
                        *dataPtr = &slot.structs[i];
                        return chip::Protocols::InteractionModel::Status::Success;
                    }
                }
                return chip::Protocols::InteractionModel::Status::InvalidCommand;
            }
        }
        return chip::Protocols::InteractionModel::Status::UnsupportedCluster;
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

    mt_mode_slot_t *slot = nullptr;
    for (auto &s : s_mode_slots) {
        if (s.used && s.ep == ep) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        for (auto &s : s_mode_slots) {
            if (!s.used) {
                slot = &s;
                break;
            }
        }
    }
    if (!slot) {
        /* Cannot happen in practice: one slot per MT_COMP_MAX_ENDPOINTS, the
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
        slot->entries[i].mode = modes[i];
        memcpy(slot->entries[i].label, labels[i], len + 1);
    }
    slot->ep    = ep;
    slot->count = count;
    slot->used  = true;

    /* Rebuild the struct array in place, contiguous, each CharSpan pointing
     * into the static label buffer above: see the CharSpan-lifetime comment
     * ahead of this class. */
    for (uint8_t i = 0; i < count; i++) {
        slot->structs[i].mode  = slot->entries[i].mode;
        slot->structs[i].label = chip::CharSpan::fromCharString(slot->entries[i].label);
        slot->structs[i].semanticTags =
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
 * Store: MT_MB_MAX_LISTS slots keyed by (ep, cluster), the same shape as
 * s_mode_slots above but with a per-entry uint16_t tag alongside the mode
 * value and label (design spec 3.1's mandatory <tag> field). Full
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
struct mt_mb_entry_t {
    uint8_t  mode;
    uint16_t tag;
    char     label[MT_MB_MAX_LABEL_LEN + 1];
};

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
     * appliance round, task 3) RefrigeratorAndTemperatureControlledCabinetMode
     * AND (task 4) OvenMode
     * (verified against each cluster's generated CommandIds.h: all four
     * declare "inline constexpr CommandId Id = 0x00000000" for ChangeToMode),
     * so one constant covers all four. MicrowaveOvenMode never reaches this
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
     * "Auto"). mt_matter_modebase_set() below applies the
     * identical table to real host-declared entries at store time. */
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
 * cluster against the five ModeBase ids (composed appliance round: task 3
 * adds RefrigeratorAndTemperatureControlledCabinetMode and task 4 adds
 * OvenMode to the three from the RVC + Microwave batch): mt_at.c stays free
 * of any esp_matter/CHIP header and cannot read those ids itself.
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
        cluster != RefrigeratorAndTemperatureControlledCabinetMode::Id && cluster != OvenMode::Id) {
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
        /* MT_MB_MAX_LISTS (8) bounds how many distinct (endpoint, cluster)
         * ModeBase lists this firmware stores at once; it is smaller than
         * MT_COMP_MAX_ENDPOINTS (16), so a composition dense enough in
         * RvcRunMode/RvcCleanMode/MicrowaveOvenMode instances CAN exhaust it
         * (e.g. more than 8 such cluster instances combined, which the
         * three-ModeBase-clusters-per-RVC-endpoint shape in this batch makes
         * reachable well under 16 endpoints). This return is the safe
         * fallback for that case: the call is refused with MT_ATTR_ERR_FAILED
         * rather than silently overwriting an unrelated (endpoint, cluster)
         * slot. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_MB_MAX_LABEL_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
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
struct mt_chime_entry_t {
    uint8_t id;
    char    name[MT_CHIME_MAX_NAME_LEN + 1];
};

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
