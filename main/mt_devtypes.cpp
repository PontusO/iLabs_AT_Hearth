/*
 * mt_devtypes.cpp - the device type table.
 *
 * One thunk per device type, because each esp_matter endpoint namespace has
 * its own config_t and the create() calls therefore do not share a signature.
 * Adding a device type is a thunk plus a row: mechanical, but not free.
 *
 * Verified against esp-matter release/v1.5 (v1.5.1). Namespace names differ
 * between esp-matter revisions, so check the header before adding a row: see
 * section 6.3 of the design spec. In particular arduino-esp32 3.3.8 bundles
 * esp_matter 1.4.1, whose names are NOT a reliable guide to ours.
 */

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_endpoint.h>

#include "mt_devtypes.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;

static const char *TAG = "mt_devtypes";

typedef endpoint_t *(*mt_devtype_ctor_t)(node_t *node);

typedef struct {
    uint32_t          id;
    mt_devtype_ctor_t create;
    const char       *name;
} mt_devtype_entry_t;

/*
 * StartUpOnOff must be null ("previous value"), not esp-matter's config
 * default of 0 ("always Off", esp_matter_feature.h:217). With 0, CHIP's
 * OnOffServer forces the light Off at every boot and PERSISTS the 0, so
 * the stored OnOff never survives a reboot (bug B63). The persistence
 * machinery itself was proven healthy: this default was overwriting it.
 */
static void mt_startup_on_off_null(on_off_with_lighting_config *c)
{
    c->on_off_lighting.start_up_on_off = nullable<uint8_t>();
}

static endpoint_t *mk_on_off_light(node_t *n)
{
    on_off_light::config_t c;
    mt_startup_on_off_null(&c);
    return on_off_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_dimmable_light(node_t *n)
{
    dimmable_light::config_t c;
    mt_startup_on_off_null(&c);
    return dimmable_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_color_temperature_light(node_t *n)
{
    color_temperature_light::config_t c;
    mt_startup_on_off_null(&c);
    return color_temperature_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_temperature_sensor(node_t *n)
{
    temperature_sensor::config_t c;
    return temperature_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

/* IDs come from esp_matter, never from a literal. */
static const mt_devtype_entry_t s_devtypes[] = {
    { on_off_light::get_device_type_id(),            mk_on_off_light,            "on_off_light"            },
    { dimmable_light::get_device_type_id(),          mk_dimmable_light,          "dimmable_light"          },
    { color_temperature_light::get_device_type_id(), mk_color_temperature_light, "color_temperature_light" },
    { temperature_sensor::get_device_type_id(),      mk_temperature_sensor,      "temperature_sensor"      },
};

static const size_t s_devtype_count = sizeof(s_devtypes) / sizeof(s_devtypes[0]);

static const mt_devtype_entry_t *find(uint32_t devtype_id)
{
    for (size_t i = 0; i < s_devtype_count; i++) {
        if (s_devtypes[i].id == devtype_id) {
            return &s_devtypes[i];
        }
    }
    return nullptr;
}

extern "C" bool mt_devtype_is_known(uint32_t devtype_id)
{
    return find(devtype_id) != nullptr;
}

extern "C" int mt_devtype_create(uint32_t devtype_id, uint16_t *out_ep_id)
{
    const mt_devtype_entry_t *e = find(devtype_id);
    if (!e || !out_ep_id) {
        ESP_LOGE(TAG, "unknown device type 0x%04X", (unsigned)devtype_id);
        return -1;
    }

    endpoint_t *ep = e->create(node::get());
    if (ep == nullptr) {
        ESP_LOGE(TAG, "creating %s (0x%04X) failed", e->name, (unsigned)devtype_id);
        return -1;
    }

    *out_ep_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "created %s (0x%04X) as endpoint %u", e->name, (unsigned)devtype_id, *out_ep_id);
    return 0;
}
