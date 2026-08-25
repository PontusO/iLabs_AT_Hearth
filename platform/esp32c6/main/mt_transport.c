/*
 * mt_transport.c - runtime transport selection for the combined WiFi+Thread
 * image.
 *
 * mt_transport_parse() and mt_transport_name() are pure and compiled on the
 * host (test/host/test_mt_transport.c). Everything below them needs real
 * NVS and the boot-latch static, so it is compiled out under MT_HOST_TEST,
 * which the host Makefile defines. This is the same guard shape
 * mt_comp_store.c would need if its codec and its NVS wrapper were ever
 * merged into one file; kept as the local convention until a second module
 * wants the same split, at which point it is worth a shared idiom.
 */

#include <stdint.h>
#include <string.h>

#include "mt_transport.h"

int mt_transport_parse(const char *arg, mt_transport_t *out)
{
    if (!arg || !out) {
        return -1;
    }
    if (strcmp(arg, "WIFI") == 0) {
        *out = MT_TRANSPORT_WIFI;
        return 0;
    }
    if (strcmp(arg, "THREAD") == 0) {
        *out = MT_TRANSPORT_THREAD;
        return 0;
    }
    return -1;
}

const char *mt_transport_name(mt_transport_t t)
{
    return (t == MT_TRANSPORT_THREAD) ? "THREAD" : "WIFI";
}

#ifndef MT_HOST_TEST

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "mt_transport";

#define MT_TRANSPORT_NVS_NAMESPACE "mt_cfg"
#define MT_TRANSPORT_NVS_KEY       "transport"

mt_transport_t mt_transport_stored(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_TRANSPORT_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* Covers ESP_ERR_NVS_NOT_FOUND (factory fresh) and anything else:
         * either way there is nothing usable to read, so default WIFI. */
        return MT_TRANSPORT_WIFI;
    }

    uint8_t v = 0;
    err = nvs_get_u8(h, MT_TRANSPORT_NVS_KEY, &v);
    nvs_close(h);

    if (err != ESP_OK || v > MT_TRANSPORT_THREAD) {
        return MT_TRANSPORT_WIFI;
    }
    return (mt_transport_t)v;
}

int mt_transport_store(mt_transport_t t)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_TRANSPORT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_set_u8(h, MT_TRANSPORT_NVS_KEY, (uint8_t)t);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing transport failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "transport stored: %s", mt_transport_name(t));
    return 0;
}

static mt_transport_t s_active = MT_TRANSPORT_WIFI;

void mt_transport_latch_active(void)
{
    s_active = mt_transport_stored();
    ESP_LOGI(TAG, "active transport latched: %s", mt_transport_name(s_active));
}

mt_transport_t mt_transport_active(void)
{
    return s_active;
}

/*
 * The weak hook the pinned esp-matter patchset calls. Latched before
 * esp_matter::start() so registration and stack launch key on one value
 * per boot.
 */
int mt_active_transport_is_thread(void)
{
    return mt_transport_active() == MT_TRANSPORT_THREAD;
}

#endif /* MT_HOST_TEST */
