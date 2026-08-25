/*
 * mt_transport.c - runtime transport selection for the combined WiFi+Thread
 * image.
 */

#include <stdint.h>
#include <string.h>

#include "hearth_log.h"
#include "hearth_port.h"

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

static const char *TAG = "mt_transport";

#define MT_TRANSPORT_NVS_NAMESPACE "mt_cfg"
#define MT_TRANSPORT_NVS_KEY       "transport"

mt_transport_t mt_transport_stored(void)
{
    uint8_t v = 0;
    /* Covers "not found" (factory fresh) and any error: either way there
     * is nothing usable to read, so default WIFI. */
    int r = hearth_kv_get_u8(MT_TRANSPORT_NVS_NAMESPACE, MT_TRANSPORT_NVS_KEY, &v);
    if (r != 0 || v > MT_TRANSPORT_THREAD) {
        return MT_TRANSPORT_WIFI;
    }
    return (mt_transport_t)v;
}

int mt_transport_store(mt_transport_t t)
{
    if (hearth_kv_set_u8(MT_TRANSPORT_NVS_NAMESPACE, MT_TRANSPORT_NVS_KEY, (uint8_t)t) != 0) {
        HEARTH_LOGE(TAG, "storing transport failed");
        return -1;
    }

    HEARTH_LOGI(TAG, "transport stored: %s", mt_transport_name(t));
    return 0;
}

static mt_transport_t s_active = MT_TRANSPORT_WIFI;

void mt_transport_latch_active(void)
{
    s_active = mt_transport_stored();
    HEARTH_LOGI(TAG, "active transport latched: %s", mt_transport_name(s_active));
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
