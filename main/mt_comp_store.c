/*
 * mt_comp_store.c - read and write the endpoint composition in NVS.
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mt_comp_store.h"

static const char *TAG = "mt_comp_store";

#define MT_COMP_NVS_NAMESPACE "mt_ep"
#define MT_COMP_NVS_KEY       "comp"
/* NVS keys are capped at 15 characters; "xport" is well inside that. */
#define MT_XPORT_NVS_KEY      "xport"

int mt_comp_store_load(mt_composition_t *out)
{
    if (!out) {
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_COMP_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 1; /* namespace never written: factory fresh */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t blob[MT_COMP_BLOB_MAX];
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, MT_COMP_NVS_KEY, blob, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 1;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return -1;
    }

    if (mt_comp_decode(blob, len, out) != 0) {
        ESP_LOGE(TAG, "stored composition is corrupt (%u bytes)", (unsigned)len);
        return -1;
    }

    ESP_LOGI(TAG, "loaded composition: %u endpoint(s)", out->count);
    return 0;
}

int mt_comp_store_save(const mt_composition_t *comp)
{
    if (!comp) {
        return -1;
    }

    uint8_t blob[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(comp, blob, sizeof(blob));
    if (n < 0) {
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_COMP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_set_blob(h, MT_COMP_NVS_KEY, blob, (size_t)n);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving composition failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "saved composition: %u endpoint(s)", comp->count);
    return 0;
}

int mt_comp_store_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_COMP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 0; /* nothing stored: already unconfigured */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_erase_key(h, MT_COMP_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* namespace exists but no composition in it */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erasing composition failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "composition erased, device is unconfigured");
    return 0;
}

/* ---- transport marker (spec 3.12.1) -------------------------------------- */

int mt_comp_store_load_transport(int *out)
{
    if (!out) {
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_COMP_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 1; /* namespace never written */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t v = 0;
    err = nvs_get_u8(h, MT_XPORT_NVS_KEY, &v);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace exists but no marker. Either the device has never been
         * commissioned, or it predates this marker. Both are "unknown", and
         * both must read as "no mismatch": inventing one would open a
         * commissioning window on a device that is working perfectly. */
        return 1;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading transport failed: %s", esp_err_to_name(err));
        return -1;
    }

    *out = (int)v;
    return 0;
}

int mt_comp_store_save_transport(int transport)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_COMP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_set_u8(h, MT_XPORT_NVS_KEY, (uint8_t)transport);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "writing transport failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "stored transport marker: %d", transport);
    return 0;
}
