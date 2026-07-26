/*
 * main.cpp - iLabs AT Matter, Phase B2 bring-up.
 *
 * B2 goal: prove the esp_matter runtime runs inside this firmware and
 * commissions like the stock esp-matter light. A single on/off-light endpoint,
 * Matter-over-WiFi, built on the shared design (IDF v5.4.1 + esp-matter).
 *
 * The AT+MT interface (at_core / mt_at) is intentionally NOT started here yet -
 * it returns in B4, together with the console/AT UART split. For B2 the console
 * and CHIP shell stay on the host-bridge UART so bring-up mirrors the stock
 * light: commission with chip-tool, toggle the OnOff attribute.
 */

#include <inttypes.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>

static const char *TAG = "mt_main";

using namespace esp_matter;
using namespace esp_matter::endpoint;

static uint16_t s_light_endpoint_id = 0;

/* Platform events (commissioning lifecycle). */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed (fail-safe timer expired)");
        break;
    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;
    default:
        break;
    }
}

/* Identify cluster (no physical indicator on this board yet). */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify: type=%u effect=%u variant=%u", type, effect_id, effect_variant);
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
    if (type == attribute::POST_UPDATE) {
        ESP_LOGI(TAG, "attr update: ep=%u cluster=0x%04" PRIx32 " attr=0x%04" PRIx32,
                 endpoint_id, cluster_id, attribute_id);
    }
    return ESP_OK;
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

    /* One on/off-light endpoint - the simplest controllable device type. */
    on_off_light::config_t light_config;
    endpoint_t *endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    if (endpoint == nullptr) {
        ESP_LOGE(TAG, "Failed to create on_off_light endpoint");
        return;
    }
    s_light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "on/off light endpoint created, id=%u", s_light_endpoint_id);

    /* Bring up the Matter stack (BLE commissioning + WiFi transport). */
    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    ESP_LOGI(TAG, "iLabs AT Matter (B2): Matter started, on/off light ready on endpoint %u",
             s_light_endpoint_id);
}
