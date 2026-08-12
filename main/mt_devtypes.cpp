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
/* For the SDK's own WaterHeaterManagementDelegateInitCB
 * (esp_matter::cluster::delegate_cb), which mk_water_heater() attaches
 * post-create because the WHM pool alloc needs the real endpoint id. */
#include <esp_matter_delegate_callbacks.h>

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
 * Its argument is the cluster the slot will serve (composed appliance round,
 * task 4: the pool now also serves the oven cavity's
 * OvenCavityOperationalState, and the delegate's command forwards carry
 * whichever cluster id it was handed), so everything below passes the base
 * OperationalState::Id.
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
    void *delegate = mt_matter_opstate_delegate_alloc(chip::app::Clusters::OperationalState::Id);
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
    void *delegate = mt_matter_opstate_delegate_alloc(chip::app::Clusters::OperationalState::Id);
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
    void *delegate = mt_matter_opstate_delegate_alloc(chip::app::Clusters::OperationalState::Id);
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
    void *opstate_delegate = mt_matter_opstate_delegate_alloc(chip::app::Clusters::OperationalState::Id);
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

/*
 * Composed appliance round, task 3: Refrigerator (0x0070).
 * refrigerator::config_t (esp_matter_endpoint.h:740-749) carries only a
 * descriptor config, no identify field at all (unlike app_base_config-derived
 * types): refrigerator::add() (esp_matter_endpoint.cpp:1319-1343) calls
 * add_device_type() and nothing else, so identify is added by hand after
 * create(), the same after-create() shape mk_power_source() and
 * mk_air_quality_sensor() above use for their own hand-added clusters.
 * cluster::identify::create()'s signature (endpoint_t*, config_t*, uint8_t
 * flags) is the one every other manual cluster::xxx::create() call in this
 * file already uses (e.g. mk_extended_color_light()'s color_control feature
 * add above), the same shape the SDK's own laundry_washer::add() body uses
 * to attach a cluster onto an endpoint that common::create() already made.
 *
 * 8.6 check (design spec, run against esp_matter_cluster.cpp before writing
 * this thunk): unlike the OperationalState trio and SmokeCoAlarm, THIS PAIR
 * IS CLEAN, no hand-added commands needed.
 *   - refrigerator_and_tcc_mode::create() (esp_matter_cluster.cpp:2903-2941)
 *     unconditionally calls mode_base::command::create_change_to_mode() and
 *     create_change_to_mode_response() after the CLUSTER_FLAG_SERVER block,
 *     the identical shape rvc_run_mode::create() (:2943-2980) uses, not the
 *     operational_state/smoke_co_alarm disease. ChangeToMode is wired for
 *     free.
 *   - refrigerator_alarm::create() (esp_matter_cluster.cpp:2870-2898) has no
 *     "Commands" section at all, matching RefrigeratorAlarm.xml (1.5.1): its
 *     one command, ModifyEnabledAlarms (0x01), carries <disallowConform/>,
 *     so there is nothing to wire.
 */
static endpoint_t *mk_refrigerator(node_t *n, uint8_t variant)
{
    (void)variant;
    void *mode_delegate = mt_matter_modebase_delegate_alloc(
        chip::app::Clusters::RefrigeratorAndTemperatureControlledCabinetMode::Id);
    if (mode_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (refrigerator mode)");
        return nullptr; /* pool exhausted; fail clean, the washer precedent */
    }
    refrigerator::config_t c;
    endpoint_t *ep = refrigerator::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    /* identify: optional conformance, but every controller UI expects it;
     * refrigerator::config_t has no identify field of its own (see above),
     * so it is added by hand, same shape as every other hand-added cluster
     * in this file. */
    cluster::identify::config_t ic;
    if (cluster::identify::create(ep, &ic, CLUSTER_FLAG_SERVER) == nullptr) {
        return nullptr;
    }
    cluster::refrigerator_and_tcc_mode::config_t mc;
    mc.delegate = mode_delegate;
    if (cluster::refrigerator_and_tcc_mode::create(ep, &mc, CLUSTER_FLAG_SERVER) == nullptr) {
        return nullptr;
    }
    cluster::refrigerator_alarm::config_t ac; /* mask=1 state=0 supported=1: door open */
    if (cluster::refrigerator_alarm::create(ep, &ac, CLUSTER_FLAG_SERVER) == nullptr) {
        return nullptr;
    }
    mt_matter_modebase_delegate_set_endpoint(mode_delegate, endpoint::get_id(ep));
    return ep;
}

/*
 * Composed appliance round, task 4: Oven (0x007B).
 * oven::config_t (esp_matter_endpoint.h:751-760) is the refrigerator's shape
 * exactly - a descriptor config and nothing else - and oven::add()
 * (esp_matter_endpoint.cpp:1345-1370) calls add_device_type() and nothing
 * else, so identify is hand-added after create() the same way
 * mk_refrigerator() above does it, for the same reason.
 *
 * The parent endpoint is deliberately BARE beyond that. The Oven device type
 * (Matter 1.5.1 Device Library) puts all of its function in its
 * Temperature Controlled Cabinet children: the oven itself carries no mode,
 * no operational state, no temperature control. Anything a host wants to
 * drive lives on a cabinet composed under this endpoint
 * (AT+MTEP's <parent_idx>, task 2), which is where
 * mt_cabinet_add_heater() below attaches the two hand-rolled clusters.
 */
static endpoint_t *mk_oven(node_t *n, uint8_t variant)
{
    (void)variant;
    oven::config_t c;
    endpoint_t *ep = oven::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    cluster::identify::config_t ic;
    if (cluster::identify::create(ep, &ic, CLUSTER_FLAG_SERVER) == nullptr) {
        return nullptr;
    }
    return ep;
}

/*
 * ---- Parent-conditional cabinet clusters (composed appliance round, task 4)
 *
 * A Temperature Controlled Cabinet (0x0071) is one device type with two
 * conformances: under a Refrigerator it is a Cooler and carries
 * RefrigeratorAndTemperatureControlledCabinetMode; under an Oven it is a
 * Heater and carries OvenMode plus OvenCavityOperationalState. Neither set is
 * encoded in the composition blob: the cabinet's row records only its device
 * type, variant and parent index, and the cluster set is DERIVED here from
 * the parent's device type at rebuild time. That is what keeps the stored
 * composition a product definition rather than a cluster list, and it is why
 * mt_devtype_parent_ok() (below) refuses a cabinet under anything else: a
 * third parent would be a cabinet with no defined conditional cluster set.
 *
 * Both functions return false on ANY failure, and mt_devtype_create() turns
 * that into -1, which aborts the whole boot rebuild (this file's own
 * failed-create() rule, CLAUDE.md). Skipping a failed augment would hand a
 * commissioned controller a cabinet endpoint that looks right in the
 * PartsList and answers nothing.
 */
static bool mt_cabinet_add_cooler(endpoint_t *ep)
{
    /* 8.6 check, run against esp_matter_cluster.cpp before writing this:
     * refrigerator_and_tcc_mode::create() (:2903-2941) unconditionally calls
     * mode_base::command::create_change_to_mode() and
     * create_change_to_mode_response() after its CLUSTER_FLAG_SERVER block,
     * and wires its own RefrigeratorAndTCCModeDelegateInitCB from
     * config.delegate. Nothing to hand-add: the same clean result task 3's
     * mk_refrigerator() got for this cluster on the parent endpoint. */
    void *mode_delegate = mt_matter_modebase_delegate_alloc(
        chip::app::Clusters::RefrigeratorAndTemperatureControlledCabinetMode::Id);
    if (mode_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (cooler cabinet mode)");
        return false;
    }
    /* Unlike the node-level thunks, the endpoint already exists here, so its
     * id is known before any cluster is created: fix the delegate's endpoint
     * immediately rather than running the usual alloc-then-create-then-fix
     * dance, which exists only because create() is what assigns the id. */
    mt_matter_modebase_delegate_set_endpoint(mode_delegate, endpoint::get_id(ep));

    cluster::refrigerator_and_tcc_mode::config_t mc;
    mc.delegate = mode_delegate;
    if (cluster::refrigerator_and_tcc_mode::create(ep, &mc, CLUSTER_FLAG_SERVER) == nullptr) {
        ESP_LOGE(TAG, "creating refrigerator_and_tcc_mode cluster failed");
        return false;
    }
    return true;
}

/*
 * HearthOvenModeInitCB and HearthOvenCavityOpStateInitCB are defined in
 * main.cpp, next to the delegate pools they construct Instances against.
 * Plain forward declarations rather than mt_matter.h's extern "C" bridge
 * pattern, for the delegate_init_callback_t linkage reason
 * HearthRvcOpStateInitCB's own declaration above explains in full.
 */
void HearthOvenModeInitCB(void *delegate, uint16_t endpoint_id);
void HearthOvenCavityOpStateInitCB(void *delegate, uint16_t endpoint_id);

/*
 * The heater cabinet's two clusters are the 8.6 disease at its most severe
 * grade yet (ARCHITECTURE.md 8.9). For the RVC's opstate the SDK at least
 * had a cluster::rvc_operational_state namespace whose create() built the
 * attribute set and left only the commands and the delegate to hand-add.
 * Here esp-matter ships NOTHING: there is no cluster::oven_mode and no
 * cluster::oven_cavity_operational_state namespace anywhere in the component
 * (verified by grep over the whole of components/esp_matter; the only oven
 * symbols it carries at all are the endpoint::oven namespace and two empty
 * MatterOven*PluginServerInitCallback stubs in
 * data_model_provider/esp_matter_plugin_server_init_callbacks.cpp). Both
 * cluster shells below are therefore mirrored line for line from the nearest
 * SDK body, read side by side while writing rather than from memory:
 *
 *   - OvenMode mirrors rvc_run_mode::create() (esp_matter_cluster.cpp:
 *     2943-2980), which is byte-identical to refrigerator_and_tcc_mode::
 *     create() (:2903-2941) apart from the cluster id and the init callback.
 *   - OvenCavityOperationalState mirrors rvc_operational_state::create()
 *     (:3103-3134), the derived-opstate shape, NOT the base
 *     operational_state::create() (:1752-1789): the base body additionally
 *     wires config.delegate through OperationalStateDelegateInitCB and
 *     creates the OperationalError event, neither of which applies to a
 *     hand-rolled derived cluster with its own init callback.
 *
 * Three things in those bodies are deliberately not reproduced, each inert
 * here: the "if (flags & CLUSTER_FLAG_SERVER)" wrapper (this firmware only
 * ever creates server clusters, so the branch is always taken), the
 * "if (config)" guard around create_current_mode() (there is no config_t to
 * be null), and rvc_operational_state::create()'s
 * "if (flags & CLUSTER_FLAG_CLIENT) create_default_binding_cluster()" tail
 * (rvc_run_mode::create() has no such tail at all). Everything else,
 * including the add_function_list(cluster, NULL, CLUSTER_FLAG_NONE) call
 * that looks like a no-op, is mirrored verbatim rather than judged.
 *
 * Cluster revisions come from the XMLs, never transcribed from a sibling
 * cluster: Mode_Oven.xml (1.5.1) is revision 2, OperationalState_Oven.xml
 * (1.5.1) is revision 2. Both differ from the values esp-matter's own
 * esp_matter_cluster_revisions.h carries for the clusters being mirrored
 * (rvc_run_mode 4, rvc_operational_state 3), which is exactly why the
 * revision cannot be copied along with the rest of the body.
 *
 * Commands. OvenMode gets ChangeToMode/ChangeToModeResponse through the
 * cluster-agnostic mode_base::command helpers (they take the cluster handle
 * and register against whatever cluster it is, so no OvenMode-specific
 * helper is needed; OvenMode's generated CommandIds.h declares the same
 * 0x00/0x01 pair). OvenCavityOperationalState gets Stop (0x01), Start (0x02)
 * and OperationalCommandResponse (0x04) ONLY: OperationalState_Oven.xml
 * revision 2 marks Pause (0x00) and Resume (0x03) <disallowConform/>
 * ("Set Pause and Resume commands as disallowed"), and the generated
 * CommandIds.h agrees (kAcceptedCommandsCount = 2, with no Pause or Resume
 * namespace in the file at all). The three helpers used are again
 * cluster-agnostic and again carry the right numeric ids
 * (operational_state::command::create_stop/create_start use
 * OperationalState::Commands::Stop::Id/Start::Id, 0x01/0x02, matching the
 * oven cluster's own; esp_matter_command.cpp:2205-2224), so no raw
 * esp_matter::command::create() is needed here the way GoHome needed one.
 * The delegate's Pause/Resume paths answer the base-class unsupported route
 * anyway (main.cpp), so a data model that somehow admitted the command still
 * would not forward it to the host.
 */
static bool mt_cabinet_add_heater(endpoint_t *ep)
{
    uint16_t ep_id = endpoint::get_id(ep);

    void *mode_delegate = mt_matter_modebase_delegate_alloc(chip::app::Clusters::OvenMode::Id);
    if (mode_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (oven mode)");
        return false;
    }
    void *opstate_delegate =
        mt_matter_opstate_delegate_alloc(chip::app::Clusters::OvenCavityOperationalState::Id);
    if (opstate_delegate == nullptr) {
        ESP_LOGE(TAG, "operational state delegate pool exhausted (oven cavity)");
        return false;
    }
    /* Endpoint already exists, so both delegates can be fixed up before
     * anything else runs: see mt_cabinet_add_cooler()'s note above. */
    mt_matter_modebase_delegate_set_endpoint(mode_delegate, ep_id);
    mt_matter_opstate_delegate_set_endpoint(opstate_delegate, ep_id);

    /* OvenMode (0x0049), mirroring rvc_run_mode::create(). */
    cluster_t *mode_cl = esp_matter::cluster::create(ep, chip::app::Clusters::OvenMode::Id, CLUSTER_FLAG_SERVER);
    if (mode_cl == nullptr) {
        ESP_LOGE(TAG, "creating OvenMode cluster failed");
        return false;
    }
    esp_matter::cluster::add_function_list(mode_cl, NULL, CLUSTER_FLAG_NONE);
    cluster::global::attribute::create_feature_map(mode_cl, 0);
    cluster::mode_base::attribute::create_supported_modes(mode_cl, NULL, 0, 0);
    cluster::global::attribute::create_cluster_revision(mode_cl, 2); /* Mode_Oven.xml 1.5.1 */
    cluster::mode_base::attribute::create_current_mode(mode_cl, 0);
    cluster::mode_base::command::create_change_to_mode(mode_cl);
    cluster::mode_base::command::create_change_to_mode_response(mode_cl);
    esp_matter::cluster::set_delegate_and_init_callback(mode_cl, HearthOvenModeInitCB, mode_delegate);

    /* OvenCavityOperationalState (0x0048), mirroring
     * rvc_operational_state::create(). */
    cluster_t *ops_cl = esp_matter::cluster::create(ep, chip::app::Clusters::OvenCavityOperationalState::Id,
                                                     CLUSTER_FLAG_SERVER);
    if (ops_cl == nullptr) {
        ESP_LOGE(TAG, "creating OvenCavityOperationalState cluster failed");
        return false;
    }
    esp_matter::cluster::add_function_list(ops_cl, NULL, CLUSTER_FLAG_NONE);
    cluster::global::attribute::create_feature_map(ops_cl, 0);
    cluster::operational_state::attribute::create_phase_list(ops_cl, NULL, 0, 0);
    cluster::operational_state::attribute::create_current_phase(ops_cl, 0);
    cluster::operational_state::attribute::create_operational_state_list(ops_cl, NULL, 0, 0);
    cluster::operational_state::attribute::create_operational_state(ops_cl, 0);
    cluster::operational_state::attribute::create_operational_error(ops_cl, NULL, 0, 0);
    cluster::global::attribute::create_cluster_revision(ops_cl, 2); /* OperationalState_Oven.xml 1.5.1 */
    cluster::operational_state::command::create_stop(ops_cl);
    cluster::operational_state::command::create_start(ops_cl);
    cluster::operational_state::command::create_operational_command_response(ops_cl);
    esp_matter::cluster::set_delegate_and_init_callback(ops_cl, HearthOvenCavityOpStateInitCB, opstate_delegate);

    return true;
}

/*
 * Composed appliance round, task 5: Cook Surface (0x0077).
 * cook_surface::config_t (esp_matter_endpoint.h:832-841) carries a descriptor
 * and a temperature_control config, which LOOKS like the cabinet's variant
 * scheme (mk_temperature_controlled_cabinet() above) but is not: unlike
 * temperature_controlled_cabinet::add(), which passes the caller's
 * temperature_control config through untouched, cook_surface::add()
 * (esp_matter_endpoint.cpp:1540) OVERWRITES
 * config->temperature_control.feature_flags with TemperatureLevel before
 * creating the cluster. Pre-set flags cannot survive an overwrite (the room
 * air conditioner's |= trick works only because that add() ORs), so variant
 * 0 shipped as a TemperatureLevel surface: on hardware, 86/0 and 86/3
 * answered +MTERR:4 while 86/4 read fine and AT+MTTEMPLEVELS was accepted
 * (bug B216, bench 2026-08-10; evidence in .superpowers/sdd/
 * 2026-08-10-composed-appliance-round/task-11-evidence/). The thunk
 * therefore does not trust cook_surface::add() for the temperature cluster:
 * for variant 0 it destroys the forced TemperatureLevel cluster after
 * create() and rebuilds temperature_control with TemperatureNumber +
 * TemperatureStep (the B119 pairing: Step 0x0003 rides its own feature, TN
 * alone answers +MTERR:4 to the host library's Step write). Destroy-after-
 * create is the shape the dormant-diagnostics scrub in main.cpp already
 * uses, and this all runs before esp_matter::start(), same as the whole
 * rebuild, so there is no CHIP event loop to race. Variant 1 wants
 * TemperatureLevel, which is exactly what the overwrite produces, so it
 * stands as created. Both paths satisfy VALIDATE_FEATURES_EXACT_ONE on
 * TN/TL (esp_matter_cluster.cpp:2849).
 *
 * OnOff rides on top with the OffOnly feature: a controller may switch a
 * surface off, never on (CookSurface.xml 1.5.1: OffOnly mandatoryConform
 * inside the optional OnOff cluster requirement).
 *
 * 8.6 check, run against the pinned tree before writing this: CLEAN, no
 * hand-fix needed. cluster::on_off::create() (esp_matter_cluster.cpp:
 * 1233-1260) adds ONLY command::create_off(); On and Toggle are added per
 * device type by esp_matter_endpoint.cpp (on_off_light's add() at :237-238
 * and its siblings), never by the cluster create, so a direct cluster-level
 * create yields an AcceptedCommandList of exactly {Off}, which is precisely
 * the OffOnly command set. feature::off_only::add() (esp_matter_feature.cpp:
 * 508-523) sets the feature-map bit and nothing else.
 *
 * v1.5.1's on_off::create() takes no feature parameter (endpoint_t*,
 * config_t*, uint8_t flags; esp_matter_cluster.h:334) and its config_t has
 * no feature_flags field either (just bool on_off, :326-332), so the feature
 * is added after create(), the same after-create() shape every other
 * hand-add in this file uses.
 */
static endpoint_t *mk_cook_surface(node_t *n, uint8_t variant)
{
    cook_surface::config_t c;
    /* Dead under the pinned SDK (B216 overwrite, see above), but kept set:
     * if an SDK bump makes add() pass the config through like the cabinet's,
     * the variant-0 scrub below degrades to an idempotent rebuild instead of
     * the empty flags tripping VALIDATE_FEATURES_EXACT_ONE. */
    if (variant == 1) {
        c.temperature_control.feature_flags =
            cluster::temperature_control::feature::temperature_level::get_id();
    } else {
        c.temperature_control.feature_flags =
            cluster::temperature_control::feature::temperature_number::get_id() |
            cluster::temperature_control::feature::temperature_step::get_id();
    }
    endpoint_t *ep = cook_surface::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    if (variant != 1) {
        /* B216 scrub: remove the TemperatureLevel cluster the SDK forced and
         * rebuild it with the features the spec's variant 0 asks for. */
        cluster_t *tc = cluster::get(ep, chip::app::Clusters::TemperatureControl::Id);
        if (tc == nullptr) {
            return nullptr;
        }
        if (cluster::destroy(tc) != ESP_OK) {
            return nullptr;
        }
        cluster::temperature_control::config_t tcfg;
        tcfg.feature_flags =
            cluster::temperature_control::feature::temperature_number::get_id() |
            cluster::temperature_control::feature::temperature_step::get_id();
        if (cluster::temperature_control::create(ep, &tcfg, CLUSTER_FLAG_SERVER) == nullptr) {
            return nullptr;
        }
    }
    /* OnOff, OffOnly feature: a controller may switch a surface off, never
     * on (CookSurface.xml: OffOnly mandatoryConform inside optional OnOff) */
    cluster::on_off::config_t oc;
    cluster_t *oncl = cluster::on_off::create(ep, &oc, CLUSTER_FLAG_SERVER);
    if (oncl == nullptr) {
        return nullptr;
    }
    if (cluster::on_off::feature::off_only::add(oncl) != ESP_OK) {
        return nullptr;
    }
    return ep;
}

/*
 * HearthEemInitCB is defined in main.cpp, next to the EEM accuracy table and
 * the one-time wildcard AttributeAccess registration it performs. Plain
 * forward declaration rather than mt_matter.h's extern "C" bridge pattern,
 * for the delegate_init_callback_t linkage reason HearthRvcOpStateInitCB's
 * own declaration above explains in full.
 */
void HearthEemInitCB(void *delegate, uint16_t endpoint_id);

/*
 * ---- Electrical measurement types (energy round A, task 4) --------------
 *
 * Two flat types on the Task 2 measurement foundation: Electrical Sensor
 * (0x0510, PowerTopology + ElectricalPowerMeasurement, plus
 * ElectricalEnergyMeasurement at variant 0) and Electrical Meter (0x0514,
 * the same minus PowerTopology). Variant 0 = power + energy, variant 1 =
 * power only (the current-clamp case). Neither device type XML requires
 * Identify (1.5.1 ElectricalSensor.xml / ElectricalMeter.xml list only the
 * measurement clusters and, for the sensor, PowerTopology), so unlike the
 * refrigerator/oven there is no hand-added identify here.
 *
 * EPM feature and optional-attribute choices are BOUND to Task 2's
 * HearthEpmDelegate (main.cpp): the SDK's own init callback
 * (ElectricalPowerMeasurementDelegateInitCB,
 * esp_matter_delegate_callbacks.cpp:418-428) constructs the Instance from
 * the endpoint's real FeatureMap attribute plus whichever optional
 * attributes actually exist on the endpoint
 * (get_electrical_power_measurement_enabled_optional_attributes, :116-174,
 * is_attribute_enabled per attribute), so what this file creates IS the
 * served surface. The cluster's create() body
 * (esp_matter_cluster.cpp:3220-3275) makes only PowerMode,
 * NumberOfMeasurementTypes, Accuracy and ActivePower; the other six
 * field-table attributes the delegate stores (mt_matter.h's MT_MEAS_F_*
 * table) are hand-added below, null until the host first pushes them.
 * AlternatingCurrent satisfies VALIDATE_FEATURES_AT_LEAST_ONE (DC/AC,
 * esp_matter_cluster.cpp:3251) and admits the AC-conformant fields
 * (Frequency, PowerFactor, RMSVoltage, RMSCurrent).
 *
 * The EEM cluster must carry exactly Imported + Exported + Cumulative:
 * HearthEemInitCB's one-time wildcard AttrAccess registration serves that
 * fixed feature set for EVERY EEM endpoint's FeatureMap read (Task 2's
 * binding contract, main.cpp), so a different mask here would make
 * FeatureMap lie. The three bits also satisfy both of the create() body's
 * VALIDATE_FEATURES_AT_LEAST_ONE checks (Imported/Exported and
 * Cumulative/Periodic, esp_matter_cluster.cpp:3301-3304).
 *
 * 8.6 check, EVENT metadata flavour (run against the pinned tree before
 * writing these thunks): CLEAN, no hand-add needed. This round's specific
 * fear was the disease's event variant (attributes present, event metadata
 * never registered, invisible until a controller subscribes), but
 * electrical_energy_measurement::create() reaches
 * feature::cumulative_energy::add() whenever the CumulativeEnergy bit is
 * set (esp_matter_cluster.cpp:3312-3314), and that add() ends with
 * event::create_cumulative_energy_measured(cluster)
 * (esp_matter_feature.cpp:2486, wrapping esp_matter::event::create() with
 * the generated event id, esp_matter_event.cpp:724-727). The SDK gap in
 * this cluster is elsewhere: its AttributeAccess is declared but never
 * registered, which is Task 2's finding and HearthEemInitCB's job
 * (ARCHITECTURE.md 8.10); the thunks below only hand that callback to
 * set_delegate_and_init_callback() themselves (null delegate by contract,
 * the HearthOvenModeInitCB precedent), since cluster create() wires no
 * init callback for this cluster at all.
 */
static bool mt_epm_add_field_attributes(endpoint_t *ep)
{
    cluster_t *cl = cluster::get(ep, chip::app::Clusters::ElectricalPowerMeasurement::Id);
    if (cl == nullptr) {
        ESP_LOGE(TAG, "ElectricalPowerMeasurement cluster missing after create");
        return false;
    }
    /* Null defaults: the AT_MT_SPEC.md 3.25 contract is that every field
     * answers null until the host first pushes it. The values are
     * Instance-served from HearthEpmDelegate's store either way; these
     * ember entries exist so the init CB's is_attribute_enabled scan admits
     * the attributes into the Instance's served set. */
    cluster::electrical_power_measurement::attribute::create_voltage(cl, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_active_current(cl, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_frequency(cl, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_power_factor(cl, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_rms_voltage(cl, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_rms_current(cl, nullable<int64_t>());
    return true;
}

static uint32_t mt_eem_feature_flags(void)
{
    return cluster::electrical_energy_measurement::feature::imported_energy::get_id() |
           cluster::electrical_energy_measurement::feature::exported_energy::get_id() |
           cluster::electrical_energy_measurement::feature::cumulative_energy::get_id();
}

static endpoint_t *mk_electrical_sensor(node_t *n, uint8_t variant)
{
    void *epm_delegate = mt_matter_epm_delegate_alloc();
    if (epm_delegate == nullptr) {
        ESP_LOGE(TAG, "EPM delegate pool exhausted (electrical sensor)");
        return nullptr;
    }
    void *ptop_delegate = mt_matter_ptop_delegate_alloc();
    if (ptop_delegate == nullptr) {
        ESP_LOGE(TAG, "PowerTopology delegate pool exhausted (electrical sensor)");
        return nullptr;
    }

    electrical_sensor::config_t c;
    /* NodeTopology alone (Task 2's binding contract: HearthPtopDelegate's
     * endpoint-list iterators only answer under SET/TREE, which are not
     * enabled). electrical_sensor::add() ORs this same bit in
     * unconditionally (esp_matter_endpoint.cpp:1510), so the explicit set
     * documents the choice rather than creating it; the |= keeps the
     * feature map at exactly one topology bit either way
     * (VALIDATE_FEATURES_EXACT_ONE, esp_matter_cluster.cpp:3199). */
    c.power_topology.feature_flags = cluster::power_topology::feature::node_topology::get_id();
    c.power_topology.delegate = ptop_delegate;
    c.electrical_power_measurement.feature_flags =
        cluster::electrical_power_measurement::feature::alternating_current::get_id();
    c.electrical_power_measurement.delegate = epm_delegate;

    endpoint_t *ep = electrical_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    if (!mt_epm_add_field_attributes(ep)) {
        return nullptr;
    }

    if (variant == 0) {
        /* The sensor's device type XML lists the energy cluster optional
         * (choice "a" with the power cluster, min 1), and
         * electrical_sensor::config_t carries no EEM member at all, so
         * variant 0 adds the cluster by hand, the same after-create()
         * shape as every other hand-added cluster in this file. */
        cluster::electrical_energy_measurement::config_t ec;
        ec.feature_flags = mt_eem_feature_flags();
        cluster_t *eem_cl =
            cluster::electrical_energy_measurement::create(ep, &ec, CLUSTER_FLAG_SERVER);
        if (eem_cl == nullptr) {
            ESP_LOGE(TAG, "creating ElectricalEnergyMeasurement cluster failed");
            return nullptr;
        }
        esp_matter::cluster::set_delegate_and_init_callback(eem_cl, HearthEemInitCB, nullptr);
    }

    uint16_t ep_id = endpoint::get_id(ep);
    mt_matter_meas_delegate_set_endpoint(epm_delegate, ep_id);
    mt_matter_meas_delegate_set_endpoint(ptop_delegate, ep_id);
    return ep;
}

static endpoint_t *mk_electrical_meter(node_t *n, uint8_t variant)
{
    void *epm_delegate = mt_matter_epm_delegate_alloc();
    if (epm_delegate == nullptr) {
        ESP_LOGE(TAG, "EPM delegate pool exhausted (electrical meter)");
        return nullptr;
    }

    electrical_meter::config_t c;
    c.electrical_power_measurement.feature_flags =
        cluster::electrical_power_measurement::feature::alternating_current::get_id();
    c.electrical_power_measurement.delegate = epm_delegate;
    /* electrical_meter::add() (esp_matter_endpoint.cpp:2275-2282) creates
     * the EEM cluster unconditionally from this config, so the feature
     * flags are set for BOTH variants: left at 0 for variant 1, create()
     * would trip its feature validation and abort the cluster mid-add()
     * (with add() ignoring the failure), a messier path than building the
     * cluster whole and destroying it below, the B216
     * destroy-after-create precedent. */
    c.electrical_energy_measurement.feature_flags = mt_eem_feature_flags();

    endpoint_t *ep = electrical_meter::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    if (!mt_epm_add_field_attributes(ep)) {
        return nullptr;
    }

    cluster_t *eem_cl = cluster::get(ep, chip::app::Clusters::ElectricalEnergyMeasurement::Id);
    if (eem_cl == nullptr) {
        ESP_LOGE(TAG, "ElectricalEnergyMeasurement cluster missing after create");
        return nullptr;
    }
    if (variant == 0) {
        esp_matter::cluster::set_delegate_and_init_callback(eem_cl, HearthEemInitCB, nullptr);
    } else {
        /* Power-only meter: destroy the cluster add() forced, before
         * esp_matter::start() the same as the whole rebuild, so there is
         * no CHIP event loop to race (the mk_cook_surface() precedent).
         * Note the 1.5.1 device type XML marks EEM mandatoryConform on
         * this device type (unlike the sensor's choice conformance), so
         * variant 1 here departs from strict conformance; it exists for
         * variant-scheme symmetry with the sensor (design spec 3.3), and
         * the conformant current-clamp declaration is the variant-1
         * SENSOR. */
        if (cluster::destroy(eem_cl) != ESP_OK) {
            ESP_LOGE(TAG, "destroying ElectricalEnergyMeasurement cluster failed");
            return nullptr;
        }
    }

    mt_matter_meas_delegate_set_endpoint(epm_delegate, endpoint::get_id(ep));
    return ep;
}

/*
 * ---- Water Heater and Heat Pump (energy round B, task 3) ----------------
 *
 * Two device types on the Round A measurement machinery plus task 1's WHM
 * delegate pool. Both thunks carry a boot-panic trap the SDK's own endpoint
 * add() either creates or fails to defuse, and the water heater additionally
 * works around two broken SDK feature helpers. Design record:
 * ARCHITECTURE.md 8.11; fact base: the Round B SDK survey (A.1, A.2, B.1,
 * B.2), every citation below re-read at source while writing this.
 *
 * Water Heater (0x050F): water_heater::add() (esp_matter_endpoint.cpp:
 * 1870-1880) builds Descriptor, Thermostat (:1875), WaterHeaterManagement
 * (:1876) and WaterHeaterMode. No Identify, no measurement clusters, no
 * tag_list.
 *
 * PANIC TRAP ONE: thermostat::create() runs VALIDATE_FEATURES_AT_LEAST_ONE
 * ("Heat,Cool", esp_matter_cluster.cpp:1445), water_heater::config_t seeds
 * thermostat.feature_flags 0, and assertions are on, so a default-config
 * create is a boot panic, boot-loop grade because the composition rebuild
 * runs every boot. Pre-seed Heating BEFORE create(), the
 * mk_room_air_conditioner() precedent above (same trap, same fix). Heating
 * alone matches the device type XML: WaterHeater.xml mandates
 * Thermostat(HEAT), and HEAT mandates only OccupiedHeatingSetpoint
 * (Thermostat.xml:752-757), all ember-served and AT+MTATTR-reachable, so no
 * new AT surface rides on this cluster (design spec 3.5).
 *
 * WHM delegate: the cluster is FULLY delegate-served (all six attributes
 * MANAGED_INTERNALLY, esp_matter_attribute.cpp:4485-4519); without a
 * delegate every read answers Failure while metadata advertises the
 * attributes (the EEM 8.10 disease, but here the fix is a real delegate:
 * the Instance IS the AttributeAccessInterface,
 * water-heater-management-server.cpp:94-100). The pool's alloc takes the
 * real endpoint id (mt_matter.h's documented protocol), which create() has
 * not assigned yet, so unlike the valve/opstate pools the slot is handed
 * out AFTER create() and attached via set_delegate_and_init_callback()
 * with the SDK's own WaterHeaterManagementDelegateInitCB
 * (esp_matter::cluster::delegate_cb, esp_matter_delegate_callbacks.h:51).
 * Attaching post-create loses nothing: every delegate init callback runs
 * at endpoint enable (invoke_init_callbacks_internal,
 * esp_matter_data_model.cpp:259-266, called during esp_matter::start()'s
 * enable pass), long after this thunk returns.
 *
 * Variant 0 hand-sets the WHM feature map to EnergyManagement|TankPercent
 * and creates the three gated attribute shells, because create() HARDCODES
 * FeatureMap 0 (esp_matter_cluster.cpp:3746; config_t has no feature_flags
 * field, esp_matter_cluster.h:1184-1197) and BOTH SDK feature helpers are
 * broken and must not be called:
 *   - energy_management::add(cluster_t *) as declared
 *     (esp_matter_feature.h:1537) has no implementation; the impl signature
 *     is add(cluster_t *, config_t *) (esp_matter_feature.cpp:3315), so a
 *     call per the header is a link failure.
 *   - tank_percent::get_id() returns Feature::kEnergyManagement, not
 *     kTankPercent (esp_matter_feature.cpp:3335 against
 *     WaterHeaterManagement/Enums.h:44-48: kEnergyManagement 0x1,
 *     kTankPercent 0x2), so tank_percent::add() sets the WRONG bit. Never
 *     use either; read the bits from CHIP's own Feature enum and set them
 *     by hand on the returned cluster handle.
 * The hand-set writes the FeatureMap attribute directly (get + set_val,
 * callbacks off), mirroring the SDK's own private update_feature_map()
 * (esp_matter_feature.cpp:31-46: FeatureMap is ATTRIBUTE_FLAG_NONE, so
 * set_val() routes to the pre-start-safe esp-matter store); since create()
 * just wrote the hardcoded 0 (:3746), writing the full mask IS the OR.
 * Timing is load-bearing: WaterHeaterManagementDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp:487-498) snapshots the endpoint's
 * FeatureMap when it news the Instance at endpoint enable, so the hand-set
 * here in the thunk (before esp_matter::start()) is what the Instance
 * serves and what mt_meas_whm_apply()'s feature gate reads (main.cpp): one
 * source, no disagreement. The three shells mirror what the two helpers
 * would have created (feature.cpp:3323-3325, :3346): TankVolume u16,
 * EstimatedHeatRequired int64, TankPercentage u8
 * (esp_matter_attribute.cpp:4497-4513), values delegate-served regardless.
 *
 * Variant 0 also grafts the Electrical Sensor composition the device type
 * XML mandates (WaterHeater.xml:84-95, composedDeviceTypes: an Electrical
 * Sensor 0x0510 with EPM and EEM; the SDK's add() provides none of it):
 * device type 0x0510 on the SAME endpoint plus PowerTopology and EPM via
 * electrical_sensor::add() (the exact call heat_pump::add() uses,
 * esp_matter_endpoint.cpp:2007-2008), then the six field attributes and
 * the EEM cluster with HearthEemInitCB, verbatim the
 * mk_electrical_sensor() variant-0 sequence above, drawing one EPM and one
 * PT slot from the Round A pools.
 *
 * Variant 1 is the SDK-bare build: WHM feature map 0 (HeaterTypes,
 * HeatDemand, BoostState only, which WHM's own feature conformance
 * permits: EM and TP are both optional), no sensor graft. DISCLOSED
 * SUB-CONFORMANT: the device type XML's composedDeviceTypes block mandates
 * the Electrical Sensor regardless, so variant 1 exists as the
 * minimal/staging declaration (the variant-1 electrical meter precedent
 * above), disclosed in the design spec, here, and ARCHITECTURE.md 8.11.
 *
 * Commands and events need no hand-adds, for once: WHM create() wires
 * Boost/CancelBoost and both Boost events unconditionally
 * (esp_matter_cluster.cpp:3755-3759), and water_heater_mode::create() is
 * the clean rvc_run_mode shape (ChangeToMode + response wired,
 * :3797-3798). The 8.6 check came back clean on both clusters.
 */
static endpoint_t *mk_water_heater(node_t *n, uint8_t variant)
{
    void *mode_delegate = mt_matter_modebase_delegate_alloc(chip::app::Clusters::WaterHeaterMode::Id);
    if (mode_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (water heater mode)");
        return nullptr;
    }

    water_heater::config_t c;
    /* Panic trap one (see above): VALIDATE_FEATURES_AT_LEAST_ONE Heat,Cool,
     * esp_matter_cluster.cpp:1445; the config's default 0 panics at boot.
     * Heating only, per WaterHeater.xml's Thermostat(HEAT) mandate. */
    c.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id();
    c.water_heater_mode.delegate = mode_delegate;
    /* c.water_heater_management.delegate stays null: the WHM pool alloc
     * needs the real endpoint id, so the delegate is attached after
     * create() below (see the class comment; init CBs run at enable). */

    endpoint_t *ep = water_heater::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    uint16_t ep_id = endpoint::get_id(ep);
    mt_matter_modebase_delegate_set_endpoint(mode_delegate, ep_id);

    void *whm_delegate = mt_matter_whm_delegate_alloc(ep_id);
    if (whm_delegate == nullptr) {
        ESP_LOGE(TAG, "WHM delegate pool exhausted (water heater)");
        return nullptr;
    }
    cluster_t *whm_cl = cluster::get(ep, chip::app::Clusters::WaterHeaterManagement::Id);
    if (whm_cl == nullptr) {
        ESP_LOGE(TAG, "WaterHeaterManagement cluster missing after create");
        return nullptr;
    }
    esp_matter::cluster::set_delegate_and_init_callback(
        whm_cl, esp_matter::cluster::delegate_cb::WaterHeaterManagementDelegateInitCB, whm_delegate);

    if (variant == 0) {
        /* WHM feature map hand-set, EnergyManagement|TankPercent. BOTH SDK
         * helpers are broken (see above): energy_management::add() as
         * declared does not link (esp_matter_feature.h:1537 vs
         * esp_matter_feature.cpp:3315), and tank_percent::get_id() returns
         * the EnergyManagement bit (esp_matter_feature.cpp:3335). The bits
         * come from CHIP's own enum, never transcribed; create() hardcoded
         * FeatureMap 0 one call ago (esp_matter_cluster.cpp:3746), so this
         * whole-mask write is the OR update_feature_map() would do. Must
         * happen in the thunk: WaterHeaterManagementDelegateInitCB
         * snapshots FeatureMap at endpoint enable
         * (esp_matter_delegate_callbacks.cpp:487-498). */
        attribute_t *fm = esp_matter::attribute::get(
            whm_cl, chip::app::Clusters::Globals::Attributes::FeatureMap::Id);
        if (fm == nullptr) {
            ESP_LOGE(TAG, "WaterHeaterManagement FeatureMap attribute missing");
            return nullptr;
        }
        esp_matter_attr_val_t fm_val = esp_matter_bitmap32(
            static_cast<uint32_t>(chip::app::Clusters::WaterHeaterManagement::Feature::kEnergyManagement) |
            static_cast<uint32_t>(chip::app::Clusters::WaterHeaterManagement::Feature::kTankPercent));
        if (esp_matter::attribute::set_val(fm, &fm_val, false) != ESP_OK) {
            ESP_LOGE(TAG, "hand-setting WaterHeaterManagement feature map failed");
            return nullptr;
        }
        /* The three gated attribute shells the broken helpers would have
         * created (feature.cpp:3323-3325, :3346; creators at
         * esp_matter_attribute.cpp:4497-4513). MANAGED_INTERNALLY: the
         * Instance serves the delegate's cached values, these entries only
         * make the attributes exist in the metadata. */
        cluster::water_heater_management::attribute::create_tank_volume(whm_cl, 0);
        cluster::water_heater_management::attribute::create_estimated_heat_required(whm_cl, 0);
        cluster::water_heater_management::attribute::create_tank_percentage(whm_cl, 0);

        /* The Electrical Sensor graft (WaterHeater.xml:84-95), the
         * mk_electrical_sensor() variant-0 sequence on this endpoint. */
        void *epm_delegate = mt_matter_epm_delegate_alloc();
        if (epm_delegate == nullptr) {
            ESP_LOGE(TAG, "EPM delegate pool exhausted (water heater)");
            return nullptr;
        }
        void *ptop_delegate = mt_matter_ptop_delegate_alloc();
        if (ptop_delegate == nullptr) {
            ESP_LOGE(TAG, "PowerTopology delegate pool exhausted (water heater)");
            return nullptr;
        }
        electrical_sensor::config_t es;
        es.power_topology.feature_flags = cluster::power_topology::feature::node_topology::get_id();
        es.power_topology.delegate = ptop_delegate;
        es.electrical_power_measurement.feature_flags =
            cluster::electrical_power_measurement::feature::alternating_current::get_id();
        es.electrical_power_measurement.delegate = epm_delegate;
        if (electrical_sensor::add(ep, &es) != ESP_OK) {
            ESP_LOGE(TAG, "electrical sensor graft failed (water heater)");
            return nullptr;
        }
        if (!mt_epm_add_field_attributes(ep)) {
            return nullptr;
        }
        cluster::electrical_energy_measurement::config_t ec;
        ec.feature_flags = mt_eem_feature_flags();
        cluster_t *eem_cl =
            cluster::electrical_energy_measurement::create(ep, &ec, CLUSTER_FLAG_SERVER);
        if (eem_cl == nullptr) {
            ESP_LOGE(TAG, "creating ElectricalEnergyMeasurement cluster failed (water heater)");
            return nullptr;
        }
        esp_matter::cluster::set_delegate_and_init_callback(eem_cl, HearthEemInitCB, nullptr);
        mt_matter_meas_delegate_set_endpoint(epm_delegate, ep_id);
        mt_matter_meas_delegate_set_endpoint(ptop_delegate, ep_id);
    }
    return ep;
}

/*
 * Heat Pump (0x0309): hand-built stack mirroring heat_pump::add()
 * (esp_matter_endpoint.cpp:1996-2021) minus DEM, read side by side while
 * writing, the oven-shell discipline.
 *
 * PANIC TRAP TWO, and why this thunk cannot call heat_pump::create() at
 * all: add() calls electrical_energy_measurement::create() with the
 * config's default feature_flags 0 (esp_matter_endpoint.cpp:2015; the
 * config_t constructor seeds nothing), and EEM create() runs two
 * VALIDATE_FEATURES_AT_LEAST_ONE checks
 * (ImportedEnergy/ExportedEnergy and CumulativeEnergy/PeriodicEnergy,
 * esp_matter_cluster.cpp:3301-3304): boot panic on default config.
 * solar_power::add() (:1913) and battery_storage::add() (:1966) receive
 * pre-seeded configs from their own callers' conventions; heat_pump's
 * forgot (survey B.2). A caller COULD pre-seed
 * config.electrical_energy_measurement.feature_flags and survive, but
 * add() would then also bolt on the full DEM pair
 * (DeviceEnergyManagement + power_adjustment, :2017-2020), which this
 * round defers whole (DE235: the 1.5.1 HeatPump.xml mandates no DEM, and
 * the 16-virtual delegate with two owned struct stores gets built once,
 * in Round C, where EVSE and the DEM device type need it for real; both
 * DEM Kconfigs stay =n, so the server directory is not even linked).
 * Hand-building the add() body minus those two calls is smaller than
 * destroying what add() forced (the B216 destroy-after-create precedent
 * stays reserved for when the SDK leaves no seam; here every piece of
 * add() is a public call).
 *
 * The stack, in add()'s own order: device type 0x0309, descriptor
 * tag_list feature (SDK-build parity), Power Source device type 0x0011
 * with the wired feature (endpoint::power_source::add, the same
 * endpoint-namespace add() the SDK calls at :2005-2006), then the
 * Electrical Sensor 0x0510 machinery exactly as the variant-0 water
 * heater graft above: electrical_sensor::add(), the six EPM field
 * attributes (a superset of the SDK build's hand-created Voltage and
 * ActiveCurrent, :2010-2013; duplicate creates warn and return existing,
 * esp_matter_data_model.cpp:567, so the overlap is harmless), and the
 * EEM cluster with mt_eem_feature_flags() pre-seeded BEFORE create(),
 * defusing the trap, plus HearthEemInitCB for the 8.10 serving-path fix.
 * One EPM slot and one PT slot from the Round A pools; AT+MTMEAS serves
 * both push families unchanged.
 *
 * DISCLOSED CONFORMANCE GAP: HeatPump.xml mandates no server clusters
 * beyond Descriptor, but its composedDeviceTypes block mandates a
 * composed Thermostat DEVICE TYPE 0x0301 with User Label alongside the
 * Electrical Sensor. This endpoint carries neither, MATCHING the SDK's
 * own omission (heat_pump::config_t has no thermostat member at all); the
 * sensor half is over-delivered, the thermostat half is a disclosed gap
 * and a possible follow-up (design spec 2.3, ARCHITECTURE.md 8.11), not
 * this round.
 */
static endpoint_t *mk_heat_pump(node_t *n, uint8_t variant)
{
    (void)variant;
    void *epm_delegate = mt_matter_epm_delegate_alloc();
    if (epm_delegate == nullptr) {
        ESP_LOGE(TAG, "EPM delegate pool exhausted (heat pump)");
        return nullptr;
    }
    void *ptop_delegate = mt_matter_ptop_delegate_alloc();
    if (ptop_delegate == nullptr) {
        ESP_LOGE(TAG, "PowerTopology delegate pool exhausted (heat pump)");
        return nullptr;
    }

    /* Raw endpoint plus descriptor, the two things common::create() does
     * before any add() body runs (esp_matter_endpoint.cpp:29-45). */
    endpoint_t *ep = endpoint::create(n, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    cluster::descriptor::config_t dc;
    cluster_t *desc_cl = cluster::descriptor::create(ep, &dc, CLUSTER_FLAG_SERVER);
    if (desc_cl == nullptr) {
        ESP_LOGE(TAG, "creating descriptor cluster failed (heat pump)");
        return nullptr;
    }
    if (endpoint::add_device_type(ep, heat_pump::get_device_type_id(),
                                  heat_pump::get_device_type_version()) != ESP_OK) {
        ESP_LOGE(TAG, "adding heat pump device type failed");
        return nullptr;
    }
    /* tag_list for parity with the SDK build (endpoint.cpp:2003-2004). */
    cluster::descriptor::feature::tag_list::add(desc_cl);

    /* Power Source device type 0x0011, wired (endpoint.cpp:2005-2006).
     * power_source::create() runs VALIDATE_FEATURES_EXACT_ONE on
     * Wired/Battery (esp_matter_cluster.cpp:982, the mk_power_source()
     * trap above); wired is the SDK build's own choice. */
    power_source::config_t pc;
    pc.power_source.feature_flags = cluster::power_source::feature::wired::get_id();
    if (power_source::add(ep, &pc) != ESP_OK) {
        ESP_LOGE(TAG, "power source add failed (heat pump)");
        return nullptr;
    }

    /* Electrical Sensor 0x0510, the variant-0 water heater graft verbatim. */
    electrical_sensor::config_t es;
    es.power_topology.feature_flags = cluster::power_topology::feature::node_topology::get_id();
    es.power_topology.delegate = ptop_delegate;
    es.electrical_power_measurement.feature_flags =
        cluster::electrical_power_measurement::feature::alternating_current::get_id();
    es.electrical_power_measurement.delegate = epm_delegate;
    if (electrical_sensor::add(ep, &es) != ESP_OK) {
        ESP_LOGE(TAG, "electrical sensor graft failed (heat pump)");
        return nullptr;
    }
    if (!mt_epm_add_field_attributes(ep)) {
        return nullptr;
    }

    /* EEM with the feature flags pre-seeded BEFORE create(): panic trap
     * two defused (heat_pump::add() forgot this at endpoint.cpp:2015;
     * the checks are esp_matter_cluster.cpp:3301-3304). */
    cluster::electrical_energy_measurement::config_t ec;
    ec.feature_flags = mt_eem_feature_flags();
    cluster_t *eem_cl =
        cluster::electrical_energy_measurement::create(ep, &ec, CLUSTER_FLAG_SERVER);
    if (eem_cl == nullptr) {
        ESP_LOGE(TAG, "creating ElectricalEnergyMeasurement cluster failed (heat pump)");
        return nullptr;
    }
    esp_matter::cluster::set_delegate_and_init_callback(eem_cl, HearthEemInitCB, nullptr);

    uint16_t ep_id = endpoint::get_id(ep);
    mt_matter_meas_delegate_set_endpoint(epm_delegate, ep_id);
    mt_matter_meas_delegate_set_endpoint(ptop_delegate, ep_id);
    return ep;
}

/*
 * ---- Solar Power, Battery Storage and DEM (energy round C1, task 3) -----
 *
 * Three device types on Round A's measurement machinery plus this round's
 * task-1 DEM delegate pool. Design record: ARCHITECTURE.md 8.12; fact base:
 * the Round C SDK survey (.superpowers/sdd/2026-08-11-energy-round-c/
 * sdk-survey.md), every citation below re-read at source while writing.
 *
 * ALL THREE THUNKS HAND-BUILD (the round's decision of record, design spec
 * section 1), and for a different reason than Round B's: there is NO
 * boot-panic trap anywhere on these paths (survey, solar section:
 * solar_power::add() is the first SDK endpoint builder callable unmodified
 * without a panic). What forces the hand-build is a set of SILENT contract
 * breaks in solar_power::add()/battery_storage::add(), invisible to any
 * build or host suite:
 *
 *   - Both add() bodies ASSIGN feature_flags over whatever the caller
 *     pre-seeded (esp_matter_endpoint.cpp:1907 and 1910 for solar's
 *     PowerSource and EEM, :1949 and :1963-1964 for battery's), so no
 *     config pre-seed can survive: the mk_room_air_conditioner() |= trick
 *     has nothing to land on.
 *   - The EEM mask both assign is exported|cumulative (:1910/:1963), which
 *     contradicts HearthEemInitCB's fixed Imported|Exported|Cumulative
 *     wildcard AttrAccess mask (Round A's binding contract, main.cpp): the
 *     served FeatureMap would lie, and CumulativeEnergyImported would not
 *     exist at all, because cumulative_energy::add() gates it on the
 *     Imported bit (esp_matter_feature.cpp:2479-2481).
 *   - Both add() bodies create the EPM Voltage and ActiveCurrent attributes
 *     as NON-NULL zeros from function locals (:1917-1919 solar, battery via
 *     its config's voltage(0)/active_current(0) defaults), breaking
 *     AT_MT_SPEC.md 3.25's null-until-pushed contract; and because a
 *     duplicate attribute create returns the existing entry
 *     (esp_matter_data_model.cpp:567), mt_epm_add_field_attributes() run
 *     afterwards could not repair the two to null.
 *
 * The DEM endpoint add() (esp_matter_endpoint.cpp:1708-1717) is thin and
 * break-free, but it hand-builds too: one discipline across the round, and
 * its two cluster creates are exactly what mt_add_dem_triple() below runs.
 */

/*
 * The Electrical Sensor graft as a named helper: the mk_electrical_sensor()
 * variant-0 sequence on an endpoint that already exists, verbatim the Round
 * B water-heater variant-0 graft above (mk_water_heater(), which keeps its
 * in-line copy untouched, as does mk_heat_pump()). Device type 0x0510 plus
 * PowerTopology and EPM via electrical_sensor::add(), the six null field
 * attributes, and (with_eem) the EEM cluster with the fixed
 * Imported|Exported|Cumulative mask and HearthEemInitCB. Draws one EPM and
 * one PT slot from the Round A pools; allocs happen after endpoint creation
 * because the endpoint id is already known here (mt_cabinet_add_cooler()'s
 * note on the same shape). Returns false on any failure: the caller returns
 * nullptr and the boot rebuild aborts, this file's contract.
 */
static bool mt_graft_electrical_sensor(endpoint_t *ep, bool with_eem, const char *who)
{
    void *epm_delegate = mt_matter_epm_delegate_alloc();
    if (epm_delegate == nullptr) {
        ESP_LOGE(TAG, "EPM delegate pool exhausted (%s)", who);
        return false;
    }
    void *ptop_delegate = mt_matter_ptop_delegate_alloc();
    if (ptop_delegate == nullptr) {
        ESP_LOGE(TAG, "PowerTopology delegate pool exhausted (%s)", who);
        return false;
    }
    electrical_sensor::config_t es;
    es.power_topology.feature_flags = cluster::power_topology::feature::node_topology::get_id();
    es.power_topology.delegate = ptop_delegate;
    es.electrical_power_measurement.feature_flags =
        cluster::electrical_power_measurement::feature::alternating_current::get_id();
    es.electrical_power_measurement.delegate = epm_delegate;
    if (electrical_sensor::add(ep, &es) != ESP_OK) {
        ESP_LOGE(TAG, "electrical sensor graft failed (%s)", who);
        return false;
    }
    /* Six null field attributes, the 3.25 null-until-pushed contract; this
     * is the repair solar_power::add() would have made impossible (see the
     * section comment above: non-null zeros first, duplicate create returns
     * existing). */
    if (!mt_epm_add_field_attributes(ep)) {
        return false;
    }
    if (with_eem) {
        cluster::electrical_energy_measurement::config_t ec;
        ec.feature_flags = mt_eem_feature_flags();
        cluster_t *eem_cl =
            cluster::electrical_energy_measurement::create(ep, &ec, CLUSTER_FLAG_SERVER);
        if (eem_cl == nullptr) {
            ESP_LOGE(TAG, "creating ElectricalEnergyMeasurement cluster failed (%s)", who);
            return false;
        }
        esp_matter::cluster::set_delegate_and_init_callback(eem_cl, HearthEemInitCB, nullptr);
    }
    uint16_t ep_id = endpoint::get_id(ep);
    mt_matter_meas_delegate_set_endpoint(epm_delegate, ep_id);
    mt_matter_meas_delegate_set_endpoint(ptop_delegate, ep_id);
    return true;
}

/*
 * The DEM triple: device type 0x050D + DeviceEnergyManagement cluster with
 * a pooled HearthDemDelegate + DeviceEnergyManagementMode with a ModeBase
 * slot. Shared by battery storage variant 0 (where it is over-delivery, see
 * mk_battery_storage()) and both mk_dem() variants.
 *
 * THE IRON RULE (design spec 2.5, ARCHITECTURE.md 8.12): DEM feature bits
 * are set ONLY via cluster::device_energy_management::feature::X::add(),
 * NEVER by writing FeatureMap. Instance::RetrieveAcceptedCommands derives
 * the advertised command list from its FeatureMap snapshot while
 * esp-matter's dispatcher looks the invoke up as an ember command_t
 * (esp_matter_command.cpp:42-44: get(cluster, command_id,
 * COMMAND_FLAG_ACCEPTED) then VerifyOrReturn, so a missing entry means the
 * invoke returns NO status at all, not an error); only feature::add()
 * creates those entries. power_adjustment::add()
 * (esp_matter_feature.cpp:3050-3067) creates PowerAdjustmentCapability and
 * OptOutState, both PA commands and both PA events alongside the bit. This
 * is the exact inverse of Round B's WHM hand-set (mk_water_heater() above),
 * which was forced by two broken WHM helpers on a cluster whose commands
 * create() had already wired; DEM's helper is healthy and its commands come
 * from nowhere else. The PA bit is also load-bearing for the AT surface:
 * without it mt_matter_demcap_set() answers MT_ATTR_ERR_ATTRIBUTE
 * (PowerAdjustmentCapability exists only under PA), so a variant-0 build
 * missing this add() turns every AT+MTDEMCAP into +MTERR:4.
 *
 * Feature-add timing: DeviceEnergyManagementDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp:368-376) news the Instance with a
 * feature bitmask SNAPSHOTTED from the endpoint's FeatureMap attribute, and
 * every delegate init callback runs at endpoint enable
 * (invoke_init_callbacks_internal, esp_matter_data_model.cpp:259-266,
 * during esp_matter::start()'s enable pass), long after this thunk: the
 * feature::add() here is guaranteed to precede the snapshot, the same
 * ordering argument as Round B's WHM hand-set. The CB keeps a static
 * single-Instance pointer overwritten per endpoint with no Shutdown of the
 * previous one; benign, delegate lifetimes are boot-long (mt_matter.h).
 *
 * Variant 1 (with_pa false) leaves the FeatureMap at the 0 create()
 * hardcodes (esp_matter_cluster.cpp:3478): a non-controllable ESA, and
 * CONFORMANT: the device type XML (1.5.1 DeviceEnergyManagement.xml) makes
 * all five DEM features an optionalConform choice (min 1) UNDER the
 * ControllableESA condition, so a device that does not declare the
 * condition owes no feature bit. The delegate is still required either way:
 * the cluster's five attributes are delegate-served
 * (cluster.cpp:3463-3494 creates them for the Instance to serve; nothing
 * answers without one, the 8.10 disease otherwise).
 *
 * Delegate wiring is the WHM post-create shape: the pool alloc takes the
 * real endpoint id (mt_matter.h's documented protocol), then
 * set_delegate_and_init_callback() attaches the SDK's own
 * DeviceEnergyManagementDelegateInitCB. DEM Mode wires its delegate through
 * its config instead (DeviceEnergyManagementModeDelegateInitCB,
 * esp_matter_cluster.cpp:3510-3513), the water_heater_mode shape. 8.6 check
 * on DEM Mode: CLEAN, create() wires ChangeToMode and its response
 * unconditionally (esp_matter_cluster.cpp:3529-3531, the rvc_run_mode
 * shape, not the operational_state disease); Mode_DeviceEnergyManagement.xml
 * carries no mandatory-tag conformance and marks StartUpMode/OnMode
 * disallowConform, correctly absent from the SDK build (the AT+MTMODES
 * kNoOptimization tag-0 default is task 2's branch, main.cpp).
 */
static bool mt_add_dem_triple(endpoint_t *ep, bool with_pa, const char *who)
{
    uint16_t ep_id = endpoint::get_id(ep);
    void *dem_delegate = mt_matter_dem_delegate_alloc(ep_id);
    if (dem_delegate == nullptr) {
        ESP_LOGE(TAG, "DEM delegate pool exhausted (%s)", who);
        return false;
    }
    void *mode_delegate =
        mt_matter_modebase_delegate_alloc(chip::app::Clusters::DeviceEnergyManagementMode::Id);
    if (mode_delegate == nullptr) {
        ESP_LOGE(TAG, "modebase delegate pool exhausted (%s DEM mode)", who);
        return false;
    }
    /* Endpoint already exists: fix the ModeBase slot's endpoint immediately,
     * the mt_cabinet_add_cooler() note. The DEM slot took its id at alloc. */
    mt_matter_modebase_delegate_set_endpoint(mode_delegate, ep_id);

    if (endpoint::add_device_type(ep, device_energy_management::get_device_type_id(),
                                  device_energy_management::get_device_type_version()) != ESP_OK) {
        ESP_LOGE(TAG, "adding DEM device type failed (%s)", who);
        return false;
    }

    cluster::device_energy_management::config_t dc;
    /* dc.delegate stays null: create() would wire the identical init CB from
     * it (cluster.cpp:3468-3472), but the pool protocol pins the explicit
     * post-create attach below (mt_matter.h), one unambiguous wiring site. */
    cluster_t *dem_cl = cluster::device_energy_management::create(ep, &dc, CLUSTER_FLAG_SERVER);
    if (dem_cl == nullptr) {
        ESP_LOGE(TAG, "creating DeviceEnergyManagement cluster failed (%s)", who);
        return false;
    }
    if (with_pa) {
        /* The iron rule's one legal site: feature::add() ONLY, never a
         * FeatureMap write (see the class comment above). */
        if (cluster::device_energy_management::feature::power_adjustment::add(dem_cl) != ESP_OK) {
            ESP_LOGE(TAG, "adding PowerAdjustment feature failed (%s)", who);
            return false;
        }
    }
    esp_matter::cluster::set_delegate_and_init_callback(
        dem_cl, esp_matter::cluster::delegate_cb::DeviceEnergyManagementDelegateInitCB, dem_delegate);

    cluster::device_energy_management_mode::config_t mc;
    mc.delegate = mode_delegate;
    if (cluster::device_energy_management_mode::create(ep, &mc, CLUSTER_FLAG_SERVER) == nullptr) {
        ESP_LOGE(TAG, "creating DeviceEnergyManagementMode cluster failed (%s)", who);
        return false;
    }
    return true;
}

/*
 * Solar Power (0x0017): hand-built stack mirroring solar_power::add()
 * (esp_matter_endpoint.cpp:1899-1922) read side by side while writing, the
 * oven-shell discipline, with the add() body's three silent breaks (the
 * section comment above) replaced by the Round A contracts: device types
 * 0x0017 + 0x0011 + 0x0510, descriptor tag_list (SDK-build parity), wired
 * Power Source, the sensor graft with six NULL EPM fields, and, variant 0
 * only, EEM with mt_eem_feature_flags() + HearthEemInitCB.
 *
 * Variant 1 (no EEM) is the current-clamp shape, the variant-1 electrical
 * sensor precedent, and DISCLOSED SUB-CONFORMANT: SolarPower.xml's
 * composedDeviceTypes block mandates EPM and EEM both on the composed
 * Electrical Sensor, so a solar declaration without energy measurement
 * departs from the letter of the device type. Disclosed in the design spec
 * (2.2), here, and ARCHITECTURE.md 8.12.
 *
 * DISCLOSED GAP, both variants: the same composedDeviceTypes block lists
 * User Label with describedConform on the sensor. This firmware ships no
 * User Label cluster anywhere (CONFIG_SUPPORT_USER_LABEL_CLUSTER stays n),
 * matching the SDK build's own omission; disclosed, not wired.
 */
static endpoint_t *mk_solar_power(node_t *n, uint8_t variant)
{
    /* Raw endpoint plus descriptor, the two things common::create() does
     * before any add() body runs (the mk_heat_pump() shape above). */
    endpoint_t *ep = endpoint::create(n, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    cluster::descriptor::config_t dc;
    cluster_t *desc_cl = cluster::descriptor::create(ep, &dc, CLUSTER_FLAG_SERVER);
    if (desc_cl == nullptr) {
        ESP_LOGE(TAG, "creating descriptor cluster failed (solar power)");
        return nullptr;
    }
    if (endpoint::add_device_type(ep, solar_power::get_device_type_id(),
                                  solar_power::get_device_type_version()) != ESP_OK) {
        ESP_LOGE(TAG, "adding solar power device type failed");
        return nullptr;
    }
    /* tag_list for parity with the SDK build (endpoint.cpp:1904-1905). */
    cluster::descriptor::feature::tag_list::add(desc_cl);

    /* Power Source 0x0011, wired: the SDK build's own choice
     * (endpoint.cpp:1907-1908), and the exact mk_heat_pump() call. */
    power_source::config_t pc;
    pc.power_source.feature_flags = cluster::power_source::feature::wired::get_id();
    if (power_source::add(ep, &pc) != ESP_OK) {
        ESP_LOGE(TAG, "power source add failed (solar power)");
        return nullptr;
    }

    if (!mt_graft_electrical_sensor(ep, variant == 0, "solar power")) {
        return nullptr;
    }
    return ep;
}

/*
 * Battery Storage (0x0018): hand-built stack mirroring
 * battery_storage::add() (esp_matter_endpoint.cpp:1941-1977) read side by
 * side while writing: device types 0x0018 + 0x0011 + 0x0510 (+ 0x050D on
 * variant 0), descriptor tag_list, battery Power Source with the eight
 * battery attributes, the sensor graft with EEM, and, variant 0 only, the
 * DEM triple.
 *
 * REAL SDK CONFORMANCE DEFECT, fixed here: battery_storage::add() assigns
 * the Power Source feature map kBattery alone (endpoint.cpp:1949) and then
 * hand-creates four attributes that PowerSourceCluster.xml gates on the
 * RECHG feature (BatTimeToFullCharge, BatChargingCurrent,
 * ActiveBatChargeFaults, BatCapacity), while the RECHG-MANDATORY
 * BatChargeState is missing entirely. The fix is battery|rechargeable in
 * the config: power_source::create()'s own feature walk
 * (esp_matter_cluster.cpp:988-992) then calls feature::rechargeable::add()
 * (esp_matter_feature.cpp:289-306, guarded on the battery bit being
 * present, which it is), creating BatChargeState and
 * BatFunctionalWhileCharging and making the four gated attributes legal.
 * The rechargeable config defaults (bat_charge_state 0 = kUnknown,
 * bat_functional_while_charging false) are the correct boot answers.
 *
 * The eight battery attributes mirror endpoint.cpp:1954-1961 with the
 * config_t's own defaults (nullable attributes null, BatCapacity 0), all
 * ember-served: no new AT surface rides on them (design spec 3.5). Six of
 * the eight are AT+MTATTR-reachable integers; the two fault lists are
 * lists, so AT+MTATTR answers +MTERR:5 on either, and they ship empty and
 * host-untouched, disclosed there. Their created ranges are NOT the SDK
 * example's: see the B263 note at the create calls below.
 *
 * The DEM triple is variant 0 ONLY, and is OVER-DELIVERY, the heat pump
 * shape: BatteryStorage.xml never requests DEM (its clusters block lists
 * Identify optional, nothing else); the SDK build bolts the triple on
 * anyway (endpoint.cpp:1972-1975). Variant 1 omits it and stays conformant.
 *
 * DISCLOSED XML INCONSISTENCY: BatteryStorage.xml revision 2's summary says
 * "Added mandate of two Power Source and Electrical Sensor composed devices
 * types", but the body encodes that as ONE composed 0x0510 block whose
 * cluster requirements are simply duplicated (EPM twice, EEM twice) and
 * lists no Power Source device type at all. A two-instance mandate is not
 * expressible by duplicating cluster rows inside one block; this endpoint
 * delivers ONE sensor and ONE power source and discloses the departure
 * (design spec 2.3, survey B, ARCHITECTURE.md 8.12). Flag, don't chase.
 */
static endpoint_t *mk_battery_storage(node_t *n, uint8_t variant)
{
    endpoint_t *ep = endpoint::create(n, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    cluster::descriptor::config_t dc;
    cluster_t *desc_cl = cluster::descriptor::create(ep, &dc, CLUSTER_FLAG_SERVER);
    if (desc_cl == nullptr) {
        ESP_LOGE(TAG, "creating descriptor cluster failed (battery storage)");
        return nullptr;
    }
    if (endpoint::add_device_type(ep, battery_storage::get_device_type_id(),
                                  battery_storage::get_device_type_version()) != ESP_OK) {
        ESP_LOGE(TAG, "adding battery storage device type failed");
        return nullptr;
    }
    /* tag_list for parity with the SDK build (endpoint.cpp:1946-1947). */
    cluster::descriptor::feature::tag_list::add(desc_cl);

    /* Power Source 0x0011, battery|rechargeable: the RECHG defect fix (see
     * above). VALIDATE_FEATURES_EXACT_ONE checks Wired against Battery only
     * (esp_matter_cluster.cpp:982), so the extra rechargeable bit passes. */
    power_source::config_t pc;
    pc.power_source.feature_flags = cluster::power_source::feature::battery::get_id() |
                                    cluster::power_source::feature::rechargeable::get_id();
    if (power_source::add(ep, &pc) != ESP_OK) {
        ESP_LOGE(TAG, "power source add failed (battery storage)");
        return nullptr;
    }
    cluster_t *ps_cl = cluster::get(ep, chip::app::Clusters::PowerSource::Id);
    if (ps_cl == nullptr) {
        ESP_LOGE(TAG, "PowerSource cluster missing after add (battery storage)");
        return nullptr;
    }
    /* The eight battery attributes, endpoint.cpp:1954-1961 mirrored with
     * the SDK config's defaults: nullables null (no reading at boot),
     * BatCapacity 0, the fault lists empty.
     *
     * B263: BatVoltage, BatTimeRemaining, BatCapacity, BatTimeToFullCharge
     * and BatChargingCurrent are all XML-typed uint32 with no <constraint>
     * element (PowerSourceCluster.xml:636, 653, 748, 774, 787), so the TYPE
     * IS THE BOUND and their real range is the full uint32 domain. The
     * SDK's own example call caps all five at 0x00..0xFFFF
     * (endpoint.cpp:1954, 1956, 1958, 1959, 1960), a number with no basis
     * in the data model: it is an example's choice, inherited. We follow
     * the XML, not the example, and bound all five 0x00..0xFFFFFFFF.
     * BatCapacity is the reported case, and it is reported because a
     * home-scale pack's nameplate does not fit in 16 bits under any
     * plausible unit; deliberately NOT asserting which unit that is here,
     * because neither PowerSourceCluster.xml nor the zap XML
     * (power-source-cluster.xml:165, plain int32u) carries one. The other
     * four widen with it for the same reason, no XML basis for a narrower
     * bound, rather than leaving a mismatched cap on their siblings.
     * BatPercentRemaining is left at 0..200: that IS the
     * real range, PowerSourceCluster.xml:643-650 constrains it to max 200
     * (half-percent). The two fault lists are left at their SDK-mirrored
     * 0,0: that pair bounds a list's element count (XML maxCount 8 and 16
     * respectively), not a value's range, so B263 does not apply to them.
     *
     * B264 rides along on all of this and is NOT fixed here: whichever
     * bound is created, a write that lands outside it is refused by ember
     * and answers a bare ERROR with no +MTERR code (main.cpp's
     * MT_ATTR_ERR_FAILED through attr_err_to_mterr()'s default arm). See
     * docs/TESTING.md section 12; fixing it needs a baseline re-record. */
    cluster::power_source::attribute::create_bat_voltage(ps_cl, nullable<uint32_t>(), 0x00, 0xFFFFFFFF);
    cluster::power_source::attribute::create_bat_percent_remaining(ps_cl, nullable<uint8_t>(), 0, 200);
    cluster::power_source::attribute::create_bat_time_remaining(ps_cl, nullable<uint32_t>(), 0x00, 0xFFFFFFFF);
    cluster::power_source::attribute::create_active_bat_faults(ps_cl, NULL, 0, 0);
    cluster::power_source::attribute::create_bat_capacity(ps_cl, 0, 0x00, 0xFFFFFFFF);
    cluster::power_source::attribute::create_bat_time_to_full_charge(ps_cl, nullable<uint32_t>(), 0x00, 0xFFFFFFFF);
    cluster::power_source::attribute::create_bat_charging_current(ps_cl, nullable<uint32_t>(), 0x00, 0xFFFFFFFF);
    cluster::power_source::attribute::create_active_bat_charge_faults(ps_cl, NULL, 0, 0);

    if (!mt_graft_electrical_sensor(ep, true, "battery storage")) {
        return nullptr;
    }
    if (variant == 0) {
        if (!mt_add_dem_triple(ep, true, "battery storage")) {
            return nullptr;
        }
    }
    return ep;
}

/*
 * Device Energy Management (0x050D) standalone: device type + DEM cluster +
 * DEM Mode, nothing else, mirroring the SDK's thin add()
 * (esp_matter_endpoint.cpp:1708-1717, which adds no tag_list: parity kept).
 * The XML classifies it a utility device type, stackable on any endpoint;
 * this row is the standalone declaration.
 *
 * Variant 0 declares the ControllableESA condition's minimum:
 * PowerAdjustment via feature::add() (the iron rule, mt_add_dem_triple()'s
 * comment above). Variant 1 is the report-only ESA: FeatureMap 0, which the
 * XML makes conformant for a device NOT declaring ControllableESA (the
 * five-feature min-1 choice applies only under the condition), and DEM Mode
 * stays legal (otherwiseConform: mandatory under ControllableESA, optional
 * otherwise). The delegate is still pooled in variant 1: the five
 * attributes are Instance-served either way, and a chip-tool
 * power-adjust-request against variant 1 answers UNSUPPORTED_COMMAND from
 * the data model (no ember command entry exists), never reaching any
 * delegate.
 */
static endpoint_t *mk_dem(node_t *n, uint8_t variant)
{
    endpoint_t *ep = endpoint::create(n, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    cluster::descriptor::config_t dc;
    if (cluster::descriptor::create(ep, &dc, CLUSTER_FLAG_SERVER) == nullptr) {
        ESP_LOGE(TAG, "creating descriptor cluster failed (device energy management)");
        return nullptr;
    }
    /* The triple adds device type 0x050D itself; here it is the endpoint's
     * primary (and only) device type rather than battery's fourth. */
    if (!mt_add_dem_triple(ep, variant == 0, "device energy management")) {
        return nullptr;
    }
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
    { refrigerator::get_device_type_id(),             mk_refrigerator,            "refrigerator",            0 },
    { oven::get_device_type_id(),                     mk_oven,                    "oven",                    0 },
    { cook_surface::get_device_type_id(),             mk_cook_surface,            "cook_surface",            1 },
    { electrical_sensor::get_device_type_id(),        mk_electrical_sensor,       "electrical_sensor",       1 },
    { electrical_meter::get_device_type_id(),         mk_electrical_meter,        "electrical_meter",        1 },
    { water_heater::get_device_type_id(),             mk_water_heater,            "water_heater",            1 },
    { heat_pump::get_device_type_id(),                mk_heat_pump,               "heat_pump",               0 },
    { solar_power::get_device_type_id(),              mk_solar_power,             "solar_power",             1 },
    { battery_storage::get_device_type_id(),          mk_battery_storage,         "battery_storage",         1 },
    { device_energy_management::get_device_type_id(), mk_dem,                     "device_energy_management", 1 },
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

/*
 * Append-time parenting policy (design spec 2.3). Tasks 3 to 5 grew this
 * function arm by arm alongside the rows each task added, and every id is
 * now read through its namespace accessor, the way this file requires
 * everywhere else (the interim ESP_MATTER_*_DEVICE_TYPE_ID macros task 2
 * started with are all retired).
 */
extern "C" bool mt_devtype_parent_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    (void)variant;
    if (devtype_id == cook_surface::get_device_type_id()) {
        /* a cook surface is only meaningful under a cooktop, and MUST be
         * parented (spec 2.3): parent_devtype 0 (the unparented form) is
         * rejected here too, since no device type id is 0 */
        return parent_devtype == cooktop::get_device_type_id();
    }
    if (devtype_id == temperature_controlled_cabinet::get_device_type_id() &&
        parent_devtype != 0) {
        /* unparented cabinets stay legal (the standalone rows); a parented
         * one only under the two appliances that give it a legal
         * conditional cluster set (mt_cabinet_add_cooler()/
         * mt_cabinet_add_heater() above are that set) */
        return parent_devtype == refrigerator::get_device_type_id() ||
               parent_devtype == oven::get_device_type_id();
    }
    return true; /* generic parenting: PartsList reflects whatever the host declares */
}

extern "C" int mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype,
                                  uint16_t parent_ep_id, uint16_t *out_ep_id)
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

    if (parent_devtype != 0) {
        endpoint_t *parent = endpoint::get(node::get(), parent_ep_id);
        if (parent == nullptr ||
            endpoint::set_parent_endpoint(ep, parent) != ESP_OK) {
            ESP_LOGE(TAG, "parenting 0x%04X under ep %u failed",
                     (unsigned)devtype_id, parent_ep_id);
            return -1;
        }
    }

    /*
     * Parent-conditional clusters (composed appliance round, task 4): a
     * Temperature Controlled Cabinet's cluster set is derived from its
     * parent's device type, never stored (see mt_cabinet_add_cooler()'s
     * comment above). Runs after the parenting call, so the endpoint is
     * already attached to its parent when the clusters land, and before
     * out_ep_id is published, so a failed augment fails the whole create -
     * mt_comp_rebuild() then aborts the composition rather than leaving a
     * commissioned device with a cabinet that answers nothing.
     * An UNPARENTED cabinet (parent_devtype 0) keeps the bare
     * TemperatureControl-only shape earlier compositions declared: this
     * block does not run for it at all.
     */
    if (devtype_id == temperature_controlled_cabinet::get_device_type_id()) {
        bool ok = true;
        if (parent_devtype == refrigerator::get_device_type_id()) {
            ok = mt_cabinet_add_cooler(ep);
        } else if (parent_devtype == oven::get_device_type_id()) {
            ok = mt_cabinet_add_heater(ep);
        }
        if (!ok) {
            ESP_LOGE(TAG, "cabinet conditional clusters under parent 0x%04X failed",
                     (unsigned)parent_devtype);
            return -1;
        }
    }

    *out_ep_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "created %s (0x%04X) variant %u as endpoint %u", e->name, (unsigned)devtype_id,
             (unsigned)variant, *out_ep_id);
    return 0;
}
