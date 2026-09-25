/*
 * mt_devtypes_sl.cpp - the devtype registry, the endpoint block arena, the
 * port-owned external attribute store and dynamic endpoint creation on
 * Silicon Labs (EFR32MG24 / MGM240P).
 *
 * The source is the nRF54L15 port, platform/nrf54l15/port/mt_devtypes_zephyr.cpp
 * in the firmware repository, transferred section by section so the two ports
 * stay diffable. Every section below names the nRF line range it came from;
 * a future round that changes one arm can find the other by that range. The
 * ranges are against that file as it stands on dev/fota-firmware and are a
 * reading aid, not a promise that the file has not moved since.
 *
 * Sections present so far, and what each one left behind:
 *
 *   shared cluster building blocks     nRF  276-320   whole
 *   on/off light (0x0100)              nRF  321-348   whole
 *   temperature sensor (0x0302)        nRF  388-416   whole
 *   dimmable light (0x0101)            nRF  349-387   whole
 *   boolean-state sensors: contact
 *     (0x0015), rain (0x0044), water
 *     freeze (0x0041), water leak
 *     (0x0043)                         nRF  417-479   whole
 *   occupancy sensor (0x0107)          nRF  480-538   whole
 *   humidity sensor (0x0307)           nRF  539-563   whole
 *   pressure sensor (0x0305)           nRF  564-588   whole
 *   light (illuminance) sensor
 *     (0x0106)                         nRF  589-620   whole
 *   flow sensor (0x0306)               nRF  621-644   whole
 *   on/off plug-in unit (0x010A)       nRF  645-670   whole
 *   dimmable plug-in unit (0x010B)     nRF  700-721   whole
 *   color temperature light (0x010C)
 *     and extended color light
 *     (0x010D)                         nRF  745-946   whole
 *   thermostat (0x0301)                nRF  947-1040  whole
 *   fan (0x002B)                       nRF 1041-1100  whole
 *   window covering (0x0202)           nRF 1125-1199  whole
 *   air quality sensor (0x002C)        nRF 1200-1246  whole
 *   door lock (0x000A)                 nRF 1248-1357  whole
 *   water valve (0x0042)               nRF 1358-1484  whole
 *   the parenting policy               nRF 3325-3428  whole (predicate + shape struct)
 *   the registry                       nRF 4402-4641  all 52 rows' identity,
 *                                                     twenty rows' cluster sets
 *   the external attribute store       nRF 4642-4658  whole
 *   the endpoint block arena           nRF 4659-5272  on a bump arena
 *   the compiler-checked floor         nRF 5273-5978  recast, this catalogue
 *   the seed table                     nRF 5979-6889  whole, verbatim
 *   seed_slots()                       nRF 6894-7005  whole but the quiet table
 *   the ember cluster init hook        nRF 7006-7051  the pattern, for
 *                                                     LevelControl not DoorLock
 *   mt_dyn_attr_slot()                 nRF 7052-7069  whole
 *   the mt_devtypes.h quartet          nRF 7189-8413  the twenty ported types
 *   the ember external-attribute hooks nRF 8415-8451  whole
 *
 * THE REGISTRY POLICY OF THIS ROUND, stated once here because it is what
 * makes the file readable. The registry carries all 52 catalogue rows, and
 * every row keeps its IDENTITY: its device type id, its max_variant, and the
 * parenting rule the policy predicate reads for it. That is what
 * mt_devtype_is_known(), mt_devtype_variant_ok() and mt_devtype_parent_ok()
 * answer from, and those three are called by core/mt/mt_at.c on the
 * AT+MTEP= line itself, so the AT surface answers for the whole catalogue
 * exactly as the nRF's does. What a row for an unported type does NOT carry
 * is a cluster set: its ep_type, ep_type_v1, device_types, device_types_v1
 * and shapes are null or empty, and each such row names the nRF batch that
 * will port it. mt_devtype_create() refuses a null-ep_type row at its first
 * check, so a composition naming one is staged and persisted like any other
 * and then fails its rebuild loudly at that entry, with the endpoints before
 * it live as a prefix (AT_MT_SPEC.md 501-506). That is the same
 * stop-at-failure semantics every other create failure has, and it is why
 * the unported rows are kept rather than deleted: deleting them would make
 * AT+MTEP=0x000A answer +MTERR:6 (unknown device type), which is a different
 * and wrong statement about a product whose wire contract names all 52.
 *
 * TWO SECTIONS OF THE nRF FILE ARE DELIBERATELY ABSENT, both because every
 * consumer they have belongs to an unported device type:
 *
 *   - The DE407 quiet table (nRF 4924-4990, kQuietNoSlot and
 *     attr_quiet_no_slot): every row in it is an
 *     ElectricalPowerMeasurement, DeviceEnergyManagement,
 *     WaterHeaterManagement or EnergyEvse 64-bit declaration. No list in
 *     this file declares an over-wide scalar, so seed_slots()'s shout is
 *     unconditional here, which is the ruling's intent: the table silences
 *     the shout one proven pair at a time and never by type, so it arrives
 *     with the batch that declares the first such pair.
 *   - The type-conditional trailing stores (nRF 5032-5197, kStoreWalk,
 *     store_walk, store_offset and the four sizeof/alignof assertions).
 *     Mode select, chime, the ModeBase families and the TemperatureLevel
 *     cabinet are the only device types that carry one. store_bytes() stays
 *     as the one place the block layout asks the question, so the batch that
 *     ports the first store-bearing type adds a TABLE and not a layout.
 *
 * The cluster-object arena lives in port/mt_matter_sl.cpp (the
 * hearth_arena block at the top of the file), and its budget is pinned by
 * DE541 to sixteen valve delegates.
 */

#include <app/util/attribute-storage.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app-common/zap-generated/callback.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <lib/core/DataModelTypes.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/interaction_model/StatusCode.h>

#include <stddef.h>
#include <string.h>

extern "C" {
#include "mt_composition.h"
#include "mt_devtypes.h"
/* For mt_air_quality_feature_mask(), the single accessor the AirQuality
 * FeatureMap seed reads instead of transcribing the bits again. */
#include "mt_matter.h"
}
#include "hearth_log.h"
#include "mt_dyn_store.h"
#include "mt_port_ids.h"

using namespace chip;
using namespace chip::app::Clusters;

using chip::Protocols::InteractionModel::Status;

/*
 * Acceptance versus capacity, checked by the compiler (nRF 133-170).
 *
 * These two constants used to be one. MT_COMP_MAX_ENDPOINTS (28) is what
 * the AT wire contract lets a host DECLARE over AT+MTEP, and it is core, so
 * it is the same on both platforms. kServiceableEndpoints (16) is what this
 * build can stand up and SERVE at once, and it is a port decision; see
 * mt_port_ids.h for the full reasoning.
 *
 * Two invariants follow, and both are asserted rather than trusted:
 *
 *   1. src/CHIPProjectConfig.h must still mirror kServiceableEndpoints. It
 *      cannot include this header (CHIP pulls it into C translation units
 *      everywhere), so the literal is duplicated there and tied here.
 *   2. Capacity must never exceed acceptance. If it did, this build would
 *      stand up endpoints the composition store cannot even describe, and
 *      s_dyn would out-run mt_composition_t's own arrays.
 *
 * The reverse (capacity BELOW acceptance) is the normal, intended state and
 * is not an error: a host may declare 28, and a stored composition longer
 * than 16 fails its rebuild loudly at the seventeenth endpoint. The abort is
 * STOP-AT-FAILURE, not roll-back (AT_MT_SPEC.md 501-506): the sixteen
 * endpoints created before it stay live as a prefix with their ids
 * unchanged, and the failed entry and everything after it are simply absent.
 * What the rule exists to prevent is a RENUMBERED model, not a partial one:
 * skipping the failed entry and continuing would shift every later endpoint
 * down by one and hand a commissioned controller a silently different data
 * model (design spec 12.1).
 */
static_assert(CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT == kServiceableEndpoints,
              "src/CHIPProjectConfig.h must mirror kServiceableEndpoints (mt_port_ids.h)");
static_assert(kServiceableEndpoints <= MT_COMP_MAX_ENDPOINTS,
              "serviceable capacity cannot exceed the composition the AT contract accepts");

/*
 * HEARTH_DECLARE_CONST_*: the SDK's DECLARE_DYNAMIC_* macros with const
 * added, and nothing else changed (nRF 172-275).
 *
 * WHY THEY EXIST. CHIP declares the dynamic-endpoint metadata arrays
 * without const:
 *
 *   attribute-storage.h:44  #define DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(n) \
 *                               EmberAfCluster n[] = {
 *   attribute-storage.h:55  #define DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(n) \
 *                               EmberAfAttributeMetadata n[] = {
 *   attribute-storage.h:41  #define DECLARE_DYNAMIC_ENDPOINT(n, cl) \
 *                               EmberAfEndpointType n = { cl, ... }
 *
 * so a port's whole device-type catalogue lands in .data: resident in RAM at
 * all times whatever the host composed, and paid for twice, since .data also
 * carries an initialiser image in flash. The nRF arm measured 8,740 B of RAM
 * across 120 symbols for its 41 device types (2026-08-30, at 6c31f09) before
 * making the change; this file's catalogue is twenty device types deep and
 * therefore saves little today, and carries the macros anyway so the batches
 * that grow the catalogue do not have to rediscover the problem. The ZAP-generated
 * equivalents for the two fixed endpoints are already const and sit in
 * .rodata, so this is a declaration accident, not a requirement.
 *
 * WHAT MUST STAY TRUE FOR THEM TO BE SAFE. Nothing may write to endpoint,
 * cluster or attribute metadata at runtime. The type chain already says so:
 * emberAfSetDynamicEndpoint() takes const EmberAfEndpointType *
 * (attribute-storage.h:277); EmberAfDefinedEndpoint::endpointType is
 * const EmberAfEndpointType * (af-types.h); EmberAfEndpointType::cluster is
 * const EmberAfCluster *; EmberAfCluster::attributes is
 * const EmberAfAttributeMetadata *, as are its command and event lists. The
 * chain is const-correct end to end, so this needs no SDK patch and is not a
 * workaround for one. The nRF's memory-reclaim round searched its tree for
 * const_cast and C-style casts to non-const metadata pointers and found
 * none; the only const-stripping cast that touches metadata at all is in
 * emAfLoadAttributeDefaults(), which casts &am->defaultValue and hands it to
 * emAfReadOrWriteAttribute() as the SOURCE buffer, inside
 * `if (!am->IsExternal())`, which every attribute row in this file fails.
 * That branch already runs against the const ZAP tables in .rodata for this
 * image's two fixed endpoints on every boot without faulting, which is
 * empirical proof rather than an argument.
 *
 * THE STANDING CONDITION. EVERY attribute row in every table in this file
 * must carry ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE). Today every row gets it
 * for free: DECLARE_DYNAMIC_ATTRIBUTE() ORs it in unconditionally
 * (attribute-storage.h:73-77, the OR itself at :76) and so does the
 * cluster-revision row that ..._LIST_END() appends (the macro is :57-61 and
 * the row's EXTERNAL_STORAGE is on :59), and this file contains no hand-rolled
 * EmberAfAttributeMetadata row at all, so the condition holds
 * SYNTACTICALLY here. It does not hold that way on the nRF arm, which has
 * two hand-rolled MIN_MAX rows (the BatPercentRemaining entries in its two
 * power source lists) that spell EXTERNAL_STORAGE out by hand and must keep
 * it: a row that lost the flag would take the ptrToMinMaxValue arm of the
 * defaults path straight into a const table in .rodata. The batch that ports
 * the power source (nRF batch 4) brings those rows here and with them the
 * weaker, discipline-only form of this condition. Read this paragraph again
 * before adding a non-EXTERNAL attribute row to a const table; nothing in
 * the compiler will catch it, and the fix is to put that table back on
 * DECLARE_DYNAMIC_* and out of .rodata.
 *
 * The DataVersion storage (s_dyn's per-endpoint versions) is written by
 * CHIP and is deliberately NOT const.
 *
 * The END macros carry no type and are the SDK's, reused unchanged; they
 * are wrapped only so the call sites read symmetrically.
 */
#define HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(attrListName)                                    \
    const EmberAfAttributeMetadata attrListName[] = {
#define HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END() DECLARE_DYNAMIC_ATTRIBUTE_LIST_END()

#define HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(clusterListName)                                   \
    const EmberAfCluster clusterListName[] = {
#define HEARTH_DECLARE_CONST_CLUSTER_LIST_END DECLARE_DYNAMIC_CLUSTER_LIST_END

#define HEARTH_DECLARE_CONST_ENDPOINT(endpointName, clusterList)                                   \
    const EmberAfEndpointType endpointName = { clusterList, MATTER_ARRAY_SIZE(clusterList), 0 }

namespace {

#include "mt_devtypes_sl_tables_core.inc"
#include "mt_devtypes_sl_tables_b2.inc"
#include "mt_devtypes_sl_tables_b3.inc"
#include "mt_devtypes_sl_registry.inc"
#include "mt_devtypes_sl_arena.inc"
#include "mt_devtypes_sl_seeds.inc"

} /* namespace */

/* ---- the ember cluster init hook (nRF 7006-7051) ---------------------- */

/*
 * Strong override of the generated weak stub in
 * zap-generated/app/callback-stub.cpp:65. THE ONE INITIALISATION A DYNAMIC
 * ENDPOINT DOES NOT GET FOR FREE, and catalogue batch 1's bench proof is what
 * found it: every MoveToLevel on a dimmable light or plug was clamped to 0.
 *
 * THE MECHANISM, because it is the same trap the occupancy sensor comment
 * above describes and the two must be read together. level-control.cpp keeps a
 * private EmberAfLevelControlState per endpoint in stateTable, sized
 * MATTER_DM_LEVEL_CONTROL_CLUSTER_SERVER_ENDPOINT_COUNT +
 * CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT, so a dynamic endpoint does get a
 * slot and getState() answers non-null. The ONLY writer of that struct's
 * minLevel and maxLevel is emberAfLevelControlClusterServerInitCallback()
 * (level-control.cpp:1465), and that is a per-cluster INIT FUNCTION, reached
 * through the "functions" array on EmberAfCluster that DECLARE_DYNAMIC_CLUSTER
 * hardcodes to NULL. So the struct kept its static zero fill, and
 * moveToLevelHandler()'s `if (state->maxLevel <= level)` made every target 0,
 * with the endpoint's own MinLevel and MaxLevel attributes reading 1 and 254
 * correctly over the wire the whole time: those are ember attributes in this
 * file's arena, the state struct is a second, private copy.
 *
 * OccupancySensing survives the identical gap because its seed row writes
 * ATTRIBUTES and ember serves them. LevelControl cannot, because the value
 * that decides the move is not an attribute.
 *
 * THE HOOK. attribute-storage.cpp:486 calls emberAfClusterInitCallback() for
 * every cluster of every endpoint, dynamic ones included, one line before it
 * looks for the functions array it will not find. The generated
 * zap-generated/app/cluster-init-callback.cpp:49-50 dispatches LevelControl's
 * to the weak emberAfLevelControlClusterInitCallback(EndpointId) below. That
 * is the hook an application is expected to define, and it is exactly the one
 * the nRF arm uses for its DoorLock init (mt_devtypes_zephyr.cpp 7006-7051),
 * so this is the arms' shared pattern rather than a Silabs workaround. That
 * dispatch is generated from the ZAP's enabled cluster set on endpoint 240:
 * Task 2 enabling LevelControl there is what created the `case
 * LevelControl::Id` in cluster-init-callback.cpp, so a cluster that needs an
 * init override must stay enabled on the catalogue endpoint, or the override
 * is never called and B525 returns silently.
 *
 * ENDPOINT 240 RETURNS AT ONCE. The catalogue endpoint is ZAP-declared, so
 * endpoint_config.h wires it a real functions array and
 * attribute-storage.cpp:487-491 runs the server init for it on the next line.
 * Calling it here as well would run it twice for no gain, and the second run
 * would re-apply the StartUp behaviour to an endpoint that has already applied
 * it.
 *
 * THE GENERIC ALTERNATIVE IS NOT TAKEN. mt_devtype_create() could walk every
 * created endpoint's cluster list and call each cluster's server init after
 * emberAfSetDynamicEndpoint() returns, which would cover every future
 * cluster at once. It is not done, for the reason this file gives for every
 * other "carried ahead of need" decision in reverse: a server init is not a
 * uniform thing. Some read a Delegate this port never sets (calling those is
 * a null dereference, which is the chime's disease on the C6), some register
 * scene handlers, some are already run by a code-driven path, and there is no
 * table that says which. One override per cluster that provably needs one,
 * each with the evidence beside it, is the form that cannot go wrong quietly.
 * The batch that ports a second such cluster adds a second function here.
 *
 * WHAT IT READS, and why the seeds are already there. The server init reads
 * MinLevel (seed 0x01), MaxLevel (seed 0xFE), the FeatureMap's Lighting bit
 * (seed 0x03, so the lighting clamps to 0x01 and 0xFE apply and change
 * nothing), CurrentLevel (seed 0xFE) and StartUpCurrentLevel (seed 0xFF, the
 * null sentinel). Every one of those is in place before this runs:
 * mt_devtype_create() calls seed_slots() and sets d.used BEFORE
 * emberAfSetDynamicEndpoint(), precisely so the cluster inits can read
 * attributes straight back through the external-storage callbacks. The
 * comment at that call site says so; this function is the first caller that
 * depends on it.
 *
 * WHAT IT WRITES, and why no +MTATTR fires. Our attribute rows are all
 * EXTERNAL_STORAGE, so emberAfIsKnownVolatileAttribute() is false for both
 * CurrentLevel and StartUpCurrentLevel (util.cpp:153-162 returns
 * !IsAutomaticallyPersisted() && !IsExternal()) and the StartUp block DOES
 * run. StartUpCurrentLevel is null, which the spec reads as "keep the previous
 * value", so the block writes CurrentLevel back as the 0xFE it just read; the
 * final min/max clamp then finds 254 inside [1, 254] and writes nothing more.
 * The seeds were chosen to agree with the init for exactly this reason. And
 * even that one same-value write cannot raise a URC: it happens inside
 * emberAfSetDynamicEndpoint() during the boot rebuild, before mt_at_start()
 * sets s_at_up, and mt_at_urc() drops anything raised before then.
 *
 * ON/OFF NEEDS NO EQUIVALENT, and that is a finding, not an omission.
 * emberAfOnOffClusterServerInitCallback() is one line,
 * OnOffServer::initOnOffServer(), whose whole body is the StartUpOnOff
 * power-up behaviour (on-off-server.cpp:492-537). It caches nothing: OnOff,
 * OnTime, OffWaitTime and GlobalSceneControl are all ember attributes this
 * file declares, so every command handler reads the same bytes the AT surface
 * does. Our StartUpOnOff seed is 0xFF, the null that means "previous value",
 * so the init would be a no-op even if it ran. The on/off light has passed
 * the harness Phase 2 control rows since round 2 task 5 without it, which is
 * the empirical half of the same statement.
 */
void emberAfLevelControlClusterInitCallback(EndpointId endpoint)
{
    if (endpoint == kCatalogueEndpointId) {
        return;
    }
    emberAfLevelControlClusterServerInitCallback(endpoint);
}

/*
 * The second half of the pattern above, and the batch the LevelControl
 * comment's closing line ("The batch that ports a second such cluster adds
 * a second function here") was waiting for. The dispatch it rides on is the
 * generated `case ColorControl::Id` in
 * zap-generated/app/cluster-init-callback.cpp:28-29 (Task 1's enabling of
 * ColorControl on the catalogue endpoint created it, exactly as
 * LevelControl's enabling created the `case LevelControl::Id` the first
 * override rides on), landing in the weak
 * emberAfColorControlClusterInitCallback() in
 * zap-generated/app/callback-stub.cpp:30 that this definition overrides.
 *
 * WHY THIS ONE JOINS THE HOOK. ColorControl's ServerInit is the one in this
 * batch with observable work behind it, the same test that put LevelControl
 * here: emberAfColorControlClusterServerInitCallback()
 * (color-control-server.cpp:3291) runs startUpColorTempCommand()
 * (:2577-2624), which applies a non-null StartUpColorTemperatureMireds to
 * ColorTemperatureMireds and FORCES ColorMode/EnhancedColorMode to
 * kColorTemperatureMireds.
 *
 * WHAT IT ACTUALLY DOES ON THIS BUILD. StartUpColorTemperatureMireds is
 * seeded 250, non-null (the seed rows above and the seed comment there), so
 * on every dynamic colour endpoint's creation the command takes its
 * indexed branch and writes quietTemperatureMireds[getEndpointIndex(endpoint)]
 * (:2609-2612), ColorTemperatureMireds, ColorMode and EnhancedColorMode. The
 * indexed write is unbounded in the SDK. The value it writes equals the seed
 * (250), so the observable result is unchanged, but the indexed write runs:
 * a reader who stops at "it is a no-op" would conclude the unbounded path is
 * never reached, and it is. The reason it is called at all is unchanged: the
 * moment the StartUp seed changes or a persisted value differs, a dynamic
 * endpoint that never ran it boots in the wrong color mode, and that is
 * exactly the class of bug B388 was.
 *
 * WHY THE INDEXED WRITE IS SAFE HERE. emberAfEndpointEnableDisable()
 * (attribute-storage.cpp:991-1003) sets the endpoint's isEnabled flag
 * BEFORE it calls initializeEndpoint(), which calls
 * emberAfClusterInitCallback() for every cluster (attribute-storage.cpp:486).
 * By the time this callback runs, the endpoint is already enabled, so
 * emberAfGetClusterServerEndpointIndex() (attribute-storage.cpp:959-964)
 * resolves a dynamic endpoint to
 * fixedClusterServerEndpointCount + (epIndex - FIXED_ENDPOINT_COUNT): this
 * build has one fixed server cluster entry (gen_config.h:111) and the fixed
 * endpoint count is 2, so a dynamic endpoint's index is 1 + (epIndex - 2),
 * and the largest serviceable dynamic endpoint is 17 (kServiceableEndpoints
 * = 16, mt_port_ids.h:63), so the index is at most 16. That is below
 * kColorControlClusterServerMaxEndpointCount = 1 + 16 = 17
 * (color-control-server.h:293, CHIPProjectConfig.h:34), the size of every
 * per-endpoint array startUpColorTempCommand() writes. The bound holds
 * because the port's own arena can create no more than kServiceableEndpoints
 * dynamic endpoints; the SDK does not enforce it, so a build that widens the
 * arena past 16 without widening the cluster config overflows the array.
 *
 * The batch's other clusters do not join: Thermostat's ServerInit is an
 * empty TODO body, FanControl and AirQuality have none at all, and
 * WindowCovering's only per-endpoint state is the delegate table this port
 * never populates.
 *
 * Endpoint 240 returns at once for the same reason the LevelControl one
 * does: it is ZAP-declared, so endpoint_config.h wires it a real functions
 * array (the ServerInit is in the array at endpoint_config.h:437) and the
 * server init already runs for it through the functions path; calling it
 * here as well would re-apply the StartUp behaviour twice.
 */
void emberAfColorControlClusterInitCallback(EndpointId endpoint)
{
    if (endpoint == kCatalogueEndpointId) {
        return;
    }
    emberAfColorControlClusterServerInitCallback(endpoint);
}

/*
 * DoorLock's per-endpoint state is the SDK's own mEndpointCtx array,
 * filled only by DoorLockServer::InitEndpoint(), which
 * DECLARE_DYNAMIC_CLUSTER's null functions array never reaches for a
 * dynamic endpoint; the generated dispatch
 * (zap-generated/app/cluster-init-callback.cpp) sends DoorLock's init
 * here. The result is parked in the two statics below because this
 * callback returns void and mt_devtype_create() reads it back after
 * emberAfSetDynamicEndpoint(). The statics sit in this file's
 * anonymous namespace, the shape the functions around the
 * ColorControl hook take. Endpoint 240's functions array runs no init at
 * all: door-lock-cluster.xml declares the server with init="false"
 * (door-lock-cluster.xml:44), so ZAP puts no init function in the array and
 * the array never calls InitEndpoint for it, the fact the audit note above
 * records. The early return keeps because endpoint 240 is disabled at boot
 * (src/main.cpp:133) and a disabled endpoint needs no lock context; the nRF
 * hook has no early return.
 */
static EndpointId s_lock_init_ep = kInvalidEndpointId;
static CHIP_ERROR s_lock_init_err = CHIP_NO_ERROR;

void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
    if (endpoint == kCatalogueEndpointId) {
        return;
    }
    s_lock_init_ep = endpoint;
    s_lock_init_err = DoorLockServer::Instance().InitEndpoint(endpoint);
    if (s_lock_init_err != CHIP_NO_ERROR) {
        HEARTH_LOGE("devtypes", "DoorLock InitEndpoint(%u) failed: %" CHIP_ERROR_FORMAT,
                    (unsigned)endpoint, s_lock_init_err.Format());
    }
}

/* nRF 7052-7069. Contract, including the locking rules, in mt_dyn_store.h. */
bool mt_dyn_attr_slot(EndpointId ep, ClusterId cluster, AttributeId attr, uint8_t **data,
                      uint8_t *size)
{
    for (auto &d : s_dyn) {
        if (!d.used || d.ep_id != ep) {
            continue;
        }
        attr_slot *slots = block_slots(d);
        for (uint16_t i = 0; i < d.slot_count; i++) {
            if (slots[i].cluster == cluster && slots[i].attr == attr) {
                *data = slots[i].data;
                *size = slots[i].size;
                return true;
            }
        }
    }
    return false;
}

/*
 * The endpoint arena's occupancy, on the console, once per boot. Called by
 * src/main.cpp's rebuild_composition() after the loop, so the line is
 * beside the "composition rebuilt: N endpoint(s)" it explains.
 *
 * Handed out plus free equals the arena's usable bytes exactly, which is the
 * shape the nRF's capacity logs were given in its fix round M1 after a
 * payload sum and a chunk-rounded free figure could not be reconciled on one
 * line. Here the two are trivially reconcilable, because the cost model is
 * rounding alone.
 */
void mt_dyn_arena_report(void)
{
    HEARTH_LOGI("devtypes", "endpoint arena: %u of %u B handed out, %u B free, %u of %u "
                            "serviceable endpoints live",
                (unsigned)s_ep_arena.used, (unsigned)s_ep_arena.cap,
                (unsigned)(s_ep_arena.cap - s_ep_arena.used), (unsigned)live_endpoints(),
                (unsigned)kServiceableEndpoints);
}

/* ---- mt_devtypes.h (nRF 7189-8413) ------------------------------------ */

extern "C" bool mt_devtype_is_known(uint32_t devtype_id)
{
    for (auto &e : s_registry) {
        if (e.id == devtype_id) {
            return true;
        }
    }
    return false;
}

extern "C" bool mt_devtype_variant_ok(uint32_t devtype_id, uint8_t variant)
{
    for (auto &e : s_registry) {
        if (e.id == devtype_id) {
            return variant <= e.max_variant;
        }
    }
    return false;
}

/*
 * The rule itself is parent_policy_ok() beside the registry, constexpr so
 * the shape maps can be checked against it at compile time; this is only the
 * extern "C" door core/mt/mt_at.c knocks on, called on the AT+MTEP= line
 * itself (mt_at.c:1897-1905) so a bad pairing answers +MTERR:1 on the line
 * that proposed it and consumes no staging slot.
 */
extern "C" bool mt_devtype_parent_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    return parent_policy_ok(devtype_id, variant, parent_devtype);
}

/* Does this endpoint type carry cluster id as a server? nRF type_has_cluster. */
static bool type_has_cluster(const EmberAfEndpointType *ep_type, ClusterId id)
{
    for (uint8_t i = 0; i < ep_type->clusterCount; i++) {
        if (ep_type->cluster[i].clusterId == id) {
            return true;
        }
    }
    return false;
}

extern "C" int mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype,
                                 uint16_t parent_ep_id, uint16_t *out_ep_id)
{
    if (!out_ep_id) {
        return -1;
    }

    /* The lock covers the whole function, so every s_dyn, s_ep_arena and
     * s_next_ep_id mutation happens under it and the invariant is statable:
     * the dynamic endpoint header table, the endpoint block arena and the
     * blocks themselves are only ever touched with the CHIP stack lock held.
     * emberAfSetDynamicEndpoint() below enables the endpoint synchronously
     * and runs cluster init callbacks that read this endpoint's block back
     * through the external-storage callbacks, so the two must not be
     * lockable separately.
     *
     * On this platform the lock is not advisory: SL_MATTER_STACK_LOCK_-
     * TRACKING_MODE is FATAL (the extension's own slc/config/
     * sl_matter_config.h), so an unlocked CHIP call from the boot task would
     * kill the device rather than race quietly. */
    chip::DeviceLayer::StackLock lock;

    const hearth_devtype *type = nullptr;
    for (auto &e : s_registry) {
        if (e.id == devtype_id) {
            type = &e;
            break;
        }
    }
    if (!type) {
        return -1;
    }

    /*
     * THE UNPORTED-TYPE ARM, and the one place this port's create differs
     * from the nRF's in kind rather than in scale.
     *
     * A row with neither an ep_type nor a shape map is a catalogue device
     * type whose IDENTITY this build knows (so AT+MTEP= accepted it, its
     * variant range was checked against the real max_variant, and its
     * parenting rule was enforced) and whose CLUSTER SET this build does not
     * declare. There is nothing to stand up, and the failure has to happen
     * HERE, before an endpoint id, a header entry or a block is spent, for
     * the same reason every pool check on the nRF aborts early: a
     * half-created endpoint is worse than none.
     *
     * The rebuild loop treats this exactly as it treats any other create
     * failure: abort, keep the prefix, do not renumber. So a host that
     * declared a door lock on a build that does not serve one gets a
     * truncated, honest AT+MTEP? rather than a lock endpoint that answers
     * nothing, and the console says which device type stopped it.
     */
    if (type->ep_type == nullptr && type->shapes.empty()) {
        HEARTH_LOGE("devtypes", "devtype 0x%04X is in this build's registry but has no cluster "
                                "set: the catalogue batch that builds it has not been ported "
                                "yet, so the composition naming it cannot be rebuilt",
                    (unsigned)devtype_id);
        return -1;
    }

    /* Re-checked here, not just at AT+MTEP staging time: a composition blob
     * persisted by a wider build can name a variant this build does not
     * implement, and silently dropping it would hand a commissioned device
     * an endpoint that is not what its stored composition says. Fail, and
     * let the rebuild loop abort. */
    if (variant > type->max_variant) {
        HEARTH_LOGE("devtypes", "devtype 0x%04X variant %u not supported by this build",
                    (unsigned)devtype_id, (unsigned)variant);
        return -1;
    }

    /*
     * The variant's own cluster list. A max_variant-1 row carries a second
     * declared EmberAfEndpointType (ep_type_v1, the registry comment)
     * because this port cannot tear a cluster out of a built list at
     * runtime; every walk below and every block-layout accessor later reads
     * this chosen list, never type->ep_type directly.
     *
     * A row whose cluster set depends on its PARENT carries a shape map
     * instead, and the pair (variant, parent_devtype) selects from it. The
     * map's domain is proven equal to parent_policy_ok()'s accept set at
     * compile time (shape_domain_matches_policy() beside the registry), so
     * the no-match arm below cannot be reached by any composition AT+MTEP
     * accepted; it is the drift alarm for the day someone edits one encoding
     * and not the other, and it aborts rather than falling back, because a
     * fallback would serve a cabinet whose stored composition says something
     * else. No row carries a shape map in this build (the registry comment);
     * the arm is carried so the batch that adds one adds a table only.
     *
     * A DELIBERATE DIVERGENCE FROM THE C6, and the better half of it. The C6
     * builds the base endpoint, calls set_parent_endpoint(), and only THEN
     * augments a composed child with its parent-conditional clusters, so a
     * failed augment leaves a half-built child already attached in the
     * parent's tree. This port SELECTS the whole cluster set here, before
     * emberAfSetDynamicEndpoint() is called at all, so a composed endpoint
     * appears complete or does not appear.
     */
    const EmberAfEndpointType *ep_type = nullptr;
    if (!type->shapes.empty()) {
        for (auto &s : type->shapes) {
            if (s.variant == variant && s.parent_devtype == parent_devtype) {
                ep_type = s.ep_type;
                break;
            }
        }
        if (ep_type == nullptr) {
            HEARTH_LOGE("devtypes",
                        "devtype 0x%04X variant %u under parent 0x%04X has no declared cluster "
                        "set; the shape map and mt_devtype_parent_ok() have drifted apart",
                        (unsigned)devtype_id, (unsigned)variant, (unsigned)parent_devtype);
            return -1;
        }
    } else {
        ep_type = (variant == 1 && type->ep_type_v1 != nullptr) ? type->ep_type_v1 : type->ep_type;
    }

    /*
     * THE PER-ENDPOINT DELEGATE HANDOUT, for the one family batch 3 brings,
     * is in this function below. The nRF's create (7405-7900) claims a slot
     * from one of fourteen pools before anything is spent, and registers the
     * Delegate or Instance after a successful emberAfSetDynamicEndpoint(),
     * for every device type whose cluster server needs one object per
     * endpoint: the valve, the OperationalState trio, chime, mode select,
     * the ModeBase families, the measurement clusters, DEM, WHM,
     * MeterIdentification, the microwave's three-way construction order and
     * the EVSE. Since catalogue batch 3 the water valve draws a delegate from
     * the cluster-object arena through the claim below and the second half
     * below; the other delegate families (the OperationalState trio, chime,
     * mode select and the rest of the nRF list) arrive with batch 4. The
     * types this build does construct, door lock and water valve included,
     * serve OnOff, LevelControl, ColorControl, Thermostat, FanControl,
     * WindowCovering, AirQuality, TemperatureMeasurement,
     * OccupancySensing, RelativeHumidityMeasurement, PressureMeasurement,
     * IlluminanceMeasurement, FlowMeasurement, BooleanState and
     * ValveConfigurationAndControl from ember storage and from CHIP's own
     * registered cluster objects, the valve's per-endpoint delegate being
     * the one exception.
     *
     * The B388 cluster-init call site is absent because this port does not
     * need one. The nRF calls
     * emberAfLevelControlClusterServerInitCallback(),
     * emberAfColorControlClusterServerInitCallback() and
     * emberAfModeSelectClusterServerInitCallback() by hand after a successful
     * create, because DECLARE_DYNAMIC_CLUSTER hardcodes functions=NULL and
     * the per-endpoint ServerInit callbacks a fixed endpoint gets through
     * GENERATED_FUNCTION_ARRAYS never run for a dynamic one. That matters
     * for a cluster whose ServerInit caches per-endpoint state (LevelControl
     * caches Min/MaxLevel, and without the call every transition clamps to
     * 0); it is a no-op for OnOff and TemperatureMeasurement, neither of
     * which has a ServerInit worth running. This port runs the LevelControl
     * and ColorControl server inits it needs from the strong
     * emberAfLevelControlClusterInitCallback() and
     * emberAfColorControlClusterInitCallback() overrides in this file (the
     * hook section above), which the generated dispatch in
     * zap-generated/app/cluster-init-callback.cpp reaches for dynamic
     * endpoints the same way it reaches them for fixed ones, so there is no
     * call site to carry. ModeSelect's init, the third one the nRF calls by
     * hand, arrives with batch 4's mode select, the same shape the
     * ColorControl override already sets.
     */

    /*
     * The two capacity limits, checked in order and both named in whichever
     * log fires. They are different resources and a composition can exhaust
     * either one first, so an integrator reading the log needs to know which
     * wall was hit and how far away the other one was.
     */
    uint16_t index = 0;
    while (index < kServiceableEndpoints && s_dyn[index].used) {
        index++;
    }
    if (index == kServiceableEndpoints) {
        HEARTH_LOGE("devtypes", "devtype 0x%04X: all %u serviceable endpoints in use (endpoint "
                                "arena %u of %u usable B used); host may declare %u, this build "
                                "serves %u",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)s_ep_arena.used, (unsigned)kArenaUsableBytes,
                    (unsigned)MT_COMP_MAX_ENDPOINTS, (unsigned)kServiceableEndpoints);
        return -1;
    }

    /* Sized for THIS device type, not for the widest in the catalogue. */
    const uint16_t n_clusters = ep_type->clusterCount;
    const uint16_t n_slots = count_slots(ep_type);
    const size_t base = block_bytes(n_clusters, n_slots);
    const size_t store = store_bytes(ep_type);
    const size_t want = base + store;

    /* The valve's delegate is claimed before any endpoint memory is spent,
     * so an exhausted cluster-object arena refuses the create cleanly (nRF
     * mt_devtypes_zephyr.cpp 7862-7883). A claim stranded by a later failure
     * in this function is bounded at one per boot: the rebuild stops at the
     * first failure and the bump arena cannot free. */
    void *valve_delegate = nullptr;
    if (type_has_cluster(ep_type, ValveConfigurationAndControl::Id)) {
        valve_delegate = mt_matter_valve_delegate_alloc();
        if (valve_delegate == nullptr) {
            HEARTH_LOGE("devtypes", "devtype 0x%04X: valve delegate unavailable: the "
                                    "cluster-object arena or the valve cap (kServiceableEndpoints) is "
                                    "exhausted; %u of %u serviceable endpoints in use",
                        (unsigned)devtype_id, (unsigned)live_endpoints(),
                        (unsigned)kServiceableEndpoints);
            return -1;
        }
    }

    void *block = hearth_arena_alloc(s_ep_arena, want, "endpoint block arena");
    if (block == nullptr) {
        /* hearth_arena_alloc() has already said what was wanted and what was
         * left; this line says what it was wanted FOR, and names the other
         * wall, the nRF's both-walls rule. Every figure reconciles: cost plus
         * handed out plus free equals the usable total. */
        HEARTH_LOGE("devtypes", "devtype 0x%04X: endpoint block costing %u B (payload %u: %u "
                                "clusters, %u slots, %u store B) does not fit; %u of %u usable B "
                                "handed out, %u B free; this is endpoint %u of %u serviceable",
                    (unsigned)devtype_id, (unsigned)hearth_arena_cost(want), (unsigned)want,
                    (unsigned)n_clusters, (unsigned)n_slots, (unsigned)store,
                    (unsigned)s_ep_arena.used, (unsigned)kArenaUsableBytes,
                    (unsigned)(kArenaUsableBytes - s_ep_arena.used), (unsigned)(index + 1),
                    (unsigned)kServiceableEndpoints);
        return -1;
    }

    /* The nRF constructs each block's type-conditional trailing store here,
     * with placement new, before the endpoint can be served. store_bytes()
     * is 0 for every device type this build declares, so there is nothing to
     * construct; the batch that ports a store-bearing type brings the
     * constructions back with the table. */

    dyn_endpoint &d = s_dyn[index];
    d.type = type;
    d.ep_type = ep_type;
    d.variant = variant;
    d.ep_id = s_next_ep_id;
    d.block = block;
    d.slot_capacity = n_slots;
    seed_slots(&d);
    /* Marked live BEFORE the call, not after: emberAfSetDynamicEndpoint()
     * enables the endpoint on the spot, which runs every cluster's init
     * callback, and those read attributes straight back through the
     * external-storage callbacks. A slot that is not findable yet reads
     * as unsupported and the cluster inits against garbage. */
    d.used = true;

    /* A variant whose cluster set loses a grafted device type must not keep
     * advertising its id. Rows without a v1 span get the shared one for free
     * via the empty-span fallback, which today is every row. */
    const Span<const EmberAfDeviceType> &device_types =
        (variant == 1 && !type->device_types_v1.empty()) ? type->device_types_v1
                                                         : type->device_types;

    CHIP_ERROR err = emberAfSetDynamicEndpoint(
        index, d.ep_id, ep_type, Span<DataVersion>(block_dv(d), n_clusters),
        device_types, (parent_devtype != 0) ? parent_ep_id : kInvalidEndpointId);
    if (err != CHIP_NO_ERROR) {
        /* The header goes back on the free list, the block does not: see the
         * allocate-only note beside the arena. This path returns -1 and the
         * rebuild stops here, so nothing allocates again before the reboot
         * that resets the arena: the orphaned block is bounded at one per
         * boot and unreachable. Freeing it would be worse than keeping it,
         * because CHIP may still hold this span in emAfEndpoints[index] on
         * its own error paths (and a bump arena cannot free at all). */
        d.used = false;
        d.block = nullptr;
        HEARTH_LOGE("devtypes", "emberAfSetDynamicEndpoint(0x%04X) failed: %" CHIP_ERROR_FORMAT,
                    (unsigned)devtype_id, err.Format());
        return -1;
    }

    /* DoorLock: the init hook above ran inside emberAfSetDynamicEndpoint();
     * an endpoint whose lock context is missing would answer invokes it
     * cannot perform, so the create aborts (nRF mt_devtypes_zephyr.cpp
     * 8093-8125). */
    if (type_has_cluster(ep_type, DoorLock::Id) &&
        (s_lock_init_ep != d.ep_id || s_lock_init_err != CHIP_NO_ERROR)) {
        HEARTH_LOGE("devtypes", "devtype 0x%04X: DoorLock init did not succeed for endpoint %u "
                                "(init ran for endpoint %u, result %" CHIP_ERROR_FORMAT ")",
                    (unsigned)devtype_id, (unsigned)d.ep_id, (unsigned)s_lock_init_ep,
                    s_lock_init_err.Format());
        emberAfClearDynamicEndpoint(index);
        d.used = false;
        d.block = nullptr;
        return -1;
    }
    /* The valve delegate's second half: SetDefaultDelegate resolves through
     * emberAfGetClusterServerEndpointIndex, so it must run after the
     * endpoint is configured and enabled (nRF 8205-8221). */
    if (valve_delegate != nullptr) {
        mt_matter_valve_delegate_set_endpoint(valve_delegate, d.ep_id);
    }

    s_next_ep_id++;
    *out_ep_id = d.ep_id;
    return 0;
}

/* ---- CHIP external attribute storage (nRF 8415-8451) ------------------ */

/*
 * Strong definitions overriding the weak ones in CHIP's
 * app/util/generic-callback-stubs.cpp. Signatures are copied from
 * app/util/generic-callbacks.h in the Silicon Labs Matter 2.8.1 tree, which
 * returns Protocols::InteractionModel::Status, not the older EmberAfStatus.
 *
 * These two are the whole reason the blocks above exist: every attribute on
 * every dynamic endpoint is declared EXTERNAL_STORAGE, so CHIP holds no
 * value bytes for them and every read and write on the fabric lands here.
 */

Status emberAfExternalAttributeReadCallback(EndpointId endpoint, ClusterId clusterId,
                                            const EmberAfAttributeMetadata *attributeMetadata,
                                            uint8_t *buffer, uint16_t maxReadLength)
{
    uint8_t *data;
    uint8_t size;
    if (!mt_dyn_attr_slot(endpoint, clusterId, attributeMetadata->attributeId, &data, &size)) {
        return Status::UnsupportedAttribute;
    }
    if (size > maxReadLength) {
        return Status::ResourceExhausted;
    }
    memcpy(buffer, data, size);
    return Status::Success;
}

Status emberAfExternalAttributeWriteCallback(EndpointId endpoint, ClusterId clusterId,
                                             const EmberAfAttributeMetadata *attributeMetadata,
                                             uint8_t *buffer)
{
    uint8_t *data;
    uint8_t size;
    if (!mt_dyn_attr_slot(endpoint, clusterId, attributeMetadata->attributeId, &data, &size)) {
        return Status::UnsupportedAttribute;
    }
    memcpy(data, buffer, size);
    return Status::Success;
}
