/*
 * mt_meter.cpp - the MeterIdentification (0x0B06) Instance pool (energy
 * round C2, task 8).
 *
 * ---- THE GAP THIS FILE FILLS ----
 *
 * Nothing in esp-matter ever constructs a
 * chip::app::Clusters::MeterIdentification::Instance, so the five
 * attributes the cluster advertises (FeatureMap and ClusterRevision aside)
 * have no AttributeAccessInterface behind them and every read answers
 * failure. Verified against the pinned tree, not assumed:
 *
 *   - MatterMeterIdentificationPluginServerInitCallback() is an EMPTY
 *     function body (meter-identification-server.cpp:260), the one
 *     esp-matter itself wires through set_plugin_server_init_callback()
 *     (esp_matter_cluster.cpp:4659-4660, meter_identification::create()).
 *   - ESPMatterMeterIdentificationClusterServerInitCallback(EndpointId) is
 *     DECLARED at zap_common/app/ClusterCallbacks.h:223 with no
 *     implementation and no caller anywhere in the pinned tree.
 *   - cluster::meter_identification::config_t IS common::config_t
 *     (esp_matter_cluster.h:1397), which carries no delegate member, so
 *     there is not even a set_delegate_and_init_callback() hook the way
 *     EnergyEvse or WaterHeaterManagement have.
 *   - The ember codegen name a hand-written app would reach for instead,
 *     emberAfMeterIdentificationClusterInitCallback() (declared at
 *     zap-generated/callback.h:1410, and the shape the upstream reference
 *     app below actually defines), is never reached on THIS firmware
 *     either, but not because its dispatcher does not exist. Fix round 1
 *     correction: an earlier version of this comment claimed
 *     emberAfClusterInitCallback(endpoint, clusterId) (declared
 *     util/generic-callbacks.h:36) "has no definition anywhere in this
 *     pinned tree" and "nothing invokes it for ANY cluster on this SDK
 *     revision". That was wrong: it is defined and called, from
 *     initializeEndpoint() in src/app/util/attribute-storage.cpp:486. The
 *     real reason it never fires here is that attribute-storage.cpp is
 *     never linked into THIS build at all: esp-matter drives endpoint
 *     enable through its own C++ data model provider
 *     (data_model_provider/esp_matter_data_model_provider.cpp, the same
 *     path mt_chime_register_all()'s doc comment in main.cpp cites), not
 *     through the classic ember/zap attribute-storage.cpp machinery.
 *     Checked on the built binary, not assumed from reading source alone:
 *     `riscv32-esp-elf-nm build_b4/ilabs_at_hearth.elf` has no symbol for
 *     either emberAfClusterInitCallback or emAfInitializeAttributes
 *     (attribute-storage.cpp's own distinctive top-level entry point), so
 *     neither the dispatcher nor its caller exists in the firmware that
 *     actually runs. This is a per-build fact, not a permanent SDK one: if
 *     a future Kconfig change ever pulled attribute-storage.cpp into the
 *     link, this reasoning would need re-checking against the binary
 *     again, not assumed to still hold.
 *
 * The chime and EEM disease (ARCHITECTURE.md 8.6, 8.10), a third organ:
 * esp-matter ships the metadata for a cluster and nothing that answers a
 * read for it.
 *
 * ---- THE FIX ----
 *
 * chip::app::Clusters::MeterIdentification::Instance itself is real CHIP
 * core code (meter-identification-server.h), an AttributeAccessInterface
 * the application is expected to construct and Init() by hand; only the
 * reference app that does so
 * (examples/energy-gateway-app/meter-identification/src/
 * MeterIdentificationInstance.cpp, 49 lines) is missing from esp-matter's
 * own tree. This file is that missing piece, adapted twice from the
 * upstream reference:
 *
 *   1. The reference VerifyOrDie()s endpointId == 1 and holds ONE global
 *      std::unique_ptr<Instance>, because its example app hardcodes a
 *      single endpoint. This firmware's endpoint ids are dynamic
 *      (host-declared over AT+MTEP, NVS-persisted) and a composition can
 *      carry more than one meter, so this is a pool of MT_METER_MAX
 *      (mt_matter.h) slots keyed by endpoint id: aligned storage plus
 *      placement-new, the same shape main.cpp's
 *      mt_air_quality_register_all() uses for the identical "Instance has
 *      no default constructor" problem (BitMask<Feature> must be supplied
 *      at construction).
 *   2. Nothing calls the reference's own
 *      emberAfMeterIdentificationClusterInitCallback() either (see above),
 *      so mt_meter_register_all() below is invoked by the firmware itself,
 *      from app_main's pre-esp_matter::start() window: the
 *      mt_chime_register_all() call site and its "why pre-start is safe"
 *      reasoning apply unchanged. Instance::Init() (verified against
 *      meter-identification-server.cpp) only calls
 *      AttributeAccessInterfaceRegistry::Instance().Register(), which needs
 *      the composition's clusters to already exist but touches neither the
 *      CHIP event loop nor a timer, so it is safe ahead of start() for the
 *      same reason the AirQuality and Chime registrations are.
 *
 * ---- NO ATTACH-ORDER TRAP HERE (checked, not assumed) ----
 *
 * This round already recorded one false ordering claim (the EVSE delegate
 * attach comment, corrected in f5a3ad9), and this task's brief says
 * explicitly: verify any ordering requirement against the SDK source before
 * writing it down. There is genuinely none between
 * mk_electrical_utility_meter()'s feature::power_threshold::add() call
 * (mt_devtypes.cpp) and mt_meter_register_all() below, and for a stronger
 * reason than the EVSE case: this Instance's FeatureMap is never read from
 * ember at all. Instance::Read() (meter-identification-server.cpp) answers
 * Attributes::FeatureMap::Id by encoding mFeature, the BitMask<Feature>
 * handed to the constructor, unconditionally; it never queries the ember
 * attribute store the way a hand-set FeatureMap bit would need to.
 * mt_meter_feature_mask() below is the one value both the thunk's
 * feature::power_threshold::add() call and this pool's Instance
 * construction read, so the two cannot disagree, but WHICH of them runs
 * first is immaterial: neither is a snapshot of the other's output.
 *
 * ---- STORAGE ----
 *
 * Same reasoning as AirQuality's s_air_quality_storage (main.cpp): Instance
 * has no default constructor, so a plain array is not an option, and raw
 * aligned storage plus placement-new gives static allocation with no heap
 * churn. UNLIKE AirQuality's pool, which is sized MT_COMP_MAX_ENDPOINTS and
 * indexed directly by composition position, this pool is sized the much
 * smaller MT_METER_MAX (2, deliberate headroom over the one meter endpoint
 * Phase 3 adds, task 3's own note): a live endpoint's composition index can
 * exceed MT_METER_MAX - 1 even when only one or two of the endpoints in the
 * whole composition are meters, so slots are handed out from a separate
 * counter as matching endpoints are found, not by composition index.
 */

#include <esp_log.h>
#include <esp_matter.h>

#include <app/clusters/meter-identification-server/meter-identification-server.h>

#include "mt_matter.h"

static const char *TAG = "mt_meter";

namespace {

using chip::app::Clusters::MeterIdentification::Feature;
using chip::app::Clusters::MeterIdentification::Instance;

struct mt_meter_entry_t {
    bool used;
    uint16_t ep;
    Instance *instance;
};

mt_meter_entry_t s_meter[MT_METER_MAX];

alignas(Instance) uint8_t s_meter_storage[MT_METER_MAX][sizeof(Instance)];

/*
 * How many electrical_utility_meter endpoints the composition rebuild has
 * successfully created so far THIS boot. Fix round 1: this counter is what
 * closes the pool-exhaustion gap review found. AT+MTEP enforces only the
 * whole-composition endpoint cap (MT_COMP_MAX_ENDPOINTS, 28), not a
 * per-devtype limit, so nothing upstream of the thunk stops a host declaring
 * more than MT_METER_MAX (2) meters. Before this counter existed, a third
 * meter's thunk always succeeded, its cluster was created, and
 * mt_meter_register_all() below simply stopped handing out slots once
 * MT_METER_MAX was reached (the `slot < MT_METER_MAX` bound in its loop),
 * silently: no log, no host-visible signal, and the third endpoint's five
 * attributes went back to answering read failures forever, the identical
 * symptom the whole cluster had before this task, just for one endpoint
 * instead of every one.
 *
 * mt_meter_reserve() (below) is called from mk_electrical_utility_meter()
 * (mt_devtypes.cpp) once its MeterIdentification cluster is confirmed to
 * exist, BEFORE the thunk returns success. Reservation and Instance
 * construction happen at two different times for the same reason they
 * always did (construction has to wait for mt_meter_register_all()'s
 * pre-start window, see the file comment above), but the CAPACITY check
 * has to happen at thunk time, not registration time: only a thunk
 * returning nullptr triggers the "abort the whole composition rebuild"
 * path (main.cpp's comp.count = 0 on any mt_devtype_create() failure), the
 * same mechanism mk_energy_evse() relies on for its own delegate pool via
 * mt_matter_evse_delegate_alloc(). A failure discovered only later, inside
 * mt_meter_register_all(), cannot un-create the cluster that already exists
 * on the wire by then.
 *
 * Boot-scoped, deliberately not reset by anything: the composition rebuild
 * this counter tracks runs exactly once per boot (main.cpp's app_main,
 * before esp_matter::start()), so there is no second call to reset it
 * against.
 *
 * Today this is the ONLY creator of a MeterIdentification cluster (grep
 * confirms electrical_utility_meter::add(), esp_matter_endpoint.cpp, is the
 * one call site for cluster::meter_identification::create() anywhere in
 * this firmware or the SDK's endpoint namespaces), so gating in this one
 * thunk is complete coverage as of this task. If a future device type ever
 * also carries this cluster, ITS thunk needs its own mt_meter_reserve()
 * call too: the gate lives per-creator, not in the generic
 * mt_meter_register_all() scan, because only a thunk's return value can
 * abort the rebuild.
 */
uint16_t s_meter_reserved = 0;

} // namespace

extern "C" uint32_t mt_meter_feature_mask(void)
{
    return static_cast<uint32_t>(Feature::kPowerThreshold);
}

extern "C" bool mt_meter_reserve(void)
{
    if (s_meter_reserved >= MT_METER_MAX) {
        return false;
    }
    s_meter_reserved++;
    return true;
}

extern "C" void mt_meter_register_all(void)
{
    chip::BitMask<Feature> features(mt_meter_feature_mask());
    uint16_t slot = 0;

    /*
     * slot < MT_METER_MAX below is now a defensive backstop, not the
     * primary gate: mt_meter_reserve() (above) already bounds how many
     * electrical_utility_meter endpoints mk_electrical_utility_meter()
     * could ever have created this boot to MT_METER_MAX, a composition
     * rebuild that exceeded it would already have aborted before this
     * function ever runs, so this scan can never actually find more than
     * MT_METER_MAX matching endpoints in practice. Kept anyway: cheap, and
     * worth calling out explicitly as defensive rather than leaving a
     * reader to wonder whether it is still load-bearing, the same "cannot
     * happen in practice" labelling main.cpp uses for bounds a prior check
     * has already made unreachable (e.g. the comments at
     * mt_chime_find_slot()'s call sites).
     */
    for (uint16_t i = 0; i < mt_matter_endpoint_count() && slot < MT_METER_MAX; i++) {
        uint32_t devtype;
        uint16_t ep;
        uint8_t variant, parent_idx;
        if (mt_matter_endpoint_info(i, &devtype, &ep, &variant, &parent_idx) != 0) {
            continue;
        }
        (void)devtype;
        (void)variant;
        (void)parent_idx;

        if (esp_matter::cluster::get(ep, chip::app::Clusters::MeterIdentification::Id) == nullptr) {
            continue;
        }

        Instance *inst = new (&s_meter_storage[slot]) Instance(ep, features);
        if (inst->Init() != CHIP_NO_ERROR) {
            /*
             * Logged, not aborted: this is the same shape
             * mt_air_quality_register_all() (main.cpp) already uses for an
             * identical Instance::Init() failure, and it is not the pool-
             * exhaustion gap fixed above. Init() failing here means
             * AttributeAccessInterfaceRegistry::Instance().Register()
             * refused an endpoint/cluster pair this same boot's thunk just
             * created moments earlier, an internal invariant violation with
             * no ordinary host action (unlike AT+MTEP over-declaring
             * meters) that can trigger it, so there is nothing a host could
             * have done differently and no host-facing signal to raise.
             */
            ESP_LOGE(TAG, "MeterIdentification instance init failed for endpoint %u", ep);
            inst->~Instance();
            continue;
        }
        s_meter[slot].used     = true;
        s_meter[slot].ep       = ep;
        s_meter[slot].instance = inst;
        slot++;
    }
}
