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

#include <inttypes.h>
#include <cstring>
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
static uint16_t s_live_count = 0;

extern "C" uint16_t mt_matter_endpoint_count(void)
{
    return s_live_count;
}

extern "C" int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id, uint8_t *variant)
{
    if (index >= s_live_count || !devtype || !ep_id || !variant) {
        return -1;
    }
    *devtype = s_live_devtype[index];
    *ep_id   = s_live_ep_id[index];
    *variant = s_live_variant[index];
    return 0;
}

extern "C" void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id, uint8_t variant)
{
    if (s_live_count >= MT_COMP_MAX_ENDPOINTS) {
        return;
    }
    s_live_devtype[s_live_count] = devtype;
    s_live_ep_id[s_live_count]   = ep_id;
    s_live_variant[s_live_count] = variant;
    s_live_count++;
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
        if (mt_devtype_create(comp.devtype[i], comp.variant[i], &ep_id) != 0) {
            /*
             * Abort the whole rebuild rather than skipping the failed entry.
             * endpoint::create() increments the id counter only after every
             * failure path has returned, so a failed create consumes no id and
             * every endpoint after it shifts down by one, silently handing a
             * commissioned device the wrong data model. See design spec 12.1.
             */
            ESP_LOGE(TAG, "endpoint %u (0x%04X) failed, aborting rebuild",
                     i, (unsigned)comp.devtype[i]);
            comp.count = 0;
            break;
        }
        mt_matter_record_endpoint(comp.devtype[i], ep_id, comp.variant[i]);
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
