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
#include "mt_matter.h"

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

static endpoint_t *mk_door_lock(node_t *n, uint8_t variant)
{
    (void)variant;
    /* Plain thunk: door_lock::config_t has no feature_flags field at all
     * (esp_matter_endpoint.h:543-555, esp_matter_cluster.h:634-651), and
     * cluster::door_lock::create() runs no VALIDATE_FEATURES macro
     * (esp_matter_cluster.cpp:2054-2098): there is no fifth-trap analogue
     * here, and none is to be invented (spec F5). Feature map stays 0 by
     * design (spec F3): no PIN/USER/COTA features this round. */
    door_lock::config_t c;
    endpoint_t *ep = door_lock::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep != nullptr) {
        /* AutoRelockTime must exist even though it is optional: after a
         * successful unlock the 6-arg SetLockState insists on reading it
         * (VerifyOrReturnError(GetAutoRelockTime(...), false),
         * door-lock-server.cpp:207) and reports FALSE for an unlock that
         * actually happened, which turned every host unlock via AT+MTLOCK
         * into a bare ERROR with the state changed underneath (bug B129).
         * 0 disables auto-relock. If a controller writes it nonzero, the
         * server relocks at expiry and the host observes the LockState
         * change as a +MTATTR URC, the same path as any controller write. */
        cluster_t *cl = cluster::get(ep, chip::app::Clusters::DoorLock::Id);
        if (cl != nullptr) {
            cluster::door_lock::attribute::create_auto_relock_time(cl, 0);
        }
    }
    return ep;
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

static endpoint_t *mk_light_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    light_sensor::config_t c;
    return light_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_flow_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    flow_sensor::config_t c;
    return flow_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_air_quality_sensor(node_t *n, uint8_t variant)
{
    (void)variant;
    air_quality_sensor::config_t c;
    endpoint_t *ep = air_quality_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    /* The base AirQuality cluster only mandates Unknown/Good/Poor; Fair,
     * Moderate, VeryPoor and ExtremelyPoor are each gated behind their own
     * feature bit (esp_matter_feature.h:587-617, namespaces
     * cluster::air_quality::feature::{fair,moderate,very_poor,
     * extremely_poor}). The host library's AirQuality_t enum publishes all
     * seven SDK values (kUnknown, kGood, kFair, kModerate, kPoor,
     * kVeryPoor, kExtremelyPoor), so the endpoint must advertise all four
     * optional features or the library can report a value the fabric's
     * feature map does not admit. air_quality::config_t is common::config_t
     * (empty, esp_matter_cluster.h:66-70), so the features are added after
     * create() rather than seeded on it, the same shape as the door lock
     * thunk's AutoRelockTime.
     *
     * Fix round 1 (C1b, bug B139): which four bits get added here has to
     * match what C1b's AirQuality server Instance constructs its
     * BitMask<Feature> from (main.cpp's mt_air_quality_register_all()), or
     * the ember feature map and the Instance's FeatureMap answer (which the
     * Instance serves once it exists, ahead of ember; see main.cpp) would
     * disagree. mt_air_quality_feature_mask() (mt_matter.h) is the single
     * accessor both sites read, so a feature added or removed on one side
     * without touching the other cannot compile silently mismatched: this
     * loop and the Instance's BitMask both trace back to the same bits. The
     * bit values themselves (0x1/0x2/0x4/0x8) are the CHIP Feature enum's
     * own (AirQuality/Enums.h: kFair/kModerate/kVeryPoor/kExtremelyPoor),
     * not transcribed here since this file has no other reason to pull in
     * that CHIP header for four plain add() calls. */
    cluster_t *cl = cluster::get(ep, chip::app::Clusters::AirQuality::Id);
    if (cl != nullptr) {
        uint32_t mask = mt_air_quality_feature_mask();
        if (mask & 0x1u) {
            cluster::air_quality::feature::fair::add(cl);
        }
        if (mask & 0x2u) {
            cluster::air_quality::feature::moderate::add(cl);
        }
        if (mask & 0x4u) {
            cluster::air_quality::feature::very_poor::add(cl);
        }
        if (mask & 0x8u) {
            cluster::air_quality::feature::extremely_poor::add(cl);
        }
    }
    return ep;
}

static endpoint_t *mk_mounted_on_off_control(node_t *n, uint8_t variant)
{
    (void)variant;
    mounted_on_off_control::config_t c;
    mt_startup_on_off_null(&c);
    return mounted_on_off_control::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_mounted_dimmable_load_control(node_t *n, uint8_t variant)
{
    (void)variant;
    mounted_dimmable_load_control::config_t c;
    mt_startup_on_off_null(&c);
    mt_startup_level_null(&c);
    return mounted_dimmable_load_control::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_air_purifier(node_t *n, uint8_t variant)
{
    (void)variant;
    air_purifier::config_t c;
    return air_purifier::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_extractor_hood(node_t *n, uint8_t variant)
{
    (void)variant;
    extractor_hood::config_t c;
    return extractor_hood::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_cooktop(node_t *n, uint8_t variant)
{
    (void)variant;
    cooktop::config_t c;
    return cooktop::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_room_air_conditioner(node_t *n, uint8_t variant)
{
    (void)variant;
    room_air_conditioner::config_t c;
    /* The thermostat trap (VALIDATE_FEATURES_AT_LEAST_ONE Heat,Cool,
     * esp_matter_cluster.cpp:1445) is pre-satisfied here: esp-matter's own
     * endpoint add() unconditionally ORs feature::cooling into
     * config->thermostat.feature_flags before create(), and unconditionally
     * adds dead_front_behavior to the on_off cluster
     * (esp_matter_endpoint.cpp:1279-1287). OR in heating here too: the
     * Room Air Conditioner device type's cluster requirements and element
     * requirements tables (Matter 1.5.1 Device Library Spec section 13.3.6
     * and 13.3.8) require the Thermostat cluster (M) but do not restrict it
     * to Cool-only, and the host library's MatterRoomAirConditioner class
     * writes OccupiedHeatingSetpoint unconditionally. This |= runs before
     * create(), so esp-matter's own OR of cooling lands on top and both
     * bits survive; do not also re-add cooling here. The setpoint lives
     * under features.cooling/features.heating, not directly on config_t
     * (cluster::thermostat::config_t at esp_matter_cluster.h:397-421 nests
     * it in the heating/cooling/... features struct, unlike the brief's
     * flat sketch). Seeds match the thermostat row's upstream boot values
     * (16 C / 24 C) rather than esp-matter's own defaults (20 C / 26 C,
     * esp_matter_feature.h:484/496): left at esp-matter's defaults, the
     * host library's cache (seeded from upstream's boot values) disagrees
     * with the fabric, and a sketch's first setpoint write is silently
     * swallowed by the cache's equality check and never reaches the wire
     * (see the thermostat row's comment above, cross-layer finding I1). */
    c.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id();
    c.thermostat.features.heating.occupied_heating_setpoint = 1600;
    c.thermostat.features.cooling.occupied_cooling_setpoint = 2400;
    /* esp-matter's config_t constructor defaults system_mode to 1 (Auto,
     * esp_matter_cluster.h:414), but the library caches mode 0 (Off,
     * SystemModeEnum::kOff) at begin(); seed to match for the same
     * first-write-swallowed reason as the setpoints above. */
    c.thermostat.system_mode = 0;
    return room_air_conditioner::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_pump(node_t *n, uint8_t variant)
{
    (void)variant;
    pump::config_t c;
    /* Sixth abort trap: pump_configuration_and_control::create() runs
     * VALIDATE_FEATURES_AT_LEAST_ONE across the operation-mode features
     * (esp_matter_cluster.cpp:2675-2679). Constant speed is the
     * least-constrained mode. */
    c.pump_configuration_and_control.feature_flags =
        cluster::pump_configuration_and_control::feature::constant_speed::get_id();
    return pump::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_water_valve(node_t *n, uint8_t variant)
{
    (void)variant;
    /*
     * The delegate must exist before create(): water_valve::create() ->
     * valve_configuration_and_control::create() consumes config.delegate
     * synchronously (it stashes the pointer for esp_matter's own deferred
     * delegate-init callback, esp_matter_cluster.cpp), but the real
     * endpoint id is not assigned until create() returns it. Hand out a
     * pool slot first, fix its endpoint after (mt_matter.h). On pool
     * exhaustion, abort loudly by returning nullptr, same as any other
     * failed create() here: the boot rebuild aborts the whole composition
     * rather than create an endpoint whose command adjudication would have
     * nowhere to go.
     */
    void *delegate = mt_matter_valve_delegate_alloc();
    if (delegate == nullptr) {
        ESP_LOGE(TAG, "water valve delegate pool exhausted");
        return nullptr;
    }
    water_valve::config_t c;
    c.valve_configuration_and_control.delegate = delegate;
    endpoint_t *ep = water_valve::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    mt_matter_valve_delegate_set_endpoint(delegate, endpoint::get_id(ep));
    return ep;
}

static endpoint_t *mk_mode_select(node_t *n, uint8_t variant)
{
    (void)variant;
    /*
     * F2: one GLOBAL SupportedModesManager serves every mode_select
     * endpoint (main.cpp's HearthSupportedModesManager); this thunk only
     * points the delegate slot at it. esp_matter's ModeSelectDelegateInitCB
     * calls chip::app::Clusters::ModeSelect::setSupportedModesManager()
     * with whatever config.delegate is and discards endpoint_id entirely
     * (main.cpp's comment ahead of HearthSupportedModesManager has the full
     * citation), so running this thunk again for a second mode_select
     * endpoint re-sets the SAME global pointer: harmless, since it is
     * already this object. Opaque void*, same shape as the water valve
     * delegate pool above: this file never has to name
     * HearthSupportedModesManager or any CHIP delegate type.
     */
    mode_select::config_t c;
    c.mode_select.delegate = mt_matter_mode_select_manager();
    return mode_select::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

/*
 * F7: unlike mode select's single global manager, OperationalState needs one
 * delegate OBJECT per endpoint (SetInstance() VerifyOrDies on sharing), so
 * each of the three thunks below hands out its own slot from the ONE shared
 * pool mt_matter_opstate_delegate_alloc() manages (main.cpp), the same
 * before-create()/after-create() endpoint-fixup shape mk_water_valve() uses.
 * dish_washer::config_t, laundry_washer::config_t and laundry_dryer::config_t
 * are all literally the same type (esp_matter_endpoint.h's
 * app_with_operational_state_config, aliased three times) and their add()
 * bodies are identical apart from the device type id
 * (esp_matter_endpoint.cpp: each just calls operational_state::create() then
 * event::create_operation_completion()), so the three thunks below differ
 * only in which namespace's create() they call.
 *
 * Task C11 (bench finding F-C10-1): operational_state::create()
 * (esp_matter_cluster.cpp:1752-1756) adds attributes and events but calls no
 * command::create_*, unlike valve_configuration_and_control::create()
 * (esp_matter_cluster.cpp:3451-3453: command::create_open(cluster);
 * command::create_close(cluster);). The four command helpers exist and are
 * declared (esp_matter_command.h:295-299:
 * operational_state::command::create_pause/create_stop/create_start/
 * create_resume/create_operational_command_response) but have no call site in
 * this SDK revision, so every OperationalState endpoint's AcceptedCommandList
 * was empty and a controller's Pause/Stop/Start/Resume invoke was rejected
 * UNSUPPORTED_COMMAND before main.cpp's HearthOpStateDelegate ever saw it
 * (chip-tool: `Status=0x81` on all four, bench evidence
 * task-C10-evidence/014244-opstate-start-allow.log). Hand-add the four
 * ACCEPTED commands after create(), the same after-create() hand-add shape
 * mk_power_source() below uses for BatPercentRemaining.
 * create_operational_command_response() is NOT added: it is
 * COMMAND_FLAG_GENERATED, the response CHIP sends back on its own once the
 * accepted command exists, not something a controller invokes.
 */
static void mt_opstate_add_commands(endpoint_t *ep)
{
    cluster_t *cl = cluster::get(ep, chip::app::Clusters::OperationalState::Id);
    if (cl != nullptr) {
        cluster::operational_state::command::create_pause(cl);
        cluster::operational_state::command::create_stop(cl);
        cluster::operational_state::command::create_start(cl);
        cluster::operational_state::command::create_resume(cl);
    }
}

static endpoint_t *mk_laundry_washer(node_t *n, uint8_t variant)
{
    (void)variant;
    void *delegate = mt_matter_opstate_delegate_alloc();
    if (delegate == nullptr) {
        ESP_LOGE(TAG, "operational state delegate pool exhausted");
        return nullptr;
    }
    laundry_washer::config_t c;
    c.operational_state.delegate = delegate;
    endpoint_t *ep = laundry_washer::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    mt_opstate_add_commands(ep);
    mt_matter_opstate_delegate_set_endpoint(delegate, endpoint::get_id(ep));
    return ep;
}

static endpoint_t *mk_dish_washer(node_t *n, uint8_t variant)
{
    (void)variant;
    void *delegate = mt_matter_opstate_delegate_alloc();
    if (delegate == nullptr) {
        ESP_LOGE(TAG, "operational state delegate pool exhausted");
        return nullptr;
    }
    dish_washer::config_t c;
    c.operational_state.delegate = delegate;
    endpoint_t *ep = dish_washer::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    mt_opstate_add_commands(ep);
    mt_matter_opstate_delegate_set_endpoint(delegate, endpoint::get_id(ep));
    return ep;
}

static endpoint_t *mk_laundry_dryer(node_t *n, uint8_t variant)
{
    (void)variant;
    void *delegate = mt_matter_opstate_delegate_alloc();
    if (delegate == nullptr) {
        ESP_LOGE(TAG, "operational state delegate pool exhausted");
        return nullptr;
    }
    laundry_dryer::config_t c;
    c.operational_state.delegate = delegate;
    endpoint_t *ep = laundry_dryer::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    mt_opstate_add_commands(ep);
    mt_matter_opstate_delegate_set_endpoint(delegate, endpoint::get_id(ep));
    return ep;
}

/*
 * F4/trap seven: smoke_co_alarm::create() runs VALIDATE_FEATURES_AT_LEAST_ONE
 * on SmokeAlarm/COAlarm (esp_matter_cluster.cpp:2019). Enable both so both
 * the SmokeState and COState attributes exist: AT+MTALARM's field table
 * (mt_matter.h's mt_matter_alarm_set()) covers both without a variant to pick
 * one over the other, and the eleven-field surface is uniform across every
 * smoke_co_alarm endpoint this firmware creates.
 *
 * Task C11 (bench finding F-C10-1): smoke_co_alarm::create()
 * (esp_matter_cluster.cpp:1991-2042) creates attributes and events but calls
 * no command::create_*, the same SDK gap as OperationalState above. The
 * helper exists and is declared (esp_matter_command.h:305:
 * smoke_co_alarm::command::create_self_test_request) but has no call site, so
 * SelfTestRequest was rejected UNSUPPORTED_COMMAND and
 * emberAfPluginSmokeCoAlarmSelfTestRequestCommand() (main.cpp) never fired
 * (chip-tool: `Status=0x81`, bench evidence
 * task-C10-evidence/014411-smoke-selftest.log). Hand-add it after create(),
 * the same after-create() shape mt_opstate_add_commands() above and
 * mk_power_source() below use.
 */
static endpoint_t *mk_smoke_co_alarm(node_t *n, uint8_t variant)
{
    (void)variant;
    smoke_co_alarm::config_t c;
    c.smoke_co_alarm.feature_flags =
        cluster::smoke_co_alarm::feature::smoke_alarm::get_id() |
        cluster::smoke_co_alarm::feature::co_alarm::get_id();
    endpoint_t *ep = smoke_co_alarm::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    cluster_t *cl = cluster::get(ep, chip::app::Clusters::SmokeCoAlarm::Id);
    if (cl != nullptr) {
        cluster::smoke_co_alarm::command::create_self_test_request(cl);
    }
    return ep;
}

/*
 * F5/trap eight: power_source::create() runs VALIDATE_FEATURES_EXACT_ONE on
 * Wired/Battery (esp_matter_cluster.cpp:982). Battery matches upstream's
 * battery-powered smoke/CO alarm devices. This is a FLAT sibling endpoint,
 * not a composed one: the Smoke/CO Alarm device type's mandate for a power
 * source is node-scoped, so a standalone Power Source endpoint elsewhere on
 * the node satisfies it, no composition tree needed (design spec decision
 * log, 2026-08-07).
 *
 * BatPercentRemaining has a creator (attribute::create_bat_percent_remaining,
 * esp_matter_attribute.h:933) but no endpoint path calls it: F5 confirms the
 * standard power_source::add() (esp_matter_endpoint.cpp) never does, so the
 * thunk adds it by hand after create(), the same "creator exists, nothing
 * wires it, add it after create()" shape mk_air_quality_sensor() and
 * mk_door_lock() above use for their own hand-added attributes. Bounds 0..200
 * (0.5% steps per the Matter spec's BatPercentRemaining) and a null default
 * (no battery reading available at boot) match the one other place in this
 * SDK revision that creates this attribute, the battery_storage device type
 * (esp_matter_endpoint.cpp's battery_storage::add()).
 */
static endpoint_t *mk_power_source(node_t *n, uint8_t variant)
{
    (void)variant;
    power_source::config_t c;
    c.power_source.feature_flags = cluster::power_source::feature::battery::get_id();
    endpoint_t *ep = power_source::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    cluster_t *cl = cluster::get(ep, chip::app::Clusters::PowerSource::Id);
    if (cl != nullptr) {
        cluster::power_source::attribute::create_bat_percent_remaining(cl, nullable<uint8_t>(), 0, 200);
    }
    return ep;
}

/*
 * F3/trap: cluster::chime::create() aborts (returns NULL) when
 * config.delegate is null, a check by pointer rather than a
 * VALIDATE_FEATURES macro like the smoke/co alarm's or power source's above.
 * Same before/after delegate-pool shape as mk_water_valve()/the
 * OperationalState trio: a slot is handed out before create() (the real
 * endpoint id is not known until create() returns it), and fixed up after.
 * On pool exhaustion, abort loudly by returning nullptr, same as every other
 * failed create() in this file.
 */
static endpoint_t *mk_chime(node_t *n, uint8_t variant)
{
    (void)variant;
    void *delegate = mt_matter_chime_delegate_alloc();
    if (delegate == nullptr) {
        ESP_LOGE(TAG, "chime delegate pool exhausted");
        return nullptr;
    }
    chime::config_t c;
    c.chime.delegate = delegate;
    endpoint_t *ep = chime::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    mt_matter_chime_delegate_set_endpoint(delegate, endpoint::get_id(ep));
    return ep;
}

/*
 * HearthRvcOpStateInitCB is defined in main.cpp, next to the
 * HearthRvcOpStateDelegate class it constructs a
 * chip::app::Clusters::RvcOperationalState::Instance for. It cannot be
 * reached through mt_matter.h's usual extern "C" bridge pattern:
 * esp_matter::cluster::set_delegate_and_init_callback() (called below) takes a
 * delegate_init_callback_t, which is a C++-linkage function pointer type
 * (esp_matter_data_model.h), and an extern "C" wrapper would hand back a
 * pointer of the wrong linkage to assign to it. A plain forward declaration
 * is enough (ordinary external C++ linkage, matching signatures across
 * translation units, exactly like any other multi-file C++ program), and
 * keeps HearthRvcOpStateDelegate itself private to main.cpp.
 */
void HearthRvcOpStateInitCB(void *delegate, uint16_t endpoint_id);

/*
 * F7/RVC: rvc_operational_state::create() (esp_matter_cluster.cpp:3103-3134,
 * verified against the pinned tree) is fully app-owned - empty config_t, no
 * delegate wiring, no command::create_* call at all, unlike
 * operational_state::create() above (which at least wires config.delegate
 * through its own generic init callback). Everything has to happen here by
 * hand:
 *   - Pause (0x00) / Resume (0x03) reuse the BASE OperationalState command
 *     helpers (create_pause/create_resume take the cluster handle, so they
 *     register against whichever cluster cl actually is - RvcOperationalState
 *     here, same numeric command ids as the base cluster, verified against
 *     RvcOperationalState/CommandIds.h).
 *   - GoHome (0x80) has no SDK helper at all (no create_go_home in
 *     esp_matter_command.h), so it is added with the raw
 *     esp_matter::command::create().
 *   - create_operational_command_response() (COMMAND_FLAG_GENERATED) IS
 *     added here, unlike mt_opstate_add_commands() above: that trio gets its
 *     generated-command wiring for free from esp-matter's own
 *     OperationalStateDelegateInitCB path, this cluster gets none from
 *     anywhere else at all.
 *   - set_delegate_and_init_callback() is called directly, since no SDK
 *     RvcOperationalStateDelegateInitCB exists to reuse (verified:
 *     esp_matter_delegate_callbacks.h declares only the base
 *     OperationalStateDelegateInitCB).
 */
static void mt_rvc_opstate_add_commands(endpoint_t *ep, void *delegate)
{
    cluster_t *cl = cluster::get(ep, chip::app::Clusters::RvcOperationalState::Id);
    if (cl != nullptr) {
        cluster::operational_state::command::create_pause(cl);
        cluster::operational_state::command::create_resume(cl);
        esp_matter::command::create(cl, chip::app::Clusters::RvcOperationalState::Commands::GoHome::Id,
                                     COMMAND_FLAG_ACCEPTED, NULL);
        cluster::operational_state::command::create_operational_command_response(cl);
        esp_matter::cluster::set_delegate_and_init_callback(cl, HearthRvcOpStateInitCB, delegate);
    }
}

/*
 * F2/F7: robotic_vacuum_cleaner::config_t (esp_matter_endpoint.h:762-769)
 * carries rvc_run_mode::config_t (has a delegate field) and
 * rvc_operational_state::config_t (common::config_t, EMPTY - no delegate
 * field at all, see mt_rvc_opstate_add_commands()'s comment above).
 * robotic_vacuum_cleaner::add() (esp_matter_endpoint.cpp:1389-1400) creates
 * identify, rvc_run_mode and rvc_operational_state (plus its
 * OperationCompletion event) but NOT rvc_clean_mode: that cluster is added
 * by hand after create(), the same "creator exists, nothing wires it, add
 * it after create()" shape mk_power_source() above uses for
 * BatPercentRemaining.
 *
 * Three delegates per endpoint: two ModeBase (RvcRunMode, RvcCleanMode,
 * Task 2's pool, keyed by (ep, cluster)) plus one RVC opstate delegate
 * (this task's own pool, keyed by ep alone - see mt_matter.h). All three
 * are allocated before create()/the hand-added cluster runs, the same
 * before/after shape every delegate pool in this file uses: the real
 * endpoint id is not known until create() returns it, but each delegate
 * must already exist (config.rvc_run_mode.delegate needs a valid pointer;
 * mt_rvc_opstate_add_commands() needs one to hand to
 * set_delegate_and_init_callback()) before that point. On pool exhaustion,
 * abort loudly by returning nullptr, same as every other failed create()
 * in this file - even after robotic_vacuum_cleaner::create() has already
 * succeeded (rvc_clean_mode exhaustion case), consuming the endpoint id
 * this project's own composition rules describe (CLAUDE.md: a failed
 * create() consumes no id, but this project accepts the whole boot rebuild
 * aborting on any single failure over silently skipping an entry).
 */
static endpoint_t *mk_rvc(node_t *n, uint8_t variant)
{
    (void)variant;
    void *run_delegate = mt_matter_modebase_delegate_alloc(chip::app::Clusters::RvcRunMode::Id);
    if (run_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (rvc run mode)");
        return nullptr;
    }
    void *clean_delegate = mt_matter_modebase_delegate_alloc(chip::app::Clusters::RvcCleanMode::Id);
    if (clean_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (rvc clean mode)");
        return nullptr;
    }
    void *opstate_delegate = mt_matter_rvc_opstate_delegate_alloc();
    if (opstate_delegate == nullptr) {
        ESP_LOGE(TAG, "rvc operational state delegate pool exhausted");
        return nullptr;
    }

    robotic_vacuum_cleaner::config_t c;
    c.rvc_run_mode.delegate = run_delegate;
    /* rvc_operational_state's config_t is common::config_t: empty, no
     * delegate field to fill (see the class comment above). */
    endpoint_t *ep = robotic_vacuum_cleaner::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }

    cluster::rvc_clean_mode::config_t clean_c;
    clean_c.delegate = clean_delegate;
    if (cluster::rvc_clean_mode::create(ep, &clean_c, CLUSTER_FLAG_SERVER) == nullptr) {
        ESP_LOGE(TAG, "creating rvc_clean_mode cluster failed");
        return nullptr;
    }

    mt_rvc_opstate_add_commands(ep, opstate_delegate);

    uint16_t ep_id = endpoint::get_id(ep);
    mt_matter_modebase_delegate_set_endpoint(run_delegate, ep_id);
    mt_matter_modebase_delegate_set_endpoint(clean_delegate, ep_id);
    mt_matter_rvc_opstate_delegate_set_endpoint(opstate_delegate, ep_id);
    return ep;
}

/*
 * F2/F7 (RVC + Microwave batch, task 4): microwave_oven::config_t
 * (esp_matter_endpoint.h:871-877) nests THREE cluster configs -
 * operational_state (inherited from its app_with_operational_state_config
 * base), microwave_oven_mode and microwave_oven_control - each needing its
 * own delegate pool slot, allocated before create() runs, the same
 * before/after shape every pool in this file follows (the real endpoint id
 * is not known until create() returns it). All three MUST be non-null
 * before create() runs: MicrowaveOvenControlDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp) looks up the mode and opstate
 * delegates by cluster id and silently builds nothing at all - no Instance,
 * no log, no error - if either comes back null alongside this cluster's own
 * delegate (main.cpp's HearthMwocDelegate class comment has the full
 * citation trail).
 *
 * microwave_oven::add() (esp_matter_endpoint.cpp:1620-1632) creates the
 * PLAIN OperationalState cluster (0x0060), not a derived one the way RVC's
 * add() does above, so the washer-era hand-add applies verbatim:
 * mt_opstate_add_commands() (same call mk_laundry_washer()/mk_dish_washer()/
 * mk_laundry_dryer() above use), same landmine (operational_state::create()
 * wires attributes and events but no command::create_*, C11's finding).
 * There is deliberately no Identify cluster on this device type
 * (microwave_oven::config_t's base has no identify field at all,
 * esp_matter_endpoint.h:213-218, unlike e.g. water_valve::config_t which
 * adds one explicitly) - not an omission to fix.
 *
 * PowerAsNumber is mandatory, not a choice: microwave_oven_control::create()
 * (esp_matter_cluster.cpp:3058-3100) runs VALIDATE_FEATURES_EXACT_ONE
 * against it alone and aborts cluster creation on anything else.
 * PowerNumberLimits is deliberately NOT set alongside it - its own add()
 * (esp_matter_feature.cpp:2975-2992) only applies when PowerAsNumber's bit
 * is ABSENT from the feature map, the inverse of the cluster's own spec
 * conformance, so with PowerAsNumber mandatory here that add() can never
 * reach its "apply" branch: dead code in this pinned tree (main.cpp's
 * HearthMwocDelegate class comment has the full trace). Read through the
 * SDK's own feature-id accessor
 * (cluster::microwave_oven_control::feature::power_as_number::get_id()),
 * the same "never transcribe a CHIP enum value by hand" precedent
 * mk_smoke_co_alarm()/mk_power_source() above follow for their own feature
 * flags.
 *
 * AddMoreTime hand-add: microwave_oven_control::create() wires
 * SetCookingParameters unconditionally but never calls
 * cluster::microwave_oven_control::command::create_add_more_time()
 * (esp_matter_command.cpp:2590) - the helper exists, declared, zero callers
 * anywhere in the pinned tree, the identical "SDK ships the creator, nothing
 * calls it" shape mt_opstate_add_commands() above and mk_smoke_co_alarm()'s
 * SelfTestRequest hand-add use. Added by hand after create(), same
 * after-create() shape as every other hand-add in this file.
 */
static endpoint_t *mk_microwave_oven(node_t *n, uint8_t variant)
{
    (void)variant;
    void *opstate_delegate = mt_matter_opstate_delegate_alloc();
    if (opstate_delegate == nullptr) {
        ESP_LOGE(TAG, "operational state delegate pool exhausted (microwave)");
        return nullptr;
    }
    void *mode_delegate = mt_matter_modebase_delegate_alloc(chip::app::Clusters::MicrowaveOvenMode::Id);
    if (mode_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (microwave oven mode)");
        return nullptr;
    }
    void *mwoc_delegate = mt_matter_mwoc_delegate_alloc();
    if (mwoc_delegate == nullptr) {
        ESP_LOGE(TAG, "microwave oven control delegate pool exhausted");
        return nullptr;
    }

    microwave_oven::config_t c;
    c.operational_state.delegate      = opstate_delegate;
    c.microwave_oven_mode.delegate    = mode_delegate;
    c.microwave_oven_control.delegate = mwoc_delegate;
    c.microwave_oven_control.feature_flags = cluster::microwave_oven_control::feature::power_as_number::get_id();

    endpoint_t *ep = microwave_oven::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }

    mt_opstate_add_commands(ep);

    cluster_t *control_cl = cluster::get(ep, chip::app::Clusters::MicrowaveOvenControl::Id);
    if (control_cl != nullptr) {
        cluster::microwave_oven_control::command::create_add_more_time(control_cl);
    }

    uint16_t ep_id = endpoint::get_id(ep);
    mt_matter_opstate_delegate_set_endpoint(opstate_delegate, ep_id);
    mt_matter_modebase_delegate_set_endpoint(mode_delegate, ep_id);
    mt_matter_mwoc_delegate_set_endpoint(mwoc_delegate, ep_id);
    return ep;
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
    { door_lock::get_device_type_id(),                mk_door_lock,               "door_lock",               0 },
    { light_sensor::get_device_type_id(),             mk_light_sensor,            "light_sensor",            0 },
    { flow_sensor::get_device_type_id(),              mk_flow_sensor,             "flow_sensor",             0 },
    { air_quality_sensor::get_device_type_id(),       mk_air_quality_sensor,      "air_quality_sensor",      0 },
    { mounted_on_off_control::get_device_type_id(),   mk_mounted_on_off_control,  "mounted_on_off_control",  0 },
    { mounted_dimmable_load_control::get_device_type_id(), mk_mounted_dimmable_load_control,
      "mounted_dimmable_load_control", 0 },
    { air_purifier::get_device_type_id(),             mk_air_purifier,            "air_purifier",            0 },
    { extractor_hood::get_device_type_id(),           mk_extractor_hood,          "extractor_hood",          0 },
    { room_air_conditioner::get_device_type_id(),     mk_room_air_conditioner,    "room_air_conditioner",    0 },
    { cooktop::get_device_type_id(),                  mk_cooktop,                 "cooktop",                 0 },
    { pump::get_device_type_id(),                     mk_pump,                    "pump",                    0 },
    { water_valve::get_device_type_id(),              mk_water_valve,             "water_valve",             0 },
    { mode_select::get_device_type_id(),              mk_mode_select,             "mode_select",             0 },
    { laundry_washer::get_device_type_id(),           mk_laundry_washer,          "laundry_washer",          0 },
    { dish_washer::get_device_type_id(),              mk_dish_washer,             "dish_washer",             0 },
    { laundry_dryer::get_device_type_id(),            mk_laundry_dryer,           "laundry_dryer",           0 },
    { smoke_co_alarm::get_device_type_id(),           mk_smoke_co_alarm,          "smoke_co_alarm",          0 },
    { power_source::get_device_type_id(),             mk_power_source,            "power_source",            0 },
    { chime::get_device_type_id(),                    mk_chime,                   "chime",                   0 },
    { robotic_vacuum_cleaner::get_device_type_id(),   mk_rvc,                     "robotic_vacuum_cleaner",  0 },
    { microwave_oven::get_device_type_id(),           mk_microwave_oven,          "microwave_oven",          0 },
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
