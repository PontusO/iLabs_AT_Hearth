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
#include <stdio.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>

#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "mt_at.h"
#include "mt_comp_store.h"
#include "mt_composition.h"
#include "mt_devtypes.h"
#include "mt_matter.h"

static const char *TAG = "mt_main";

using namespace esp_matter;
using namespace esp_matter::endpoint;

/*
 * Map an esp_matter attribute value to/from a plain integer for the AT+MTATTR
 * command (host sends/receives integers; strings/floats/arrays are unsupported).
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
    default: return false;
    }
}

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
        mt_at_event(MT_EVT_COMMISSION_WINDOW_OPEN, nullptr);
        break;
    case DeviceEventType::kCommissioningWindowClosed:
        mt_at_event(MT_EVT_COMMISSION_WINDOW_CLOSED, nullptr);
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
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
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

extern "C" int mt_matter_fabric_count(void)
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

extern "C" int mt_matter_state(void)
{
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
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager()
        .OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(timeout_s),
                                      chip::CommissioningWindowAdvertisement::kAllSupported);
    return (err == CHIP_NO_ERROR) ? 0 : -1;
}

extern "C" int mt_matter_onboarding_codes(char *qr, size_t qr_len, char *manual, size_t manual_len)
{
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
    esp_matter::factory_reset();
}

extern "C" int mt_matter_net_info(int *transport, int *enabled, int *connected)
{
    if (!transport || !enabled || !connected) {
        return -1;
    }
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
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
static uint16_t s_live_count = 0;

extern "C" uint16_t mt_matter_endpoint_count(void)
{
    return s_live_count;
}

extern "C" int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id)
{
    if (index >= s_live_count || !devtype || !ep_id) {
        return -1;
    }
    *devtype = s_live_devtype[index];
    *ep_id   = s_live_ep_id[index];
    return 0;
}

extern "C" void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id)
{
    if (s_live_count >= MT_COMP_MAX_ENDPOINTS) {
        return;
    }
    s_live_devtype[s_live_count] = devtype;
    s_live_ep_id[s_live_count]   = ep_id;
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
static void mt_boot_window_policy(bool configured)
{
    if (configured) {
        ESP_LOGI(TAG, "boot window policy: configured device, default CHIP behaviour");
    } else {
        ESP_LOGW(TAG, "boot window policy: UNCONFIGURED, spec section 5.5 wants no "
                      "commissioning window here (open question P2)");
    }
}

extern "C" void app_main(void)
{
    /* Platform NVS (fabric/credentials/attribute persistence live here). */
    nvs_flash_init();

    /* Matter node with the mandatory Root Node device type on endpoint 0. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }
    (void)node; /* mt_devtype_create() reaches the node via node::get() */

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
        if (mt_devtype_create(comp.devtype[i], &ep_id) != 0) {
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
        mt_matter_record_endpoint(comp.devtype[i], ep_id);
    }

    ESP_LOGI(TAG, "composition rebuilt: %u endpoint(s)", mt_matter_endpoint_count());

    /* Bring up the Matter stack (BLE commissioning + WiFi transport). */
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
}
