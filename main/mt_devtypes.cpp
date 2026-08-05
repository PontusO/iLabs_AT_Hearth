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

typedef endpoint_t *(*mt_devtype_ctor_t)(node_t *node, uint8_t variant);

typedef struct {
    uint32_t          id;
    mt_devtype_ctor_t create;
    const char       *name;
    uint8_t           max_variant; /* legal variant range is 0..max_variant */
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

/*
 * StartUpCurrentLevel has the same B63 shape as StartUpOnOff: esp-matter
 * defaults it to 0 (esp_matter_feature.h:256), which tells CHIP's
 * LevelControl to force level 0 at every boot and persist it, so a stored
 * brightness never survives a reboot. Null means "previous value". Found
 * while adding the plug-in units, whose upstream class nulls both fields
 * for exactly this reason.
 *
 * Templated, not typed to a single config_t: dimmable_light::config_t and
 * extended_color_light::config_t share level_control_lighting through
 * inheritance (extended_color_light::config_t : dimmable_light::config_t),
 * but dimmable_plug_in_unit::config_t is an unrelated type that happens to
 * declare the same field independently (esp_matter_endpoint.h:400-402
 * against :288-290). A single pointer type cannot span both; the field
 * shape can.
 */
template <typename T>
static void mt_startup_level_null(T *c)
{
    c->level_control_lighting.start_up_current_level = nullable<uint8_t>();
}

static endpoint_t *mk_on_off_light(node_t *n, uint8_t variant)
{
    (void)variant;
    on_off_light::config_t c;
    mt_startup_on_off_null(&c);
    return on_off_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_dimmable_light(node_t *n, uint8_t variant)
{
    (void)variant;
    dimmable_light::config_t c;
    mt_startup_on_off_null(&c);
    mt_startup_level_null(&c);
    return dimmable_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_color_temperature_light(node_t *n, uint8_t variant)
{
    (void)variant;
    color_temperature_light::config_t c;
    mt_startup_on_off_null(&c);
    mt_startup_level_null(&c);
    return color_temperature_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_temperature_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    temperature_sensor::config_t c;
    return temperature_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_on_off_plug_in_unit(node_t *n, uint8_t variant)
{
    (void)variant;
    on_off_plug_in_unit::config_t c;
    mt_startup_on_off_null(&c);
    return on_off_plug_in_unit::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_dimmable_plug_in_unit(node_t *n, uint8_t variant)
{
    (void)variant;
    dimmable_plug_in_unit::config_t c;
    mt_startup_on_off_null(&c);
    mt_startup_level_null(&c);
    return dimmable_plug_in_unit::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_contact_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    contact_sensor::config_t c;
    return contact_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_humidity_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    humidity_sensor::config_t c;
    return humidity_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_pressure_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    pressure_sensor::config_t c;
    return pressure_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_rain_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    rain_sensor::config_t c;
    return rain_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_water_freeze_detector(node_t *n, uint8_t variant)
{
    (void)variant;
    water_freeze_detector::config_t c;
    return water_freeze_detector::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_water_leak_detector(node_t *n, uint8_t variant)
{
    (void)variant;
    water_leak_detector::config_t c;
    return water_leak_detector::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_fan(node_t *n, uint8_t variant)
{
    (void)variant;
    fan::config_t c;
    return fan::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_occupancy_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    occupancy_sensor::config_t c;
    /* Default feature_flags of 0 ASSERTS in cluster create
     * (VALIDATE_FEATURES_AT_LEAST_ONE, esp_matter_cluster.cpp:2341-2346):
     * setting a feature is load-bearing for boot safety, not optional.
     * PIR is the sensor-type default upstream presents. 1.5.1 calls the
     * field feature_flags (1.4.1 called it features, cluster::occupancy_sensing
     * ::config_t at esp_matter_cluster.h:735-746). */
    c.occupancy_sensing.feature_flags =
        cluster::occupancy_sensing::feature::passive_infrared::get_id();
    return occupancy_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_window_covering(node_t *n, uint8_t variant)
{
    (void)variant;
    window_covering::config_t c;
    /* Default feature_flags of 0 ASSERTS in cluster create
     * (VALIDATE_FEATURES_AT_LEAST_ONE, esp_matter_cluster.cpp:2137): at least
     * one of Lift/Tilt is mandatory. Enable both, position-aware, which is
     * the surface the host library's percent100ths API drives. The
     * position_aware_lift/tilt sub-configs (config->features, cluster
     * ::window_covering::config_t at esp_matter_cluster.h:656-668) default
     * to null current/target values, which is correct at boot. */
    c.window_covering.feature_flags =
        cluster::window_covering::feature::lift::get_id() |
        cluster::window_covering::feature::tilt::get_id() |
        cluster::window_covering::feature::position_aware_lift::get_id() |
        cluster::window_covering::feature::position_aware_tilt::get_id();
    return window_covering::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_thermostat(node_t *n, uint8_t variant)
{
    (void)variant;
    thermostat::config_t c;
    /* Default feature_flags of 0 ASSERTS in cluster create
     * (VALIDATE_FEATURES_AT_LEAST_ONE, esp_matter_cluster.cpp:1445): at least
     * one of Heat/Cool is mandatory. Enable both. */
    c.thermostat.feature_flags =
        cluster::thermostat::feature::heating::get_id() |
        cluster::thermostat::feature::cooling::get_id();
    /* esp-matter's setpoint feature configs default to 2000/2600 hundredths
     * (20 C / 26 C, esp_matter_feature.h:484/496), but upstream arduino-esp32
     * devices boot at 1600/2400 (16 C / 24 C), and the host library's cache
     * seeds assume those upstream values. Left at esp-matter's defaults, a
     * fresh device and the library disagree on the starting setpoints: a
     * sketch's first setCoolingSetpoint(24.0) matches the library's cache
     * guess of 2400, is silently swallowed by its equality check, and never
     * reaches the wire, while the fabric still holds esp-matter's 2600. Seed
     * to upstream's boot values here so the first write is never a no-op.
     * Cross-layer finding I1, host library final review. */
    c.thermostat.features.heating.occupied_heating_setpoint = 1600;
    c.thermostat.features.cooling.occupied_cooling_setpoint = 2400;
    return thermostat::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_extended_color_light(node_t *n, uint8_t variant)
{
    (void)variant;
    extended_color_light::config_t c;
    mt_startup_on_off_null(&c);
    mt_startup_level_null(&c);
    endpoint_t *ep = extended_color_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    /* The standard namespace enables color-temp and XY only. The host
     * library mirrors arduino-esp32's HSV-driven class, so CurrentHue and
     * CurrentSaturation must exist: bolt the HueSaturation feature onto the
     * color control cluster (plain u8 attributes, fully inside the generic
     * AT+MTATTR surface). color_control::create() does not gate this
     * against XY/ColorTemperature (no EXACT_ONE validation in
     * esp_matter_cluster.cpp's color_control::create), so adding it after
     * the standard create() is safe. */
    cluster_t *cc = cluster::get(ep, chip::app::Clusters::ColorControl::Id);
    if (cc != nullptr) {
        cluster::color_control::feature::hue_saturation::config_t hs;
        cluster::color_control::feature::hue_saturation::add(cc, &hs);
    }
    return ep;
}

static endpoint_t *mk_generic_switch(node_t *n, uint8_t variant)
{
    (void)variant;
    generic_switch::config_t c;
    /* Exactly one of Latching/Momentary is mandatory: a default
     * feature_flags of 0 ASSERTS in cluster create
     * (VALIDATE_FEATURES_EXACT_ONE, esp_matter_cluster.cpp:2192-2194).
     * Momentary matches the upstream arduino-esp32 class, and the feature
     * add also registers the InitialPress event metadata that
     * mt_matter_switch_click() emits. */
    c.switch_cluster.feature_flags =
        cluster::switch_cluster::feature::momentary_switch::get_id();
    return generic_switch::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_temperature_controlled_cabinet(node_t *n, uint8_t variant)
{
    temperature_controlled_cabinet::config_t c;
    /* Fifth abort trap: temperature_control::create() runs
     * VALIDATE_FEATURES_EXACT_ONE on TemperatureNumber/TemperatureLevel
     * (esp_matter_cluster.cpp:2849). Exactly one bit, chosen by the
     * composition variant: 0 = number, 1 = level. */
    if (variant == 1) {
        c.temperature_control.feature_flags =
            cluster::temperature_control::feature::temperature_level::get_id();
    } else {
        /* Step (attr 0x0003) is NOT a TemperatureNumber attribute: it rides
         * the separate TemperatureStep feature, added only when its flag is
         * set alongside TN (esp_matter_cluster.cpp:2852-2857). The host
         * library's begin() writes Step, so TN without TS answers +MTERR:4
         * to every upstream sketch (found on hardware, bug B119). */
        c.temperature_control.feature_flags =
            cluster::temperature_control::feature::temperature_number::get_id() |
            cluster::temperature_control::feature::temperature_step::get_id();
    }
    return temperature_controlled_cabinet::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

/* IDs come from esp_matter, never from a literal. */
static const mt_devtype_entry_t s_devtypes[] = {
    { on_off_light::get_device_type_id(),            mk_on_off_light,            "on_off_light",            0 },
    { dimmable_light::get_device_type_id(),          mk_dimmable_light,          "dimmable_light",          0 },
    { color_temperature_light::get_device_type_id(), mk_color_temperature_light, "color_temperature_light", 0 },
    { temperature_sensor::get_device_type_id(),      mk_temperature_sensor,      "temperature_sensor",      0 },
    { on_off_plug_in_unit::get_device_type_id(),     mk_on_off_plug_in_unit,     "on_off_plug_in_unit",     0 },
    { dimmable_plug_in_unit::get_device_type_id(),   mk_dimmable_plug_in_unit,   "dimmable_plug_in_unit",   0 },
    { contact_sensor::get_device_type_id(),          mk_contact_sensor,          "contact_sensor",          0 },
    { occupancy_sensor::get_device_type_id(),        mk_occupancy_sensor,        "occupancy_sensor",        0 },
    { humidity_sensor::get_device_type_id(),         mk_humidity_sensor,         "humidity_sensor",         0 },
    { pressure_sensor::get_device_type_id(),         mk_pressure_sensor,         "pressure_sensor",         0 },
    { rain_sensor::get_device_type_id(),             mk_rain_sensor,             "rain_sensor",             0 },
    { water_freeze_detector::get_device_type_id(),   mk_water_freeze_detector,   "water_freeze_detector",   0 },
    { water_leak_detector::get_device_type_id(),     mk_water_leak_detector,     "water_leak_detector",     0 },
    { fan::get_device_type_id(),                     mk_fan,                     "fan",                     0 },
    { window_covering::get_device_type_id(),         mk_window_covering,         "window_covering",         0 },
    { thermostat::get_device_type_id(),              mk_thermostat,              "thermostat",              0 },
    { extended_color_light::get_device_type_id(),    mk_extended_color_light,    "extended_color_light",    0 },
    { generic_switch::get_device_type_id(),          mk_generic_switch,          "generic_switch",          0 },
    { temperature_controlled_cabinet::get_device_type_id(), mk_temperature_controlled_cabinet,
      "temperature_controlled_cabinet", 1 },
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

extern "C" bool mt_devtype_variant_ok(uint32_t devtype_id, uint8_t variant)
{
    const mt_devtype_entry_t *e = find(devtype_id);
    if (!e) {
        return false;
    }
    return variant <= e->max_variant;
}

extern "C" int mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint16_t *out_ep_id)
{
    const mt_devtype_entry_t *e = find(devtype_id);
    if (!e || !out_ep_id) {
        ESP_LOGE(TAG, "unknown device type 0x%04X", (unsigned)devtype_id);
        return -1;
    }

    endpoint_t *ep = e->create(node::get(), variant);
    if (ep == nullptr) {
        ESP_LOGE(TAG, "creating %s (0x%04X) variant %u failed", e->name, (unsigned)devtype_id,
                 (unsigned)variant);
        return -1;
    }

    *out_ep_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "created %s (0x%04X) variant %u as endpoint %u", e->name, (unsigned)devtype_id,
             (unsigned)variant, *out_ep_id);
    return 0;
}
