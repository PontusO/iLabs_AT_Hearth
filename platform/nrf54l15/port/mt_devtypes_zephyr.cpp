/*
 * mt_devtypes_zephyr.cpp - the milestone devtype registry (Matter core
 * spec section 5): per-devtype cluster declarations, the port-owned
 * external attribute store, and dynamic endpoint creation. The nRF
 * sibling of platform/esp32c6/main/mt_devtypes.cpp.
 *
 * The C6 builds endpoints through esp_matter's endpoint::create(); CHIP
 * on Zephyr has no such layer, so the cluster and attribute metadata are
 * declared here by hand with the DECLARE_DYNAMIC_* macros and handed to
 * emberAfSetDynamicEndpoint(). The AT contract above this file is
 * identical on both platforms.
 */

#include <app/util/attribute-storage.h>
/* Catalogue batch 3: DoorLockServer::InitEndpoint(), called from this
 * file's strong emberAfDoorLockClusterInitCallback override below. The
 * lock's COMMAND hooks live in mt_matter_zephyr.cpp with the rest of the
 * AT bridge; only the per-endpoint init is here, because its failure has
 * to abort mt_devtype_create() and that is a decision this file owns. */
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app-common/zap-generated/callback.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <lib/core/DataModelTypes.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/interaction_model/StatusCode.h>

/* <new> for the placement-new that value-initializes a block's
 * type-conditional trailing store at create time (store reclaim round):
 * heap bytes are not zero-initialized the way the old .bss stores were,
 * and mt_mode_store_t's ModeOptionStruct members make a bare memset the
 * wrong tool for constructing them. */
#include <new>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/sys/sys_heap.h>

extern "C" {
#include "mt_composition.h"
#include "mt_devtypes.h"
/* For mt_air_quality_feature_mask(), the single accessor the AirQuality
 * FeatureMap seed reads instead of transcribing the bits again. */
#include "mt_matter.h"
}
#include "mt_dyn_store.h"
#include "mt_port_ids.h"

/* Fix round M3: the chime pool's unclaim, the port-local completion of
 * mt_matter.h's alloc/set_endpoint pair, called on every
 * mt_devtype_create() failure path between the claim and set_endpoint so
 * a failed create no longer strands a pool slot. Deliberately declared
 * HERE and not in core/include/mt_matter.h (read-only this round): this
 * file is the only caller. Defined beside the pool in
 * mt_matter_zephyr.cpp, which documents the most-recent-claim-only
 * contract. */
extern "C" void mt_matter_chime_delegate_unclaim(void *delegate);

/* Catalogue batch 7a: the per-EEM-endpoint registration (the one-time
 * wildcard AttributeAccessInterface plus SetMeasurementAccuracy), the
 * port's HearthEemInitCB equivalent. Port-local like the chime unclaim
 * above (core/include/mt_matter.h is read-only and names no such function
 * because the C6 wires its equivalent through esp-matter's init-callback
 * machinery); this file is the only caller, defined beside the
 * measurement pools in mt_matter_zephyr.cpp. */
extern "C" void mt_matter_eem_register(uint16_t ep);
/* Capacity gate for the SDK's measurement table, claimed before create()
 * and consumed by the register above; see its definition in
 * mt_matter_zephyr.cpp for why the count lives in this port. */
extern "C" bool mt_matter_eem_reserve(void);

/* Catalogue batch 7a: the DEM Instance's second half (construct + soft
 * Init with the variant's feature mask). Port-local for the same reason
 * as the two above: the C6 needs no such name because esp-matter's
 * DeviceEnergyManagementDelegateInitCB news the Instance from the
 * endpoint's FeatureMap at enable time; here the create path passes the
 * variant's own with_pa, the same predicate seed_slots()'s DEM FeatureMap
 * special case uses, so the seeded shadow and the Instance mask agree by
 * construction. Defined beside the DEM pool in mt_matter_zephyr.cpp. */
extern "C" void mt_matter_dem_register(void *delegate, uint16_t ep, bool with_pa);

/* Catalogue batch 7b: the WaterHeaterManagement Instance's second half
 * (construct + soft Init with the variant's feature mask), the DEM
 * register's exact shape one cluster over. Port-local for the same reason:
 * the C6 needs no such name because esp-matter's
 * WaterHeaterManagementDelegateInitCB news the Instance from the
 * endpoint's FeatureMap at enable time (esp_matter_delegate_callbacks.cpp:
 * 487-498); here the create path passes the variant's own with_em_tp, the
 * same predicate seed_slots()'s WHM FeatureMap special case uses, so the
 * seeded shadow and the Instance mask agree by construction. Defined
 * beside the WHM pool in mt_matter_zephyr.cpp. */
extern "C" void mt_matter_whm_register(void *delegate, uint16_t ep, bool with_em_tp);

/* Catalogue batch 8: the ONE process-global TemperatureControl iterator
 * delegate, handed to the SDK's free-function SetInstance(), which has no
 * caller anywhere in the SDK itself. Port-local for the reason the three
 * registers above are; the mode select manager is the same shape one
 * cluster over (a single global the SDK dispatches per endpoint), and it
 * differs only in taking no arguments, because this delegate learns its
 * endpoint from the SDK's own Reset() before every iteration. */
extern "C" void mt_matter_temp_levels_register(void);

/* Catalogue batch 8: the microwave's three-way second half. Port-local for
 * the reason every register above is, and a SINGLE function rather than
 * three calls for a reason none of them had: MicrowaveOvenControl::Instance's
 * constructor takes C++ REFERENCES to the live OperationalState and ModeBase
 * Instances, so those two must be fully constructed before it can even be
 * built. That order is a correctness requirement nothing at runtime
 * enforces, and every way of getting it wrong is silent, so it is owned by
 * one function with the sequence fixed and commented rather than by the call
 * order of three independent-looking lines here. */
/* Catalogue EVSE: the EnergyEvse Instance's second half (construct with the
 * variant's feature mask, then a SOFT Init(), the WHM register's shape).
 * Port-local for the reason every register in this block is: the alloc /
 * set_endpoint pair core/include/mt_matter.h publishes has no place to carry
 * a variant predicate, and that header is read-only this round. */
extern "C" void mt_matter_evse_register(void *delegate, uint16_t ep, bool with_soc);

extern "C" void mt_matter_mwoc_register(void *mwoc_delegate, void *opstate_delegate,
                                        void *mode_delegate, uint16_t ep);

using namespace chip;
using namespace chip::app::Clusters;

using chip::Protocols::InteractionModel::Status;

LOG_MODULE_REGISTER(hearth_devtypes, LOG_LEVEL_INF);

/*
 * Acceptance versus capacity, checked by the compiler.
 *
 * These two constants used to be one. MT_COMP_MAX_ENDPOINTS (28) is what
 * the AT wire contract lets a host DECLARE over AT+MTEP, and it is core, so
 * it is the same on both platforms. kServiceableEndpoints (16) is what this
 * build can stand up and SERVE at once, and it is a port decision about a
 * 256 KB part. Tying them together made every CHIP per-endpoint pool and
 * this file's own endpoint table pay for 28 endpoints that a real
 * composition on this part will not reach; see mt_port_ids.h for the full
 * reasoning and the LM20 note.
 *
 * Two invariants follow, and both are asserted rather than trusted:
 *
 *   1. chip_project_config.h must still mirror kServiceableEndpoints. It
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
 * A twenty-endpoint composition therefore serves endpoints 1..16. What the
 * rule exists to prevent is a RENUMBERED model, not a partial one: skipping
 * the failed entry and continuing would shift every later endpoint down by
 * one and hand a commissioned controller a silently different data model
 * (design spec 12.1).
 */
static_assert(CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT == kServiceableEndpoints,
              "chip_project_config.h must mirror kServiceableEndpoints (mt_port_ids.h)");
static_assert(kServiceableEndpoints <= MT_COMP_MAX_ENDPOINTS,
              "serviceable capacity cannot exceed the composition the AT contract accepts");

/*
 * HEARTH_DECLARE_CONST_*: the SDK's DECLARE_DYNAMIC_* macros with const
 * added, and nothing else changed.
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
 * so this port's whole 41-device-type catalogue landed in .data: resident
 * in RAM at all times whatever the host composed, and paid for twice,
 * since .data also carries an initialiser image in flash. Measured 8,740 B
 * of RAM across 120 symbols (41 cluster lists, 38 attribute lists, 41
 * endpoint descriptors) on 2026-08-30 at 6c31f09. The ZAP-generated
 * equivalents for the two fixed endpoints are already const and sit in
 * .rodata, so this is a declaration accident, not a requirement.
 *
 * WHAT MUST STAY TRUE FOR THEM TO BE SAFE. Nothing may write to endpoint,
 * cluster or attribute metadata at runtime. The type chain already says
 * so, and the SDK was searched to confirm it (memory reclaim round A,
 * 2026-08-30, against ~/ncs/v3.3.4/modules/lib/matter):
 *
 *   - emberAfSetDynamicEndpoint() takes const EmberAfEndpointType *
 *     (attribute-storage.h:277); EmberAfDefinedEndpoint::endpointType is
 *     const EmberAfEndpointType * (af-types.h:221);
 *     EmberAfEndpointType::cluster is const EmberAfCluster *
 *     (af-types.h:182); EmberAfCluster::attributes is
 *     const EmberAfAttributeMetadata * (af-types.h:88). The command,
 *     event and function lists on EmberAfCluster are const too. The chain
 *     is const-correct end to end, so this needs no SDK patch and is not
 *     a workaround for one.
 *   - grep for const_cast on any of these types across all of matter/src:
 *     zero hits. grep for a C-style cast to a non-const metadata pointer
 *     type: zero hits.
 *   - The only const-stripping cast that touches metadata at all is in
 *     emAfLoadAttributeDefaults() (attribute-storage.cpp:1340, 1346,
 *     1353, 1358), which casts &am->defaultValue to uint8_t * and hands
 *     it to emAfReadOrWriteAttribute(..., write=true) as the SOURCE
 *     buffer. It is never written through, and the whole branch sits
 *     inside `if (!am->IsExternal())` (attribute-storage.cpp:1320), which
 *     every attribute row in this file fails today.
 *   - That same defaults path already runs against the const ZAP tables in
 *     .rodata for the two fixed endpoints, and not incidentally: of the
 *     412 generated attribute rows in this build, 154 are non-EXTERNAL and
 *     124 of those carry a real default (107 ZAP_SIMPLE_DEFAULT plus 17
 *     ZAP_MIN_MAX_DEFAULTS_INDEX; there are 19 MIN_MAX rows in all, two of
 *     them EXTERNAL and so not on this path), so every boot executes those
 *     four casts against read-only memory 124 times without faulting. That
 *     is empirical proof, not an argument.
 *
 * THE STANDING CONDITION, AND IT IS A DISCIPLINE RATHER THAN A GUARANTEE.
 * Read this part twice before adding an attribute row here.
 *
 * EVERY attribute row in every table in this file must carry
 * ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE). Most of them get it for free:
 * DECLARE_DYNAMIC_ATTRIBUTE() ORs it in unconditionally
 * (attribute-storage.h:73-77, the OR at :76) and so does the
 * cluster-revision row that ..._LIST_END() appends (:57-59). But the
 * macros do NOT enforce it, because this file also contains TWO
 * hand-rolled EmberAfAttributeMetadata rows that bypass them: the
 * BatPercentRemaining MIN_MAX entries in powerSourceAttrs (below, look for
 * "Hand-rolled MIN_MAX entry") and in rechargeableBatteryPowerSourceAttrs.
 * Both spell EXTERNAL_STORAGE out by hand, and both MUST keep it.
 *
 * Those two rows are exactly the dangerous shape: each carries a real
 * defaultValue pointing at kBatPercentRemainingBounds with the MIN_MAX
 * mask, so a row that lost the EXTERNAL flag would take the
 * ptrToMinMaxValue arm, the deepest of the four casts, straight into a
 * const table now living in .rodata. kBatPercentRemainingBounds is itself
 * constexpr and already read-only, so the write would fault rather than
 * corrupt, but a boot-time fault on a composed battery endpoint is not a
 * failure mode worth discovering on a bench.
 *
 * So: a future non-EXTERNAL attribute row in a const table is the exact
 * fault this comment exists to prevent, and nothing in the compiler will
 * catch it. If one is ever needed, that table goes back to
 * DECLARE_DYNAMIC_* and out of .rodata. (Fix re-review P2: an earlier
 * version of this comment claimed the file declares zero hand-written
 * metadata rows and that the condition therefore held syntactically. It
 * does not; it holds by discipline over the two rows named above, which is
 * a materially weaker guarantee and is why it is spelled out here.)
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

/* ---- shared cluster building blocks ---------------------------------- */

/*
 * Descriptor carries no attribute metadata of its own beyond the
 * ClusterRevision that DECLARE_DYNAMIC_ATTRIBUTE_LIST_END() appends.
 *
 * Its four lists (DeviceTypeList, ServerList, ClientList, PartsList) are
 * served by CHIP's registered DescriptorCluster object, which wins before
 * ember storage is ever consulted (CodegenDataModelProvider_Read.cpp:116
 * checks the cluster registry ahead of the metadata path), so declaring
 * them here bought nothing. It cost correctness: emberAfSetDynamicEndpoint
 * validates every declared attribute size against the ember attribute IO
 * buffer (attribute-storage.cpp:334-356), and that buffer is
 * ATTRIBUTE_LARGEST from our generated endpoint_config.h:1160, which is 66.
 * A 254-byte ARRAY declaration therefore failed the check and made every
 * single endpoint creation return CHIP_ERROR_NO_MEMORY.
 */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Identify's attributes DO reach the external store below: unlike
 * Descriptor, MatterIdentifyClusterInitCallback (identify-server/
 * CodegenIntegration.cpp:174) only logs, and the IdentifyCluster object is
 * built solely by constructing a legacy Identify instance (:147), which
 * nothing does for a dynamic endpoint.
 *
 * The incoming command list is nullptr for the same reason: with no cluster
 * object there is no handler, and this tree has no ember fallback for
 * Identify either (our generated IMClusterCommandHandler.cpp dispatches
 * only Groups, LevelControl, OtaSoftwareUpdateRequestor, OnOff and
 * ThreadNetworkDiagnostics). Advertising Identify in AcceptedCommandList
 * would be a lie. Per-endpoint IdentifyCluster instances are a later
 * round's work. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(identifyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyTime::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* One metadata array per cluster, shared by every endpoint type that
 * carries that cluster: EmberAfCluster holds a const pointer to it and
 * nothing ever writes through that pointer, so a copy per devtype would
 * only cost flash. */

/* ---- on/off light (0x0100) ------------------------------------------- */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::GlobalSceneControl::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnTime::Id, INT16U, 2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OffWaitTime::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::StartUpOnOff::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kOnOffIncoming[] = { OnOff::Commands::Off::Id, OnOff::Commands::On::Id,
                                         OnOff::Commands::Toggle::Id, kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(onOffLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(onOffLightEndpoint, onOffLightClusters);

constexpr EmberAfDeviceType kOnOffLightTypes[] = { { 0x0100, 3 } };

/* ---- dimmable light (0x0101) ----------------------------------------- */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(levelAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::CurrentLevel::Id, INT8U, 1,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::RemainingTime::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MinLevel::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MaxLevel::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::Options::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::OnLevel::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::StartUpCurrentLevel::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kLevelIncoming[] = {
    LevelControl::Commands::MoveToLevel::Id,          LevelControl::Commands::Move::Id,
    LevelControl::Commands::Step::Id,                 LevelControl::Commands::Stop::Id,
    LevelControl::Commands::MoveToLevelWithOnOff::Id, LevelControl::Commands::MoveWithOnOff::Id,
    LevelControl::Commands::StepWithOnOff::Id,        LevelControl::Commands::StopWithOnOff::Id,
    kInvalidCommandId
};

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(dimmableLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(dimmableLightEndpoint, dimmableLightClusters);

constexpr EmberAfDeviceType kDimmableLightTypes[] = { { 0x0101, 3 } };

/* ---- temperature sensor (0x0302) ------------------------------------- */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(tempAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MinMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(temperatureSensorClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureMeasurement::Id, tempAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(temperatureSensorEndpoint, temperatureSensorClusters);

/* Fix round 2, M1: data_model/1.5/device_types/TemperatureSensor.xml
 * revision is 3, not 2 -- a milestone-round mistake predating this batch,
 * caught while verifying the eleven new device types against the same
 * source. */
constexpr EmberAfDeviceType kTemperatureSensorTypes[] = { { 0x0302, 3 } };

/* ---- boolean-state sensors: contact (0x0015), rain (0x0044), water
 * freeze (0x0041), water leak (0x0043) --------------------------------- */

/*
 * BooleanState (0x0045) is one of the clusters CHIP has migrated to the
 * newer code-driven ServerClusterInterface path
 * (src/app/clusters/boolean-state-server/CodegenIntegration.cpp):
 * MatterBooleanStateClusterInitCallback fires for every endpoint carrying
 * the cluster, dynamic ones included (its instance pool is explicitly sized
 * kFixedClusterCount + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT), and
 * constructs a BooleanStateCluster object whose ReadAttribute() answers
 * StateValue from its own mStateValue member, never consulting this arena
 * -- the Descriptor situation above, but for a value the host is meant to
 * update live rather than a static list. This was bench-confirmed (an
 * AT+MTATTR write of StateValue=1 fired the URC below, but chip-tool still
 * read FALSE) and mechanically confirmed (CodegenDataModelProvider_
 * Read.cpp:116 checks that registry before ember's external-storage
 * fallback is ever consulted).
 *
 * Fix round 2, C1: bridged, not left as a gap. mt_matter_attr_read/write
 * (mt_matter_zephyr.cpp) call the classic emberAfReadAttribute/
 * WriteAttribute path and always reach this arena, so AT+MTATTR against
 * StateValue reads and writes correctly here; MatterPostAttributeChangeCallback
 * additionally looks up the registered BooleanStateCluster object via
 * BooleanState::FindClusterOnEndpoint() and calls SetStateValue() on it, so
 * a host write now also reaches a real Matter controller's read AND emits
 * the cluster's StateChange event -- see the comment at that call site for
 * the full mechanism and why it cannot recurse. Declared here regardless,
 * same as before the bridge: the AT+MTATTR contract must still resolve the
 * attribute against this arena, and every other cluster in this file
 * besides Descriptor is a plain ember external-storage cluster with no
 * such split.
 */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(booleanStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::StateValue::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Fix round 2, M2: Contact/Rain/Water Freeze/Water Leak all compose to the
 * exact same cluster set (BooleanState + Identify + Descriptor, mandatory
 * clusters only), so one cluster list and one EmberAfEndpointType serve
 * all four -- the same "one metadata array per cluster, shared by every
 * endpoint type that carries that cluster" principle the top of this file
 * states for onOffAttrs, extended here to the whole cluster list since the
 * whole composition, not just one cluster, is identical across these four.
 * Only the EmberAfDeviceType (id, revision) differs per device type, and
 * that is what s_registry keys off. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(booleanStateSensorClusters)
DECLARE_DYNAMIC_CLUSTER(BooleanState::Id, booleanStateAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(booleanStateSensorEndpoint, booleanStateSensorClusters);

constexpr EmberAfDeviceType kContactSensorTypes[] = { { 0x0015, 2 } };
constexpr EmberAfDeviceType kRainSensorTypes[] = { { 0x0044, 1 } };
constexpr EmberAfDeviceType kWaterFreezeDetectorTypes[] = { { 0x0041, 1 } };
constexpr EmberAfDeviceType kWaterLeakDetectorTypes[] = { { 0x0043, 1 } };

/* ---- occupancy sensor (0x0107) ---------------------------------------- */

/*
 * Occupancy, OccupancySensorType and OccupancySensorTypeBitmap are none of
 * them nullable (controller-clusters.matter: plain, non-nullable
 * attributes).
 *
 * Fix round 2, I1: the previous version of this comment claimed
 * emberAfOccupancySensingClusterServerInitCallback fires for this cluster
 * on any endpoint, dynamic included, "because cluster-callbacks.cpp
 * dispatches it by cluster id regardless of static/dynamic" -- that
 * reasoning does not hold up. There is no per-build cluster-callbacks.cpp
 * in this checked-in tree; the file with that name lives only under a
 * build directory's gen/matter-data-model-codegen path and dispatches a
 * DIFFERENT, unrelated function (emberAfOccupancySensingClusterInitCallback, no
 * "Server" in the name -- a weak no-op stub, unused). The actual function
 * that fires here, emberAfOccupancySensingClusterServerInitCallback
 * (occupancy-sensor-server.cpp), is reached only through the per-cluster
 * "functions" array on EmberAfCluster (MATTER_CLUSTER_FLAG_INIT_FUNCTION,
 * checked by emberAfFindClusterFunction() in attribute-storage.cpp's
 * initializeEndpoint()). endpoint_config.h wires that array
 * (chipFuncArrayOccupancySensingServer) for the STATIC, ZAP-declared
 * cluster on endpoint 240 only; DECLARE_DYNAMIC_CLUSTER below hardcodes
 * `.functions = NULL` for every dynamic cluster it builds, with no way to
 * attach one. So on this endpoint -- a dynamically created one -- that
 * init callback never runs at all, and the seed row below is the only
 * writer of OccupancySensorType/OccupancySensorTypeBitmap.
 *
 * Fix round 2, I2: occupancy-sensor-server.cpp does define an
 * AttributeAccessInterface Instance class, and its object file is linked
 * into this build (occupancy-sensor-server.cpp.obj, confirmed in the DK
 * build log) -- the earlier claim that no such Instance "is constructed
 * anywhere in this tree" undersold that; the code is present and
 * reachable, it is simply never instantiated by anything in this
 * firmware. With no Instance registered, reads and writes for this
 * cluster are plain ember external storage, exactly like every other
 * sensor in this file.
 */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(occupancyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::Occupancy::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::OccupancySensorType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id, BITMAP8, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(occupancySensorClusters)
DECLARE_DYNAMIC_CLUSTER(OccupancySensing::Id, occupancyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(occupancySensorEndpoint, occupancySensorClusters);

constexpr EmberAfDeviceType kOccupancySensorTypes[] = { { 0x0107, 4 } };

/* ---- humidity sensor (0x0307) ------------------------------------------ */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(humidityAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(humiditySensorClusters)
DECLARE_DYNAMIC_CLUSTER(RelativeHumidityMeasurement::Id, humidityAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(humiditySensorEndpoint, humiditySensorClusters);

constexpr EmberAfDeviceType kHumiditySensorTypes[] = { { 0x0307, 2 } };

/* ---- pressure sensor (0x0305) ------------------------------------------ */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(pressureAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::MinMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(pressureSensorClusters)
DECLARE_DYNAMIC_CLUSTER(PressureMeasurement::Id, pressureAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(pressureSensorEndpoint, pressureSensorClusters);

constexpr EmberAfDeviceType kPressureSensorTypes[] = { { 0x0305, 2 } };

/* ---- light (illuminance) sensor (0x0106) -------------------------------
 *
 * IlluminanceMeasurement's MeasuredValue is uint16 (INT16U), unlike
 * TemperatureMeasurement/PressureMeasurement's signed int16s: the null
 * sentinel is therefore the type MAXIMUM (0xFFFF, NumericAttributeTraits::
 * GetNullValue() for an unsigned type), not the signed-type minimum 0x8000
 * the temperature/pressure seeds use. Same attr_null_sentinel() convention
 * (mt_matter_zephyr.cpp), different type -> different sentinel bytes. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(illuminanceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::MeasuredValue::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(lightSensorClusters)
DECLARE_DYNAMIC_CLUSTER(IlluminanceMeasurement::Id, illuminanceAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(lightSensorEndpoint, lightSensorClusters);

constexpr EmberAfDeviceType kLightSensorTypes[] = { { 0x0106, 3 } };

/* ---- flow sensor (0x0306) ----------------------------------------------- */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(flowAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::MeasuredValue::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(flowSensorClusters)
DECLARE_DYNAMIC_CLUSTER(FlowMeasurement::Id, flowAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(flowSensorEndpoint, flowSensorClusters);

constexpr EmberAfDeviceType kFlowSensorTypes[] = { { 0x0306, 2 } };

/* ---- on/off plug-in unit (0x010A) --------------------------------------
 *
 * Reuses onOffAttrs/kOnOffIncoming verbatim: OnOffPlug-inUnit.xml mandates
 * the SAME OnOff feature (LT, "Lighting") as OnOffLight.xml -- surprising
 * for a plug, but confirmed against both the device-type XML
 * (mandatoryConform on feature LT for the On/Off cluster) and the C6's own
 * esp_matter build (esp_matter_endpoint.cpp on_off_plug_in_unit::add()
 * calls on_off::feature::lighting::add() for this same device type), so
 * the plug's OnOff FeatureMap seed is 0x01, identical to the light's, not
 * 0. Groups and Scenes Management are also mandatoryConform in the XML for
 * this device type, matching OnOffLight.xml exactly, and are left out here
 * for the same reason the milestone on/off light above leaves them out:
 * this catalogue serves attribute-only clusters this build declares, and
 * Groups/Scenes were never added for the lights either. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(onOffPlugInUnitClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(onOffPlugInUnitEndpoint, onOffPlugInUnitClusters);

constexpr EmberAfDeviceType kOnOffPlugInUnitTypes[] = { { 0x010A, 4 } };

/* ---- mounted on/off control (0x010F) ----------------------------------
 *
 * Catalogue batch 5, a registry row reusing &onOffPlugInUnitEndpoint. The
 * C6's mounted_on_off_control::add() (esp_matter_endpoint.cpp:1794-1811)
 * creates Identify with TriggerEffect, Groups, OnOff with the Lighting
 * feature plus On/Toggle (Off from on_off::create() itself), and
 * ScenesManagement with CopyScene: the on/off plug-in unit's OnOff shape
 * exactly (the device library's classification row even marks 0x010F a
 * superset of 0x010A). On this port that collapses to the same three
 * clusters the plug declares, and every OnOff seed (FeatureMap 0x01
 * Lighting, StartUpOnOff null, revision 6) is already in s_seeds.
 *
 * Groups and ScenesManagement are mandatoryConform for this device type in
 * the XML, and are deliberately NOT added: the standing convention argued
 * on onOffPlugInUnitClusters above (this catalogue serves attribute-only
 * clusters this build declares, and Groups/Scenes were never added for the
 * lights or plugs either) covers this type identically.
 *
 * One row, one device type, per the port's one-devtype-per-row convention:
 * the superset relation could justify appending { 0x010F } to the plug's
 * own EmberAfDeviceType span instead, but no row in this registry declares
 * two device types and this batch does not start.
 *
 * Revision 2 per data_model/1.5/device_types/MountedOnOffControl.xml (the
 * device-revision authority the temperature sensor fix pinned; the
 * zap-templates matter-devices.xml still says 1 for this type and is stale
 * against the 1.5 snapshot). */
constexpr EmberAfDeviceType kMountedOnOffControlTypes[] = { { 0x010F, 2 } };

/* ---- dimmable plug-in unit (0x010B) -------------------------------------
 *
 * Reuses onOffAttrs/levelAttrs/kOnOffIncoming/kLevelIncoming verbatim:
 * DimmablePlug-InUnit.xml mandates OnOff feature LT and LevelControl
 * features OO+LT, the identical set DimmableLight.xml mandates, so this
 * cluster list and its seeds (FeatureMap 0x01 for OnOff, 0x03 for
 * LevelControl) are the same as the dimmable light above; see that
 * device type's seed rows in s_seeds, none of which are duplicated here. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(dimmablePlugInUnitClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(dimmablePlugInUnitEndpoint, dimmablePlugInUnitClusters);

constexpr EmberAfDeviceType kDimmablePlugInUnitTypes[] = { { 0x010B, 5 } };

/* ---- mounted dimmable load control (0x0110) ---------------------------
 *
 * Catalogue batch 5, a registry row reusing &dimmablePlugInUnitEndpoint:
 * the C6's mounted_dimmable_load_control::add() (esp_matter_endpoint.cpp:
 * 1831-1851) composes the dimmable plug's exact OnOff (Lighting) plus
 * LevelControl (OnOff|Lighting) set, with the same Groups/ScenesManagement
 * omission argued on the mounted on/off control above. The device
 * library's element requirements (Device-Library-Specification.md:
 * 2080-2087) mandate OnOff/LT, LevelControl/OO, LevelControl/LT and the
 * CurrentLevel 1..254 / MinLevel 1 / MaxLevel 254 bounds, all of which the
 * dimmable light's existing seeds already provide.
 *
 * B388 check, verified rather than assumed: the LevelControl ServerInit
 * call site in mt_devtype_create() below keys on the CLUSTER list (it
 * walks type->ep_type->cluster[] for LevelControl::Id), not on the device
 * type id, so this row's endpoint gets its Min/MaxLevel cache initialised
 * exactly as 0x0101/0x010B do and nothing new joins the call site.
 *
 * Revision 2 per data_model/1.5/device_types/MountedDimmableLoadControl.xml
 * (matter-devices.xml's 1 is stale against the snapshot, same note as the
 * mounted on/off control). */
constexpr EmberAfDeviceType kMountedDimmableLoadControlTypes[] = { { 0x0110, 2 } };

/* ---- color temperature light (0x010C) and extended color light (0x010D)
 *
 * Catalogue batch 2 audit, ColorControl (0x0300). Three questions, three
 * answers from this tree:
 *
 *   Code-driven? No. There is no CodegenIntegration.cpp under
 *   src/app/clusters/color-control-server/, and adding the cluster to
 *   hearth.zap emitted no case in zap-generated/CodeDrivenInitShutdown.cpp
 *   (that file still lists exactly the thirteen clusters it did before this
 *   batch, BooleanState last). So no registered ServerCluster object wins
 *   ahead of ember, unlike BooleanState/Descriptor: reads and writes land in
 *   the arena below.
 *
 *   AAI? No. MatterColorControlPluginServerInitCallback() is empty
 *   (color-control-server.cpp:3345); nothing registers an
 *   AttributeAccessInterface for this cluster.
 *
 *   ServerInit? YES, and it does observable work, so this cluster joins the
 *   B388 call site in mt_devtype_create() below.
 *   emberAfColorControlClusterServerInitCallback() (:3291) calls
 *   startUpColorTempCommand(), which applies a non-null
 *   StartUpColorTemperatureMireds to ColorTemperatureMireds and forces
 *   ColorMode/EnhancedColorMode to kColorTemperatureMireds (:2577-2624).
 *   With this batch's seeds the value it writes equals the seed already
 *   there, so today it is a no-op; it is called anyway, because the moment
 *   the StartUp seed changes (or a persisted value differs) a dynamic
 *   endpoint that never ran it boots in the wrong color mode, and that is
 *   exactly the class of bug B388 was.
 *
 *   Per-endpoint state arrays: safe. ColorControlServer's transition and
 *   quiet-reporting arrays are sized kColorControlClusterServerMaxEndpointCount
 *   = MATTER_DM_COLOR_CONTROL_CLUSTER_SERVER_ENDPOINT_COUNT +
 *   CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT (color-control-server.h:293),
 *   so dynamic endpoints are accounted for, and
 *   emberAfGetClusterServerEndpointIndex() returns fixedCount + (epIndex -
 *   FIXED_ENDPOINT_COUNT) for one (attribute-storage.cpp:957-962).
 *
 * The two device types differ only in which ColorControl attributes they
 * declare and in three seeds (FeatureMap, ColorCapabilities), which is why
 * s_seeds below grew an optional devtype qualifier rather than a second
 * table. Everything else - OnOff, LevelControl, Identify, Descriptor - is
 * the dimmable light's set verbatim.
 *
 * ColorTemperatureLight.xml mandates ColorControl feature CT only;
 * ExtendedColorLight.xml mandates XY and CT and lists HS as
 * optionalConform. HS is therefore inside the device type, not an extension
 * of it: taking it is the C6's deliberate step beyond the MANDATORY set
 * (platform/esp32c6/main/mt_devtypes.cpp mk_extended_color_light() bolts
 * hue_saturation onto the cluster after create() so the host library's
 * HSV-driven class has CurrentHue/CurrentSaturation to write); mirrored
 * here. EHUE and CL are set on neither: the cluster XML makes HS mandatory
 * only when EHUE is set and EHUE mandatory only when CL is, so HS|XY|CT
 * conforms with neither of them present.
 *
 * NumberOfPrimaries is here because it is mandatoryConform in
 * ColorControl.xml with no feature gate (the ZAP conformance checker flagged
 * its absence outright on the first regeneration); it is nullable and seeded
 * null, since a co-processor has no idea how many physical primaries the
 * host's lamp has. Options and CoupleColorTempToLevelMinMireds are likewise
 * mandatory (the latter under CT) and not in the round's scope list; they
 * are declared because the cluster XML binds them, the same OccupancySensing
 * lesson as batch 1. RemainingTime is optional and stays out. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(colorTempAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, INT16U, 2,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::StartUpColorTemperatureMireds::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id, BITMAP16, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::NumberOfPrimaries::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* The extended light's list is colorTempAttrs plus the HS and XY quartet.
 * Spelled out rather than composed: DECLARE_DYNAMIC_ATTRIBUTE_LIST_* builds a
 * plain array and there is no concatenation macro. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(extendedColorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentHue::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentSaturation::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentX::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CurrentY::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, INT16U, 2,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::StartUpColorTemperatureMireds::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id, BITMAP16, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::NumberOfPrimaries::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/*
 * These two lists are the FULL mandatory command set for the feature map
 * each device type advertises, not a chosen subset. Every command in
 * ColorControl.xml is `mandatoryConform` on a feature, so advertising CT or
 * HS or XY and then omitting one of that feature's commands is a
 * conformance gap rather than a scope decision:
 *
 *   CT              -> 0x0A MoveToColorTemperature, 0x4B MoveColorTemperature,
 *                      0x4C StepColorTemperature
 *   HS              -> 0x00 MoveToHue, 0x01 MoveHue, 0x02 StepHue,
 *                      0x03 MoveToSaturation, 0x04 MoveSaturation,
 *                      0x05 StepSaturation, 0x06 MoveToHueAndSaturation
 *   XY              -> 0x07 MoveToColor, 0x08 MoveColor, 0x09 StepColor
 *   HS or XY or CT  -> 0x47 StopMoveStep (an orTerm over the three)
 *
 * So 0x010C (CT) owes four commands and 0x010D (HS|XY|CT) owes fourteen.
 *
 * All fourteen were audited the way the CT trio was, and all pass. Each
 * handler validates its parameters, fetches its per-endpoint transition
 * state through the bounds-checked getEndpointIndex() ->
 * get*TransitionStateByIndex() pair (color-control-server.cpp:820-828,
 * :848-856, :2075-2083, :2103-2111, :2451-2459 each return nullptr for an
 * out-of-range index) and answers Status::UnsupportedEndpoint if that comes
 * back null, then FULLY initialises the state it is about to run
 * (initialValue, currentValue, finalValue, stepsRemaining, stepsTotal,
 * timeRemaining, transitionTime, endpoint and the low/high limits) BEFORE
 * scheduleTimerCallbackMs() is reached. No path calls a delegate; the ember
 * callbacks (:3099-3290) are plain thunks into ColorControlServer plus
 * AddStatus. The handlers themselves: moveHueCommand :1414, stepHueCommand
 * :1664, moveSaturationCommand :1751, stepSaturationCommand :1837,
 * moveColorCommand :2260, stepColorCommand :2344, stopMoveStepCommand :473.
 *
 * StopMoveStep is compiled unconditionally: its definition at :3283 sits
 * outside all three MATTER_DM_PLUGIN_COLOR_CONTROL_SERVER_{HSV,XY,TEMP}
 * guards (:3097-3281), and the HSV-specific half of its body is separately
 * guarded, so it serves the CT-only light too.
 *
 * EHUE and CL commands (0x40-0x44) stay out: neither feature is advertised,
 * so the XML does not mandate them.
 */
constexpr CommandId kColorTempIncoming[] = { ColorControl::Commands::MoveToColorTemperature::Id,
                                             ColorControl::Commands::MoveColorTemperature::Id,
                                             ColorControl::Commands::StepColorTemperature::Id,
                                             ColorControl::Commands::StopMoveStep::Id,
                                             kInvalidCommandId };

constexpr CommandId kExtendedColorIncoming[] = {
    ColorControl::Commands::MoveToHue::Id,
    ColorControl::Commands::MoveHue::Id,
    ColorControl::Commands::StepHue::Id,
    ColorControl::Commands::MoveToSaturation::Id,
    ColorControl::Commands::MoveSaturation::Id,
    ColorControl::Commands::StepSaturation::Id,
    ColorControl::Commands::MoveToHueAndSaturation::Id,
    ColorControl::Commands::MoveToColor::Id,
    ColorControl::Commands::MoveColor::Id,
    ColorControl::Commands::StepColor::Id,
    ColorControl::Commands::MoveToColorTemperature::Id,
    ColorControl::Commands::MoveColorTemperature::Id,
    ColorControl::Commands::StepColorTemperature::Id,
    ColorControl::Commands::StopMoveStep::Id,
    kInvalidCommandId
};

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(colorTemperatureLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, colorTempAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kColorTempIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(colorTemperatureLightEndpoint, colorTemperatureLightClusters);

constexpr EmberAfDeviceType kColorTemperatureLightTypes[] = { { 0x010C, 4 } };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(extendedColorLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, extendedColorAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kExtendedColorIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(extendedColorLightEndpoint, extendedColorLightClusters);

constexpr EmberAfDeviceType kExtendedColorLightTypes[] = { { 0x010D, 4 } };

/* ---- thermostat (0x0301) ---------------------------------------------
 *
 * Catalogue batch 2 audit, Thermostat (0x0201). This is the one server in
 * the batch that registers a wildcard AttributeAccessInterface, so it was
 * audited attribute by attribute rather than by presence alone.
 *
 *   Code-driven? No: no CodegenIntegration.cpp under thermostat-server/, and
 *   no case emitted in zap-generated/CodeDrivenInitShutdown.cpp.
 *
 *   AAI? Yes, and for EVERY endpoint: gThermostatAttrAccess is constructed
 *   with Optional<EndpointId>::Missing() (thermostat-server.h:54) and
 *   registered from MatterThermostatPluginServerInitCallback()
 *   (thermostat-server.cpp:1415), which the generated MATTER_PLUGINS_INIT
 *   now calls. It is nonetheless harmless here, because its Read() and
 *   Write() switches fall through to "just read/write the attribute store"
 *   for everything this file declares (:702-706 and :800-804 respectively):
 *     - LocalTemperature is intercepted ONLY when the
 *       LocalTemperatureNotExposed feature is set (:539-544). It is not.
 *     - RemoteSensing, likewise gated on LTNE, is not declared here.
 *     - Every delegate-backed case (PresetTypes, NumberOfPresets, Presets,
 *       ActivePresetHandle, ScheduleTypes, Schedules, MaxThermostatSuggestions,
 *       ThermostatSuggestions, CurrentThermostatSuggestion,
 *       ThermostatSuggestionNotFollowingReason) belongs to the Presets /
 *       MatterScheduleConfiguration / ThermostatSuggestions features, none of
 *       which is in FeatureMap and none of whose attributes is declared, so
 *       no read can reach a GetDelegate() call that would return nullptr.
 *     - ClusterRevision is the one attribute the AAI answers itself, from
 *       Thermostat::kRevision (:701). The seed below is that same 9, so the
 *       arena and the fabric agree; a stale seed here would show up as
 *       AT+MTATTR and a controller disagreeing about the revision.
 *
 *   ServerInit? emberAfThermostatClusterServerInitCallback()
 *   (thermostat-server.cpp:866) is an empty TODO body. It caches nothing, so
 *   this cluster does NOT join the B388 call site.
 *
 *   Delegate for the command? None. emberAfThermostatClusterSetpointRaiseLower
 *   Callback() (:1176) works entirely off FeatureMap and the setpoint
 *   attributes and never touches GetDelegate(); EnforceHeating/Cooling
 *   SetpointLimits() fall back to spec defaults when the optional Abs*
 *   limits are absent (:85-107), which is why they are not declared.
 *
 *   Deadband: MatterThermostatClusterServerAttributeChangedCallback() ->
 *   EnsureDeadband() returns immediately unless the AutoMode feature is set
 *   (:485-488), and it is function-array-bound so it never runs on a dynamic
 *   endpoint anyway.
 *
 * Thermostat.xml makes HEAT and COOL a choice="a" min="1" group (at least
 * one), so Heating|Cooling = 0x03 conforms and matches the C6, whose
 * mk_thermostat() ORs exactly those two. Thermostat.xml (device type) marks
 * SCH disallowConform; not set. SystemMode is seeded Off (0) rather than
 * esp-matter's constructor default of Auto (1): Auto is only meaningful with
 * the AutoMode feature, which this endpoint does not advertise. The setpoint
 * seeds are 1600/2400 hundredths, the C6's own deliberate departure from
 * esp-matter's 2000/2600 (cross-layer finding I1: the host library caches
 * upstream's boot values, and a first write matching the cache is swallowed
 * before it reaches the wire). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(thermostatAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::LocalTemperature::Id, TEMPERATURE, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::OccupiedCoolingSetpoint::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::OccupiedHeatingSetpoint::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MinHeatSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MaxHeatSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MinCoolSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::MaxCoolSetpointLimit::Id, TEMPERATURE, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::ControlSequenceOfOperation::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::SystemMode::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kThermostatIncoming[] = { Thermostat::Commands::SetpointRaiseLower::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(thermostatClusters)
DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(thermostatEndpoint, thermostatClusters);

constexpr EmberAfDeviceType kThermostatTypes[] = { { 0x0301, 4 } };

/* ---- fan (0x002B) -----------------------------------------------------
 *
 * Catalogue batch 2 audit, FanControl (0x0202). The simplest of the batch:
 * no CodegenIntegration.cpp and no CodeDrivenInitShutdown case, no
 * AttributeAccessInterface at all, and MatterFanControlPluginServerInit
 * Callback() is an empty definition in CHIP's own src/app/util/util.cpp:112.
 * There is no emberAfFanControlClusterServerInitCallback either, so nothing
 * to join the B388 call site with. Plain ember external storage.
 *
 * FanControl.xml declares all six features optionalConform with no choice
 * group, so FeatureMap 0 conforms - unlike OccupancySensing in batch 1,
 * whose min-1 group was the lesson that sent us to the cluster XML in the
 * first place. Every attribute declared below is mandatoryConform with no
 * feature gate; SpeedMax/SpeedSetting/SpeedCurrent (MultiSpeed),
 * RockSupport/RockSetting (Rocking), WindSupport/WindSetting (Wind) and
 * AirflowDirection are each behind a feature this endpoint does not
 * advertise, and Rocking/Wind/AirflowDirection/Step are delegate territory
 * (emberAfFanControlClusterStepCallback, fan-control-server.cpp:453, calls
 * GetDelegate() at :473 and answers Status::Failure at :478-482 when there
 * is none), which is why the Step feature is deliberately absent and no
 * incoming command is advertised.
 *
 * FanModeSequence is seeded OffLowMedHigh (0), NOT esp-matter's constructor
 * default of OffLowMedHighAuto (2). FanControl.xml gates enum values 2, 3
 * and 4 behind the AUT feature (choice "b") and values 0, 1 and 5 behind
 * !AUT (choice "a"), so with FeatureMap 0 the esp-matter default is not a
 * conformant value. Deliberate divergence from the C6, whose mk_fan() takes
 * the default; noted rather than copied.
 *
 * Known limitation, documented rather than bridged: the server's own
 * FanMode <-> PercentSetting coupling lives in MatterFanControlCluster
 * ServerAttributeChangedCallback (:327-451), which is reached through the
 * per-cluster functions array and therefore never runs on a dynamic
 * endpoint (the same mechanism as the OccupancySensing init in batch 1).
 * Both attributes read and write correctly over AT+MTATTR and over a
 * controller's IM; what does not happen is FanMode=Off zeroing PercentSetting
 * by itself. The host owns that coupling, which is the co-processor model
 * everywhere else in this file. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(fanControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanMode::Id, ENUM8, 1,
                          ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanModeSequence::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentSetting::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentCurrent::Id, PERCENT, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(fanClusters)
DECLARE_DYNAMIC_CLUSTER(FanControl::Id, fanControlAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(fanEndpoint, fanClusters);

constexpr EmberAfDeviceType kFanTypes[] = { { 0x002B, 4 } };

/* ---- air purifier (0x002D) --------------------------------------------
 *
 * Catalogue batch 5. A registry row, not a new endpoint type: the C6's
 * air_purifier::add() creates Identify and FanControl and nothing else
 * (esp_matter_endpoint.cpp:745-754), which is byte-identical to the fan
 * 0x002B's composition above, so &fanEndpoint is reused verbatim and only
 * the EmberAfDeviceType differs. The device library lists Groups, OnOff
 * and the two filter-monitoring clusters (HepaFilterMonitoring 0x0071,
 * ActivatedCarbonFilterMonitoring 0x0072) as optional on this device type
 * (Device-Library-Specification.md:3936-3939) and the C6 takes none of
 * them, so resource-monitoring is deliberately NOT composed here either.
 *
 * The fan's known FanMode <-> PercentSetting coupling gap (the audit note
 * on fanControlAttrs above: MatterFanControlClusterServerAttributeChanged
 * Callback is functions-array-bound and never runs on a dynamic endpoint)
 * now ships under a SECOND device-type name. Restated deliberately, not
 * silently inherited: an air purifier host owns that coupling exactly the
 * way a fan host does, and nothing about this row re-solves it.
 *
 * Revision 2 per data_model/1.5/device_types/AirPurifier.xml (the same
 * source the temperature sensor's fix-round correction pinned as this
 * file's authority for device revisions). */
constexpr EmberAfDeviceType kAirPurifierTypes[] = { { 0x002D, 2 } };

/* ---- window covering (0x0202) -----------------------------------------
 *
 * Catalogue batch 2 audit, WindowCovering (0x0102).
 *
 *   Code-driven? No: no CodegenIntegration.cpp, no CodeDrivenInitShutdown
 *   case.
 *
 *   AAI? Yes, wildcard-endpoint (WindowCoverAttrAccess is constructed with
 *   Optional<EndpointId>::Missing(), window-covering-server.h:79, registered
 *   from MatterWindowCoveringPluginServerInitCallback(), :994). Its Read()
 *   is four lines long (:118-128): it answers ClusterRevision from
 *   WindowCovering::kRevision and falls through to the attribute store for
 *   everything else. There is no Write() override at all. So the seed below
 *   must carry that same revision (5), and every other attribute is plain
 *   ember external storage.
 *
 *   ServerInit? There is no emberAfWindowCoveringClusterServerInitCallback
 *   in this tree - the answer to the round's "the WC server caches
 *   per-endpoint state?" question is no, it does not, and this cluster does
 *   NOT join the B388 call site. The only per-endpoint state is the delegate
 *   table, sized MATTER_DM_WINDOW_COVERING_CLUSTER_SERVER_ENDPOINT_COUNT +
 *   CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT (:47-48), so a dynamic
 *   endpoint index cannot run off it; GetDelegate() returns nullptr and
 *   every command handler explicitly tolerates that.
 *
 *   Commands with no delegate: correct, not merely tolerated. UpOrOpen
 *   (:637), DownOrClose (:687), StopMotion (:736) and GoToLiftPercentage
 *   (:837) each set TargetPositionLiftPercent100ths in the arena first, then
 *   log "WindowCovering has no delegate set" and still answer Success. That
 *   is exactly the co-processor split we want: the fabric's target lands in
 *   the arena, MatterPostAttributeChangeCallback turns it into a +MTATTR
 *   URC, and the host moves the motor and writes CurrentPosition back.
 *
 * WindowCovering.xml makes LF and TL a choice="a" min="1" group; Lift plus
 * PositionAwareLift (0x05) is the pair the percent100ths surface needs.
 * Tilt is left out this round (the C6 enables all four bits; this is a
 * narrower, honest subset rather than a parity bug - the tilt attributes and
 * their two commands are simply not declared). ConfigStatus is seeded
 * Operational|LiftPositionAware (0x09) rather than the C6's 0: the attribute
 * has default="desc" in the XML, GetMotionLockStatus() (:585) reads it, and
 * describing a position-aware, operational covering is the truthful answer
 * for what this endpoint presents. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(windowCoveringAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::Type::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::ConfigStatus::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::OperationalStatus::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id,
                              PERCENT100THS, 2, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::EndProductType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id,
                              PERCENT100THS, 2, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::Mode::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kWindowCoveringIncoming[] = { WindowCovering::Commands::UpOrOpen::Id,
                                                  WindowCovering::Commands::DownOrClose::Id,
                                                  WindowCovering::Commands::StopMotion::Id,
                                                  WindowCovering::Commands::GoToLiftPercentage::Id,
                                                  kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(windowCoveringClusters)
DECLARE_DYNAMIC_CLUSTER(WindowCovering::Id, windowCoveringAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kWindowCoveringIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(windowCoveringEndpoint, windowCoveringClusters);

constexpr EmberAfDeviceType kWindowCoveringTypes[] = { { 0x0202, 5 } };

/* ---- air quality sensor (0x002C) --------------------------------------
 *
 * Catalogue batch 2 audit, AirQuality (0x005B). This is the "Instance that
 * requires app construction" case from the round's lesson list, and it comes
 * out clean:
 *
 *   Code-driven? No: no CodegenIntegration.cpp under air-quality-server/, no
 *   CodeDrivenInitShutdown case.
 *
 *   Instance? air-quality-server.cpp defines an Instance whose Init()
 *   registers it as an AttributeAccessInterface (:45-50), and if one existed
 *   it WOULD answer AirQuality and FeatureMap ahead of ember. Nothing in
 *   this firmware constructs one: the object file is linked, but
 *   MatterAirQualityPluginServerInitCallback() is CHIP's own empty
 *   definition in src/app/util/util.cpp:115, not something the cluster
 *   provides, and this port has no equivalent of the C6's
 *   mt_air_quality_register_all(). With no Instance registered, reads and
 *   writes are plain ember external storage against the arena below - the
 *   same shape as OccupancySensing in batch 1.
 *
 *   ServerInit? None exists; nothing joins the B388 call site.
 *
 * The four optional features (Fair, Moderate, VeryPoor, ExtremelyPoor) are
 * all advertised, so the host library's seven-value AirQuality_t enum can
 * never report a value this endpoint's feature map does not admit. The mask
 * is NOT a literal in s_seeds: seed_slots() reads it from
 * mt_air_quality_feature_mask() (mt_matter.h), the single accessor whose
 * whole point is that the ember feature map and any future Instance's
 * BitMask<Feature> cannot drift apart. On this platform that function was a
 * stub returning 0 until this batch; it now lives in mt_matter_zephyr.cpp
 * with the real bits. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(airQualityAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(AirQuality::Attributes::AirQuality::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(AirQuality::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(airQualitySensorClusters)
DECLARE_DYNAMIC_CLUSTER(AirQuality::Id, airQualityAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(airQualitySensorEndpoint, airQualitySensorClusters);

constexpr EmberAfDeviceType kAirQualitySensorTypes[] = { { 0x002C, 1 } };

/* ---- door lock (0x000A) -----------------------------------------------
 *
 * Catalogue batch 3 audit, DoorLock (0x0101). The first cluster on this
 * platform whose commands need an application VERDICT, so the audit asked a
 * fourth question beyond the usual three: where does the app's answer
 * attach, and what does the SDK do with it.
 *
 *   Code-driven? No. door-lock-server is the only door-lock directory in
 *   the tree, and DoorLock is absent from the CodeDrivenClusters list
 *   (src/app/common/templates/config-data.yaml:144-168). Confirmed
 *   empirically: regenerating hearth.zap with the cluster on ep240 left
 *   zap-generated/CodeDrivenInitShutdown.cpp byte-identical.
 *
 *   AAI? Yes, wildcard-endpoint (DoorLockServer itself derives from
 *   AttributeAccessInterface, door-lock-server.h:99, constructed with
 *   Optional<EndpointId>::Missing() at :102 and registered from
 *   MatterDoorLockPluginServerInitCallback, door-lock-server.cpp:4312).
 *   Harmless for this attribute set: Read() (:4421) has a case for
 *   nothing but the nine Aliro attributes and then `default: break;
 *   return CHIP_NO_ERROR`, i.e. falls through to the attribute store. None
 *   of the Aliro attributes is declared below, so every read of this
 *   endpoint reaches the arena. There is no Write() override.
 *
 *   ServerInit? YES, and this one is REQUIRED, not merely worth running.
 *   The catch is the name. door-lock-cluster.xml:44 declares
 *   <server tick="false" init="false">, so ZAP puts NO init function in the
 *   cluster's function array and emberAfDoorLockClusterServerInitCallback
 *   (with "Server") is declared but never called. The hook that IS called
 *   is emberAfDoorLockClusterInitCallback (no "Server"), dispatched from
 *   emberAfClusterInitCallback() inside initializeEndpoint()
 *   (attribute-storage.cpp:480). Unlike the LevelControl/ColorControl
 *   inits below, that path DOES run for a dynamic endpoint: it is reached
 *   through emberAfEndpointEnableDisable(), which emberAfSetDynamicEndpoint
 *   calls itself (:393), and the isEnabled bit is set BEFORE
 *   initializeEndpoint runs (:991-996), so the endpoint index resolves.
 *   So this cluster does NOT join the B388 call site: the strong override
 *   of emberAfDoorLockClusterInitCallback below is what calls
 *   DoorLockServer::InitEndpoint(), and ember drives it. That is also what
 *   the nRF lock sample does (nrf/samples/matter/lock/src/
 *   zcl_callbacks.cpp:97-99), though it uses the deprecated InitServer
 *   alias and discards the error; see the override for why this port does
 *   neither.
 *
 *   The delegate surface: NOTHING is link-mandatory. All thirty
 *   emberAfPluginDoorLock* application hooks carry weak defaults in
 *   door-lock-server-callback.cpp (the first at :46), which
 *   app_config_dependent_sources.cmake:19 compiles unconditionally
 *   alongside the server. Feature gating is at RUNTIME, off the FeatureMap
 *   attribute (GetFeatures(), door-lock-server.cpp:1471), not by #ifdef, so
 *   a lock with FeatureMap 0 never reaches the user/credential/schedule
 *   hooks at all. This firmware therefore overrides exactly two of them,
 *   emberAfPluginDoorLockOnDoorLockCommand and ...OnDoorUnlockCommand
 *   (mt_matter_zephyr.cpp), and leaves the other twenty-eight to their weak
 *   stubs. Same two the C6 defines.
 *
 * FeatureMap 0 is conformant. Every feature in DoorLock's feature list is
 * optionalConform, and USR is mandatory only under (PIN|RID|FPG|FACE),
 * which none of them is here; the device type agrees
 * (matter-devices.xml:2003-2025). ZAP's own default FeatureMap of 0x0001
 * (door-lock-cluster.xml:49) is a seed value in that file, not a
 * requirement.
 *
 * AutoRelockTime is OPTIONAL and is declared anyway, deliberately. This is
 * bug B129 from the C6, and it reproduces verbatim in this tree: the 7-arg
 * SetLockState() ends an UNLOCK with
 * VerifyOrReturnError(GetAutoRelockTime(endpointId, autoRelockTime), false)
 * (door-lock-server.cpp:206). With the attribute absent that read fails and
 * the function returns false for an unlock that actually happened and
 * actually emitted its LockOperation event, which turns every host
 * AT+MTLOCK unlock into a bare ERROR with the state changed underneath.
 * Seeded 0, which disables auto-relock. If a controller writes it non-zero
 * the server relocks at expiry through its own timer
 * (DoorLockOnAutoRelockCallback, :4342) and the host observes the LockState
 * change as a +MTATTR URC, the same path as any controller write.
 *
 * LockDoor and UnlockDoor are the two mandatory commands and the only two
 * declared. Both are mustUseTimedInvoke in door-lock-cluster.xml's command
 * list. UnlockWithTimeout and UnboltDoor are not declared, so their
 * handlers are unreachable and the extra hooks they would need stay
 * unwritten. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(doorLockAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::LockState::Id, ENUM8, 1,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::LockType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::ActuatorEnabled::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::AutoRelockTime::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::OperatingMode::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::SupportedOperatingModes::Id, BITMAP16, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kDoorLockIncoming[] = { DoorLock::Commands::LockDoor::Id,
                                            DoorLock::Commands::UnlockDoor::Id,
                                            kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(doorLockClusters)
DECLARE_DYNAMIC_CLUSTER(DoorLock::Id, doorLockAttrs, ZAP_CLUSTER_MASK(SERVER), kDoorLockIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(doorLockEndpoint, doorLockClusters);

constexpr EmberAfDeviceType kDoorLockTypes[] = { { 0x000A, 3 } };

/* ---- water valve (0x0042) ---------------------------------------------
 *
 * Catalogue batch 3 audit, ValveConfigurationAndControl (0x0081). The other
 * verdict type, and the one that needs a DELEGATE OBJECT rather than free
 * ember callbacks.
 *
 *   Code-driven? No, despite appearances. The directory's files were
 *   renamed valve-configuration-and-control-server.cpp to -cluster.cpp in
 *   2025 and its BUILD.gn is now an empty group(), which makes it LOOK
 *   like a port to the ServerCluster path. It is not: there is no
 *   CodegenIntegration.cpp, no ServerClusterInterface subclass, and the
 *   cluster is absent from CodeDrivenClusters (config-data.yaml:144-168).
 *   Confirmed empirically the same way as the door lock:
 *   CodeDrivenInitShutdown.cpp came back byte-identical.
 *
 *   AAI? Yes, wildcard-endpoint, and for exactly ONE attribute.
 *   ValveConfigAndControlAttrAccess (cluster.cpp:131, constructed with
 *   Optional<EndpointId>::Missing() at :134) answers RemainingDuration from
 *   a private shadow array gRemainingDuration[] (:61-67) and falls through
 *   the default: for everything else without encoding, which the provider
 *   reads as "not handled" (CodegenDataModelProvider_Read.cpp:112 tries the
 *   AAI first, :59 decides what counts as handled). No Write() override. So
 *   OpenDuration, DefaultOpenDuration, CurrentState and TargetState are
 *   plain ember external storage against the arena; RemainingDuration is
 *   the BooleanState-shaped split, documented in the seed comment below and
 *   in the platform README rather than bridged: it is a firmware-managed
 *   countdown with no host write path, so there is nothing for a bridge to
 *   push.
 *
 *   ServerInit? No per-endpoint init at all. The only init is
 *   MatterValveConfigurationAndControlPluginServerInitCallback()
 *   (cluster.cpp:528), which takes no endpoint, runs once from
 *   MATTER_PLUGINS_INIT, and does nothing but register that AAI. This
 *   cluster does NOT join the B388 call site.
 *
 *   The delegate: chip::app::Clusters::ValveConfigurationAndControl::
 *   Delegate (delegate.h:34), three pure virtuals (:40-42), registered per
 *   endpoint with the free function SetDefaultDelegate(EndpointId,
 *   Delegate*) (cluster.h:40, impl cluster.cpp:262). See mt_devtype_create()
 *   below for the ordering rule this forces and why it differs from the
 *   C6's.
 *
 * FeatureMap 0. TS (bit 0) is left clear DELIBERATELY and not merely by
 * omission: with TS set but no Time Synchronization cluster server on the
 * image, SetValveLevel() returns CHIP_ERROR_NOT_IMPLEMENTED (cluster.cpp:
 * 336) and every Open answers Status::Failure. LVL (bit 1) is optional and
 * not taken, which is why CurrentLevel and TargetLevel are not declared
 * (AT_MT_SPEC.md 3.19 documents the consequence for AT+MTVALVE's <level>,
 * and it is the same consequence the C6 has for the same reason).
 * AutoCloseTime is TS-gated and absent for the same reason.
 *
 * ValveFault (0x0009) is optional and is NOT declared. It is the cluster's
 * only escape hatch for failing a command on the wire: both handlers check
 * it first and answer AddClusterSpecificFailure(kFailureDueToFault)
 * (cluster.cpp:445 for Open, :511 for Close) before the delegate is
 * consulted at all. Declaring it would not help. The +MTCMD verdict arrives
 * INSIDE the delegate call, which is past that check, so a deny still could
 * not fail the in-flight command; the attribute would only let a host
 * pre-arm a fault for the NEXT command, which is not what the verdict frame
 * is for and is not a surface AT_MT_SPEC.md 3.19 describes. Left out, and
 * the "verdict cannot fail the valve command" property stays exactly what
 * the spec already documents.
 *
 * ---- the auto-close re-entry (fix round, I1) --------------------------
 *
 * One consequence of declaring DefaultOpenDuration writable, which the XML
 * makes it and the mandatory set requires: a TIMED open is reachable on
 * this build, and the server closes the valve itself when the countdown
 * expires. That path re-enters the delegate.
 *
 * onValveConfigurationAndControlTick() (cluster.cpp:214) decrements
 * RemainingDuration and re-arms a 1 s SystemLayer timer through
 * startRemainingDurationTick() (:235, :248); on the terminal tick it calls
 * CloseValve() instead (:250-252), and CloseValve() calls
 * delegate->HandleCloseValve() (:303). So a single controller Open with a
 * duration produces a SECOND, UNSOLICITED +MTCMD for cluster 129 command 1
 * that no controller asked for, raised from timer context.
 *
 * That forward's verdict is MEANINGLESS, and worse than the ordinary valve
 * case. CloseValve() has already set TargetState kClosed (:283), set
 * CurrentState kTransitioning (:285), nulled OpenDuration (:287) and
 * RemainingDuration (:296), cancelled the tick timer (:298) and emitted
 * ValveStateChanged (:300) BEFORE the delegate is called at all. A deny
 * cannot undo any of it, so the fabric-visible state changes either way.
 * It also blocks the CHIP event loop for up to the mailbox's full 1000 ms
 * from a timer callback.
 *
 * Not fixed here, and no in-scope fix exists: the forward happens because
 * HandleCloseValve() is the only hook the SDK offers and it cannot tell an
 * invoke from an auto-close, and core owns the blocking semantics. The C6
 * has exactly the same behaviour for exactly the same reason. Documented
 * in the platform README so a host author knows a 129/1 forward may be
 * server-initiated; the AT_MT_SPEC amendment and the bench case are the
 * controller's. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(valveAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::OpenDuration::Id, ELAPSED_S, 4,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::DefaultOpenDuration::Id,
                              ELAPSED_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE) | ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::RemainingDuration::Id,
                              ELAPSED_S, 4, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::CurrentState::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::TargetState::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kWaterValveIncoming[] = { ValveConfigurationAndControl::Commands::Open::Id,
                                              ValveConfigurationAndControl::Commands::Close::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(waterValveClusters)
DECLARE_DYNAMIC_CLUSTER(ValveConfigurationAndControl::Id, valveAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kWaterValveIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(waterValveEndpoint, waterValveClusters);

constexpr EmberAfDeviceType kWaterValveTypes[] = { { 0x0042, 1 } };

/* ---- power source (0x0011) --------------------------------------------
 *
 * Catalogue batch 4 audit, PowerSource (0x002F). Registry row and seeds
 * only: there is no mt_matter_power* port function in core/include/
 * mt_matter.h at all, so the generic AT+MTATTR bridge
 * (mt_matter_attr_read/write, mt_matter_zephyr.cpp) is the whole host
 * surface for this type.
 *
 *   Code-driven? No. PowerSource is absent from CodeDrivenClusters
 *   (src/app/common/templates/config-data.yaml:144-168). Confirmed
 *   empirically, the batch 3 method: regenerating hearth.zap with the
 *   cluster on ep240 left zap-generated/CodeDrivenInitShutdown.cpp
 *   byte-identical.
 *
 *   AAI? Yes, wildcard-endpoint, for three attributes. PowerSourceAttrAccess
 *   (power-source-server.h:48, ctor at :52 with
 *   Optional<EndpointId>::Missing(), registered from
 *   MatterPowerSourcePluginServerInitCallback, power-source-server.cpp:
 *   88-91) answers ActiveBatFaults (:108, not declared here), EndpointList
 *   (:112, from PowerSourceServer's own table; empty list until something
 *   calls SetEndpointList, which nothing on this firmware does) and
 *   ClusterRevision (:131, from PowerSource::kRevision), and falls through
 *   default: for everything else. So Status, Order, Description and the
 *   three Bat* scalars are plain ember external storage against the arena,
 *   and the ClusterRevision seed below must be that same kRevision (3,
 *   PowerSource/Metadata.h:20), the Thermostat/WindowCovering discipline.
 *   The server's per-endpoint table is already dynamic-endpoint aware:
 *   sPowerSourceClusterInfo is sized MATTER_DM_POWER_SOURCE_CLUSTER_SERVER_
 *   ENDPOINT_COUNT + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT
 *   (power-source-server.cpp:75-81), which this file's static assert ties
 *   to kServiceableEndpoints.
 *
 *   ServerInit? None per endpoint. MatterPowerSourcePluginServerInitCallback
 *   only registers the AAI; no emberAfPowerSourceCluster*InitCallback
 *   exists. Does NOT join the B388 call site.
 *
 * NO Identify, a documented exception to this port's "Identify rides on
 * every device type" convention: the device type mandates Descriptor plus
 * PowerSource and nothing else (matter-devices.xml:98-115, MA-powersource,
 * lockOthers with exactly those two includes), and the C6
 * composes exactly PowerSource plus Descriptor (esp_matter_endpoint.cpp
 * power_source::add(), which creates no Identify). Adding Identify here
 * would be a gratuitous divergence from the C6's data model and would cost
 * four extra slots per endpoint.
 *
 * Feature set: the cluster VALIDATEs exactly one of Wired/Battery on the
 * C6 (esp-matter's VALIDATE_FEATURES_EXACT_ONE); Battery (0x2,
 * PowerSource/Enums.h:282-283) is taken, matching the C6's choice for
 * upstream's battery-powered smoke/CO alarm devices, and the three
 * BAT-gated attributes it makes mandatory (BatChargeLevel,
 * BatReplacementNeeded, BatReplaceability) are declared.
 *
 * Description (char_string, length 60, so 61 ember bytes) and EndpointList
 * (array) get no attribute slot: attr_gets_slot() refuses both, and
 * seed_slots() skips them quietly as deliberate metadata-only declarations.
 * They are declared anyway so AttributeList is truthful. EndpointList is
 * served by the AAI (empty list); Description is served by nothing on this
 * platform and reads UNSUPPORTED_ATTRIBUTE, pending a string-store
 * decision. Both sizes pass emberAfSetDynamicEndpoint's IO-buffer check
 * (ATTRIBUTE_LARGEST is 66, endpoint_config.h).
 *
 * BatPercentRemaining is NULLABLE with real 0..200 bounds, matching the C6
 * thunk's hand-add (platform/esp32c6/main/mt_devtypes.cpp mk_power_source(),
 * bounds copied there from battery_storage::add()). DECLARE_DYNAMIC_ATTRIBUTE
 * cannot express MIN_MAX, so the entry below is hand-rolled to the exact
 * shape ZAP generates for a bounded attribute: a MIN_MAX-flagged metadata
 * row whose default union points at an EmberAfAttributeMinMaxValue. ember's
 * bounds check runs on every write regardless of storage kind
 * (attribute-table.cpp:368-406), so an out-of-range AT+MTATTR write answers
 * +MTERR:1 here exactly as it does on the C6; the null sentinel stays
 * writable because "null value is always in-range for a nullable attribute"
 * (attribute-table.cpp:401-403). Seeded null. */

constexpr EmberAfAttributeMinMaxValue kBatPercentRemainingBounds = {
    (uint16_t)0xFF, /* default: null */
    (uint16_t)0x00, (uint16_t)0xC8 /* 0..200, half-percent units */
};

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(powerSourceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Status::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Order::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Description::Id, CHAR_STRING, 61, 0),
    /* Hand-rolled MIN_MAX entry; see the audit note above. The mask spells
     * out the two flags DECLARE_DYNAMIC_ATTRIBUTE always adds. */
    { &kBatPercentRemainingBounds, PowerSource::Attributes::BatPercentRemaining::Id, 1,
      ZAP_TYPE(INT8U),
      ZAP_ATTRIBUTE_MASK(MIN_MAX) | ZAP_ATTRIBUTE_MASK(NULLABLE) |
          ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE) | ZAP_ATTRIBUTE_MASK(READABLE) },
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargeLevel::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplacementNeeded::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplaceability::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::EndpointList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(powerSourceClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, powerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(powerSourceEndpoint, powerSourceClusters);

constexpr EmberAfDeviceType kPowerSourceTypes[] = { { 0x0011, 1 } };

/* ---- smoke/co alarm (0x0076) ------------------------------------------
 *
 * Catalogue batch 4 audit, SmokeCoAlarm (0x005C).
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; confirmed empirically, regenerating hearth.zap with the
 *   cluster on ep240 left CodeDrivenInitShutdown.cpp byte-identical). Also
 *   NOT CommandHandlerInterface-only (absent from
 *   CommandHandlerInterfaceOnlyClusters, config-data.yaml:21-88), so
 *   SelfTestRequest is dispatched by the generated IMClusterCommandHandler:
 *   the regeneration added exactly the SmokeCoAlarm dispatch case, which is
 *   why this cluster had to enter hearth.zap at all.
 *
 *   Delegate or plain ember? Plain ember with a SINGLETON server.
 *   SmokeCoAlarmServer is `static SmokeCoAlarmServer sInstance`
 *   (smoke-co-alarm-server.h:163), reached through Instance() (:36); every
 *   setter takes an EndpointId and reads or writes ember storage through
 *   the generated Accessors, so all nine declared attributes land in this
 *   arena. No per-endpoint object, no pool, no placement-new: the simplest
 *   server shape in the batch.
 *
 *   ServerInit? None. MatterSmokeCoAlarmPluginServerInitCallback and its
 *   Shutdown twin are empty bodies (smoke-co-alarm-server.cpp:514-515), the
 *   XML declares <server tick="false" init="false">
 *   (smoke-co-alarm-cluster.xml:32), and no emberAfSmokeCoAlarmCluster*
 *   InitCallback exists beyond the generated weak stub. Does NOT join the
 *   B388 call site.
 *
 *   The one LINK-MANDATORY symbol in the batch:
 *   emberAfPluginSmokeCoAlarmSelfTestRequestCommand(), declared at
 *   smoke-co-alarm-server.h:178 and called from smoke-co-alarm-server.cpp
 *   :112 (RequestSelfTest) and :450 (HandleRemoteSelfTestRequest), has NO
 *   weak default anywhere under src/; the only definitions in the tree are
 *   example apps'. Omitting it is a link error. Defined in
 *   mt_matter_zephyr.cpp with the other command hooks, notify-only.
 *
 * Both features, SmokeAlarm 0x1 and CoAlarm 0x2 (SmokeCoAlarm/Enums.h:
 * 115-119): AT+MTALARM's field table has no single-feature variant, the
 * same reason the C6 enables both, so both feature-gated state attributes
 * exist and FeatureMap is 3, which is also the XML's own declared default
 * (smoke-co-alarm-cluster.xml:37). Identify is mandatory on this device
 * type (matter-devices.xml:2522-2545, MA-smokecoalarm), so the port
 * convention holds here, in contrast to the power source above. The
 * device type's Power Source mandate is satisfied the C6's way: a flat
 * sibling 0x0011 endpoint the host declares alongside, no composition
 * tree.
 *
 * Nine attributes, every one a 1-byte enum or boolean plus the two
 * globals (smoke-co-alarm-cluster.xml:49-76), so all of them slot
 * cleanly. The optional DeviceMuted, InterconnectSmokeAlarm,
 * InterconnectCOAlarm, ContaminationState and SmokeSensitivityLevel are
 * NOT declared, matching the C6's composition exactly; an AT+MTALARM
 * write to one of those fields passes its range check and then fails
 * with a bare ERROR because the setter's underlying attribute write
 * fails, the identical behaviour on both platforms. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(smokeCoAlarmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::ExpressedState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::SmokeState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::COState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::BatteryAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::TestInProgress::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::HardwareFaultAlert::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::EndOfServiceAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kSmokeCoAlarmIncoming[] = { SmokeCoAlarm::Commands::SelfTestRequest::Id,
                                                kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(smokeCoAlarmClusters)
DECLARE_DYNAMIC_CLUSTER(SmokeCoAlarm::Id, smokeCoAlarmAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kSmokeCoAlarmIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(smokeCoAlarmEndpoint, smokeCoAlarmClusters);

constexpr EmberAfDeviceType kSmokeCoAlarmTypes[] = { { 0x0076, 1 } };

/* ---- laundry washer (0x0073), dishwasher (0x0075), laundry dryer (0x007C)
 *
 * Catalogue batch 4 audit, OperationalState (0x0060). The three appliance
 * types compose identically (OperationalState + Identify + Descriptor;
 * the C6's three config_t are literally one aliased type and its three
 * add() bodies differ only in the device type id), so one cluster list
 * and one EmberAfEndpointType serve all three, the
 * booleanStateSensorClusters principle; only the EmberAfDeviceType (id,
 * revision) differs per registry row. No mode cluster, no controls
 * cluster, no Temperature Control: the device library makes Operational
 * State the only mandatory application cluster on all three, and the C6
 * composes the same.
 *
 * Identify IS composed here although the device XML lists it server="false"
 * on all three types (matter-devices.xml:2718 washer, :2609 dishwasher,
 * :2749 dryer) and the C6 composes only OperationalState plus Descriptor.
 * Optional-not-forbidden: server="false" under lockOthers means "not
 * required as a server", not "locked out" (serverLocked="false" on the same
 * lines), so adding it is legal, and the port's standing convention
 * (Identify rides on every device type; the batch 4 brief directed it for
 * this trio) wins over C6 parity here. The chime below makes the OPPOSITE
 * call on the same XML shape because there the C6 also omits it and no
 * brief direction says otherwise; the divergence is priced in the heap
 * table and recorded in the batch report.
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; regenerating hearth.zap with the cluster on ep240 left
 *   CodeDrivenInitShutdown.cpp byte-identical). It IS
 *   CommandHandlerInterface-only (config-data.yaml:63), so no generated
 *   IMClusterCommandHandler dispatch exists or is needed; the same
 *   regeneration left IMClusterCommandHandler.cpp byte-identical too, and
 *   invokes reach the Instance's own CommandHandlerInterface.
 *
 *   Delegate or plain ember? Instance PLUS Delegate, one PAIR per
 *   endpoint, the batch's genuinely new machinery. Instance derives from
 *   both CommandHandlerInterface and AttributeAccessInterface, each scoped
 *   MakeOptional(aEndpointId), and its constructor calls
 *   mDelegate->SetInstance(this) (operational-state-server.cpp:43-52);
 *   Delegate::SetInstance() VerifyOrDies if a second Instance tries to
 *   share one delegate object (operational-state-server.h:349-355), so the
 *   pools in mt_matter_zephyr.cpp are strictly one delegate and one
 *   Instance per endpoint, kServiceableEndpoints deep. All six cluster
 *   attributes are AAI-served, never ember (the Read() switch from
 *   operational-state-server.cpp:336: OperationalStateList :341,
 *   OperationalState :360, OperationalError :365, PhaseList :370,
 *   CurrentPhase :398, ClusterRevision :408 from Metadata kRevision).
 *   They are declared below anyway so AttributeList is truthful; the two
 *   scalars get inert slots, the arrays and the struct get none
 *   (attr_gets_slot refuses ARRAY and STRUCT; the declared ARRAY/STRUCT
 *   sizes are 0, mirroring what ZAP generates for External-storage list
 *   attributes on the static endpoint).
 *
 *   Split to know on the bench: an AT+MTATTR read goes through the classic
 *   ember path (emberAfReadAttribute) and answers this ARENA, not the
 *   Instance, so OperationalState over AT reads the inert seed (0,
 *   Stopped) no matter what the Instance last published; the fabric sees
 *   the Instance. The host's state authority is its own AT+MTOPSTATE round
 *   trip, and Instance-owned attributes never raise +MTATTR URCs, the
 *   same rule the C6 documents for every Instance-served cluster.
 *
 *   ServerInit? None by name: no emberAfOperationalStateClusterServerInit
 *   Callback exists, and MatterOperationalStatePluginServerInitCallback is
 *   an empty definition in src/app/util/util.cpp:131. The REAL init is
 *   Instance::Init(), the application's job, and its first act is
 *   `if (!emberAfContainsServer(mEndpointId, mClusterId)) return
 *   CHIP_ERROR_INVALID_ARGUMENT` (operational-state-server.cpp:65-70),
 *   which forces construct-and-Init() to run only AFTER
 *   emberAfSetDynamicEndpoint() succeeds: the same forced ordering as the
 *   valve's SetDefaultDelegate(), one mechanism further in. It does NOT
 *   join the B388 call site (that site substitutes for the ember
 *   INIT_FUNCTION slot; this is not that mechanism); construction happens
 *   in mt_matter_opstate_delegate_set_endpoint(), the delegate handout's
 *   second half in mt_devtype_create() below.
 *
 *   Never destroyed, matching the valve pool and the allocate-only block
 *   heap: ~Instance() would unregister both interfaces
 *   (operational-state-server.cpp:55-63), but no teardown path exists on
 *   this platform (the composition is edited by AT+MTEP and applied by a
 *   reboot, which resets every pool wholesale, the same policy the valve
 *   delegate pool set), so no explicit destructor call is ever needed or
 *   made.
 *
 * Commands are hand-declared: the Instance registers NO
 * EnumerateAcceptedCommands override, so AcceptedCommandList comes from
 * this ember metadata, and operational_state::create() on the C6 famously
 * created no command entries at all (finding F-C10-1, every invoke
 * answered 0x81 until the thunk hand-added them). Pause 0x00, Stop 0x01,
 * Start 0x02, Resume 0x03; the OperationalCommandResponse is the
 * server-to-client response, so it lives in the OUTGOING list (fix round
 * I1/DE399, see kOpStateOutgoing below), never the incoming one.
 *
 * The delegate publishes exactly Stopped/Running/Paused/Error in
 * OperationalStateList and answers CHIP_ERROR_NOT_FOUND at phase index 0
 * so PhaseList reads null; the verdict mapping (allow kNoError, deny
 * kUnableToCompleteOperation, and why the delegate never calls
 * SetOperationalState on allow) is on HearthOpStateDelegate in
 * mt_matter_zephyr.cpp. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(opStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::PhaseList::Id, ARRAY, 0,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::CurrentPhase::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalStateList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalError::Id, STRUCT, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kOpStateIncoming[] = { OperationalState::Commands::Pause::Id,
                                           OperationalState::Commands::Stop::Id,
                                           OperationalState::Commands::Start::Id,
                                           OperationalState::Commands::Resume::Id,
                                           kInvalidCommandId };

/* Fix round, review I1, RULED (graph DE399): the outgoing list is declared,
 * NOT left nullptr like every other cluster in this file, because this is
 * the port's first cluster that answers its commands with a RESPONSE
 * COMMAND rather than a plain status: every one of the four handlers
 * builds an OperationalCommandResponse and AddResponse()s it
 * (operational-state-server.cpp:443-446 for Pause, and identically in the
 * Stop/Start/Resume handlers), and the cluster spec requires that command
 * in GeneratedCommandList when its triggers are supported. This KNOWINGLY
 * diverges from the C6, whose identical nullptr gap is now tracked
 * separately as bug B400: fabric metadata truthfulness beats parity of an
 * untruth, and GeneratedCommandList is not part of the AT contract, so no
 * host-visible behaviour moves. */
constexpr CommandId kOpStateOutgoing[] = {
    OperationalState::Commands::OperationalCommandResponse::Id, kInvalidCommandId
};

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(applianceOpStateClusters)
DECLARE_DYNAMIC_CLUSTER(OperationalState::Id, opStateAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kOpStateIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(applianceOpStateEndpoint, applianceOpStateClusters);

constexpr EmberAfDeviceType kLaundryWasherTypes[] = { { 0x0073, 2 } };
constexpr EmberAfDeviceType kDishwasherTypes[] = { { 0x0075, 2 } };
constexpr EmberAfDeviceType kLaundryDryerTypes[] = { { 0x007C, 2 } };

/* ---- mode select (0x0027) ---------------------------------------------
 *
 * Catalogue batch 4 audit, ModeSelect (0x0050).
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; the regeneration left CodeDrivenInitShutdown.cpp
 *   byte-identical). NOT CommandHandlerInterface-only either, so
 *   ChangeToMode needs the generated IMClusterCommandHandler dispatch,
 *   which the regeneration added, exactly one case; that dispatch is why
 *   the cluster had to enter hearth.zap.
 *
 *   Delegate or plain ember? Neither exactly. Description,
 *   StandardNamespace and CurrentMode are plain ember against this arena;
 *   SupportedModes is served by gModeSelectAttrAccess, a wildcard AAI
 *   registered once from MatterModeSelectPluginServerInitCallback
 *   (mode-select-server.cpp:416-419), which reads a single PROCESS-GLOBAL
 *   SupportedModesManager fetched fresh on every access
 *   (getSupportedModesManager(), :57; the setter at :62 stores a bare
 *   global pointer). One manager for the whole device, not per endpoint:
 *   the manager itself dispatches on endpoint id, so a second mode select
 *   endpoint re-registering it is harmless, and the registration lives in
 *   mt_matter_mode_select_manager() (mt_matter_zephyr.cpp), called from
 *   mt_devtype_create() before any resource is spent.
 *
 *   ServerInit? YES, a real strong body, and this cluster JOINS the B388
 *   by-hand call site. emberAfModeSelectClusterServerInitCallback
 *   (mode-select-server.cpp:318-398) is reachable only through the
 *   per-cluster functions array (Mode Select sits in
 *   ClustersWithInitFunctions, config-data.yaml:97; the XML itself says
 *   init="false", mode-select-cluster.xml:44), and DECLARE_DYNAMIC_CLUSTER
 *   nulls that array, the exact B388 mechanism. Called by hand below next
 *   to LevelControl/ColorControl. What it does HERE, traced in this tree
 *   rather than assumed: its volatility gate passes on this endpoint
 *   (emberAfIsKnownVolatileAttribute, util.cpp:156-166, answers false both
 *   for our EXTERNAL-storage CurrentMode, IsExternal, and for the
 *   undeclared StartUpMode, metadata null), and the body then skips at
 *   Attributes::StartUpMode::Get() answering UnsupportedAttribute
 *   (:334), StartUpMode not being declared. So the init is a no-op on
 *   this composition today; it is called anyway because the moment a
 *   later round declares StartUpMode, a dynamic endpoint that never ran
 *   it boots with the wrong CurrentMode, which is exactly bug B388's
 *   class. Bench-verify item: confirm no CurrentMode write and no error
 *   log at rebuild time.
 *
 * Identify is composed per convention AND per mandate: unlike the trio
 * above, this device type requires the Identify server outright
 * (matter-devices.xml:2470, MA-modeselect).
 *
 * Description (char_string 64, so 65 ember bytes) gets no slot and reads
 * UNSUPPORTED_ATTRIBUTE on this platform, pending a string-store
 * decision; declared so AttributeList is truthful, same note as the
 * power source's Description. StartUpMode and OnMode are optional and
 * deliberately not declared (no DEPONOFF feature, no OnOff cluster on
 * this endpoint), which also parks the cluster's
 * PreAttributeChanged mode-membership guard: it only checks those two
 * attributes (mode-select-server.cpp:434-454, dispatching to
 * verifyModeValue at :462), is function-array-bound
 * anyway, and CurrentMode is not writable over the fabric. A
 * controller's ChangeToMode validates membership against the manager and
 * writes CurrentMode itself (emberAfModeSelectClusterChangeToModeCallback,
 * :284-311), which lands in this arena and reaches the host as an
 * ordinary +MTATTR URC, the C6's documented contract for this type. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(modeSelectAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::Description::Id, CHAR_STRING, 65, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::StandardNamespace::Id, ENUM16, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::SupportedModes::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::CurrentMode::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kModeSelectIncoming[] = { ModeSelect::Commands::ChangeToMode::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(modeSelectClusters)
DECLARE_DYNAMIC_CLUSTER(ModeSelect::Id, modeSelectAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kModeSelectIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(modeSelectEndpoint, modeSelectClusters);

constexpr EmberAfDeviceType kModeSelectTypes[] = { { 0x0027, 1 } };

/* ---- chime (0x0146) ---------------------------------------------------
 *
 * Catalogue batch 4 audit, Chime (0x0556; the DEVICE type is 0x0146, the
 * cluster 0x0556, easy to transpose). Marked apiMaturity="provisional" in
 * this tree's XML (chime-cluster.xml:31).
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; regeneration left CodeDrivenInitShutdown.cpp byte-identical).
 *   It IS CommandHandlerInterface-only (config-data.yaml:34), so
 *   PlayChimeSound reaches the per-endpoint ChimeServer's own
 *   CommandHandlerInterface and the same regeneration left
 *   IMClusterCommandHandler.cpp byte-identical too.
 *
 *   Delegate or plain ember? Per-endpoint ChimeServer object PLUS a
 *   ChimeDelegate, the OperationalState shape with the roles renamed.
 *   ChimeServer(EndpointId, ChimeDelegate&) privately inherits
 *   AttributeAccessInterface and CommandHandlerInterface
 *   (chime-server.h:35, ctor :45) and is non-default-constructible, so
 *   its pool in mt_matter_zephyr.cpp is alignas raw storage,
 *   placement-constructed once per endpoint and never destroyed, the
 *   same policy as the OperationalState Instances. Init() (:52, impl
 *   chime-server.cpp:59-66) loads the two persisted attributes and
 *   registers both endpoint-scoped interfaces, so it runs only AFTER
 *   emberAfSetDynamicEndpoint() succeeds; LoadPersistentAttributes()
 *   keys its KVS reads on GetEndpointId() (chime-server.cpp:68-97).
 *
 *   ServerInit? None: MatterChimePluginServerInitCallback and its
 *   Shutdown twin are empty bodies (chime-server.cpp:285-286), the XML
 *   says init="false" (chime-cluster.xml:38). Does NOT join B388; the
 *   construction lives in mt_matter_chime_delegate_set_endpoint().
 *
 * NO Identify, matching the C6: the device type lists Identify with
 * server="false" (matter-devices.xml:1179-1192, MA-chime), so only the
 * Chime cluster and Descriptor are mandated.
 *
 * SelectedChime and Enabled are AAI-SHADOWED: the ChimeServer holds them
 * as members (chime-server.h:98-99), serves every fabric read and write
 * through its own AAI, and PERSISTS writes through
 * SafeAttributePersistenceProvider into the settings partition
 *   (SetSelectedChime/SetEnabled, chime-server.cpp:206-248; restored by
 *   LoadPersistentAttributes at every endpoint create). Two consequences
 *   worth naming: the ember slots declared below are inert on the fabric
 *   (declared WRITABLE to keep the metadata honest about the cluster's
 *   contract), and unlike everything else in this catalogue the pair
 *   SURVIVES a reboot server-side, so a chatty host driving AT+MTCHIME is
 *   a new ZMS wear source on a settings_storage partition whose occupancy
 *   is already a watch item (platform README, measured section).
 *
 *   Fix round, review M6, platform note: the persistence keys on
 *   ConcreteAttributePath(GetEndpointId(), Chime::Id, ...) and NOTHING on
 *   this platform ever clears those keys (no endpoint teardown path
 *   exists), so the pair survives a COMPOSITION change too, not just a
 *   reboot: a host that re-edits its composition such that a chime lands
 *   on an endpoint id a previous chime once occupied inherits that
 *   chime's persisted SelectedChime and Enabled. The C6 does not have
 *   this (its pair was not KVS-persisted). Deliberately documented, not
 *   cleared-on-create: writing the fresh defaults at create would also
 *   erase the LEGITIMATE reboot persistence the server implements on
 *   purpose, and the create path cannot tell "same chime as last boot"
 *   from "different chime, recycled endpoint id". Belongs in the
 *   controller's AT_MT_SPEC amendment beside the DE396 payload note.
 *
 * InstalledChimeSounds is the host-fed array (AT+MTCHIMESOUNDS), served
 * by the server's AAI walking the delegate's GetChimeSoundByIndex();
 * no slot, declared for AttributeList truth.
 *
 * The PlayChimeSound wire divergence from the C6, DECIDED (user ruling
 * DE396): in THIS tree the command takes NO argument
 * (chime-cluster.xml:43-45 declares none; the delegate's PlayChimeSound()
 * is argument-free, chime-server.h:164), where the esp tree's revision
 * carries an optional ChimeID. The +MTCMD payload arity stays
 * byte-identical to the C6 by forwarding the owning server's
 * GetSelectedChime() as the single trailing field: the payload means
 * "the chime id that will play". Also gone in this revision, both spec
 * paragraphs handled by the controller side: the unknown-chimeID
 * NotFound pre-check (HandlePlayChimeSound checks only mEnabled,
 * chime-server.cpp:260-274) and the ChimeStartedPlaying event (no
 * <event> in chime-cluster.xml at all). Enabled false answers Success
 * with NO delegate call (:266-271), so no +MTCMD is raised then, same
 * as the C6. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(chimeAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Chime::Attributes::InstalledChimeSounds::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Chime::Attributes::SelectedChime::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Chime::Attributes::Enabled::Id, BOOLEAN, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Chime::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kChimeIncoming[] = { Chime::Commands::PlayChimeSound::Id, kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(chimeClusters)
DECLARE_DYNAMIC_CLUSTER(Chime::Id, chimeAttrs, ZAP_CLUSTER_MASK(SERVER), kChimeIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(chimeEndpoint, chimeClusters);

constexpr EmberAfDeviceType kChimeTypes[] = { { 0x0146, 1 } };

/* ---- generic switch (0x000F) ------------------------------------------
 *
 * Catalogue batch 5 audit, Switch (0x003B). The port's first EVENT
 * EMITTER: this device type exists to raise Switch events at controllers,
 * not to serve writable state.
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; regenerating hearth.zap with the cluster on ep240 left
 *   CodeDrivenInitShutdown.cpp byte-identical, the batch 3 method). Not
 *   CommandHandlerInterface-only either (config-data.yaml:21-88), which is
 *   harmless: switch-cluster.xml declares NO commands at all (:83-151 is
 *   attributes and events only), so no generated dispatch case exists or
 *   is needed, and the same regeneration left IMClusterCommandHandler.cpp
 *   byte-identical too.
 *
 *   Delegate or plain ember? Plain ember plus a STATELESS singleton.
 *   SwitchServer is `static SwitchServer instance` (switch-server.h:82,
 *   defined switch-server.cpp:41) with no data members and no per-endpoint
 *   state: all seven On* methods take an EndpointId and do nothing but
 *   build the event struct and LogEvent() it. No delegate, no AAI, no
 *   Init(), no pool, nothing to hand out at create time. The declared
 *   value attributes are ordinary ember external storage against this
 *   arena.
 *
 *   ServerInit? None. MatterSwitchPluginServerInitCallback and its
 *   Shutdown twin are empty bodies (switch-server.cpp:154-155), no
 *   emberAfSwitchCluster*InitCallback exists, and the XML says
 *   <server init="false" tick="false"> (switch-cluster.xml:31). Does NOT
 *   join the B388 call site.
 *
 *   Event emission, the load-bearing part. The server never self-emits:
 *   mt_matter_switch_click() (mt_matter_zephyr.cpp) calls
 *   SwitchServer::Instance().OnInitialPress(ep, 1), whose whole body is
 *   LogEvent(event, endpoint, eventNumber) (switch-server.cpp:66-78). On a
 *   dynamic endpoint DECLARE_DYNAMIC_CLUSTER leaves eventList/eventCount
 *   zero (attribute-storage.h:48-51 supplies only eight initializers), so
 *   the EventList global attribute reads EMPTY on this endpoint; that does
 *   NOT block emission, which never consults it. The event's read
 *   privilege resolves through CodegenDataModelProvider::EventInfo(),
 *   which falls back to RequiredPrivilege::ForReadEvent(path) when no
 *   registered cluster object claims the path
 *   (CodegenDataModelProvider.cpp:249-257); this build's generated
 *   access.h carries event privilege overrides only for Access Control
 *   (access.h:284-300, byte-identical through this batch's regeneration),
 *   so InitialPress defaults to kView and no access.h change is needed.
 *   InitialPress is priority="info" (switch-cluster.xml:98) and lands in
 *   the info event buffer the image already allocates. Argued on the
 *   source end to end; the bench proof (a subscribed controller receiving
 *   InitialPress from a dynamic endpoint) is the controller's, recorded in
 *   the batch report.
 *
 * FeatureMap 0x02 is MomentarySwitch (Switch/Enums.h:35): the composed
 * appendix mandates MS for this device type
 * (Device-Library-Specification.md:8075), it matches the C6 thunk's
 * explicit choice (mk_generic_switch(), mt_devtypes.cpp:257-270, where a
 * feature_flags of 0 would assert in esp-matter's VALIDATE_FEATURES_
 * EXACT_ONE), and it is what makes the InitialPress event conformant.
 * MultiPressMax is MSM-gated (switch-cluster.xml:85-89) and not declared;
 * the richer momentary features (MSR, MSL, MSM) are not advertised and
 * their five events are never emitted on either platform.
 *
 * The writability split, stated precisely (batch 5 fix round, review
 * I2, which caught this note over-claiming "read-only over AT"):
 * NumberOfPositions and CurrentPosition carry no WRITABLE mask below, so
 * a CONTROLLER'S IM write is refused; that is the read-only the device
 * library means and what AT_MT_SPEC.md 462-467 describes. A LOCAL
 * AT+MTATTR write, however, reaches ember and lands in the arena: this
 * port's generic write bridge deliberately has no IsWritable() gate (the
 * argued delta at mt_matter_zephyr.cpp:779 and :805-822, where a gate
 * would break the bench-mandated MeasuredValue write), and that is C6
 * parity, not a gap: esp-matter's set_val() skips its WRITABLE-less
 * refusal for a plain, not-managed-internally attribute, which both
 * create_number_of_positions() and create_current_position() produce. So
 * "no writable attribute" here means no attribute a host has any REASON
 * to write (the type is an event emitter; a host that pokes
 * CurrentPosition is talking to itself) and none a controller CAN write.
 * CurrentPosition is NOT updated by the event path on either platform
 * (SwitchServer::OnInitialPress only calls LogEvent). The command that
 * drives the type is AT+MTSWITCH. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(switchAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Switch::Attributes::NumberOfPositions::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Switch::Attributes::CurrentPosition::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Switch::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(genericSwitchClusters)
DECLARE_DYNAMIC_CLUSTER(Switch::Id, switchAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(genericSwitchEndpoint, genericSwitchClusters);

/* Revision 3 per data_model/1.5/device_types/GenericSwitch.xml. */
constexpr EmberAfDeviceType kGenericSwitchTypes[] = { { 0x000F, 3 } };

/* ---- the featureless OnOff list (pump 0x0303, room air conditioner
 * 0x0072) ---------------------------------------------------------------
 *
 * Catalogue batch 5. A THREE-slot OnOff list for device types whose OnOff
 * cluster carries no Lighting feature: OnOff, FeatureMap and the implicit
 * ClusterRevision, nothing else. onOffAttrs above must NEVER be reused for
 * these types, because it declares GlobalSceneControl, OnTime, OffWaitTime
 * and StartUpOnOff, and all four are mandatoryConform ON FEATURE LT in the
 * cluster XML (onoff-cluster.xml:92-112): declaring them on an endpoint
 * whose FeatureMap is 0 (pump) or 0x02 (RAC's DeadFrontBehavior) is a
 * conformance break no build check would catch. The C6 composes the same
 * three-attribute shape for both types (pump::add() and
 * room_air_conditioner::add() call on_off::create() with no lighting::add;
 * the RAC adds dead_front_behavior::add(), which sets the feature bit and
 * creates nothing, esp_matter_feature.cpp:491-505).
 *
 * The two types need DIFFERENT FeatureMap seeds on the same cluster (pump
 * 0x00, RAC 0x02 DeadFrontBehavior, the latter mandatory per the RAC's
 * element requirements, Device-Library-Specification.md:5137), which is
 * exactly what s_seeds' per-device-type qualifier exists for: two rows
 * naming 0x0303 and 0x0072 win over the wildcard OnOff FeatureMap row
 * (0x01, the Lighting types'). Named for the RAC because that type is the
 * reason the list exists; the pump lands first in the build order and
 * shares it. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(racOnOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* ---- pump (0x0303) ----------------------------------------------------
 *
 * Catalogue batch 5 audit, PumpConfigurationAndControl (0x0200).
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; regenerating hearth.zap with the cluster on ep240 left
 *   CodeDrivenInitShutdown.cpp byte-identical). Not CHI-only either
 *   (config-data.yaml:21-88), harmlessly: the cluster declares no commands
 *   at all (pump-configuration-and-control-cluster.xml is attributes and
 *   events only), and the regeneration left IMClusterCommandHandler.cpp
 *   byte-identical too. One generated file did move, against the batch
 *   audit's prediction: access.h gained the OperationMode write-privilege
 *   row (the XML gives it access op="write" privilege="manage"), joining
 *   the DoorLock/WindowCovering/Thermostat manage-write entries already
 *   there. That is the generated access module doing its job for a
 *   controller write; nothing on this port consults it locally.
 *
 *   Delegate or plain ember? Plain ember, no server object of any kind:
 *   pump-configuration-and-control-server.cpp registers no AAI and defines
 *   no class; every declared attribute is external storage against this
 *   arena.
 *
 *   ServerInit? The cluster sits in ClustersWithInitFunctions,
 *   ClustersWithAttributeChangedFunctions AND
 *   ClustersWithPreAttributeChangeFunctions (config-data.yaml:100, :111,
 *   :136), all three reached only through the per-cluster functions array
 *   DECLARE_DYNAMIC_CLUSTER nulls, so none of them runs here. Its
 *   emberAf...ServerInitCallback is a real strong body whose entire
 *   content is one ChipLogProgress line (pump-configuration-and-control-
 *   server.cpp:248-251): it does NOT join the B388 call site, whose bar is
 *   "per-endpoint boot state the endpoint would otherwise never get", and
 *   running a log line buys none. Do not add it later either: the
 *   attribute-changed callback's setEffectiveModes() reads
 *   emberAfLocateAttributeMetadata(...)->defaultValue unconditionally when
 *   ControlMode is absent (:82-86), which on a dynamic endpoint is
 *   ZAP_EMPTY_DEFAULT garbage; unreachable today only because the
 *   functions array is null.
 *
 *   Two never-run callbacks, documented as HOST responsibilities rather
 *   than rediscovered on a bench:
 *     - MatterPumpConfigurationAndControlClusterServerAttributeChanged
 *       Callback (:355-369) derives EffectiveOperationMode/
 *       EffectiveControlMode from OperationMode/ControlMode. It never runs
 *       on a dynamic endpoint, so a write of OperationMode (host AT+MTATTR
 *       or controller IM) does NOT update EffectiveOperationMode by
 *       itself; the host owns that coupling, the FanControl
 *       FanMode/PercentSetting precedent above.
 *     - The PreAttributeChanged guard (:253-353) answers ConstraintError
 *       for a ControlMode value whose feature is absent. ControlMode is
 *       not composed here (optional, C6 parity), so a controller cannot
 *       change the control mode at all and the guard is moot twice over.
 *
 * Feature ConstantSpeed (0x8, PumpConfigurationAndControl/Enums.h:67), the
 * C6 thunk's documented choice (mk_pump(), mt_devtypes.cpp:467-478:
 * esp-matter VALIDATEs at least one operation-mode feature and constant
 * speed is the least constrained), which is what makes MinConstSpeed and
 * MaxConstSpeed mandatory and everything else feature-gated-absent.
 * ControlModeEnum::kConstantSpeed and OperationModeEnum::kNormal are both
 * 0x00 (Enums.h:34, :50), so the three zero-fill mode seeds are legal
 * under this feature map. Eleven slots; the optional PumpStatus and
 * ControlMode are not created, matching the C6. Eight events are declared
 * in the XML, all optional, none emitted by either platform. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(pumpAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxPressure::Id, INT16S, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxSpeed::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxFlow::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MinConstSpeed::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::MaxConstSpeed::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::EffectiveOperationMode::Id,
                              ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::EffectiveControlMode::Id,
                              ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::Capacity::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::OperationMode::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PumpConfigurationAndControl::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(pumpClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(PumpConfigurationAndControl::Id, pumpAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(pumpEndpoint, pumpClusters);

/* Revision 3 per data_model/1.5/device_types/Pump.xml. */
constexpr EmberAfDeviceType kPumpTypes[] = { { 0x0303, 3 } };

/* ---- room air conditioner (0x0072) ------------------------------------
 *
 * Catalogue batch 5. No new cluster at all: a new CLUSTER LIST combining
 * the featureless OnOff (racOnOffAttrs above, FeatureMap seeded 0x02
 * DeadFrontBehavior per the element requirements,
 * Device-Library-Specification.md:5137) with the existing Thermostat,
 * Identify and Descriptor. The C6's room_air_conditioner::add()
 * (esp_matter_endpoint.cpp:1274-1288) creates Identify, OnOff with
 * dead_front_behavior plus On/Toggle, and Thermostat after an
 * unconditional `|= cooling` on the feature flags; its thunk
 * (mk_room_air_conditioner(), mt_devtypes.cpp:429-465) ORs Heating in as
 * well, because the device library does not restrict the Thermostat to
 * Cool-only and the host library writes OccupiedHeatingSetpoint
 * unconditionally. The optional Groups/ScenesManagement/HEPA/
 * ActivatedCarbon/FanControl are composed on neither platform
 * (Device-Library-Specification.md:5093-5104; the C6 takes no optionals,
 * so resource-monitoring stays out of this batch entirely).
 *
 * Every Thermostat seed row above is already correct for this device
 * type, verified rather than assumed: FeatureMap 0x03 Heating|Cooling,
 * ControlSequenceOfOperation 4 kCoolingAndHeating, SystemMode 0 Off (the
 * first-write-swallowed cache reasoning on thermostatAttrs), setpoints
 * 1600/2400 (the C6's cross-layer I1 values) and the 700/3000/1600/3200
 * limits. So this type adds NO Thermostat rows and no thermostat code;
 * only the OnOff FeatureMap row keyed 0x0072 (beside the pump's in
 * s_seeds) is its own.
 *
 * Reusing onOffAttrs here instead of racOnOffAttrs would declare the four
 * Lighting-gated attributes on a FeatureMap 0x02 cluster, the conformance
 * break the racOnOffAttrs comment names; that is the one trap on this
 * type. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(roomAirConditionerClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(roomAirConditionerEndpoint, roomAirConditionerClusters);

/* Revision 3 per data_model/1.5/device_types/RoomAirConditioner.xml. */
constexpr EmberAfDeviceType kRoomAirConditionerTypes[] = { { 0x0072, 3 } };

/* ---- robotic vacuum cleaner (0x0074) ----------------------------------
 *
 * Catalogue batch 5's weight: RvcRunMode 0x0054 + RvcCleanMode 0x0055
 * (one server, mode-base-server) and RvcOperationalState 0x0061, plus
 * Identify and Descriptor. Five clusters, fourteen slots, and the two
 * host-fed ModeBase stores that make this the widest block in the
 * catalogue (see the sizing table below).
 *
 * The C6 composes the same five: robotic_vacuum_cleaner::add()
 * (esp_matter_endpoint.cpp:1389-1400) creates Identify, RvcRunMode and
 * RvcOperationalState (plus its OperationCompletion event), and the thunk
 * hand-adds RvcCleanMode after create() (mk_rvc(), mt_devtypes.cpp:
 * 846-852). RvcCleanMode is optional in the device library
 * (Device-Library-Specification.md:4780) and STAYS composed here, ruled
 * DE404: parity with the C6's deliberate hand-add beats the one design
 * lever that would have bought capacity (dropping it would cost a real
 * cross-platform data-model divergence to fit 16 endpoints instead of 9).
 *
 * ---- the two ModeBase clusters (RvcRunMode, RvcCleanMode) -------------
 *
 *   Code-driven? No for both (config-data.yaml:144-168; this batch's
 *   regeneration left CodeDrivenInitShutdown.cpp byte-identical). Both
 *   ARE CommandHandlerInterface-only ("RVC Clean Mode" config-data.yaml:
 *   67, "RVC Run Mode" :69): ChangeToMode reaches the per-(endpoint,
 *   cluster) ModeBase::Instance's own handler and IMClusterCommandHandler
 *   stayed byte-identical too.
 *
 *   Delegate or plain ember? ModeBase::Instance PLUS ModeBase::Delegate,
 *   one PAIR per (endpoint, cluster): both interfaces are scoped
 *   Optional<EndpointId>(aEndpointId) with the cluster id
 *   (mode-base-server.cpp:44-53), so the one RVC endpoint carries TWO
 *   pairs. The pools and the Init() ordering rules live in
 *   mt_matter_zephyr.cpp beside the delegate; the create-path handout is
 *   below in mt_devtype_create(). The ordering is this batch's sharpest
 *   constraint and is documented at both ends: Instance::Init()
 *   VerifyOrDies on emberAfContainsServer (mode-base-server.cpp:77), a
 *   PANIC where the opstate trio's same check soft-bails, so construction
 *   and Init() run strictly below a successful emberAfSetDynamicEndpoint().
 *
 *   Which attributes the Instance's AAI answers: CurrentMode, StartUpMode,
 *   OnMode, FeatureMap and SupportedModes (Instance::Read(),
 *   mode-base-server.cpp:325-347), and the switch has NO default arm, so
 *   ClusterRevision falls through to ember and its seeds are LIVE:
 *   RvcRunMode 4 and RvcCleanMode 5 per this tree's Metadata.h:20, per
 *   cluster in s_seeds. The CurrentMode and FeatureMap slots are inert
 *   shadows (the opstate split), carved out for AT+MTATTR by
 *   k_instance_served in mt_matter_zephyr.cpp. StartUpMode and OnMode are
 *   optional and deliberately not declared, the ModeSelect precedent.
 *
 *   Neither joins B388: MatterRvcRunMode/RvcCleanModePluginServerInit
 *   Callback are empty bodies (src/app/util/util.cpp:126-127), no
 *   emberAf...InitCallback exists, and the real init is Instance::Init(),
 *   the create path's job.
 *
 *   The one metadata list serves BOTH clusters: the ModeBase aliases share
 *   attribute ids (SupportedModes 0x0000, CurrentMode 0x0001) and command
 *   ids (ChangeToMode 0x00 / ChangeToModeResponse 0x01, verified against
 *   both clusters' generated CommandIds.h), so modeBaseAttrs and the two
 *   command lists below are declared once, the shared-metadata principle
 *   at the top of this file. The response command is declared in the
 *   outgoing list for the same two reasons the opstate trio's is
 *   (DE399 truthfulness AND, here, C6 parity: esp-matter's
 *   rvc_run_mode/rvc_clean_mode::create() add ChangeToModeResponse
 *   unconditionally).
 *
 * ---- RvcOperationalState ----------------------------------------------
 *
 *   Costs no new translation unit: zap_cluster_list.json maps
 *   OPERATIONAL_STATE_RVC_CLUSTER to operational-state-server, already in
 *   this build, and RvcOperationalState::Instance lives in the same
 *   operational-state-server.cpp (:520-570). CHI-only (config-data.yaml:
 *   68), no generated dispatch. The Instance derives from
 *   OperationalState::Instance, constructed Instance(Delegate*, ep)
 *   forwarding Id as the cluster id (operational-state-server.h:421-423);
 *   the delegate is a NEW subclass (HearthRvcOpStateDelegate,
 *   mt_matter_zephyr.cpp) because the base HearthOpStateDelegate has no
 *   GoHome hook, and it takes its own pool: the base
 *   Delegate::SetInstance() VerifyOrDies on sharing
 *   (operational-state-server.h:349-353), so handing an RVC Instance a
 *   pooled trio delegate would abort the device.
 *
 *   The incoming command list is EXACTLY {Pause 0x00, Resume 0x03, GoHome
 *   0x80}, and declaring Start/Stop would be a real defect, not just
 *   noise: Instance::InvokeCommand() switches on Pause/Resume/Start/Stop
 *   unconditionally before InvokeDerivedClusterCommand()
 *   (operational-state-server.cpp:296-334), so a declared Start would
 *   route a controller invoke into the base handler and out to the
 *   derived delegate's kUnknownEnumValue stub, where the RVC cluster XML
 *   declares no Start/Stop at all. The outgoing list carries
 *   OperationalCommandResponse 0x04, the DE399 discipline (kOpStateOutgoing
 *   reused: same numeric id on the derived cluster's CommandIds.h).
 *
 *   Server-side guards that run BEFORE the delegate, so the +MTCMD
 *   forwards only ever carry legitimate adjudication requests: Pause is
 *   additionally compatible with kSeekingCharger (:522-525), Resume with
 *   kCharging/kDocked (:527-531); GoHome from kCharging/kDocked answers
 *   kCommandInvalidInState WITHOUT calling the delegate (:556-559) and
 *   from kSeekingCharger answers success without calling it either
 *   (:561).
 *
 *   Attribute split: Instance::Read() (operational-state-server.cpp:
 *   335-408) serves everything except FeatureMap, which falls through to
 *   ember (live seed 0); ClusterRevision is answered by the AAI as the
 *   BASE cluster's kRevision constant (:408-409), so unlike the ModeBase pair
 *   the revision seed here is INERT; both constants are 1 in this tree
 *   (OperationalState/Metadata.h:20, RvcOperationalState/Metadata.h:20)
 *   and the seed is written 1 anyway for truthful arena metadata. The
 *   opStateAttrs list above is reused verbatim: the derived cluster
 *   shares the base's attribute ids and this composition declares the
 *   same subset (CountdownTime stays optional-absent on both platforms).
 *
 *   Events: OperationalError and OperationCompletion are declared in the
 *   XML (the former optional="false"); this firmware emits neither, the
 *   C6 emits neither, and the dynamic endpoint's empty eventList makes
 *   EventList read empty without blocking emission (the generic switch
 *   note above). The C6's rvc create() has its own conformance gap here
 *   (no OperationalError event metadata where the base cluster creates
 *   one); nothing for this port to mirror since dynamic eventList is
 *   empty regardless.
 *
 * Revision 4 per data_model/1.5/device_types/RoboticVacuumCleaner.xml. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(modeBaseAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(RvcRunMode::Attributes::SupportedModes::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RvcRunMode::Attributes::CurrentMode::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RvcRunMode::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kModeBaseIncoming[] = { RvcRunMode::Commands::ChangeToMode::Id,
                                            kInvalidCommandId };
constexpr CommandId kModeBaseOutgoing[] = { RvcRunMode::Commands::ChangeToModeResponse::Id,
                                            kInvalidCommandId };

constexpr CommandId kRvcOpStateIncoming[] = { RvcOperationalState::Commands::Pause::Id,
                                              RvcOperationalState::Commands::Resume::Id,
                                              RvcOperationalState::Commands::GoHome::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(rvcClusters)
DECLARE_DYNAMIC_CLUSTER(RvcRunMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming,
                        kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(RvcCleanMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(RvcOperationalState::Id, opStateAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kRvcOpStateIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(rvcEndpoint, rvcClusters);

constexpr EmberAfDeviceType kRvcTypes[] = { { 0x0074, 4 } };

/* ---- electrical sensor (0x0510) and electrical meter (0x0514) ---------
 *
 * Catalogue batch 7a audit (batch7-audit.md sections 2.2 and 3.1/3.2),
 * every citation re-verified against the pinned NCS tree while writing.
 * The two measurement-push device types behind AT+MTMEAS, and the port's
 * first variant-carrying registry rows: variant 0 is power + energy,
 * variant 1 power only (the current-clamp case). On the C6 the variant-1
 * electrical meter DESTROYS the EEM cluster its SDK add() forced
 * (mt_devtypes.cpp:1512-1526, the B216 destroy-after-create precedent);
 * this port has no cluster::destroy() analogue, so each variant is its own
 * declared cluster list and the registry row carries both (ep_type_v1).
 *
 * ElectricalPowerMeasurement (0x0090), the batch's three-answers shape:
 *
 *   Code-driven? No (absent from config-data.yaml's CodeDrivenClusters
 *   :144-168; confirmed empirically, the batch 3 method: regenerating
 *   hearth.zap with all six batch 7a clusters left CodeDrivenInitShutdown
 *   .cpp byte-identical, sha256-checked).
 *
 *   CHI-only? Listed yes (config-data.yaml:46), harmlessly: the cluster
 *   declares no commands, so IMClusterCommandHandler.cpp stayed
 *   byte-identical too.
 *
 *   AAI? YES, per endpoint: ElectricalPowerMeasurement::Instance's Read()
 *   has a case for EVERY attribute the cluster defines including all the
 *   int64 measurement values, and no default arm (electrical-power-
 *   measurement-server.cpp:65-221), so of the declared set only
 *   ClusterRevision ever falls through to ember. The Instance is
 *   constructed by the port itself in
 *   mt_matter_meas_delegate_set_endpoint() (mt_matter_zephyr.cpp; no SDK
 *   init callback exists on this platform); its Init() is SOFT (AAI
 *   registration only, :43-47), no VerifyOrDie anywhere in this batch's
 *   energy clusters.
 *
 * THE DE407 OPTION-C DECLARATIONS: the seven push fields (Voltage,
 * ActiveCurrent, ActivePower, RMSVoltage, RMSCurrent, Frequency,
 * PowerFactor) are declared with their TRUE 64-bit ZCL types
 * (voltage_mv/amperage_ma/power_mw/int64s, electrical-power-measurement-
 * cluster.xml:64-138) and size 8, because the AAI is only consulted for
 * attributes that have ember metadata (CodegenDataModelProvider_Read.cpp:
 * 108-109, "we only allow AAI on ember-registered clusters" and the
 * metadata gate it explains) and because
 * AttributeList must be truthful. They get NO arena slot
 * (attr_gets_slot()'s existing md.size <= kSlotDataBytes refusal) and NO
 * seed, and they are in seed_slots()'s narrow per-(cluster, attribute)
 * quiet table below, so the boot log stays silent for exactly these and
 * keeps shouting for any other over-wide scalar. AT+MTATTR reads reach
 * them through the k_instance_served carve-out (mt_matter_zephyr.cpp),
 * answering the live delegate cache; writes answer +MTERR:11.
 *
 * PowerMode and NumberOfMeasurementTypes ARE slot-served over AT: their
 * seeds (kAc = 2, 1) equal the constants the delegate serves the fabric
 * (HearthEpmDelegate::GetPowerMode/GetNumberOfMeasurementTypes), the
 * FanControl seed-agreement discipline. Accuracy is ARRAY (AAI-served
 * list, no slot, quiet by type). The FeatureMap slot is an inert shadow
 * (Instance::Read() serves mFeature) seeded 0x2 AlternatingCurrent to
 * agree with the BitMask the Instance is constructed with; the
 * ClusterRevision seed is LIVE (no Read() case) and is this tree's
 * ElectricalPowerMeasurement/Metadata.h:20 kRevision = 3.
 *
 * ElectricalEnergyMeasurement (0x0091): no Instance and no delegate at
 * all; ONE wildcard AttributeAccessInterface serves every EEM endpoint
 * (ElectricalEnergyMeasurementCluster.h:36-57), registered once by
 * mt_matter_eem_register() (mt_matter_zephyr.cpp) because nothing in the
 * SDK ever calls its Init(). Its fixed Imported|Exported|Cumulative mask
 * is what every FeatureMap read on every EEM endpoint answers, so the
 * seed here is 0x7 and may never diverge from that construction site
 * (both sides cross-reference). The three value attributes are STRUCTs
 * (Accuracy MeasurementAccuracyStruct, the two cumulative counters
 * EnergyMeasurementStruct, electrical-energy-measurement-cluster.xml:
 * 61-79), declared size-0 metadata-only like batch 4's OperationalError,
 * refused by attr_gets_slot() and quiet by type; an AT+MTATTR read of any
 * of them answers +MTERR:5 from attr_type_info()'s refusal, the same code
 * the C6 answers for all of them (AT_MT_SPEC.md 3.9's 0x0510 row). The
 * ClusterRevision seed is LIVE (the wildcard AAI's Read() has no revision
 * case, ElectricalEnergyMeasurementCluster.cpp:57-130) and is
 * ElectricalEnergyMeasurement/Metadata.h:20 kRevision = 2.
 *
 * PowerTopology (0x009C, the sensor only): NodeTopology alone, the C6's
 * binding choice (mt_devtypes.cpp:1432-1447): the two endpoint-list
 * attributes exist only under the SET/TREE features and are NOT declared,
 * so the cluster contributes FeatureMap (inert shadow, seeded 0x1) and
 * ClusterRevision (LIVE: Instance::Read() serves FeatureMap and the two
 * absent lists only, power-topology-server.cpp:64-77; PowerTopology/
 * Metadata.h:20 kRevision = 1).
 *
 * No Identify on either type: neither ElectricalSensor.xml nor
 * ElectricalMeter.xml (data_model/1.5) requires it, and the C6 adds none
 * (mt_devtypes.cpp:1342-1346).
 *
 * Conformance disclosures, restated from AT_MT_SPEC.md 3.9:652-673 at the
 * declaration per the batch brief: the SENSOR's device type XML lists its
 * two measurement clusters as a pick-at-least-one choice, so the power-only
 * variant-1 sensor is fully conformant; the METER's XML marks both
 * clusters mandatory, so variant 1 on 0x0514 is a DISCLOSED departure that
 * exists for variant-scheme symmetry, and the strictly conformant
 * current-clamp declaration is the variant-1 SENSOR.
 *
 * Device revisions: both 1 per data_model/1.5/device_types/
 * ElectricalSensor.xml / ElectricalMeter.xml (the pinned authority). */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(epmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::PowerMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::NumberOfMeasurementTypes::Id,
                              INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::Accuracy::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::Voltage::Id, VOLTAGE_MV, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id,
                              AMPERAGE_MA, 8, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::ActivePower::Id, POWER_MW, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::RMSVoltage::Id, VOLTAGE_MV,
                              8, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::RMSCurrent::Id, AMPERAGE_MA,
                              8, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::Frequency::Id, INT64S, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::PowerFactor::Id, INT64S, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalPowerMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(eemAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::Accuracy::Id, STRUCT, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id,
                              STRUCT, 0, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::CumulativeEnergyExported::Id,
                              STRUCT, 0, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ElectricalEnergyMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(ptopAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerTopology::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalSensorClusters)
DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1, power only: the same list minus the energy cluster. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalSensorPowerOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalMeterClusters)
DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1, the disclosed sub-conformant power-only meter (the section
 * comment above): EPM and Descriptor alone. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(electricalMeterPowerOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(electricalSensorEndpoint, electricalSensorClusters);
HEARTH_DECLARE_CONST_ENDPOINT(electricalSensorPowerOnlyEndpoint, electricalSensorPowerOnlyClusters);
HEARTH_DECLARE_CONST_ENDPOINT(electricalMeterEndpoint, electricalMeterClusters);
HEARTH_DECLARE_CONST_ENDPOINT(electricalMeterPowerOnlyEndpoint, electricalMeterPowerOnlyClusters);

constexpr EmberAfDeviceType kElectricalSensorTypes[] = { { 0x0510, 1 } };
constexpr EmberAfDeviceType kElectricalMeterTypes[] = { { 0x0514, 1 } };

/* ---- heat pump (0x0309) and solar power (0x0017) ----------------------
 *
 * Catalogue batch 7a audit sections 3.4/3.5. Zero new clusters: both types
 * are the Electrical Sensor composition grafted onto the SAME endpoint as
 * a Wired power source, the C6's hand-built stacks (mk_heat_pump(),
 * mt_devtypes.cpp:1790-1867, and mk_solar_power(), :2101-2136) rendered as
 * declared lists. The endpoint advertises THREE device types at once
 * (0x0309 or 0x0017, plus 0x0011 Power Source and 0x0510 Electrical
 * Sensor, same endpoint, NOT children), the C6's exact span through
 * add_device_type().
 *
 * A NEW Wired PowerSource attribute list, never the battery-shaped
 * powerSourceAttrs above: reusing that list here would advertise
 * BatChargeLevel/BatReplacementNeeded/BatReplaceability on a mains device,
 * a conformance break no build check catches (the audit's heat pump risk
 * line). The wired list is Status, Order, Description (CHAR_STRING,
 * metadata-only, no slot), WiredCurrentType (mandatory under the WIRED
 * feature, PowerSource.xml), EndpointList (ARRAY, served by the cluster's
 * wildcard AAI as an empty list, the batch 4 note on powerSourceAttrs)
 * and FeatureMap 0x1 Feature::kWired, seeded per device type below since
 * the battery wildcard seed row must keep winning for 0x0011.
 * WiredCurrentType boots kAc, which is 0 (PowerSource/Enums.h
 * WiredCurrentTypeEnum, kAc = 0x00), the zero-fill, so it carries no seed
 * row; the C6's wired feature::add() creates it 0 identically.
 *
 * THE C6'S WORKAROUNDS DO NOT TRANSFER, THE POSITIVE OBLIGATIONS DO (the
 * audit's solar risk line, restated here as the batch brief directs).
 * mk_heat_pump() exists because heat_pump::add() boot-panics on EEM
 * feature validation with a default config and would bolt on a DEM pair
 * (esp_matter_endpoint.cpp:2015-2020); mk_solar_power() exists because
 * solar_power::add() silently ASSIGNS feature flags over any pre-seed,
 * fixes an EEM mask of exported|cumulative that would contradict the
 * wildcard AAI's Imported|Exported|Cumulative and lose
 * CumulativeEnergyImported entirely, and creates Voltage/ActiveCurrent as
 * non-null zeros, irreversibly breaking the null-until-pushed contract
 * (mt_devtypes.cpp:1890-1903). None of those SDK code paths exist on this
 * port; what carries over is what they were protecting: (1) the seven EPM
 * fields are declared 64-bit, slotless and null until pushed (epmAttrs);
 * (2) the EEM mask is exactly Imported|Exported|Cumulative
 * (mt_matter_eem_register and the eemAttrs seed); (3) the EEM cluster is
 * present only where declared below.
 *
 * Variants: heat pump has ONE variant (the C6 row's max_variant 0). Solar
 * spends its variant on the graft's EEM: variant 0 with, variant 1
 * without, and variant 1 is DISCLOSED SUB-CONFORMANT against
 * SolarPower.xml's composedDeviceTypes block, which mandates EPM and EEM
 * both on the composed sensor (AT_MT_SPEC.md 3.9:735-739; the conformant
 * declaration is variant 0).
 *
 * DISCLOSED GAPS, matching the C6 and the SDK build's own omissions
 * (mt_devtypes.cpp:1781-1789, :2096-2099): HeatPump.xml's
 * composedDeviceTypes block also mandates a composed Thermostat device
 * type with User Label, which neither platform builds; both types' User
 * Label describedConform rows likewise. Disclosed, not wired.
 *
 * Device revisions: both 1 per data_model/1.5/device_types/HeatPump.xml /
 * SolarPower.xml. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(wiredPowerSourceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Status::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Order::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Description::Id, CHAR_STRING, 61, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::WiredCurrentType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::EndpointList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* One cluster list serves the heat pump and the variant-0 solar (the
 * booleanStateSensorClusters sharing principle: the whole composition is
 * identical, only the EmberAfDeviceType span differs per row). */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(wiredElectricalSensorClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, wiredPowerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Solar variant 1: the same stack without the energy cluster. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(wiredElectricalSensorPowerOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, wiredPowerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(wiredElectricalSensorEndpoint, wiredElectricalSensorClusters);
HEARTH_DECLARE_CONST_ENDPOINT(wiredElectricalSensorPowerOnlyEndpoint,
                         wiredElectricalSensorPowerOnlyClusters);

/* The same-endpoint grafts: three device types per span, the C6's
 * add_device_type() sequence (0x0011 revision 1 matching the standalone
 * power source row, 0x0510 revision 1 matching the sensor row above). */
constexpr EmberAfDeviceType kHeatPumpTypes[] = { { 0x0309, 1 }, { 0x0011, 1 }, { 0x0510, 1 } };
constexpr EmberAfDeviceType kSolarPowerTypes[] = { { 0x0017, 1 }, { 0x0011, 1 }, { 0x0510, 1 } };

/* ---- electrical utility meter (0x0511) --------------------------------
 *
 * Catalogue batch 7a audit section 3.9. One new cluster,
 * MeterIdentification (0x0B06), the cheapest type in the batch: Instance
 * only, NO delegate of any kind (the Instance owns its own attribute
 * storage, three 64-byte string buffers included: 304 B measured in the
 * step-0 build), pool MT_METER_MAX (2, core/include/mt_matter.h:1273, the
 * C6 depth).
 *
 *   Code-driven? No; CHI-only? Not listed, harmless (no commands). Both
 *   confirmed empirically by the batch's one zap regeneration
 *   (byte-identical trio, the epmAttrs note).
 *
 *   AAI? YES, per endpoint, and NOTHING IN THE SDK EVER CONSTRUCTS IT:
 *   Instance::Init() (meter-identification-server.cpp:59-68, soft: nulls
 *   the five attributes and registers the AAI) has no SDK caller anywhere
 *   in the pinned tree, the chime/EEM disease in the organ the C6's
 *   mt_meter.cpp documents at length. The port's fix is the same shape:
 *   mt_meter_register_all() (mt_matter_zephyr.cpp), a registration scan
 *   over the live composition run once from main.cpp after
 *   rebuild_composition() and before mt_at_start(), so every meter
 *   endpoint answers before +MTREADY lets the host ask. Forget the scan
 *   and the endpoint advertises five attributes and answers none of them,
 *   the exact failure mode the C6 round found on the bench (the audit's
 *   risk line).
 *
 *   Capacity is claimed at CREATE time, not registration time:
 *   mt_meter_reserve() below runs with the other pre-create pool claims,
 *   because only a create failure can abort the composition rebuild; a
 *   shortfall discovered in the later scan could not un-create the
 *   cluster already on the wire (the C6's fix-round-2 lesson,
 *   mt_meter.cpp's s_meter_reserved comment).
 *
 * Slots: MeterType (ENUM8, nullable, k_instance_served live-read; its
 * inert shadow seeds the null sentinel), FeatureMap (inert shadow of the
 * Instance's construction mask, mt_meter_feature_mask() = kPowerThreshold
 * 0x1) and ClusterRevision (LIVE: Instance::Read() serves the five
 * attributes and FeatureMap only, meter-identification-server.cpp:
 * 215-245; MeterIdentification/Metadata.h:20 kRevision = 1). The three
 * char_strings (64 bytes each, so 65 ember bytes, under the 66-byte IO
 * buffer) and the PowerThresholdStruct are metadata-only, no slot, quiet
 * by type; an AT+MTATTR read of any of the four answers +MTERR:5. On the
 * C6 those four +MTERR:5 answers come from two different mechanisms (the
 * strings reach the Instance and fail conversion, the struct is refused
 * before dispatch by an esp-matter ARRAY refusal, AT_MT_SPEC.md 3.9:
 * 808-822); on this port all four come from attr_type_info()'s one
 * refusal, same wire, different plumbing, the spec's platform-note gap
 * the batch report flags.
 *
 * Identify IS composed, a conformance requirement, not decoration:
 * ElectricalUtilityMeter.xml declares superset="Meter Reference Point"
 * and MeterReferencePoint.xml's own body mandates Identify; a superset
 * conformance is transitive (the C6's mk_electrical_utility_meter()
 * reasoning, mt_devtypes.cpp:2547-2557).
 *
 * TimeSyncCond is DISCLOSED, NOT FIXED, on both platforms: this
 * firmware's root endpoint carries no Time Synchronization cluster on any
 * image, so no meter endpoint is conformant against that condition
 * (AT_MT_SPEC.md 3.9:800-806; adding the cluster to endpoint 0 is a
 * whole-composition decision out of this batch's scope).
 *
 * The five attributes are set in ONE call, AT+MTMETERID
 * (mt_matter_meter_set_identity, mt_matter_zephyr.cpp), all-or-nothing;
 * there is no other write path and deliberately no read-back verb
 * (AT_MT_SPEC.md 3.29). One variant. Device revision 1 per
 * data_model/1.5/device_types/ElectricalUtilityMeter.xml. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(meterIdAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::MeterType::Id, ENUM8, 1,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::PointOfDelivery::Id, CHAR_STRING,
                              65, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::MeterSerialNumber::Id, CHAR_STRING,
                              65, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::ProtocolVersion::Id, CHAR_STRING,
                              65, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::PowerThreshold::Id, STRUCT, 0,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(MeterIdentification::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(utilityMeterClusters)
DECLARE_DYNAMIC_CLUSTER(MeterIdentification::Id, meterIdAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(utilityMeterEndpoint, utilityMeterClusters);

constexpr EmberAfDeviceType kElectricalUtilityMeterTypes[] = { { 0x0511, 1 } };

/* ---- device energy management (0x050D) --------------------------------
 *
 * Catalogue batch 7a audit section 3.7. DeviceEnergyManagement (0x0098)
 * plus DeviceEnergyManagementMode (0x009F, a ModeBase alias riding the
 * batch 5 mode-base-server frame: no new translation unit, one more
 * kStoreWalk row, one more pool consumer) plus Descriptor. No Identify and
 * no tag_list, SDK parity (the C6's mk_dem(), mt_devtypes.cpp:2287-2304).
 * The XML classifies it a utility device type stackable on any endpoint;
 * this row is the standalone declaration.
 *
 * DeviceEnergyManagement's three answers: code-driven no, CHI-only YES
 * (config-data.yaml:41: its eight commands have no generated dispatch;
 * the byte-identical IMClusterCommandHandler check covered it), AAI YES
 * per endpoint (Instance derives from both AttributeAccessInterface and
 * CommandHandlerInterface, device-energy-management-server.h:207-217;
 * Init() registers CHI then AAI, SOFT on both,
 * device-energy-management-server.cpp:40-47). Instance::Read() serves the
 * six delegate scalars, the two structs and FeatureMap and falls through
 * to ember for the rest (:60-105), so only ClusterRevision's seed is
 * live.
 *
 * THE IRON RULE, INVERTED (the C6's mt_add_dem_triple() comment,
 * mt_devtypes.cpp:1974-2025, and ARCHITECTURE.md 8.12, restated verbatim
 * in reasoning as the batch brief directs). On the C6, DEM feature bits
 * are set ONLY via feature::power_adjustment::add(), never by writing
 * FeatureMap, because that one call is what creates
 * PowerAdjustmentCapability and OptOutState, both PA commands and both PA
 * events alongside the bit; a hand-set bit would advertise conformance
 * with no ember entry behind it, and a missing entry means an invoke
 * returns NO status at all on that platform. On this port there is no
 * feature::add(): FeatureMap seeds and command lists are written by hand,
 * so the rule inverts into a declaration obligation. SEEDING THE
 * POWERADJUSTMENT BIT ON VARIANT 0 OBLIGES, all four together:
 *   - declaring PowerAdjustmentCapability (STRUCT, metadata-only,
 *     Instance-served, feature-gated in Read());
 *   - declaring OptOutState (enum8; its slot is an inert shadow, the
 *     Instance serves it, the carve-out reads it live);
 *   - both PA commands in the INCOMING list (kDemIncoming below; the
 *     Instance's CHI handles the invokes and its
 *     RetrieveAcceptedCommands derives the advertised list from its own
 *     feature mask, device-energy-management-server.cpp:110-127, so the
 *     declared list and the mask must agree);
 *   - NOTHING in the outgoing list: neither PA command has a response
 *     (the cluster XML), the DE399 truthfulness discipline.
 * Getting it half right advertises commands with no capability behind
 * them and turns every AT+MTDEMCAP into +MTERR:4, with no build check to
 * catch it: the variant-0 list below is the four together, the variant-1
 * list is NONE of them (report-only ESA, FeatureMap 0, conformant: the
 * XML's five-feature min-1 choice applies only under the ControllableESA
 * condition, which variant 1 does not declare).
 *
 * The FeatureMap seed itself is the one variant-dependent boot value in
 * the catalogue, written by seed_slots()'s DEM special case from
 * d->variant (the AirQuality not-a-literal precedent) because s_seeds
 * rows key on device type and both variants share 0x050D; the same
 * variant predicate feeds mt_matter_dem_register()'s Instance mask, so
 * shadow and Instance agree by construction.
 *
 * DEMMode reuses modeBaseAttrs and kModeBaseIncoming/kModeBaseOutgoing
 * verbatim: every ModeBase alias numbers SupportedModes 0, CurrentMode 1,
 * ChangeToMode 0 and ChangeToModeResponse 1 identically (the generated
 * per-cluster headers), and the batch 5 audit notes on modeBaseAttrs
 * apply unchanged, the sharp one included: ModeBase Instance::Init()
 * VerifyOrDies on ordering (mode-base-server.cpp:77) and reads the
 * delegate's index 0 first (:74), so the DEMMode store construction below
 * and the strictly-after-create set_endpoint are both load-bearing.
 * ClusterRevision 2 (DeviceEnergyManagementMode/Metadata.h:20, LIVE, the
 * ModeBase no-default-arm rule); tag-0 default kNoOptimization
 * (AT_MT_SPEC.md 3.20's table row).
 *
 * Slots: v0 = ESAType, ESACanGenerate, ESAState, OptOutState, FeatureMap,
 * ClusterRevision (6) + DEMMode 3; v1 drops OptOutState (5 + 3). The two
 * power_mw attributes are DE407 metadata-only declarations (quiet-table
 * rows), Instance-served, carve-out read. Block: v0 3 clusters, 9 slots,
 * 306 B store = 462 B payload, 472 B heap (roundup(462 + 4, 8)); v1 446
 * B payload, 456 B heap. Far under the RVC's 864 B widest.
 *
 * ESAState's shadow seeds kOnline (1), the delegate's default and the
 * spec's pre-first-push answer (3.25); ESAType/ESACanGenerate/OptOutState
 * zero-fill to match the delegate defaults. ClusterRevision 4 is
 * DeviceEnergyManagement/Metadata.h:20 in THIS tree.
 *
 * Device revision 3 per data_model/1.5/device_types/
 * DeviceEnergyManagement.xml. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(demAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESACanGenerate::Id, BOOLEAN, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMinPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMaxPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::PowerAdjustmentCapability::Id,
                              STRUCT, 0, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::OptOutState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Variant 1, the report-only ESA: no PA-gated attributes (the iron rule's
 * other half: FeatureMap 0 may not advertise what is not declared). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(demReportOnlyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESACanGenerate::Id, BOOLEAN, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::ESAState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMinPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::AbsMaxPower::Id, POWER_MW, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DeviceEnergyManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kDemIncoming[] = { DeviceEnergyManagement::Commands::PowerAdjustRequest::Id,
                                       DeviceEnergyManagement::Commands::CancelPowerAdjustRequest::Id,
                                       kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(demClusters)
DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kDemIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(demReportOnlyClusters)
DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demReportOnlyAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(demEndpoint, demClusters);
HEARTH_DECLARE_CONST_ENDPOINT(demReportOnlyEndpoint, demReportOnlyClusters);

constexpr EmberAfDeviceType kDemTypes[] = { { 0x050D, 3 } };

/* ---- water heater (0x050F) --------------------------------------------
 *
 * Catalogue batch 7b audit (batch7-audit.md section 3.3), every citation
 * re-verified against the pinned NCS tree while writing. One new cluster,
 * WaterHeaterManagement (0x0094), plus the fourth ModeBase alias
 * (WaterHeaterMode 0x009E, mode-base-server per zap_cluster_list.json:367,
 * no new translation unit), on the C6's mk_water_heater()
 * (mt_devtypes.cpp:1628-1736, class comment :1531-1627).
 *
 * WaterHeaterManagement's three answers: code-driven no, CHI-only YES
 * (config-data.yaml:81; the batch's zap regeneration left
 * IMClusterCommandHandler.cpp and CodeDrivenInitShutdown.cpp
 * byte-identical, sha256-checked; access.h did NOT stay byte-identical,
 * against the audit 5.5 prediction: it gained Boost and CancelBoost with
 * the manage invoke privilege the cluster XML really declares,
 * water-heater-management-cluster.xml:94 and :100, a correct delta the
 * batch 3 empirical check exists to catch). AAI YES per endpoint: the
 * Instance derives from both AttributeAccessInterface and
 * CommandHandlerInterface (water-heater-management-server.h:139-149, ctor
 * Instance(EndpointId, Delegate &, Feature) whose body sets the delegate's
 * endpoint); Init() registers CHI then AAI, SOFT on both
 * (water-heater-management-server.cpp:94-100, no emberAfContainsServer
 * check, no VerifyOrDie), constructed by the port's own
 * mt_matter_whm_register() below a successful emberAfSetDynamicEndpoint(),
 * the DEM handout's shape, pool MT_WHM_MAX (4, core/include/
 * mt_matter.h:939, the C6 depth per DE407).
 *
 * THE ONE INERT REVISION SEED IN THE ENERGY FAMILY: Instance::Read()
 * serves ClusterRevision ITSELF (water-heater-management-server.cpp:
 * 146-147, encoding kClusterRevision = 2, :40), the only energy cluster
 * that does; every other case falls through for NOTHING (the switch covers
 * all six attributes plus FeatureMap). So the WHM ClusterRevision seed
 * below is an inert shadow kept equal to that served 2
 * (WaterHeaterManagement/Metadata.h:20 agrees), the Thermostat
 * seed-agrees-with-AAI discipline, while EPM/EEM/PTOP/DEM revision seeds
 * stay LIVE.
 *
 * Slots, v0: HeaterTypes, HeatDemand, TankVolume, TankPercentage,
 * BoostState, FeatureMap, ClusterRevision (7). EstimatedHeatRequired is
 * energy_mwh (water-heater-management-cluster.xml:78, min 0), declared
 * true-width 8 B for the AAI gate (DE407 option C), no slot, its own
 * kQuietNoSlot row, k_instance_served live-read row. v1 drops the
 * feature-gated pair whole per the cluster XML's gates (TankVolume and
 * EstimatedHeatRequired mandatory under EM only, :73-82; TankPercentage
 * under TP only, :83-86; HeaterTypes/HeatDemand/BoostState mandatory
 * unconditionally, :67-71 and :88-90): 5 slots. All six values are
 * Instance-served from the HearthWhmDelegate cache (AT+MTMEAS 0x94 is the
 * write path, AT_MT_SPEC.md 3.25); the enum/bitmap/u16 slots are inert
 * shadows zero-filled to the delegate defaults (everything 0, BoostState
 * kInactive = 0, WaterHeaterManagement/Enums.h:32-36).
 *
 * THE FEATUREMAP IS THE CATALOGUE'S SECOND VARIANT-DEPENDENT BOOT VALUE
 * (the DEM special case's exact problem: both variants share devtype
 * 0x050F, so an s_seeds row cannot split them). Variant 0 is
 * EnergyManagement|TankPercent = 0x3 (Enums.h:44-48; the value the C6
 * hand-sets because BOTH esp-matter feature helpers are broken,
 * mt_devtypes.cpp:1594-1607: energy_management::add() as declared does
 * not link and tank_percent::get_id() returns the kEnergyManagement bit;
 * neither helper exists on this port, so the obligation reduces to the
 * bits themselves, read from CHIP's own enum). Variant 1 is 0 (HeaterTypes
 * /HeatDemand/BoostState only, which the cluster's own feature conformance
 * permits: EM and TP are both optionalConform, cluster XML :57-64).
 * seed_slots()'s WHM special case writes it from d->variant, and the
 * create path hands the IDENTICAL predicate to mt_matter_whm_register(),
 * whose Instance mask is what Read() actually answers for FeatureMap
 * (server.cpp:144-145 encodes mFeature, snapshotted at construction):
 * seed and Instance agree by construction, single-sourced on the variant.
 *
 * Thermostat: REUSES thermostatAttrs whole with a HEATING-ONLY FeatureMap
 * seed (0x01) keyed to 0x050F below (the seed table's per-devtype keying;
 * the standalone thermostat and the room air conditioner keep the wildcard
 * 0x03), per WaterHeater.xml's Thermostat(HEAT) mandate (:75-81; HEAT
 * alone makes only OccupiedHeatingSetpoint mandatory, the C6 class
 * comment's own conformance trace). ControlSequenceOfOperation gets a
 * keyed 0x02 (HeatingOnly) seed: the wildcard 0x04 (CoolingAndHeating)
 * would advertise cooling this endpoint does not have. DELIBERATE
 * DIVERGENCE from the C6, which serves esp-matter's inherited config
 * default 4 on its water heater (esp_matter_cluster.h:415, the thunk does
 * not override it); the FanModeSequence precedent, noted rather than
 * copied. DISCLOSED OVER-DECLARATION riding on the reuse (the batch
 * brief's ruled trade): thermostatAttrs also declares
 * OccupiedCoolingSetpoint and the four setpoint-limit attributes, three of
 * which Thermostat.xml gates on COOL; the C6's water heater creates no
 * cooling attributes at all, so this endpoint's AttributeList is wider
 * than the C6's and than the letter of HEAT-only conformance. Disclosed
 * here and in the batch report, not silently inherited. No panic-trap
 * analogue exists on this port (the C6's VALIDATE_FEATURES_AT_LEAST_ONE
 * pre-seed, mt_devtypes.cpp:1638-1641, is esp-matter machinery); what
 * carries over is the conformance conclusion, the 0x01 seed.
 *
 * WaterHeaterMode reuses modeBaseAttrs and kModeBaseIncoming/
 * kModeBaseOutgoing verbatim (every ModeBase alias numbers its elements
 * identically, the DEMMode note above), one more kStoreWalk row and one
 * more pool consumer; the batch 5 ModeBase notes apply unchanged,
 * including the Init() ordering panic and the placeholder-mode-0 policy.
 * ChangeToMode forwards on cluster 158, already in AT_MT_SPEC.md 3.17's
 * registration list (the six registrations; only EnergyEvseMode 0x9D is
 * the list's known gap, and it is out of this batch by ruling DE408).
 * ClusterRevision 1 LIVE (WaterHeaterMode/Metadata.h:20; the ModeBase AAI
 * has no revision case). Tag-0 default kManual on every mode
 * (AT_MT_SPEC.md 3.20's table row; WaterHeaterMode/Enums.h:45).
 *
 * Commands: Boost (0x00) and CancelBoost (0x01) in the incoming list; the
 * server's InvokeCommand dispatches both to the delegate
 * (water-heater-management-server.cpp:154-169), and access.h carries their
 * manage privilege (above). NOTHING outgoing: neither command has a
 * response (the cluster XML), the DE399 truthfulness discipline. The
 * five-field Boost payload and the CancelBoost in-state guard live in the
 * delegate (mt_matter_zephyr.cpp, where the guard's SDK-versus-C6 placement
 * is traced).
 *
 * Variant 0 additionally grafts the Electrical Sensor composition on the
 * SAME endpoint, which WaterHeater.xml:84-95 mandates (composedDeviceTypes:
 * an Electrical Sensor with EPM and EEM): PTOP + EPM + EEM with the batch
 * 7a lists verbatim, drawing one EPM and one PTOP slot from the
 * MT_MEAS_MAX pools, and the span advertises 0x0510 alongside 0x050F, the
 * C6's exact span (electrical_sensor::add() calls add_device_type(0x0510),
 * esp_matter_endpoint.cpp:1505-1508, and mk_water_heater() grafts it only
 * on variant 0, so the C6's variant-1 span is 0x050F ALONE: this port's
 * first variant-dependent device-type span, carried by the registry's new
 * device_types_v1 member below). Variant 1 is the SDK-bare build, no
 * graft, DISCLOSED SUB-CONFORMANT against the mandated composed sensor
 * (the variant-1 electrical meter precedent; AT_MT_SPEC.md 3.9's 0x050F
 * row and ARCHITECTURE.md 8.11 carry it on the C6's side).
 *
 * No Identify on either variant: WaterHeater.xml marks it optionalConform
 * (:66-68) and the C6's water_heater::add() creates none.
 *
 * Events: BoostStarted and BoostEnded, derived FIRMWARE-SIDE from
 * BoostState transitions pushed over AT+MTMEAS, with the cached-parameters
 * lifecycle (core/include/mt_matter.h:865-874, binding); the derivation
 * state machine and its exhaustive commentary live beside the delegate in
 * mt_matter_zephyr.cpp. Plus CumulativeEnergyMeasured on v0 (the EEM push
 * path, batch 7a).
 *
 * Block: v0 7 clusters, 29 slots, one 306 B mt_mb_store_t = 798 B payload,
 * 808 B heap (roundup(798 + 4, 8) = 808, kHeapCostOf); v1 4 clusters, 19
 * slots, one store = 626 B payload, 632 B heap. Both under the RVC's
 * 864 B; the floor candidate below is compile-time-derived and asserted.
 *
 * Device revision 1 per data_model/1.5/device_types/WaterHeater.xml:60. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(whmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeaterTypes::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeatDemand::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::TankVolume::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::EstimatedHeatRequired::Id,
                              ENERGY_MWH, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::TankPercentage::Id, PERCENT, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::BoostState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Variant 1, the bare cluster: the three unconditionally mandatory values
 * only (the feature-gate trace in the section comment; FeatureMap 0 may
 * not advertise what is not declared, the DEM report-only list's rule). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(whmBareAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeaterTypes::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::HeatDemand::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::BoostState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(WaterHeaterManagement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kWhmIncoming[] = { WaterHeaterManagement::Commands::Boost::Id,
                                       WaterHeaterManagement::Commands::CancelBoost::Id,
                                       kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(waterHeaterClusters)
DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterManagement::Id, whmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kWhmIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1, the SDK-bare build: no sensor graft (the disclosure in the
 * section comment). */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(waterHeaterBareClusters)
DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterManagement::Id, whmBareAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kWhmIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(WaterHeaterMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(waterHeaterEndpoint, waterHeaterClusters);
HEARTH_DECLARE_CONST_ENDPOINT(waterHeaterBareEndpoint, waterHeaterBareClusters);

/* The variant-0 span grafts 0x0510 on the same endpoint (revision 1,
 * matching the standalone sensor row); variant 1 has no graft and no
 * second id, the C6's exact behaviour (the section comment). */
constexpr EmberAfDeviceType kWaterHeaterTypes[] = { { 0x050F, 1 }, { 0x0510, 1 } };
constexpr EmberAfDeviceType kWaterHeaterBareTypes[] = { { 0x050F, 1 } };

/* ---- battery storage (0x0018) -----------------------------------------
 *
 * Catalogue batch 7b audit (batch7-audit.md section 3.6). NO new cluster:
 * the whole composition is batch 7a machinery (the sensor graft, the DEM
 * pair, the pools, AT+MTDEMCAP, the PA event protocol) under a THIRD
 * PowerSource attribute list, on the C6's mk_battery_storage()
 * (mt_devtypes.cpp:2182-2264, class comment :2137-2181). Only the
 * registry rows, the spans and this note are new; every create-path arm,
 * seed special case and carve-out row is keyed on cluster ids these lists
 * already carry.
 *
 * THE RECHG CONFORMANCE FIX, the reason the battery-shaped
 * powerSourceAttrs above cannot be reused (the audit's risk line; the C6
 * comment at :2146-2159 records the SDK defect this corrects):
 * battery_storage::add() sets kBattery alone and then hand-creates four
 * attributes PowerSourceCluster.xml gates on the RECHG feature, while the
 * RECHG-MANDATORY BatChargeState is missing entirely. The fix is
 * Battery|Rechargeable together (Feature 0x2 | 0x4 = 0x6,
 * PowerSource/Enums.h:280-286, the keyed FeatureMap seed below) AND
 * declaring the full attribute population that makes it true: the four
 * RECHG-gated attributes (BatTimeToFullCharge, BatChargingCurrent,
 * ActiveBatChargeFaults, BatCapacity) plus BatChargeState plus
 * BatFunctionalWhileCharging, alongside the battery family the existing
 * list already carries. Declaring the gated four without BatChargeState,
 * or without the Rechargeable bit, reproduces the exact SDK defect the C6
 * thunk was written to correct, with no build check to catch it.
 *
 * B263 BINDS THE DECLARED WIDTHS (the C6's own note at :2221-2246): the
 * five uint32 battery attributes are XML-typed int32u with NO <constraint>
 * element (power-source-cluster.xml:99 BatVoltage, :110 BatTimeRemaining,
 * :165 BatCapacity, :183 BatTimeToFullCharge, :193 BatChargingCurrent),
 * so the TYPE IS THE BOUND: all five are declared INT32U size 4, never an
 * invented narrower size, and ember serves the full uint32 domain the C6
 * serves. On this port the obligation is only the declared width (there
 * is no created min/max to widen); a 4 B slot holds all five, so they are
 * ordinary AT+MTATTR territory, plain slots, NO carve-out rows, no quiet
 * rows, the audit's "no new AT surface" line. Nullable per the XML:
 * BatVoltage, BatTimeRemaining, BatTimeToFullCharge and BatChargingCurrent
 * are isNullable (seeded to the 4-byte unsigned sentinel below);
 * BatCapacity and BatChargeState are NOT nullable (zero-fill: 0 and
 * kUnknown = 0, PowerSource/Enums.h:109-115), BatFunctionalWhileCharging
 * boots false, all matching the C6 config defaults. The two fault lists
 * (ActiveBatFaults :135, ActiveBatChargeFaults :199) are arrays, declared
 * metadata-only, no slot, quiet by type; AT+MTATTR answers +MTERR:5 on
 * either, and they ship empty and host-untouched (AT_MT_SPEC.md 3.9's
 * 0x0018 row). BatPercentRemaining keeps its real 0..200 bounds through
 * the same hand-rolled MIN_MAX entry as the battery list.
 *
 * Variants: v0 carries the DEM pair (demAttrs + kDemIncoming, the
 * WITH-PowerAdjustment list: the create path's variant predicate feeds
 * both seed_slots()'s DEM FeatureMap special case and
 * mt_matter_dem_register(), and variant 0 IS the PA variant here exactly
 * as on the standalone 0x050D row, so every DEM obligation of the
 * inverted iron rule holds unchanged). The triple is OVER-DELIVERY
 * matching the SDK build (BatteryStorage.xml never requests DEM, the C6
 * comment :2170-2172); v1 omits it and stays conformant. Both variants
 * carry the full sensor graft WITH EEM (unlike solar, which spends its
 * variant on the EEM: the two types' variant bytes mean different things,
 * AT_MT_SPEC.md 3.9's shared paragraph).
 *
 * Spans: v0 advertises 0x0018 (revision 2, BatteryStorage.xml:60) +
 * 0x0011 + 0x0510 + 0x050D, the C6's exact sequence; v1 drops 0x050D with
 * the DEM pair (the C6 adds that id inside the variant-0 branch only), the
 * water heater's device_types_v1 mechanism.
 *
 * DISCLOSED XML INCONSISTENCY, flagged not chased (the C6 comment
 * :2173-2181): BatteryStorage.xml revision 2's summary says "two Power
 * Source and Electrical Sensor composed devices types" while the body
 * encodes ONE composed 0x0510 block with duplicated cluster rows and no
 * Power Source device type at all; this endpoint delivers one sensor and
 * one power source and discloses the departure on both platforms' terms.
 *
 * No Identify (BatteryStorage.xml lists it optional, nothing else); no
 * tag_list, the port-wide rule.
 *
 * Block: v0 7 clusters, 32 slots, one 306 B mt_mb_store_t = 846 B
 * payload, 856 B heap; v1 5 clusters, 23 slots, no store = 388 B payload,
 * 392 B heap. The audit's 3.6 table said 31 slots / 830 / 832 for v0: it
 * under-counted this list by one slot (its "about 14" PowerSource figure
 * against the real 15 below) and mis-rounded the heap cost; re-derived
 * here per the batch brief, still under the RVC's 864 B, and the floor
 * candidate below pins the corrected arithmetic. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(rechargeableBatteryPowerSourceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Status::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Order::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::Description::Id, CHAR_STRING, 61, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatVoltage::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    /* Hand-rolled MIN_MAX entry, shared with the battery list; see the
     * powerSourceAttrs audit note. */
    { &kBatPercentRemainingBounds, PowerSource::Attributes::BatPercentRemaining::Id, 1,
      ZAP_TYPE(INT8U),
      ZAP_ATTRIBUTE_MASK(MIN_MAX) | ZAP_ATTRIBUTE_MASK(NULLABLE) |
          ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE) | ZAP_ATTRIBUTE_MASK(READABLE) },
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatTimeRemaining::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargeLevel::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplacementNeeded::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatReplaceability::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::ActiveBatFaults::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatCapacity::Id, INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargeState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatTimeToFullCharge::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatFunctionalWhileCharging::Id, BOOLEAN, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::BatChargingCurrent::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::ActiveBatChargeFaults::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::EndpointList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(PowerSource::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(batteryStorageClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, rechargeableBatteryPowerSourceAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kDemIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1: the same stack without the over-delivered DEM pair. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(batteryStorageNoDemClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, rechargeableBatteryPowerSourceAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(batteryStorageEndpoint, batteryStorageClusters);
HEARTH_DECLARE_CONST_ENDPOINT(batteryStorageNoDemEndpoint, batteryStorageNoDemClusters);

/* The v0 span carries the DEM id; v1 drops it with the cluster pair (the
 * section comment; revisions match the standalone rows: 0x0011 rev 1,
 * 0x0510 rev 1, 0x050D rev 3). */
constexpr EmberAfDeviceType kBatteryStorageTypes[] = { { 0x0018, 2 },
                                                       { 0x0011, 1 },
                                                       { 0x0510, 1 },
                                                       { 0x050D, 3 } };
constexpr EmberAfDeviceType kBatteryStorageNoDemTypes[] = { { 0x0018, 2 },
                                                            { 0x0011, 1 },
                                                            { 0x0510, 1 } };

/* ======================================================================
 * Catalogue batch 8: the composed appliances
 * ======================================================================
 *
 * The five device type ids the PARENTING POLICY names, given names here
 * because they appear in three places that must agree: mt_devtype_parent_ok()
 * below, the shape maps that render the same policy as cluster sets, and the
 * registry rows themselves. Every other row in this file keeps its bare hex
 * literal, because no other row's id is read by anything but the lookup.
 */
constexpr uint32_t kDtRefrigerator          = 0x0070;
constexpr uint32_t kDtTempControlledCabinet = 0x0071;
constexpr uint32_t kDtCookSurface           = 0x0077;
constexpr uint32_t kDtCooktop               = 0x0078;
constexpr uint32_t kDtOven                  = 0x007B;

/*
 * ---- the parenting policy, encoding one of two ------------------------
 *
 * Catalogue batch 8. Until this batch mt_devtype_parent_ok() was a
 * `return true` stub, because no device type in s_registry required a parent
 * or restricted which device type could parent it. Two now do
 * (AT_MT_SPEC.md 367-379), and this constexpr predicate is where the rule
 * lives; the extern "C" entry point the AT layer calls is a one-line
 * forwarder further down.
 *
 * It is constexpr for one reason: the SAME policy is encoded a second time,
 * as the per-row shape maps below, and the two can disagree. That hazard is
 * exactly the one ARCHITECTURE.md:1350-1364 records the C6 rejecting when it
 * declined to give the cabinet a variant bit ("a second encoding of a fact
 * the tree already carries, able to disagree with it"). Here the second
 * encoding is unavoidable (the cluster set has to come from somewhere) so it
 * is made CHECKABLE instead: shape_domain_matches_policy() below evaluates
 * this function over the whole registry at compile time and fails the build
 * on any disagreement in either direction.
 *
 * variant is a parameter of the contract (core/include/mt_devtypes.h) and is
 * deliberately unused: neither rule depends on it today. It stays in the
 * signature, and in the compile-time check's enumeration, so a future rule
 * that DOES depend on it needs no plumbing.
 */
constexpr bool parent_policy_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    (void)variant;
    /* Cook Surface: legal ONLY under a Cooktop. The unparented form is
     * rejected by the same expression, since no device type id is 0 and
     * parent_devtype 0 is how the AT layer spells "no parent"
     * (core/mt/mt_at.c:1903-1905). This is the catalogue's only device type
     * that REQUIRES a parent; the C6's rule is mt_devtypes.cpp:2767-2772 and
     * carries the identical one-expression note. */
    if (devtype_id == kDtCookSurface) {
        return parent_devtype == kDtCooktop;
    }
    /* Temperature Controlled Cabinet: legal unparented (the standalone rows
     * earlier compositions declared keep working), or under a Refrigerator
     * or an Oven and nothing else. Those two are the appliances that give a
     * child cabinet a coherent conditional cluster set (Cooler, Heater); a
     * third parent would be a cabinet with no defined set at all, which is
     * why the policy refuses rather than falling back to the bare shape.
     * The C6's rule is mt_devtypes.cpp:2773-2783. */
    if (devtype_id == kDtTempControlledCabinet) {
        return parent_devtype == 0 || parent_devtype == kDtRefrigerator ||
               parent_devtype == kDtOven;
    }
    /* Everything else is permissive, including the unparented form: PartsList
     * simply reflects whatever the host declares and this firmware holds no
     * opinion about whether the pairing makes sense (AT_MT_SPEC.md 366-370). */
    return true;
}

/*
 * ---- shape selection, the second encoding ----------------------------
 *
 * A shape is one realised cluster set for one (variant, parent devtype)
 * pair. Rows whose cluster set does not depend on the parent carry no
 * shapes at all and keep the ep_type / ep_type_v1 pair the struct comment
 * below describes; only a row the policy RESTRICTS needs a shape map, and
 * the compile-time check makes that an if-and-only-if rather than a
 * convention.
 *
 * parent_devtype 0 is the unparented shape, the same spelling
 * mt_devtype_create()'s parent_devtype argument uses (main.cpp passes 0 when
 * comp.parent[i] is MT_COMP_NO_PARENT).
 *
 * WHY NOT A WIDENED ROW FOR ALL 44 EXISTING ROWS. Ruled in the batch brief
 * (DE416). A Span<const shape> replacing ep_type/ep_type_v1 on every row is
 * the more general mechanism and remains the right refactor for the day a
 * SECOND parent-conditional device type appears; landing it in the same
 * commit as seven new device types, a heap resize, three new SDK translation
 * units and a zap regeneration would make the whole batch unreviewable. This
 * member is additive by construction: it is the LAST member, so aggregate
 * initialization zero-fills it to an empty span for every row that does not
 * name it, and all 44 pre-batch-8 rows keep their exact original text and
 * their exact original bytes. That is the same trick batches 7a and 7b each
 * relied on once (ep_type_v1, device_types_v1) and this is its third use.
 *
 * WHY NOT DUPLICATE REGISTRY ROWS keyed by parent: mt_devtype_is_known() and
 * mt_devtype_variant_ok() are called from core/mt/mt_at.c:1873 and :1885
 * BEFORE any parent has been parsed, so the registry's primary key has to
 * stay the bare device type id. Changing it to (id, parent) would have made
 * AT+MTEP=0x0071 answer +MTERR:6.
 */
struct hearth_shape {
    uint8_t variant;
    uint32_t parent_devtype;
    const EmberAfEndpointType *ep_type;
};

/* ---- cooktop (0x0078) --------------------------------------------------
 *
 * Catalogue batch 8 audit, OnOff with the OffOnly feature.
 *
 *   Nothing new CHIP-side: OnOff is already compiled, already dispatched by
 *   the generated IMClusterCommandHandler, already in hearth.zap, already
 *   seeded. The regeneration for this batch leaves it untouched.
 *
 *   The C6's mk_cooktop() (platform/esp32c6/main/mt_devtypes.cpp:422-427) is
 *   a bare thunk: cooktop::add() calls add_device_type(), on_off::create()
 *   and on_off::feature::off_only::add() (esp_matter_endpoint.cpp:1562-1573),
 *   and cooktop::config_t is {descriptor, on_off} with NO identify field, so
 *   there is no Identify on a cooktop on either platform. Cooktop.xml:66-67
 *   lists Identify as optional only, so the omission is conformant and is
 *   the C6-parity choice, not an oversight.
 *
 * THE ONE TRAP ON THIS DEVICE TYPE, and it is the whole reason for the
 * command list below. Cooktop.xml:69-74 makes OnOff mandatory WITH the
 * OFFONLY feature mandatory inside it: a controller may switch a cooktop
 * off, never on. Reusing kOnOffIncoming (Off/On/Toggle, declared with the
 * lighting types above) would advertise On and Toggle in
 * AcceptedCommandList, and the generated OnOff dispatch would happily
 * execute them, so a controller could switch a cooktop ON against the
 * device type's own conformance. Turning a cooktop on is the host's act, an
 * AT+MTATTR write of 0x0006/0x0000 (AT_MT_SPEC.md 640-655 says so for the
 * cook surface, and the same rule governs its parent).
 *
 * The C6 arrives at the same AcceptedCommandList by a different road, worth
 * recording because it reads like an accident there and is a declaration
 * here: cluster::on_off::create() adds ONLY command::create_off()
 * (esp_matter_cluster.cpp:1233-1260); On and Toggle are added per device
 * type by esp_matter_endpoint.cpp, and cooktop::add() does not add them. So
 * the C6's cooktop also accepts Off alone, and feature::off_only::add() sets
 * the feature-map bit and nothing else (esp_matter_feature.cpp:508-523).
 *
 * racOnOffAttrs is reused rather than onOffAttrs for the reason its own note
 * gives: the four Lighting-gated attributes (GlobalSceneControl, OnTime,
 * OffWaitTime, StartUpOnOff) have no place on a featureless-but-for-OffOnly
 * OnOff. The FeatureMap seed is device-type-qualified (0x04,
 * OnOff::Feature::kOffOnly, OnOff/Enums.h:85) against the wildcard 0x01 the
 * lighting types take.
 *
 * Revision 1 per data_model/1.5/device_types/Cooktop.xml. */
constexpr CommandId kOnOffOffOnlyIncoming[] = { OnOff::Commands::Off::Id, kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cooktopClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffOffOnlyIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cooktopEndpoint, cooktopClusters);

constexpr EmberAfDeviceType kCooktopTypes[] = { { 0x0078, 1 } };

/* ---- oven (0x007B) -----------------------------------------------------
 *
 * Catalogue batch 8 audit. Identify plus Descriptor, four slots, the
 * cheapest block in the catalogue.
 *
 *   Nothing new CHIP-side: Identify is already compiled and already seeded.
 *
 *   The C6's mk_oven() (mt_devtypes.cpp:1037-1049) calls oven::create() and
 *   then hand-adds Identify, because oven::add() calls add_device_type() and
 *   nothing else (esp_matter_endpoint.cpp:1360-1367) and oven::config_t
 *   carries no identify field. This list is the result, declared.
 *
 * THE OVEN IS A BARE PARENT ENDPOINT BY DESIGN (AT_MT_SPEC.md 529-534): all
 * of its function lives in its Temperature Controlled Cabinet children, so a
 * useful oven is always at least two AT+MTEP rows. That is also a disclosed
 * conformance hole shared with the refrigerator: Oven.xml:67-71 puts a
 * mandatory conditionRequirement on a 0x0071 child, so a lone AT+MTEP=0x007B
 * row is sub-conformant. The firmware does not and should not refuse it: a
 * composition-level requirement is not an append-level one, the host can
 * only satisfy it by declaring a second row, and the C6 has the identical
 * hole. AT_MT_SPEC.md carries the sentence instead of the code carrying a
 * check.
 *
 * Revision 2 per data_model/1.5/device_types/Oven.xml. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(ovenClusters)
DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(ovenEndpoint, ovenClusters);

constexpr EmberAfDeviceType kOvenTypes[] = { { 0x007B, 2 } };

/* ---- extractor hood (0x007A) -------------------------------------------
 *
 * Catalogue batch 8 audit. FanControl plus Descriptor, and NO Identify.
 *
 *   Nothing new CHIP-side: FanControl is already compiled, already in
 *   hearth.zap, already seeded, and its ClustersWithAttributeChangedFunctions
 *   entry (config-data.yaml:106-112) already works for 0x002B.
 *
 * WHY THIS IS A NEW TWO-CLUSTER LIST AND NOT A SECOND fanEndpoint ROW. The
 * air purifier (0x002D) above reuses &fanEndpoint verbatim, and its note is
 * explicit about why that is legitimate: air_purifier::add() really does
 * create Identify AND FanControl (esp_matter_endpoint.cpp:745-754), so the
 * reused list mirrors what the C6 actually builds. extractor_hood::add()
 * does NOT: it calls add_device_type() and fan_control::create() only
 * (esp_matter_endpoint.cpp:1650-1657), and extractor_hood::config_t is
 * {descriptor, fan_control} with no identify field. Reusing fanEndpoint here
 * would carry an Identify cluster the C6 does not, breaking the stated
 * invariant that this file's lists mirror what the C6 actually builds, for a
 * cluster ExtractorHood.xml:66-67 lists as optional anyway. Ruled in the
 * batch brief: compose WITHOUT Identify. The cost is four slots and 64 heap
 * bytes cheaper per endpoint, which is incidental.
 *
 * Conformance note: ExtractorHood.xml:79-85 marks the FanControl Wind,
 * AirflowDirection and Rocking features disallowConform. This port seeds
 * FanControl's FeatureMap 0 for every fan-bearing type, so nothing is
 * advertised and nothing disallowed is present. Clean.
 *
 * The fan's known FanMode/PercentSetting coupling gap (the audit note on
 * fanControlAttrs above: MatterFanControlClusterServerAttributeChangedCallback
 * is functions-array-bound and never runs on a dynamic endpoint) now ships
 * under a THIRD device-type name. Restated deliberately, the air purifier's
 * rule: an extractor hood host owns that coupling exactly the way a fan host
 * does, and nothing about this row re-solves it.
 *
 * Revision 1 per data_model/1.5/device_types/ExtractorHood.xml. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(extractorHoodClusters)
DECLARE_DYNAMIC_CLUSTER(FanControl::Id, fanControlAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(extractorHoodEndpoint, extractorHoodClusters);

constexpr EmberAfDeviceType kExtractorHoodTypes[] = { { 0x007A, 1 } };

/* ---- refrigerator (0x0070) ---------------------------------------------
 *
 * Catalogue batch 8 audit, RefrigeratorAlarm (0x0057) and
 * RefrigeratorAndTemperatureControlledCabinetMode (0x0052).
 *
 * RefrigeratorAlarm 0x0057:
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; the regeneration left CodeDrivenInitShutdown.cpp
 *   byte-identical). NOT CommandHandlerInterface-only either
 *   (config-data.yaml:21-88), and harmlessly so: the cluster declares no
 *   commands at all, matching RefrigeratorAlarm.xml's disallowConform on
 *   ModifyEnabledAlarms and this tree's own CommandIds.h, which declares
 *   none. The regeneration left IMClusterCommandHandler.cpp byte-identical.
 *
 *   NO Instance, NO Delegate, NO per-endpoint object of any kind. This is
 *   the batch's one genuinely free cluster: RefrigeratorAlarmServer is a
 *   process-global singleton (`static RefrigeratorAlarmServer instance`,
 *   refrigerator-alarm-server.h:53) reached through Instance(), and it reads
 *   and writes the three attributes through the generated Accessors, which
 *   on a dynamic endpoint resolve straight into this file's arena through
 *   the external-storage callbacks. 1 byte of .bss for the whole device.
 *
 *   ServerInit? None: MatterRefrigeratorAlarmPluginServerInitCallback() is
 *   an empty body (refrigerator-alarm-server.cpp:198). No ordering hazard of
 *   any kind, which is why this device type has nothing at all in
 *   mt_devtype_create()'s claim block except its ModeBase slot.
 *
 *   Events: Notify (0x0000). Emitted by SetStateValue() itself, which writes
 *   the State attribute and then LogEvent()s the transition in one call
 *   (refrigerator-alarm-server.cpp:115-149) - the "one call, both effects"
 *   shape AT_MT_SPEC.md §3.22 describes, and the reason AT+MTALARM
 *   rather than AT+MTATTR is the write path for State. The dynamic
 *   endpoint's empty eventList makes EventList read empty without blocking
 *   emission, the generic switch precedent.
 *
 *   Note there is no attribute 0x0001: the ids are Mask 0x0000, State
 *   0x0002, Supported 0x0003 (RefrigeratorAlarm/AttributeIds.h:19-29). Every
 *   one is a plain BITMAP32 and every one is ordinary AT+MTATTR-reachable;
 *   only State's WRITE path is diverted to AT+MTALARM (AT_MT_SPEC.md
 *   2356-2362 says reading all three and writing Mask or Supported works
 *   over AT+MTATTR like any other attribute, and that only writing State
 *   goes through AT+MTALARM so the Notify event actually fires; nothing in
 *   the code enforces that, and a raw write would land in the arena and
 *   skip the event).
 *
 * RefrigeratorAndTemperatureControlledCabinetMode 0x0052 is a plain
 * mode-base-server alias (zap_cluster_list.json:311-313), already compiled
 * since batch 5, so modeBaseAttrs and kModeBaseIncoming/kModeBaseOutgoing
 * are reused verbatim: every ModeBase alias numbers ChangeToMode 0x00 and
 * ChangeToModeResponse 0x01 in its own generated CommandIds.h. Its
 * VerifyOrDie-on-ordering Init() is batch 5's already-domesticated hazard.
 *
 * The C6's own quirk check on this device type came back CLEAN and is worth
 * carrying, because so few in this catalogue did: refrigerator_and_tcc_mode::
 * create() wires create_change_to_mode() and create_change_to_mode_response()
 * unconditionally (esp_matter_cluster.cpp:2934-2937), unlike the
 * OperationalState and SmokeCoAlarm creates that famously did not, and
 * refrigerator_alarm::create() has no command section at all (:2869-2898),
 * which is what conformance wants.
 *
 * Identify is hand-added on the C6 (mk_refrigerator(), mt_devtypes.cpp:
 * 1005-1008) because refrigerator::config_t carries no identify field;
 * declared here, same result.
 *
 * Revision 2 per data_model/1.5/device_types/Refrigerator.xml. Composing a
 * Temperature Controlled Cabinet under this endpoint is legal and gives that
 * cabinet the Cooler cluster set (the parenting policy above); a lone
 * refrigerator row is sub-conformant against Refrigerator.xml:66-72's
 * mandatory 0x0071 child, the oven's disclosed hole exactly. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(refrigeratorAlarmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::Mask::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::State::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::Supported::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(RefrigeratorAlarm::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(refrigeratorClusters)
DECLARE_DYNAMIC_CLUSTER(RefrigeratorAndTemperatureControlledCabinetMode::Id, modeBaseAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(RefrigeratorAlarm::Id, refrigeratorAlarmAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(refrigeratorEndpoint, refrigeratorClusters);

constexpr EmberAfDeviceType kRefrigeratorTypes[] = { { 0x0070, 2 } };

/* ---- TemperatureControl (0x0056), and its two mutually exclusive shapes -
 *
 * Catalogue batch 8 audit. One cluster, two declared attribute lists, and
 * the variant picks which. Shared verbatim by the Temperature Controlled
 * Cabinet (0x0071) and the Cook Surface (0x0077), which is what forced the
 * seed table's variant column: the two device types need the SAME
 * variant-dependent FeatureMap, so no device-type-qualified row could split
 * them.
 *
 *   Code-driven? No (absent from CodeDrivenClusters, config-data.yaml:
 *   144-168; the regeneration left CodeDrivenInitShutdown.cpp
 *   byte-identical). NOT CommandHandlerInterface-only either
 *   (config-data.yaml:21-88), and this time that MATTERS: the cluster
 *   declares SetTemperature, so regenerating hearth.zap added a
 *   TemperatureControl case to IMClusterCommandHandler.cpp, the FIRST new
 *   generated dispatch case since batch 3. Before it, this build dispatched
 *   only Groups, LevelControl, OtaSoftwareUpdateRequestor, OnOff,
 *   SmokeCoAlarm, ModeSelect and Thermostat.
 *
 *   No Instance and no per-endpoint Delegate. What the SDK keeps is one
 *   file-scope TemperatureControlAttrAccess constructed
 *   Optional<EndpointId>::Missing() (temperature-control-server.cpp:40-49),
 *   i.e. a wildcard AttributeAccessInterface serving
 *   SupportedTemperatureLevels on every endpoint, and one process-global
 *   SupportedTemperatureLevelsIteratorDelegate * behind free-function
 *   GetInstance()/SetInstance() (:37, :56-66). The AAI is registered by
 *   MatterTemperatureControlPluginServerInitCallback() (:232-235), which is
 *   emitted into MATTER_PLUGINS_INIT only because the cluster is now in the
 *   zap; miss that and SupportedTemperatureLevels answers
 *   UnsupportedAttribute on every cabinet, silently. The iterator delegate
 *   is the SDK's usual "the app must supply it" hole: SetInstance() has no
 *   caller anywhere in the SDK, so the port calls it itself, exactly as the
 *   C6 does (main.cpp:1829).
 *
 *   Init()? None by that name, and no VerifyOrDie anywhere in this cluster.
 *   emberAfTemperatureControlClusterServerInitCallback() is an empty body
 *   (:230). No ordering hazard at all.
 *
 *   Events: none.
 *
 * SetTemperature IS NOT A +MTCMD CONSUMER, on either platform, and that is
 * worth stating loudly because every other adjudicated cluster in this
 * firmware is. The whole command is handled by the ember callback
 * (temperature-control-server.cpp:112-224): it range-checks the target
 * against MinTemperature/MaxTemperature, enforces the Step modulus
 * (:150-168), then writes TemperatureSetpoint (:169) or
 * SelectedTemperatureLevel (:186). Those writes land in this port's arena
 * and DO raise a +MTATTR URC through MatterPostAttributeChangeCallback(). So
 * a controller changing a cabinet's setpoint reaches the host as an
 * attribute URC, never as a command forward, and the host has no veto. The
 * C6 overrides nothing here either.
 *
 * THE FEATUREMAP IS FUNCTIONALLY LOAD-BEARING, not descriptive. The callback
 * reads it back through TemperatureControlHasFeature() to choose its branch
 * (:117-124, :183), so the seeded value and the declared list must agree or
 * a healthy endpoint rejects valid commands: with BOTH bits set it answers
 * Failure outright (:118-123), and on the TemperatureLevel branch a null
 * iterator delegate answers NotFound (:188-192). The C6 enforces
 * exactly-one at cluster-create time through VALIDATE_FEATURES_EXACT_ONE;
 * here the two separate declared lists plus the two variant-qualified seed
 * rows are what enforce it.
 *
 * The Step bit rides variant 0 with TemperatureNumber, and that is the B119
 * fix carried over rather than rediscovered: Step is its own feature
 * (esp_matter_feature.cpp:1943-1966), and a TemperatureNumber cabinet
 * without it answered +MTERR:4 to every host-library begin(). */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(temperatureControlNumberAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::TemperatureSetpoint::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::MinTemperature::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::MaxTemperature::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::Step::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* SupportedTemperatureLevels is declared ARRAY size 0: metadata-only, so
 * AttributeList stays truthful while attr_gets_slot() refuses it a slot and
 * seed_slots() skips it quietly. It is served by the SDK's wildcard AAI out
 * of this endpoint's block-resident label store, and it doubles as the
 * kStoreWalk discriminator that tells this list from the number list above
 * (they share a cluster id). AT+MTATTR has no path to it at all
 * (AT_MT_SPEC.md 1249-1251); AT+MTTEMPLEVELS is how a host fills it. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(temperatureControlLevelAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::SelectedTemperatureLevel::Id, INT8U, 1,
                          0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::SupportedTemperatureLevels::Id,
                              ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* SetTemperature is mandatory on both variants (temperature-control-cluster.xml
 * :83 declares it with no optional attribute) and, unlike every other
 * incoming list in this file, it is answered entirely inside the SDK: the
 * generated dispatch case the zap regeneration added calls
 * emberAfTemperatureControlClusterSetTemperatureCallback(), which validates
 * and writes an arena attribute. No outgoing list: the command's response is
 * a plain status, not a response command. */
constexpr CommandId kTemperatureControlIncoming[] = {
    TemperatureControl::Commands::SetTemperature::Id, kInvalidCommandId
};

/* ---- temperature controlled cabinet (0x0071), unparented -----------------
 *
 * Catalogue batch 8. The bare cabinet: TemperatureControl and Descriptor and
 * nothing else, which is exactly what the C6 builds, because
 * temperature_controlled_cabinet::add() calls add_device_type() then
 * temperature_control::create() and stops (esp_matter_endpoint.cpp:
 * 1308-1316), and its config_t is {descriptor, temperature_control} with no
 * identify field. TemperatureControlledCabinet.xml lists no Identify either,
 * so there is NO Identify on a cabinet on either platform.
 *
 * These are two of the cabinet's six realised shapes, the two the parent
 * does not touch; the Cooler and Heater halves are below. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cabinetNumberEndpoint, cabinetNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetLevelEndpoint, cabinetLevelClusters);

/* Revision 5 per data_model/1.5/device_types/TemperatureControlledCabinet.xml.
 * The advertised span does NOT change with the parent, unlike the water
 * heater's and battery storage's variant spans: a Cooler cabinet and a
 * Heater cabinet are both 0x0071 revision 5, and the conformance difference
 * lives in the cluster set alone. */
constexpr EmberAfDeviceType kCabinetTypes[] = { { 0x0071, 5 } };

/* ---- the cabinet's parent-conditional halves --------------------------
 *
 * Catalogue batch 8. Four more shapes, and the batch's one genuinely good
 * surprise: where the C6 had to HAND-ROLL two entire cluster shells because
 * esp-matter ships no cluster::oven_mode and no
 * cluster::oven_cavity_operational_state namespace at all
 * (ARCHITECTURE.md:1386-1401 records that as the section 8.6 disease at its
 * worst grade), CHIP ships both server halves. OvenMode is a plain
 * mode-base-server alias (zap_cluster_list.json:279), so modeBaseAttrs and
 * the two shared ModeBase command lists carry it verbatim, and
 * OvenCavityOperationalState::Instance is a purpose-built public subclass
 * that forwards to the protected three-argument OperationalState::Instance
 * ctor with the id baked in (operational-state-server.h:459-478). It adds no
 * data members, so opStateAttrs is reused verbatim too and the existing
 * OperationalState Instance storage is exactly the right size (asserted at
 * the pool in mt_matter_zephyr.cpp).
 *
 * THE CAVITY'S COMMAND LIST IS NARROWER THAN THE BASE CLUSTER'S, and that is
 * not an omission. operational-state-oven-cluster.xml declares Stop (0x01)
 * and Start (0x02) only; Pause (0x00) and Resume (0x03) are disallowConform
 * on this derived cluster, so a controller invoking either gets
 * UNSUPPORTED_COMMAND from the data model with nothing reaching the host at
 * all (AT_MT_SPEC.md 1338-1348). The outgoing list is the shared
 * kOpStateOutgoing: OperationalCommandResponse is 0x04 in the cavity's own
 * generated CommandIds.h as well, so one constant serves both.
 *
 * Revisions are this tree's own Metadata.h, as ever, and BOTH diverge
 * downward from the C6, which hand-set 2 for each from the 1.5.1 XMLs
 * (mt_devtypes.cpp:1200, :1218) rather than copying the 4 and 3 its mirrored
 * rvc_run_mode / rvc_operational_state bodies carried. This tree says
 * OvenMode 1 and OvenCavityOperationalState 1. Disclosable, not a bug: the
 * port's convention has been this tree's kRevision since batch 2, and the
 * cavity's is inert anyway because Instance::Read() answers ClusterRevision
 * itself. */
constexpr CommandId kOvenCavityIncoming[] = { OvenCavityOperationalState::Commands::Stop::Id,
                                              OvenCavityOperationalState::Commands::Start::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetCoolerNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(RefrigeratorAndTemperatureControlledCabinetMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetCoolerLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(RefrigeratorAndTemperatureControlledCabinetMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetHeaterNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OvenMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(OvenCavityOperationalState::Id, opStateAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kOvenCavityIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cabinetHeaterLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OvenMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(OvenCavityOperationalState::Id, opStateAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kOvenCavityIncoming, kOpStateOutgoing),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cabinetCoolerNumberEndpoint, cabinetCoolerNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetCoolerLevelEndpoint, cabinetCoolerLevelClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetHeaterNumberEndpoint, cabinetHeaterNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cabinetHeaterLevelEndpoint, cabinetHeaterLevelClusters);

/* All six realised shapes, and the whole point of the mechanism: the
 * composition blob stores only device type, variant and parent index, and
 * this table is what turns the pair into a cluster set at rebuild time. A
 * host cannot ask for a different set, and changing a cabinet's parent
 * changes the cabinet (AT_MT_SPEC.md 495-505). */
constexpr hearth_shape kCabinetShapes[] = {
    { 0, 0, &cabinetNumberEndpoint },
    { 1, 0, &cabinetLevelEndpoint },
    { 0, kDtRefrigerator, &cabinetCoolerNumberEndpoint },
    { 1, kDtRefrigerator, &cabinetCoolerLevelEndpoint },
    { 0, kDtOven, &cabinetHeaterNumberEndpoint },
    { 1, kDtOven, &cabinetHeaterLevelEndpoint },
};

/* ---- cook surface (0x0077) ---------------------------------------------
 *
 * Catalogue batch 8. TemperatureControl (either variant) plus an OffOnly
 * OnOff plus Descriptor, and no Identify: cook_surface::config_t is
 * {descriptor, temperature_control} (esp_matter_endpoint.h:832-841) and
 * CookSurface.xml lists only OnOff, TemperatureControl and
 * TemperatureMeasurement.
 *
 * THE ONLY DEVICE TYPE IN THE CATALOGUE THAT REQUIRES A PARENT. Both shapes
 * name a Cooktop; the unparented form has no shape at all, which is the
 * cluster-set half of what mt_devtype_parent_ok() says on the AT+MTEP= line
 * itself (+MTERR:1, AT_MT_SPEC.md 640-655). It is also the only append this
 * platform can reject for a reason no earlier nRF test exercises.
 *
 * THE C6'S B216 SCRUB IS NOT MIRRORED, AND THAT IS DELIBERATE.
 * cook_surface::add() OVERWRITES config->temperature_control.feature_flags
 * with temperature_level before creating the cluster
 * (esp_matter_endpoint.cpp:1540), so pre-set flags cannot survive; the C6's
 * thunk therefore destroys the forced TemperatureLevel cluster after
 * create() and rebuilds it as TemperatureNumber plus Step, for variant 0
 * only (mt_devtypes.cpp:1305-1321). None of that transfers: this port
 * declares its lists, so there is nothing to overwrite and nothing to
 * destroy. What DOES transfer is the reason B216 existed, that the two
 * variants must be distinguishable on the wire, and the two declared lists
 * deliver it directly. The regression net that caught B216 (AT+MTATTR on
 * 0x0056/0x0000 and 0x0003 answering on v0 and +MTERR:4 on v1, and
 * AT+MTTEMPLEVELS accepted on v1 only) is the right check here too.
 *
 * The OffOnly OnOff is the cooktop's list one endpoint down: racOnOffAttrs
 * with kOnOffOffOnlyIncoming, and the same device-type-qualified 0x04
 * FeatureMap seed. CookSurface.xml:67-73 makes OnOff optional with OFFONLY
 * mandatory inside it, so composing it at all is the C6's superset choice
 * mirrored, not a requirement; AT_MT_SPEC.md 649-655 already tells hosts
 * that turning a surface on is an AT+MTATTR write of 0x0006/0x0000.
 *
 * Revision 2 per data_model/1.5/device_types/CookSurface.xml. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cookSurfaceNumberClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlNumberAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kOnOffOffOnlyIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(cookSurfaceLevelClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureControl::Id, temperatureControlLevelAttrs,
                        ZAP_CLUSTER_MASK(SERVER), kTemperatureControlIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(OnOff::Id, racOnOffAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kOnOffOffOnlyIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(cookSurfaceNumberEndpoint, cookSurfaceNumberClusters);
HEARTH_DECLARE_CONST_ENDPOINT(cookSurfaceLevelEndpoint, cookSurfaceLevelClusters);

constexpr EmberAfDeviceType kCookSurfaceTypes[] = { { 0x0077, 2 } };

constexpr hearth_shape kCookSurfaceShapes[] = {
    { 0, kDtCooktop, &cookSurfaceNumberEndpoint },
    { 1, kDtCooktop, &cookSurfaceLevelEndpoint },
};

/* ---- microwave oven (0x0079) -------------------------------------------
 *
 * Catalogue batch 8 audit, MicrowaveOvenMode (0x005E) and
 * MicrowaveOvenControl (0x005F). The heaviest device type in the batch by
 * every measure: three per-endpoint object pairs on one endpoint, and the
 * one type whose object draw moved HEARTH_OBJ_HEAP_BYTES.
 *
 * MicrowaveOvenMode 0x005E:
 *
 *   Code-driven? No. CHI-only? No either (absent from config-data.yaml:
 *   21-88), and harmlessly, because IT DECLARES NO COMMANDS AT ALL. The
 *   regeneration left IMClusterCommandHandler.cpp byte-identical.
 *
 *   THE TRAP, and it is a copy-paste trap rather than a reasoning one:
 *   MicrowaveOvenMode inherits ChangeToMode from ModeBase and then marks it
 *   disallowConform, along with ChangeToModeResponse
 *   (data_model/1.5/clusters/Mode_MicrowaveOven.xml, the commands block),
 *   which is exactly the treatment the oven cavity gives Pause and Resume
 *   forty lines above; the generated MicrowaveOvenMode/CommandIds.h
 *   consequently carries kAcceptedCommandsCount 0 and kGeneratedCommandsCount
 *   0. Every other ModeBase alias in this file reuses kModeBaseIncoming and
 *   kModeBaseOutgoing, and this one MUST NOT: its incoming and outgoing lists
 *   are both nullptr, so both lists come out empty. The mode is selected
 *   through MicrowaveOvenControl's SetCookingParameters cookMode field
 *   instead (AT_MT_SPEC.md 1381-1382, 1980-1984; batch 5's audit found the
 *   same). Advertising ChangeToMode here would let a controller invoke a
 *   command the cluster disallows, and the ModeBase Instance's own
 *   CommandHandlerInterface would execute it.
 *
 *   Everything else about it is the standard alias: modeBaseAttrs verbatim,
 *   a ModeBase pool slot, a block-resident mt_mb_store_t, and Init()'s
 *   VerifyOrDie ordering hazard.
 *
 * MicrowaveOvenControl 0x005F:
 *
 *   Code-driven? No. CHI-only? YES (config-data.yaml:61), so no generated
 *   dispatch for its two commands; the Instance's own
 *   CommandHandlerInterface takes them. Events: none.
 *
 *   Its Instance is the batch's sharpest hazard, and the whole ordering
 *   contract lives at mt_matter_mwoc_register() in mt_matter_zephyr.cpp.
 *
 *   PowerAsNumber is MANDATORY and PowerNumberLimits is dead code, and that
 *   pair of facts is why mwocAttrs is four attributes and not nine.
 *   Instance::Init() enforces exactly one of PowerAsNumber/PowerInWatts and
 *   that PowerNumberLimits implies PowerAsNumber
 *   (microwave-oven-control-server.cpp:66-96); esp-matter's own create() runs
 *   VALIDATE_FEATURES_EXACT_ONE against PowerAsNumber alone
 *   (esp_matter_cluster.cpp:3083-3084), and its power_number_limits::add()
 *   only applies when PowerAsNumber is ABSENT, the inverse of conformance
 *   and therefore dead in the pinned tree. Consequence, on both platforms:
 *   MinPower, MaxPower and PowerStep are NOT declared and read the SDK's own
 *   compiled-in 10/100/10 (microwave-oven-control-server.cpp:163-179,
 *   constants at the header's :34-38). SupportedWatts, SelectedWattIndex and
 *   WattRating are PowerInWatts-side and absent for the same reason.
 *
 *   CookTime, MaxCookTime and PowerSetting ARE declared, and all three are
 *   Instance-served: Instance::Read() answers CookTime from its own member
 *   and the other two from the delegate (:142-157). The slots exist for
 *   AttributeList truthfulness and are seeded to exactly what those sources
 *   serve at boot, the FanControl agreement discipline, so an AT+MTATTR read
 *   answers the same value a controller sees until the first cooking command
 *   moves the live value. NO k_instance_served carve-out, deliberately and
 *   against the audit's suggestion: AT_MT_SPEC.md 1441-1456 says plainly
 *   that neither attribute raises a +MTATTR URC and that a host reads them
 *   back only through a commissioned controller, because SetCookTimeSec()
 *   and the delegate's own bookkeeping bypass emberAfWriteAttribute
 *   entirely. A carve-out would have made the AT read live and contradicted
 *   the published contract; the shadow going stale is what the spec
 *   describes.
 *
 * AddMoreTime is where this port is SIMPLER than the C6. esp-matter ships
 * create_add_more_time() (esp_matter_command.cpp:2590) with zero callers, so
 * the C6's thunk hand-adds it after create() (mt_devtypes.cpp:946-949). Here
 * the accepted-command list is declared metadata and AddMoreTime is simply a
 * second entry in it.
 *
 * OperationalState needs its OWN attribute list, and this is the one place
 * opStateAttrs could not be reused: MicrowaveOven.xml:76-88 makes
 * CountdownTime (0x0002) MANDATORY and opStateAttrs does not declare it.
 * Instance::Read() serves it from the delegate's GetCountdownTime()
 * (operational-state-server.cpp:402-406), which this port's delegate answers
 * NullNullable for, so the ember slot is an inert shadow seeded to the uint32
 * null sentinel: the arena read and the served value agree on null, both
 * answer +MTERR:5, and no carve-out row is needed. That agreement is exactly
 * why CountdownTime is NOT in k_instance_served even though its two
 * neighbours on the same cluster are.
 *
 * NO Identify: microwave_oven::config_t derives from
 * app_with_operational_state_config, whose base has no identify field at all
 * (mt_devtypes.cpp:885-889), and MicrowaveOven.xml agrees.
 *
 * ONE DISCLOSED CONFORMANCE GAP, and it is worth naming because it is the
 * thing revision 2 exists for. MicrowaveOven.xml's own revision history
 * reads "2: Mandate OperationCompletion Event", and this firmware emits
 * neither OperationCompletion nor OperationalError, on either platform: the
 * host owns appliance state transitions and reports them with AT+MTOPSTATE,
 * which writes the attribute and reports it without generating the cluster's
 * events. The dynamic endpoint's eventList is empty regardless, so EventList
 * reads empty rather than advertising an event that never fires. The
 * advertised device-type revision stays 2 because it matches the XML this
 * file has taken as its authority since batch 1 AND what the C6 advertises
 * (ESP_MATTER_MICROWAVE_OVEN_DEVICE_TYPE_VERSION 2,
 * esp_matter_endpoint.h:90), so dropping to 1 would invent a divergence
 * rather than fix one. The same gap exists unremarked on the washer trio and
 * the RVC; it is named here because here the revision number points straight
 * at it.
 *
 * Revision 2 per data_model/1.5/device_types/MicrowaveOven.xml. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(microwaveOpStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::PhaseList::Id, ARRAY, 0,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::CurrentPhase::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::CountdownTime::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalStateList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalError::Id, STRUCT, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(mwocAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::CookTime::Id, INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::MaxCookTime::Id, INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::PowerSetting::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(MicrowaveOvenControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kMwocIncoming[] = {
    MicrowaveOvenControl::Commands::SetCookingParameters::Id,
    MicrowaveOvenControl::Commands::AddMoreTime::Id, kInvalidCommandId
};

/* kOpStateIncoming keeps all four commands here, and Start is load-bearing
 * beyond its own invoke: SetCookingParameters with a startAfterSetting field
 * asks the data model provider whether OperationalState's Start command is
 * accepted on this endpoint, and answers InvalidCommand if it is not
 * (microwave-oven-control-server.cpp:249-273). Dropping Start from this list
 * as "the host starts it anyway" would break a legal cooking command. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(microwaveOvenClusters)
DECLARE_DYNAMIC_CLUSTER(OperationalState::Id, microwaveOpStateAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kOpStateIncoming, kOpStateOutgoing),
    /* nullptr, NOT kModeBaseIncoming: this alias has no ChangeToMode at all
     * (the section comment's trap). */
    DECLARE_DYNAMIC_CLUSTER(MicrowaveOvenMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(MicrowaveOvenControl::Id, mwocAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kMwocIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(microwaveOvenEndpoint, microwaveOvenClusters);

constexpr EmberAfDeviceType kMicrowaveOvenTypes[] = { { 0x0079, 2 } };

/* ---- energy EVSE (0x050C) ----------------------------------------------
 *
 * The last device type, and the one ruling DE408 excluded when the board sat
 * at 96.67 percent of RAM. The memory reclaim rounds left 46.5 KB free and
 * made the row staging buffers pay per use, so the exclusion is lifted WITH A
 * CAP: MT_EVSE_MAX (2, core/include/mt_matter.h), the C6's own constant, and
 * a third declared EVSE fails its create with the established loud line while
 * the composition serves the prefix before it, exactly as the measurement
 * pools do.
 *
 * Batch 7 audit section 3.8 is the lead this was written from, and it was
 * written before the const catalogue macros, the object heap, store_walk and
 * batch 8's shape mechanism existed. Everything below is re-verified against
 * the pinned NCS tree and against current main; where the audit and the code
 * disagree the code wins and the disagreement is named at the line.
 *
 * ---- THE COMPOSITION, verified against the C6's thunk ------------------
 *
 * mk_energy_evse() (platform/esp32c6/main/mt_devtypes.cpp) calls the SDK's
 * energy_evse::create() and then grafts, so the endpoint's real cluster set
 * is the union of four things:
 *
 *   energy_evse::add()      EnergyEvse 0x0099, EnergyEvseMode 0x009D and a
 *                           bare DeviceEnergyManagement 0x0098
 *   feature::charging_preferences::add(), called UNCONDITIONALLY at
 *                           esp_matter_cluster.cpp:3395, OUTSIDE the
 *                           CLUSTER_FLAG_SERVER guard that closes one line
 *                           above it: the four NextCharge* attributes and
 *                           SetTargets/GetTargets/ClearTargets
 *   mt_graft_electrical_sensor(ep, true, ...)   PowerTopology 0x009C,
 *                           ElectricalPowerMeasurement 0x0090 and
 *                           ElectricalEnergyMeasurement 0x0091, with_eem TRUE
 *                           on BOTH variants (EVSE.xml revision 2 mandates a
 *                           composed Electrical Sensor carrying both)
 *   mt_add_dem_triple(ep, FALSE, ...)   the over-delivered DEM rewired with
 *                           its delegate and DeviceEnergyManagementMode
 *                           0x009F, and the 0x050D device type
 *
 * EIGHT clusters, the largest list in the catalogue by three, on both
 * variants. THE AUDIT AND THE CODE DISAGREE ON NOTHING HERE, but one of its
 * remarks does not survive the port: it says the DEM cluster is "kept, not
 * stripped ... relying on esp-matter's idempotent duplicate cluster create".
 * On this port there is no duplicate to reconcile, because nothing creates a
 * bare DEM first: the list below simply declares DEM once, with the
 * report-only attribute set.
 *
 * ---- THE TWO VARIANTS --------------------------------------------------
 *
 * Variant 0 adds SOC reporting, variant 1 does not, and it is a genuine
 * wire-visible difference rather than decoration: with the SOC feature
 * Instance::ValidateTargets() makes targetSoC MANDATORY in every charging
 * target and answers InvalidCommand when it is missing, while without it
 * targetSoC must be absent or exactly 100. evse_targets_apply_locked()
 * enforces the identical rule on the AT path so a host cannot install a
 * schedule a controller's SetTargets would have been refused. Both variants
 * are conformant; the difference is two attributes and one validation rule.
 *
 * ---- WHAT IS NEVER BUILT, AND WHY EACH ONE IS A DELIBERATE OMISSION ----
 *
 * PREF (charging preferences) is UNCONDITIONALLY ON, both variants, because
 * the SDK's own Instance derives the advertised AcceptedCommandList from its
 * feature mask (RetrieveAcceptedCommands, energy-evse-server.cpp:186-211) and
 * this port hands it kChargingPreferences. So the four NextCharge*
 * attributes and SetTargets/GetTargets/ClearTargets exist on every EVSE.
 *
 * V2X IS NEVER BUILT, and this is the iron rule (ARCHITECTURE.md 8.12) in its
 * clearest form yet. Setting the V2X bit would put EnableDischarging in that
 * derived AcceptedCommandList, and the four V2X attributes
 * (DischargingEnabledUntil, MaximumDischargeCurrent, SessionEnergyDischarged
 * and the discharge half of the session) are NOT declared below, so a
 * hand-set bit would advertise a command with no attribute behind it and a
 * capability this firmware has no host story for. On the C6 the rule is
 * "never write FeatureMap, always feature::add()"; here there is no
 * feature::add(), so it inverts into "the mask handed to the Instance and the
 * declared list are one decision", which is what mt_matter_evse_register()'s
 * with_soc argument and this list are.
 *
 * RFID IS NEVER BUILT either. On the C6, feature::rfid::add() sets the bit
 * and creates nothing at all, not even the Rfid event metadata the XML
 * declares, so an advertising endpoint would answer an empty shell to any
 * controller that subscribed. Here it would be one bit with no consequence,
 * which is worse rather than better: a truthful FeatureMap is the whole
 * point.
 *
 * PLUG-AND-CHARGE IS NEVER BUILT: VehicleID is a char_string with no host
 * story, and the delegate's getter answers a null CharSpan for exactly that
 * reason.
 *
 * STARTDIAGNOSTICS IS NEVER ADVERTISED: the Instance is constructed with an
 * EMPTY OptionalCommands mask, so RetrieveAcceptedCommands never appends it
 * and an invoke answers UnsupportedCommand. The delegate implements the pure
 * virtual and can never be reached, which is coherent and needs no fix. The
 * C6 reaches the same wire by a different route (esp-matter declares
 * command::create_start_diagnostics and has zero callers for it).
 *
 * THE THREE OPTIONAL WRITABLES are not declared: UserMaximumChargeCurrent,
 * RandomizationDelayWindow and ApproximateEVEfficiency. esp-matter creates
 * none of them, the Instance gates each one on an OptionalAttributes mask
 * this port constructs empty, and AT+MTMEAS fields 7, 8 and 13 therefore
 * answer +MTERR:4 on every variant, exactly as AT_MT_SPEC.md 3.25 says.
 *
 * ---- THE ATTRIBUTE LISTS ------------------------------------------------
 *
 * EnergyEvse's three answers: code-driven no, CHI-only YES (its seven
 * commands have no generated dispatch; the batch's zap regeneration left
 * IMClusterCommandHandler.cpp and CodeDrivenInitShutdown.cpp byte-identical,
 * as batch 7's audit 5.5 predicted), AAI YES per endpoint (Instance derives
 * from both AttributeAccessInterface and CommandHandlerInterface,
 * energy-evse-server.h:176-186, and its ctor calls both SetEndpointId() and
 * SetInstance() on the delegate). Init() registers the CHI then the AAI, SOFT
 * on both (energy-evse-server.cpp:40-46): no emberAfContainsServer check, no
 * VerifyOrDie, so an ordering mistake here cannot panic the device, unlike
 * the ModeBase alias riding beside it.
 *
 * Instance::Read() serves ALL 23 attributes from the delegate and falls
 * through to ember only for ClusterRevision (:68-133), so every slot below is
 * an INERT SHADOW except ClusterRevision, whose seed is live, and the DE397
 * carve-out in mt_matter_zephyr.cpp is what makes an AT read answer the same
 * value a subscribed controller sees.
 *
 * SIX of the declarations are metadata-only, the DE407 option-C shape: the
 * three amperage_ma currents, NextChargeRequiredEnergy, BatteryCapacity and
 * SessionEnergyCharged are 8-byte ZCL types that attr_gets_slot() refuses a
 * 4-byte slot, and they are Instance-served, so they take quiet-table rows
 * below rather than shouting at boot.
 *
 * Slots: v0 = State, SupplyState, FaultState, ChargingEnabledUntil,
 * SessionID, SessionDuration, FeatureMap, NextChargeStartTime,
 * NextChargeTargetTime, NextChargeTargetSoC, StateOfCharge and
 * ClusterRevision = 12; v1 drops StateOfCharge = 11. That is the audit's
 * 12/11 exactly.
 *
 * ---- THE BLOCK ---------------------------------------------------------
 *
 * 8 clusters, 31 slots on v0 (12 EVSE + 3 EvseMode + 5 DEM + 3 DEMMode + 2
 * PTOP + 4 EPM + 2 EEM) and TWO mt_mb_store_t, the second type after the RVC
 * to carry two: 32 + 496 + 612 = 1,140 B payload, 1,144 B of heap. The widest
 * block in the catalogue by 280 B, and the number that made the floor
 * assertion cap-aware (see kEvseBlockBytes below). Variant 1 is 1,128 B.
 *
 * ---- EVENTS -------------------------------------------------------------
 *
 * CumulativeEnergyMeasured, from the EEM push path (batch 7a), and nothing
 * else: EVSEs, EVNotDetected, EnergyTransferStarted/Stopped and Fault are all
 * declared by the cluster XML and emitted by nobody, on either platform,
 * because this firmware has no host verb that would say when one happened.
 *
 * Device revision 2 per data_model/1.5/device_types/EVSE.xml:60. */

HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(evseAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::State::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SupplyState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FaultState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::ChargingEnabledUntil::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::CircuitCapacity::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MinimumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MaximumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeStartTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeRequiredEnergy::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetSoC::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::StateOfCharge::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::BatteryCapacity::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionID::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionDuration::Id, ELAPSED_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionEnergyCharged::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Variant 1, without SOC reporting: the same list minus StateOfCharge and
 * BatteryCapacity, the two attributes soc_reporting::add() creates. The
 * FeatureMap seed drops the bit with them, and mt_matter_evse_register()
 * builds the Instance's mask by asking THIS list for StateOfCharge (the
 * create path's second half), so the declared list and the Instance cannot
 * disagree: one is derived from the other. The SEED is the one of the three
 * that is not, being a table row keyed on the variant, and it agrees because
 * the registry selects the list by that same variant; if a future row ever
 * breaks that, the seed is the copy that goes wrong, and it is an inert
 * shadow (the Instance serves FeatureMap from mFeature), so it would go
 * wrong quietly. That is the DEM report-only list's rule with its one real
 * weak point named rather than glossed. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(evseNoSocAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::State::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SupplyState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FaultState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::ChargingEnabledUntil::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::CircuitCapacity::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MinimumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::MaximumChargeCurrent::Id, AMPERAGE_MA, 8, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeStartTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetTime::Id, EPOCH_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeRequiredEnergy::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::NextChargeTargetSoC::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionID::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionDuration::Id, ELAPSED_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::SessionEnergyCharged::Id, ENERGY_MWH, 8,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(EnergyEvse::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* The five commands the Instance really advertises under
 * kChargingPreferences, and no more: Disable and EnableCharging are
 * unconditional, SetTargets/GetTargets/ClearTargets ride PREF, and
 * EnableDischarging (V2X) and StartDiagnostics (an optional command with an
 * empty mask) are absent from BOTH this list and the Instance's own derived
 * one. The two must agree: RetrieveAcceptedCommands is what a controller
 * reads, and a declared-but-unadvertised command is the quiet incoherence the
 * DE399 truthfulness discipline exists to prevent. */
constexpr CommandId kEvseIncoming[] = { EnergyEvse::Commands::Disable::Id,
                                        EnergyEvse::Commands::EnableCharging::Id,
                                        EnergyEvse::Commands::SetTargets::Id,
                                        EnergyEvse::Commands::GetTargets::Id,
                                        EnergyEvse::Commands::ClearTargets::Id,
                                        kInvalidCommandId };

/* GetTargetsResponse is the port's second outgoing command list, after the
 * OperationalState trio's, and unlike that one it is not a DE399 disclosure:
 * GetTargets really does answer with it (HandleGetTargets calls
 * AddResponse()), so declaring it is the truthful thing to do. */
constexpr CommandId kEvseOutgoing[] = { EnergyEvse::Commands::GetTargetsResponse::Id,
                                        kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(energyEvseClusters)
DECLARE_DYNAMIC_CLUSTER(EnergyEvse::Id, evseAttrs, ZAP_CLUSTER_MASK(SERVER), kEvseIncoming,
                        kEvseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(EnergyEvseMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demReportOnlyAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

/* Variant 1: the identical eight clusters with the no-SOC EnergyEvse list.
 * Nothing else moves, which is why the two variants draw the same six
 * per-endpoint objects and differ by 16 block bytes. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(energyEvseNoSocClusters)
DECLARE_DYNAMIC_CLUSTER(EnergyEvse::Id, evseNoSocAttrs, ZAP_CLUSTER_MASK(SERVER), kEvseIncoming,
                        kEvseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(EnergyEvseMode::Id, modeBaseAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagement::Id, demReportOnlyAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(DeviceEnergyManagementMode::Id, modeBaseAttrs,
                            ZAP_CLUSTER_MASK(SERVER), kModeBaseIncoming, kModeBaseOutgoing),
    DECLARE_DYNAMIC_CLUSTER(PowerTopology::Id, ptopAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalPowerMeasurement::Id, epmAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ElectricalEnergyMeasurement::Id, eemAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(energyEvseEndpoint, energyEvseClusters);
HEARTH_DECLARE_CONST_ENDPOINT(energyEvseNoSocEndpoint, energyEvseNoSocClusters);

/* Three device types on one endpoint, the same span on both variants: the
 * sensor graft and the DEM triple are unconditional here, unlike the water
 * heater's (whose variant 1 loses the graft and its 0x0510 id together), so
 * this row needs no device_types_v1. 0x050C revision 2 per EVSE.xml:60,
 * 0x0510 revision 1 and 0x050D revision 3 matching their standalone rows. */
constexpr EmberAfDeviceType kEnergyEvseTypes[] = { { 0x050C, 2 }, { 0x0510, 1 }, { 0x050D, 3 } };

/* ---- the registry ---------------------------------------------------- */

/* Catalogue batch 7a added ep_type_v1, the port's rendering of the C6's
 * max_variant semantics: a max_variant-1 row's variant 1 is a SECOND
 * declared cluster list, because this port has no cluster::destroy()
 * analogue to carve one list down at runtime (the C6's B216
 * destroy-after-create move on the electrical meter). It is the LAST
 * member on purpose: aggregate initialization zero-fills members a brace
 * list does not reach, so every max_variant-0 row keeps its exact
 * original text and gets nullptr for free, and mt_devtype_create()
 * falls back to ep_type whenever ep_type_v1 is null. The device-type
 * span is shared by both variants (the advertised ids and revisions do
 * not change with the cluster set) FOR EVERY ROW UP TO BATCH 7A; batch 7b
 * broke that generalization and added device_types_v1 by the identical
 * mechanism (LAST member, zero-fill = empty span, create falls back to
 * device_types): the water heater's variant 0 grafts the Electrical
 * Sensor and advertises 0x0510 alongside 0x050F while variant 1 does not,
 * and Battery Storage's variant 1 drops the DEM pair and with it the
 * 0x050D id, so a shared span would advertise device types those
 * variants' cluster sets cannot back (the C6 has no such member because
 * each add_device_type() call sits inside the variant branch that earns
 * it, mk_water_heater()/mk_battery_storage()). */
struct hearth_devtype {
    uint32_t id;
    uint8_t max_variant;
    const EmberAfEndpointType *ep_type;
    Span<const EmberAfDeviceType> device_types;
    const EmberAfEndpointType *ep_type_v1;
    Span<const EmberAfDeviceType> device_types_v1;
    /* Catalogue batch 8, the LAST member for the zero-fill reason above.
     * Empty (the zero-fill) means "this row's cluster set does not depend on
     * its parent"; a non-empty span REPLACES the ep_type/ep_type_v1
     * selection entirely for that row, and must cover exactly the pairs
     * parent_policy_ok() accepts. */
    Span<const hearth_shape> shapes;
};

/* constexpr, not merely const, since batch 8: shape_domain_matches_policy()
 * below has to READ this table at compile time to prove the shape maps and
 * the parenting policy agree, and a const object of class type is not usable
 * in a constant expression. Every initializer here was already a constant
 * expression (ids and counts are literals, the endpoint pointers are
 * addresses of static objects, the device-type spans are built from
 * constexpr arrays through Span's constexpr array constructor,
 * Span.h:68), so this is a one-token change that moves no byte: the 44
 * pre-batch-8 rows below are unchanged, text and value. */
constexpr hearth_devtype s_registry[] = {
    { 0x0100, 0, &onOffLightEndpoint, Span<const EmberAfDeviceType>(kOnOffLightTypes) },
    { 0x0101, 0, &dimmableLightEndpoint, Span<const EmberAfDeviceType>(kDimmableLightTypes) },
    { 0x0302, 0, &temperatureSensorEndpoint, Span<const EmberAfDeviceType>(kTemperatureSensorTypes) },
    { 0x0015, 0, &booleanStateSensorEndpoint, Span<const EmberAfDeviceType>(kContactSensorTypes) },
    { 0x0107, 0, &occupancySensorEndpoint, Span<const EmberAfDeviceType>(kOccupancySensorTypes) },
    { 0x0307, 0, &humiditySensorEndpoint, Span<const EmberAfDeviceType>(kHumiditySensorTypes) },
    { 0x0305, 0, &pressureSensorEndpoint, Span<const EmberAfDeviceType>(kPressureSensorTypes) },
    { 0x0044, 0, &booleanStateSensorEndpoint, Span<const EmberAfDeviceType>(kRainSensorTypes) },
    { 0x0041, 0, &booleanStateSensorEndpoint,
      Span<const EmberAfDeviceType>(kWaterFreezeDetectorTypes) },
    { 0x0043, 0, &booleanStateSensorEndpoint,
      Span<const EmberAfDeviceType>(kWaterLeakDetectorTypes) },
    { 0x0106, 0, &lightSensorEndpoint, Span<const EmberAfDeviceType>(kLightSensorTypes) },
    { 0x0306, 0, &flowSensorEndpoint, Span<const EmberAfDeviceType>(kFlowSensorTypes) },
    { 0x010A, 0, &onOffPlugInUnitEndpoint, Span<const EmberAfDeviceType>(kOnOffPlugInUnitTypes) },
    { 0x010B, 0, &dimmablePlugInUnitEndpoint, Span<const EmberAfDeviceType>(kDimmablePlugInUnitTypes) },
    /* Catalogue batch 2: the server-interaction types. */
    { 0x010C, 0, &colorTemperatureLightEndpoint,
      Span<const EmberAfDeviceType>(kColorTemperatureLightTypes) },
    { 0x010D, 0, &extendedColorLightEndpoint,
      Span<const EmberAfDeviceType>(kExtendedColorLightTypes) },
    { 0x0301, 0, &thermostatEndpoint, Span<const EmberAfDeviceType>(kThermostatTypes) },
    { 0x002B, 0, &fanEndpoint, Span<const EmberAfDeviceType>(kFanTypes) },
    { 0x0202, 0, &windowCoveringEndpoint, Span<const EmberAfDeviceType>(kWindowCoveringTypes) },
    { 0x002C, 0, &airQualitySensorEndpoint, Span<const EmberAfDeviceType>(kAirQualitySensorTypes) },
    /* Catalogue batch 3: the command-verdict types. */
    { 0x000A, 0, &doorLockEndpoint, Span<const EmberAfDeviceType>(kDoorLockTypes) },
    { 0x0042, 0, &waterValveEndpoint, Span<const EmberAfDeviceType>(kWaterValveTypes) },
    /* Catalogue batch 4: the appliance and notification types. */
    { 0x0011, 0, &powerSourceEndpoint, Span<const EmberAfDeviceType>(kPowerSourceTypes) },
    { 0x0076, 0, &smokeCoAlarmEndpoint, Span<const EmberAfDeviceType>(kSmokeCoAlarmTypes) },
    { 0x0073, 0, &applianceOpStateEndpoint, Span<const EmberAfDeviceType>(kLaundryWasherTypes) },
    { 0x0075, 0, &applianceOpStateEndpoint, Span<const EmberAfDeviceType>(kDishwasherTypes) },
    { 0x007C, 0, &applianceOpStateEndpoint, Span<const EmberAfDeviceType>(kLaundryDryerTypes) },
    { 0x0027, 0, &modeSelectEndpoint, Span<const EmberAfDeviceType>(kModeSelectTypes) },
    { 0x0146, 0, &chimeEndpoint, Span<const EmberAfDeviceType>(kChimeTypes) },
    /* Catalogue batch 5: the standalone remainder. */
    { 0x002D, 0, &fanEndpoint, Span<const EmberAfDeviceType>(kAirPurifierTypes) },
    { 0x010F, 0, &onOffPlugInUnitEndpoint,
      Span<const EmberAfDeviceType>(kMountedOnOffControlTypes) },
    { 0x0110, 0, &dimmablePlugInUnitEndpoint,
      Span<const EmberAfDeviceType>(kMountedDimmableLoadControlTypes) },
    { 0x000F, 0, &genericSwitchEndpoint, Span<const EmberAfDeviceType>(kGenericSwitchTypes) },
    { 0x0303, 0, &pumpEndpoint, Span<const EmberAfDeviceType>(kPumpTypes) },
    { 0x0072, 0, &roomAirConditionerEndpoint,
      Span<const EmberAfDeviceType>(kRoomAirConditionerTypes) },
    { 0x0074, 0, &rvcEndpoint, Span<const EmberAfDeviceType>(kRvcTypes) },
    /* Catalogue batch 7a: the energy foundation types, the registry's
     * first variant-carrying rows (variant 0 = power + energy, variant 1
     * = power only; the disclosure split is on the section comment). */
    { 0x0510, 1, &electricalSensorEndpoint, Span<const EmberAfDeviceType>(kElectricalSensorTypes),
      &electricalSensorPowerOnlyEndpoint },
    { 0x0514, 1, &electricalMeterEndpoint, Span<const EmberAfDeviceType>(kElectricalMeterTypes),
      &electricalMeterPowerOnlyEndpoint },
    { 0x0309, 0, &wiredElectricalSensorEndpoint, Span<const EmberAfDeviceType>(kHeatPumpTypes) },
    { 0x0017, 1, &wiredElectricalSensorEndpoint, Span<const EmberAfDeviceType>(kSolarPowerTypes),
      &wiredElectricalSensorPowerOnlyEndpoint },
    { 0x0511, 0, &utilityMeterEndpoint,
      Span<const EmberAfDeviceType>(kElectricalUtilityMeterTypes) },
    { 0x050D, 1, &demEndpoint, Span<const EmberAfDeviceType>(kDemTypes), &demReportOnlyEndpoint },
    /* Catalogue batch 7b: the delegate-served energy pair. The first rows
     * with a variant-dependent device-type span (device_types_v1, the
     * struct comment): the water heater's v1 loses the sensor graft and
     * its 0x0510 id together. */
    { 0x050F, 1, &waterHeaterEndpoint, Span<const EmberAfDeviceType>(kWaterHeaterTypes),
      &waterHeaterBareEndpoint, Span<const EmberAfDeviceType>(kWaterHeaterBareTypes) },
    { 0x0018, 1, &batteryStorageEndpoint, Span<const EmberAfDeviceType>(kBatteryStorageTypes),
      &batteryStorageNoDemEndpoint, Span<const EmberAfDeviceType>(kBatteryStorageNoDemTypes) },
    /* Catalogue batch 8: the composed appliances. These three carry no
     * shapes: the policy is permissive for all of them, and a cooktop's or
     * an oven's own cluster set does not change when a child is composed
     * under it (only the child's does, and only PartsList moves on the
     * parent, which the provider serves). */
    { kDtCooktop, 0, &cooktopEndpoint, Span<const EmberAfDeviceType>(kCooktopTypes) },
    { kDtOven, 0, &ovenEndpoint, Span<const EmberAfDeviceType>(kOvenTypes) },
    { 0x007A, 0, &extractorHoodEndpoint, Span<const EmberAfDeviceType>(kExtractorHoodTypes) },
    { kDtRefrigerator, 0, &refrigeratorEndpoint,
      Span<const EmberAfDeviceType>(kRefrigeratorTypes) },
    /* The registry's first shape-bearing rows. ep_type and ep_type_v1 stay
     * null on both: a shape map REPLACES that selection rather than
     * refining it, and leaving them null keeps the two mechanisms from
     * looking interchangeable at a glance. */
    { kDtTempControlledCabinet, 1, nullptr, Span<const EmberAfDeviceType>(kCabinetTypes), nullptr,
      Span<const EmberAfDeviceType>(), Span<const hearth_shape>(kCabinetShapes) },
    { kDtCookSurface, 1, nullptr, Span<const EmberAfDeviceType>(kCookSurfaceTypes), nullptr,
      Span<const EmberAfDeviceType>(), Span<const hearth_shape>(kCookSurfaceShapes) },
    { 0x0079, 0, &microwaveOvenEndpoint, Span<const EmberAfDeviceType>(kMicrowaveOvenTypes) },
    /* The EVSE round: the fifty-second and last row, closing the catalogue.
     * Two variants split on SOC reporting, one device-type span for both (the
     * declaration's note), no shape map: its cluster set does not depend on a
     * parent and the policy is permissive for it. */
    { 0x050C, 1, &energyEvseEndpoint, Span<const EmberAfDeviceType>(kEnergyEvseTypes),
      &energyEvseNoSocEndpoint },
};

/*
 * ---- the tie between the two encodings of the parenting policy --------
 *
 * Catalogue batch 8, and the reason parent_policy_ok() and s_registry are
 * both constexpr. This walks the whole registry at compile time and proves,
 * for every row, every variant it accepts and every device type in the
 * catalogue used as a parent (plus the unparented form), that:
 *
 *   a row the policy RESTRICTS carries a shape map, and its domain is
 *   EXACTLY the policy's accept set: one shape for every accepted pair, no
 *   shape for any rejected pair, and never two shapes for one pair;
 *
 *   a row the policy leaves PERMISSIVE carries no shape map, because it
 *   needs none: its cluster set is chosen by variant alone.
 *
 * The parent universe is s_registry itself, so nothing here is a second list
 * that could go stale: adding a device type automatically widens the
 * enumeration, and a new restricted row without a matching shape map, or a
 * shape map with a hole in it, fails THIS build rather than a boot rebuild
 * on someone's bench.
 *
 * Fix round N4, the honest scope of that universe: a shape row naming a
 * parent device type that is NOT in the registry is never enumerated by the
 * second loop and so is not checked against the policy there. It cannot
 * matter, because parent_devtype at create time is either 0 or the device
 * type of a live endpoint, which is by construction a registry id, so such a
 * row would be unreachable rather than wrong. The per-shape loop above still
 * checks its variant and its ep_type. Said plainly rather than left implied
 * by "every shape is accepted".
 *
 * Which direction is load-bearing, said plainly. "Every shape is accepted"
 * catches a dead shape row, which is confusing but harmless. "Every accepted
 * pair has a shape" catches the dangerous one: a pairing AT+MTEP accepts at
 * staging time whose cluster set does not exist, which would be discovered
 * only at the next boot, as a create failure that aborts the whole rebuild
 * and truncates the composition (mt_devtype_create()'s no-shape arm logs it,
 * but by then the device is already serving a prefix).
 */
constexpr bool shape_domain_matches_policy()
{
    constexpr size_t kRows = sizeof(s_registry) / sizeof(s_registry[0]);
    for (size_t r = 0; r < kRows; r++) {
        const hearth_devtype &e = s_registry[r];
        for (size_t s = 0; s < e.shapes.size(); s++) {
            if (e.shapes.data()[s].variant > e.max_variant) {
                return false;
            }
            if (e.shapes.data()[s].ep_type == nullptr) {
                return false;
            }
        }
        /* Fix round M4, the mirror of the check just above. A row with NO
         * shape map selects through the ep_type / ep_type_v1 fallback, and
         * that arm of mt_devtype_create() has no null guard of its own (the
         * shape arm aborts loudly; the fallback would hand a null list
         * straight to type_has_cluster() and fault at boot). Asserting it
         * here turns that into a build failure. ep_type_v1 needs no check:
         * null is its documented "this row has one variant" value and the
         * fallback already tests it. */
        if (e.shapes.empty() && e.ep_type == nullptr) {
            return false;
        }
        for (uint8_t v = 0; v <= e.max_variant; v++) {
            /* p == kRows is the unparented probe; the rest walk the
             * catalogue's own ids. */
            for (size_t p = 0; p <= kRows; p++) {
                const uint32_t parent = (p == kRows) ? 0u : s_registry[p].id;
                size_t matches = 0;
                for (size_t s = 0; s < e.shapes.size(); s++) {
                    if (e.shapes.data()[s].variant == v &&
                        e.shapes.data()[s].parent_devtype == parent) {
                        matches++;
                    }
                }
                if (matches > 1) {
                    return false;
                }
                const bool accepted = parent_policy_ok(e.id, v, parent);
                if (e.shapes.empty()) {
                    /* No shape map: the policy must be permissive for this
                     * row, or a legal pairing would have no cluster set. */
                    if (!accepted) {
                        return false;
                    }
                } else if (accepted != (matches == 1)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(shape_domain_matches_policy(),
              "the shape maps and mt_devtype_parent_ok() disagree: a restricted device type "
              "has no shape map, a shape map has a hole or a duplicate, a permissive row "
              "grew a shape map, or a shapeless row has no ep_type to fall back to");

/* ---- the external attribute store ------------------------------------ */

/* One attribute-value slot per served attribute. Every dynamic attribute
 * declared above is 4 bytes or fewer (BITMAP32 is the widest), so 4-byte
 * slots hold all of them; attr_gets_slot() below refuses anything larger
 * rather than overrunning. sizeof is 16: two 4-byte ids, a size byte and a
 * 4-byte payload, padded up to the ids' alignment. */
struct attr_slot {
    ClusterId cluster;
    AttributeId attr;
    uint8_t size;
    uint8_t data[4];
};

constexpr size_t kSlotDataBytes = sizeof(attr_slot::data);

/*
 * ---- the endpoint block heap ----------------------------------------
 *
 * What changed and why. Until this round every dynamic endpoint carried a
 * FLAT arena: dyn_endpoint held DataVersion dv[kMaxClusters] and
 * attr_slot slots[kMaxSlots] sized for the WIDEST device type in the
 * catalogue, and the table was MT_COMP_MAX_ENDPOINTS (28) deep. That cost
 * 28 x 648 = 18,144 bytes of .bss whether or not a single endpoint was ever
 * created, and an on/off light (11 slots) paid the extended colour light's
 * 36-slot bill. Catalogue batch 2 pushed the total to 91.7% of RAM and made
 * the waste the obvious thing to reclaim.
 *
 * Now: a slim static header table (below) plus exactly ONE heap block per
 * created endpoint, holding that endpoint's DataVersion array, its
 * attribute slots and (store reclaim round, below) any host-fed store its
 * device type carries, sized for ITS device type:
 *
 *     +----------------------------+  <- dyn_endpoint::block
 *     | DataVersion dv[n_clusters] |     4 * clusterCount bytes
 *     +----------------------------+
 *     | attr_slot slots[n_slots]   |     16 * count_slots() bytes
 *     +----------------------------+
 *     | host-fed store(s), only    |     store_bytes(): mt_mode_store_t
 *     | for types that carry one   |     (436 B) on a mode select
 *     +----------------------------+     endpoint, mt_chime_store_t
 *                                        (273 B) on a chime endpoint,
 *                                        nothing on every other type
 *
 * The trailing region is the STORE RECLAIM round (T398). Catalogue batch 4
 * put the AT+MTMODES and AT+MTCHIMESOUNDS stores in .bss as 16-deep pools,
 * 7,040 B and 4,448 B that every composition paid whether or not it
 * contained a single mode select or chime endpoint (batch 4 concern C1, at
 * 92.6% RAM). They are per-created-endpoint state, which is exactly what
 * this heap is for, so each moved into its endpoint's own block: only
 * compositions that declare those types pay, priced into the sizing table
 * below. The shapes and the accessors are in mt_dyn_store.h; every other
 * type's block is unchanged, byte for byte.
 *
 * dv first is what keeps the layout alignment-free. k_heap_alloc() routes
 * through sys_heap_noalign_alloc() (kheap.c:119-129), and a chunk's memory
 * starts one chunk header past an 8-byte-aligned chunk boundary
 * (chunk_mem(), heap.c:24-27), so on this build a block is 4-byte aligned,
 * NOT 8. That is exactly what every region needs and no more: DataVersion
 * is uint32_t and attr_slot's leading members are uint32_t, so both want
 * 4-byte alignment, and an integral number of uint32_t of dv can never
 * leave the slots misaligned. The trailing region starts at
 * 4 * clusterCount + 16 * n_slots, a multiple of 4; store_walk() places
 * each store within it at an offset rounded up to that store's own
 * alignof (fix round 2), and neither store type may want more than the 4
 * the region start guarantees (asserted beside kStoreWalk, so a store
 * gaining a wider member cannot silently misalign). block_dv(),
 * block_slots() and store_walk() are the only places that know this
 * layout.
 *
 * A DEDICATED heap, not the system heap. Three reasons, all of which matter
 * more than the handful of bytes k_heap's own metadata costs:
 *
 *   - Failure is CONTAINED. An oversized composition cannot starve the CHIP
 *     stack, mbedTLS or OpenThread, all of which draw on other pools; it
 *     fails here, at the endpoint that does not fit, and nowhere else.
 *   - Failure is MEASURABLE. The cap is one number in one place, so the
 *     LOG_ERR in mt_devtype_create() can name exactly what was asked for
 *     and what was left, rather than reporting a generic allocation
 *     failure whose real cause is somewhere else entirely.
 *   - The budget is auditable. "8 KB of endpoint blocks" is a line item in
 *     the README's capacity table; "some of the system heap" is not.
 *
 * ALLOCATE-ONLY, and that is what makes fragmentation a non-question rather
 * than a risk to be managed. Blocks are allocated in exactly one place,
 * mt_devtype_create(), which is reached from exactly one caller,
 * main.cpp's rebuild_composition(), which runs once at boot before the
 * Matter server starts serving. Nothing frees. There is no AT command that
 * destroys an endpoint: the composition is edited by AT+MTEP and applied by
 * a reboot, and a reboot resets this heap wholesale. So the allocation
 * sequence over the device's life is a single monotonic run of allocations
 * in composition order, which is the one access pattern a first-fit
 * allocator cannot fragment.
 *
 * The one deliberate leak: if emberAfSetDynamicEndpoint() fails AFTER a
 * successful allocation, the block is not returned. That path returns -1
 * and the rebuild stops there, so no further endpoint is created and the
 * block can never be reached again before the reboot that resets the heap.
 * Bounded at one block per boot.
 *
 * Freeing it would be actively worse, not merely pointless. CHIP keeps the
 * dataVersions span it was handed in emAfEndpoints[index]
 * (attribute-storage.cpp), and its own failure path does not always clear
 * that slot: with CHIP_CONFIG_USE_ENDPOINT_UNIQUE_ID the later error returns
 * inside emberAfSetDynamicEndpoint() leave the entry populated. Handing the
 * memory back to the heap would turn that into a dangling pointer for the
 * next allocation to reuse. Not freeing is strictly safer AND keeps the
 * allocate-only invariant that makes this heap's behaviour easy to reason
 * about.
 *
 * ---- sizing -----------------------------------------------------------
 *
 * Block payload is 4 * clusterCount + 16 * slots, plus the type-conditional
 * trailing store(s) for the three types that carry any: mode select's
 * mt_mode_store_t is 436 B (a count byte, 8 x 34 B mode/label entries,
 * padding to the structs' 4-byte alignment, 8 x 20 B ModeOptionStruct)
 * and chime's mt_chime_store_t is 273 B (a count byte, 8 x 34 B id/name
 * entries), both from the store reclaim round; the RVC carries TWO
 * 306 B mt_mb_store_t (catalogue batch 5, one per ModeBase cluster,
 * a count byte plus 8 x 38 B mode/tag/label entries). All three sizeofs
 * are asserted beside store_bytes() so these rows cannot go stale
 * silently. Zephyr's sys_heap
 * adds a 4-byte chunk header for a heap this size (heap.h
 * chunk_header_bytes(), small heap because 8192/8 = 1024 chunks is far
 * below the 0x7fff big-heap threshold) and rounds the total up to
 * CHUNK_UNIT (8 bytes), so the true cost per endpoint is
 * roundup(payload + 4, 8):
 *
 *   device type                        clusters  slots  payload  heap cost
 *   robotic vacuum cleaner  0x0074            5     14      856        864
 *   battery storage v0      0x0018            7     32      846        856
 *   water heater v0         0x050F            7     29      798        808
 *   cabinet heater v1       0x0071            4     10      755        760
 *   cabinet cooler v1       0x0071            3      6      687        696
 *   water heater v1         0x050F            4     19      626        632
 *   extended colour light   0x010D            5     36      596        600
 *   mode select             0x0027            3      8      576        584
 *   colour temperature lt   0x010C            5     32      532        536
 *   cabinet heater v0       0x0071            4     13      530        536
 *   microwave oven          0x0079            4     13      530        536
 *   refrigerator            0x0070            4     12      514        520
 *   cabinet cooler v0       0x0071            3      9      462        472
 *   device energy mgmt v0   0x050D            3      9      462        472
 *   device energy mgmt v1   0x050D            3      8      446        456
 *   battery storage v1      0x0018            5     23      388        392
 *   cook surface v1         0x0077            3      6      381        392
 *   chime                   0x0146            2      4      345        352
 *   dimmable light/plug/mnt  0x0101 0x010B 0x0110  4  20      336        344
 *   cabinet unparented v1   0x0071            2      3      329        336
 *   pump / room air cond    0x0303 0x0072     4     18      304        312
 *   thermostat              0x0301            3     15      252        256
 *   heat pump / solar v0    0x0309 0x0017     5     13      228        232
 *   smoke/co alarm          0x0076            3     13      220        224
 *   window covering         0x0202            3     13      220        224
 *   door lock               0x000A            3     12      204        208
 *   solar power v1          0x0017            4     11      192        200
 *   on/off light/plug/mount  0x0100 0x010A 0x010F  3  11      188        192
 *   water valve             0x0042            3     11      188        192
 *   fan / air purifier      0x002B 0x002D     3     10      172        176
 *   cook surface v0         0x0077            3      9      156        160
 *   temp/humidity/pressure/light/flow         3      9      156        160
 *   occupancy sensor        0x0107            3      9      156        160
 *   electrical sensor v0    0x0510            4      8      144        152
 *   washer/dish/dryer 0x0073 0x0075 0x007C    3      8      140        144
 *   generic switch          0x000F            3      8      140        144
 *   power source            0x0011            2      8      136        144
 *   electrical utility mtr  0x0511            3      7      124        128
 *   boolean-state sensors, air quality        3      7      124        128
 *   electrical sensor v1    0x0510            3      6      108        112
 *   cabinet unparented v0   0x0071            2      6      104        112
 *   extractor hood          0x007A            2      6      104        112
 *   electrical meter v0     0x0514            3      6      108        112
 *   oven                    0x007B            2      4       72         80
 *   electrical meter v1     0x0514            2      4       72         80
 *   cooktop                 0x0078            2      3       56         64
 *
 * (Mode select payload is 140 + 436 store, chime 72 + 273 store; both rows
 * sat at bare 140/72 before the reclaim round moved their stores in. The
 * RVC's payload is 244 + two 306 B ModeBase stores, catalogue batch 5:
 * the widest type since the reclaim round made stores block-resident,
 * past the extended colour light's 600. The water heater rows are
 * 492/320 + one 306 B ModeBase store and the battery storage v0 row is
 * 540 + one, catalogue batch 7b: the first store-bearing rows to slot
 * ABOVE the extended colour light without taking the RVC's title, the
 * battery ten payload bytes shy of it.)
 *
 * HEARTH_EP_HEAP_BYTES is 8192, which leaves 8,112 usable after the
 * heap's own header and bucket table. Against kServiceableEndpoints = 16:
 *
 *   16 x anything from the dimmable light down  <= 16 x 344 = 5,504   fits
 *   16 x chime                              16 x 352 = 5,632    fits
 *   16 x colour temperature light           15 fit (8,040)    ONE SHORT
 *   16 x mode select                        13 fit (7,592)    THREE SHORT
 *   16 x extended colour light              13 fit (7,800)    THREE SHORT
 *   16 x robotic vacuum cleaner              9 fit (7,776)    SEVEN SHORT
 *   a realistic mixed composition, say
 *     2 extended colour + 2 dimmable +
 *     12 assorted sensors               1,200+688+1,920 = 3,808   fits easily
 *
 * That is the deliberate trade the round was asked for: sizing for 16 of
 * the heaviest type would want 9,600 bytes and buy a composition nobody
 * builds, when the same RAM is worth more elsewhere. The mode select line
 * is the store reclaim round's deliberate, user-accepted consequence of
 * the same trade: an all-mode-select composition that fit 16 when the
 * store was a .bss pool now serves the 13-endpoint prefix under the
 * stop-at-failure semantics above, and bought the whole build 11.5 KB of
 * .bss back. The RVC line (catalogue batch 5) is the same trade at its
 * widest: two block-resident ModeBase stores make it the catalogue's
 * heaviest type at 864 B, an all-RVC composition serves a 9-endpoint
 * prefix, and DE404 ruled RvcCleanMode STAYS composed (dropping the
 * optional cluster would have bought 16 endpoints at the price of a real
 * cross-platform data-model divergence). A composition that does ask for
 * more gets the loud, specific
 * failure in mt_devtype_create() rather than a silent truncation, and the
 * README's "Endpoint capacity" section tells an integrator the numbers up
 * front.
 *
 * Catalogue batch 7a added a THIRD capacity bound beyond the table and
 * this heap: the per-family delegate pools at the C6's own depths (DE407).
 * Every EPM/PowerTopology-bearing endpoint (electrical sensor, electrical
 * meter, heat pump, solar power; batch 7b adds the variant-0 water heater
 * and battery storage) draws on the MT_MEAS_MAX (8) pools, DEM endpoints
 * on MT_DEM_MAX (4), utility meters on MT_METER_MAX (2), water heaters on
 * MT_WHM_MAX (4), so an all-energy composition hits a pool wall before
 * either older wall; the exhaustion aborts the create with the pool
 * named, the same stop-at-failure prefix semantics as ever. None of the
 * six batch 7a energy types approaches the RVC's 864 B block; batch 7b's
 * two come closer (808 and 856 B) but still lose, so the floor assertion
 * below is untouched and both are compile-time candidates rather than
 * bystanders.
 *
 * The floor is compiler-checked below, so the table above cannot go stale
 * without the build noticing.
 */
/* Stepped out of the anonymous namespace deliberately: K_HEAP_DEFINE emits
 * a STRUCT_SECTION_ITERABLE that Zephyr's own static-heap init walks at
 * boot, and a section-placed object with external linkage is the shape that
 * machinery expects. */
} /* namespace */

K_HEAP_DEFINE(hearth_ep_heap, HEARTH_EP_HEAP_BYTES);

namespace {

/* Chunk-rounded bytes handed out so far, kHeapCostOf(want) per block: the
 * heap's REAL occupancy, not the raw payload sum. Fix round M1: the
 * payload sum shared every capacity log with the chunk-rounded free_bytes
 * from sys_heap_runtime_stats_get(), and the two could not be reconciled
 * on the same line (at the fourteenth mode select the old text read
 * 7,488 handed out against 520 free, summing to nothing printable). Now
 * handed-out plus free equals kHeapUsableBytes exactly (13 x 584 = 7,592
 * plus 520 = 8,112 on the bench's 13-prefix leg), and every log printing
 * this names the usable figure, not the gross define, as its
 * denominator. */
size_t s_ep_heap_used;

/*
 * Which attributes get a slot. The single predicate seed_slots() and
 * count_slots() both consult, so the size counted at allocation time and
 * the number actually written can never disagree: a mismatch would either
 * overrun the block or leave an attribute unserved.
 *
 * ARRAY-typed attributes are the list globals (AttributeList,
 * AcceptedCommandList, GeneratedCommandList), which CHIP's own machinery
 * answers and which would not fit a 4-byte slot anyway. Anything wider than
 * the slot payload is refused rather than truncated; seed_slots() logs it,
 * this predicate stays silent so the two loops agree exactly.
 */
bool attr_gets_slot(const EmberAfAttributeMetadata &md)
{
    /* STRUCT joined ARRAY in catalogue batch 4: OperationalState's
     * OperationalError is a struct served entirely by the cluster's own
     * AttributeAccessInterface, declared only so AttributeList is
     * truthful, and a 4-byte slot could not hold a meaningful
     * ErrorStateStruct whatever its declared ember size. */
    return md.attributeType != ZAP_TYPE(ARRAY) && md.attributeType != ZAP_TYPE(STRUCT) &&
           md.size <= kSlotDataBytes;
}

/*
 * The DE407 quiet table (catalogue batch 7a): the specific (cluster,
 * attribute) pairs whose over-wide scalar declarations are DELIBERATE
 * metadata-only 64-bit declarations, each proven Instance-served against
 * the pinned tree at its declaration's audit note, admitted one pair at a
 * time and NEVER by type: the ruling is explicit that a future
 * ember-served 64-bit attribute must keep failing loudly (declared but
 * slotless, it would answer a bare ERROR over AT, the silent failure the
 * LOG_ERR in seed_slots() exists to catch at boot instead of on a bench).
 * The declarations themselves are refused a slot by attr_gets_slot()'s
 * pre-existing size clause; this table only silences the boot shout for
 * the proven pairs. Batch 7b's WaterHeaterManagement and EnergyEvse
 * 64-bit attributes add their own rows here when their device types land.
 */
struct quiet_no_slot_attr {
    ClusterId cluster;
    AttributeId attr;
};

constexpr quiet_no_slot_attr kQuietNoSlot[] = {
    /* ElectricalPowerMeasurement: the seven AT+MTMEAS push fields, served
     * by Instance::Read()'s own cases from the HearthEpmDelegate cache
     * (electrical-power-measurement-server.cpp:90-217; the
     * k_instance_served carve-out serves the AT side). */
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Voltage::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActivePower::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::RMSVoltage::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::RMSCurrent::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Frequency::Id },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::PowerFactor::Id },
    /* DeviceEnergyManagement: the two power_mw scalars, served by
     * Instance::Read()'s own cases from the HearthDemDelegate cache
     * (device-energy-management-server.cpp:70-73; the carve-out serves
     * the AT side). */
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::AbsMinPower::Id },
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::AbsMaxPower::Id },
    /* WaterHeaterManagement (batch 7b): the one energy_mwh scalar, served
     * by Instance::Read()'s own case from the HearthWhmDelegate cache
     * (water-heater-management-server.cpp:128-133; the carve-out serves
     * the AT side). Declared 8 B in whmAttrs, variant 0 only. */
    { WaterHeaterManagement::Id, WaterHeaterManagement::Attributes::EstimatedHeatRequired::Id },
    /* EnergyEvse (the EVSE round): the six 8-byte scalars, served by
     * Instance::Read()'s own cases from the HearthEvseDelegate cache
     * (energy-evse-server.cpp:73-127; the k_instance_served carve-out serves
     * the AT side). Three amperage_ma currents that are declared on both
     * variants, NextChargeRequiredEnergy which rides unconditional PREF,
     * SessionEnergyCharged which is mandatory, and BatteryCapacity which is
     * variant 0 only; a variant-1 endpoint never declares that last one, so
     * its row is simply never consulted there. Admitted one pair at a time
     * per the DE407 ruling, never by type. */
    { EnergyEvse::Id, EnergyEvse::Attributes::CircuitCapacity::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::MinimumChargeCurrent::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::MaximumChargeCurrent::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeRequiredEnergy::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::BatteryCapacity::Id },
    { EnergyEvse::Id, EnergyEvse::Attributes::SessionEnergyCharged::Id },
};

bool attr_quiet_no_slot(ClusterId cluster, AttributeId attr)
{
    for (auto &q : kQuietNoSlot) {
        if (q.cluster == cluster && q.attr == attr) {
            return true;
        }
    }
    return false;
}

/* Descriptor is served by CHIP's own DescriptorCluster server object,
 * registered per endpoint by the cluster init callback that
 * emberAfSetDynamicEndpoint() fires; its reads never reach the
 * external-storage callbacks, so it gets no slots and no block space. */
bool cluster_gets_slots(const EmberAfCluster &cl)
{
    return cl.clusterId != Descriptor::Id;
}

/* Does this device type carry a given cluster? Used by mt_devtype_create()
 * for the per-endpoint server wiring that only some device types need (the
 * valve's delegate today), so the question is asked of the declared cluster
 * list rather than of a second table of device type ids that could drift
 * from it. */
bool type_has_cluster(const EmberAfEndpointType *t, ClusterId id)
{
    for (uint8_t c = 0; c < t->clusterCount; c++) {
        if (t->cluster[c].clusterId == id) {
            return true;
        }
    }
    return false;
}

/* How many slots this device type needs. Walks the same two loops
 * seed_slots() walks, through the same two predicates. */
uint16_t count_slots(const EmberAfEndpointType *t)
{
    uint16_t n = 0;
    for (uint8_t c = 0; c < t->clusterCount; c++) {
        const EmberAfCluster &cl = t->cluster[c];
        if (!cluster_gets_slots(cl)) {
            continue;
        }
        for (uint16_t a = 0; a < cl.attributeCount; a++) {
            if (attr_gets_slot(cl.attributes[a])) {
                n++;
            }
        }
    }
    return n;
}

constexpr size_t block_bytes(size_t n_clusters, size_t n_slots)
{
    return sizeof(DataVersion) * n_clusters + sizeof(attr_slot) * n_slots;
}

/*
 * The type-conditional trailing stores (store reclaim round): the host-fed
 * stores mt_dyn_store.h defines, laid out after the block's dv and slot
 * regions. Membership is asked of the declared cluster list through
 * type_has_cluster(), the same no-second-table-to-drift rule that
 * predicate exists for.
 *
 * Fix round 2 (re-review of M2): the layout is alignment-correct BY
 * CONSTRUCTION, not by ordering discipline. Round 1 pinned the
 * mode-before-chime order with a static_assert that the re-review proved
 * vacuous (alignof(mt_chime_store_t) is 1, so the modulo held for ANY
 * mode-store size, a compiled misaligned counterexample included), and an
 * order that only comments kept safe is exactly the hazard Minor 2 named.
 * Now store_walk(), the one function both store_bytes() and
 * store_offset() are, rounds the running offset up to each store's own
 * alignof before placing it, so the walk order in kStoreWalk is TASTE:
 * flip it, or grow a store to an odd size, and every store still lands
 * aligned, only the padding moves. Sizing and placement are the same walk
 * with a different stop condition, so they can never disagree.
 *
 * With the current types every align_up is a no-op (the mode store leads
 * from the 4-aligned region start and the chime store accepts any
 * offset), so this round changes no layout byte: every store offset, the
 * 436 and 273 sizes and the 584/352 cost rows are unchanged.
 *
 * What stays load-bearing is the REGION START: it is only ever 4-aligned
 * (the layout note beside K_HEAP_DEFINE), so align_up can deliver at most
 * 4-byte alignment in memory whatever it computes. The two
 * alignof-versus-DataVersion asserts below carry that real constraint,
 * unchanged.
 */
/*
 * Catalogue batch 8 widened the key. Until this batch a store's presence was
 * a question about a CLUSTER: an endpoint carries ModeSelect, therefore it
 * carries a mode store. TemperatureControl breaks that, and it is the first
 * store in the file that has to be finer-grained than a cluster id: BOTH
 * variants of a cabinet and BOTH variants of a cook surface carry
 * TemperatureControl, and only the TemperatureLevel variant carries a label
 * store. Keying on the cluster alone would charge every TemperatureNumber
 * endpoint 273 bytes it never reads, and worse, would make store_offset()
 * answer a live offset for a store nothing constructed.
 *
 * The discriminator is the presence of a declared ATTRIBUTE, and
 * SupportedTemperatureLevels is exactly the right one: it is the attribute
 * the store exists to serve, it appears in the TemperatureLevel list and in
 * no other, and it is metadata-only (an ARRAY, refused a slot), so asking
 * about it costs a walk of the declared list rather than a second table
 * anyone could forget to update. kInvalidAttributeId, spelled out on the
 * five older rows rather than left to a zero-fill, means "cluster presence
 * alone": attribute id 0 is a real id on most clusters and would have made a
 * silent sentinel dangerous.
 */
struct store_desc {
    ClusterId cluster;
    AttributeId attr;
    size_t size;
    size_t align;
};

/* Does this device type declare `attr` on `cluster`? Only asked by the store
 * walk, and only of metadata-only attributes so far, so it walks the
 * declared list rather than consulting the block. */
bool type_has_attr(const EmberAfEndpointType *t, ClusterId cluster, AttributeId attr)
{
    for (uint8_t c = 0; c < t->clusterCount; c++) {
        if (t->cluster[c].clusterId != cluster) {
            continue;
        }
        for (uint16_t a = 0; a < t->cluster[c].attributeCount; a++) {
            if (t->cluster[c].attributes[a].attributeId == attr) {
                return true;
            }
        }
        return false;
    }
    return false;
}

constexpr store_desc kStoreWalk[] = {
    { ModeSelect::Id, kInvalidAttributeId, sizeof(mt_mode_store_t), alignof(mt_mode_store_t) },
    { Chime::Id, kInvalidAttributeId, sizeof(mt_chime_store_t), alignof(mt_chime_store_t) },
    /* Catalogue batch 5: the RVC's two host-fed ModeBase lists. Both rows
     * are PRESENT on one type (0x0074 carries RvcRunMode and RvcCleanMode
     * at once), the first time the walk places two stores in one block;
     * store_walk() already iterates the whole table and align_up() makes
     * the order taste, so the mechanism extends without change. */
    { RvcRunMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    { RvcCleanMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 7a: the DEM device type's mode list, the third
     * ModeBase store row; the walk mechanism is unchanged (the batch 5
     * note above), only membership grows. */
    { DeviceEnergyManagementMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t),
      alignof(mt_mb_store_t) },
    /* Catalogue batch 7b: the water heater's mode list, the fourth. Same
     * mechanism, same 306 B shape. */
    { WaterHeaterMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the refrigerator's mode list, the fifth. */
    { RefrigeratorAndTemperatureControlledCabinetMode::Id, kInvalidAttributeId,
      sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the heater cabinet's mode list, the sixth. */
    { OvenMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the microwave's mode list, the seventh. */
    { MicrowaveOvenMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* The EVSE round: the eighth and last ModeBase store row. The EVSE is the
     * SECOND type after the RVC to carry two stores in one block, and unlike
     * the RVC's pair these come from two different rows of this table
     * (EnergyEvseMode here, DeviceEnergyManagementMode above), which the walk
     * has handled since batch 5 without change. */
    { EnergyEvseMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the temperature-level label store, and the ONLY row
     * with an attribute discriminator (the struct comment). Present on a
     * variant-1 cabinet or cook surface, absent on their variant-0 forms,
     * which carry the same cluster id. */
    { TemperatureControl::Id, TemperatureControl::Attributes::SupportedTemperatureLevels::Id,
      sizeof(mt_temp_levels_store_t), alignof(mt_temp_levels_store_t) },
};

constexpr size_t align_up(size_t off, size_t align) { return ((off + align - 1) / align) * align; }

/* stop_at = a store's cluster id answers that store's aligned offset
 * within the region (the caller has established the type carries it);
 * stop_at = kInvalidClusterId (matching no store) walks to the end and
 * answers the region's total size. */
size_t store_walk(const EmberAfEndpointType *t, ClusterId stop_at)
{
    size_t off = 0;
    for (auto &s : kStoreWalk) {
        if (!type_has_cluster(t, s.cluster)) {
            continue;
        }
        /* Catalogue batch 8: the finer key. A row naming an attribute is
         * present only when the declared list carries that attribute too;
         * kInvalidAttributeId is every older row's "cluster presence
         * alone". */
        if (s.attr != kInvalidAttributeId && !type_has_attr(t, s.cluster, s.attr)) {
            continue;
        }
        off = align_up(off, s.align);
        if (s.cluster == stop_at) {
            return off;
        }
        off += s.size;
    }
    return off;
}

size_t store_bytes(const EmberAfEndpointType *t)
{
    return store_walk(t, kInvalidClusterId);
}

size_t store_offset(const EmberAfEndpointType *t, ClusterId which)
{
    return store_walk(t, which);
}

/* The sizing table's two store figures, pinned so the table cannot go
 * stale without the build noticing (the same discipline as the widest-type
 * floor below); recompute the mode select and chime rows if either
 * fires. */
static_assert(sizeof(mt_mode_store_t) == 436,
              "mt_mode_store_t changed size; redo the sizing table's mode select row");
static_assert(sizeof(mt_chime_store_t) == 273,
              "mt_chime_store_t changed size; redo the sizing table's chime row");
static_assert(sizeof(mt_mb_store_t) == 306,
              "mt_mb_store_t changed size; redo the sizing table's rvc row");
static_assert(sizeof(mt_temp_levels_store_t) == 273,
              "mt_temp_levels_store_t changed size; redo the sizing table's cabinet and cook "
              "surface variant-1 rows");

/* The trailing store begins at 4 * clusterCount + 16 * n_slots, a multiple
 * of 4, and a k_heap block is 4-byte aligned (the layout note beside
 * K_HEAP_DEFINE), so nothing wanting more than DataVersion's alignment may
 * ever live there. */
static_assert(alignof(mt_mode_store_t) <= alignof(DataVersion),
              "mt_mode_store_t must not out-align the block's store offset");
static_assert(alignof(mt_chime_store_t) <= alignof(DataVersion),
              "mt_chime_store_t must not out-align the block's store offset");
static_assert(alignof(mt_mb_store_t) <= alignof(DataVersion),
              "mt_mb_store_t must not out-align the block's store offset");
static_assert(alignof(mt_temp_levels_store_t) <= alignof(DataVersion),
              "mt_temp_levels_store_t must not out-align the block's store offset");

/*
 * The header table. Four fields plus the two counts the block walk needs;
 * everything else about an endpoint lives in its heap block, which `block`
 * points at. 16 of these is 256 bytes of .bss, against the 18,144 the flat
 * arena cost.
 *
 * slot_capacity is what count_slots() said when the block was sized, and
 * slot_count is how many seed_slots() actually wrote. They agree in every
 * normal case; keeping both lets seed_slots() bound its writes by the
 * allocation rather than by a shared constant that could drift from it.
 */
struct dyn_endpoint {
    bool used;
    EndpointId ep_id;
    const hearth_devtype *type;
    /* Catalogue batch 7a: the cluster list this endpoint was actually
     * created with. Until the variant-carrying rows arrived this was
     * always type->ep_type; now it is the variant's own list
     * (type->ep_type or type->ep_type_v1), chosen once in
     * mt_devtype_create(), and every block-layout walk (block_slots, the
     * store accessors, seed_slots) reads it from here so the layout can
     * never be computed against the wrong variant's list. The variant is
     * kept beside it for the one seed that depends on it (the DEM
     * FeatureMap special case in seed_slots(), batch 7a's DEM step). */
    const EmberAfEndpointType *ep_type;
    uint8_t variant;
    void *block;
    uint16_t slot_capacity;
    uint16_t slot_count;
};

/* How many header slots are currently taken. Only ever used to make a
 * failure log name both walls rather than one; the header-table check in
 * mt_devtype_create() does its own scan because it needs the free index,
 * not the count. */
uint16_t live_endpoints();

/* The two accessors that know the block layout. Both assume d.block is
 * non-null, which every caller guarantees by checking d.used first: a
 * header is only marked used after a successful allocation. */
DataVersion *block_dv(const dyn_endpoint &d)
{
    return static_cast<DataVersion *>(d.block);
}

attr_slot *block_slots(const dyn_endpoint &d)
{
    return reinterpret_cast<attr_slot *>(static_cast<uint8_t *>(d.block) +
                                         sizeof(DataVersion) * d.ep_type->clusterCount);
}

/*
 * ---- the compiler-checked floor under the heap sizing ------------------
 *
 * count_slots() runs at create time, so no device type's size is a
 * compile-time constant any more, and the old kMaxSlots / kMaxClusters
 * ceilings no longer bound any array: dv and slots are sized exactly. They
 * are gone rather than kept as decoration.
 *
 * What DOES still need guarding is the heap sizing story.
 * HEARTH_EP_HEAP_BYTES was chosen against the table above, and that table
 * holds only while the catalogue's widest device type stays roughly the
 * size it is now. Add a device type with 80 slots and the "13 extended
 * colour lights fit" line goes quietly wrong: no build error, no runtime
 * error, until a real composition hits the wall on someone's bench.
 *
 * So the widest type is still computed at compile time, from the same
 * DECLARE_DYNAMIC_* arrays the registry is built from, and asserted against
 * a floor: the heap must always hold at least kMinWidestEndpoints of the
 * heaviest device type.
 *
 * WHAT THIS DOES NOT PROTECT, said once and plainly, because everything below
 * reads more confidently than the mechanism deserves: the candidate set is
 * HAND-MAINTAINED. Every constant in the kMax2 chain below is one someone
 * remembered to write, and nothing ties the chain to s_registry. A
 * fifty-third device type added with no candidate of its own is simply not
 * guarded, exactly as it was before this block was made cap-aware. What the
 * assertions do guarantee is that a candidate which IS declared cannot be
 * quietly weakened: its block is derived from the declarations, its payload
 * and heap cost are pinned, and a capped one cannot lose its cap without the
 * build saying so. Deriving the candidate set from s_registry the way
 * shape_domain_matches_policy() derives the parent universe would close the
 * gap and is the right shape for whoever needs it next; count_slots() is not
 * constexpr, which is what has stopped it so far.
 *
 * Eight rather than sixteen because sizing for sixteen of the heaviest is
 * precisely the trade this round declined; eight is the point below which the
 * capacity table would be describing a different device.
 *
 * THE FLOOR IS CAP-AWARE SINCE THE EVSE ROUND, and the change is worth
 * understanding before anyone weakens it, because a floor nobody understands
 * is the one that gets weakened.
 *
 * The assertion used to be a single inequality over a single kWidestBlockBytes:
 * room for eight of whatever the widest candidate happened to be. That was
 * exactly right while every candidate was a device type a host could declare
 * sixteen of. It stops being right the moment a device type carries its OWN
 * capacity cap, because then "eight of it must fit" is a demand no
 * composition can ever make: the Energy EVSE is capped at MT_EVSE_MAX (2) by
 * its delegate's charging-target store, so a host cannot ask for a third, let
 * alone an eighth, and an assertion demanding room for eight would be
 * demanding 9,152 bytes of an 8,112-byte heap to serve a composition that
 * cannot exist.
 *
 * What the floor MEANS, stated so the arithmetic can be checked against it:
 * for every candidate, the heap must hold as many blocks of that candidate as
 * a composition could actually contain, up to the eight the capacity table is
 * written around. So each candidate's demand is
 *
 *     min(that candidate's own cap, kMinWidestEndpoints)
 *
 * and an UNCAPPED candidate's cap is kServiceableEndpoints (16), which is
 * above eight, so its demand is eight, unchanged. Every guarantee the old
 * single assertion made is therefore still made, byte for byte: the change
 * only stops the floor asking for compositions the port refuses to build.
 *
 * Two things keep it from going vacuous:
 *
 *   the capped candidates are asserted SEPARATELY, each against its own cap,
 *   rather than dropped out of the maximum. Dropping one out of the kMax2
 *   chain and asserting nothing in its place is the obvious way to make a
 *   failing floor pass, and it is the one this shape refuses;
 *
 *   a capped candidate must be genuinely capped BELOW kMinWidestEndpoints,
 *   asserted below. Raise MT_EVSE_MAX to eight or more and the build fails
 *   telling you to move that candidate back into the uncapped maximum, where
 *   it will then have to fit eight times over or force a real
 *   HEARTH_EP_HEAP_BYTES decision.
 *
 * One thing that assertion is NOT, since the shape invites the reading:
 * kEvseEndpointCap IS MT_EVSE_MAX rather than a copy of it, so there is no
 * drift between the floor's idea of the cap and the create path's for it to
 * catch. What makes the cap a promise rather than a wish is
 * mt_matter_evse_reserve() refusing the third endpoint before anything is
 * built; this floor depends on that promise and does not verify it.
 */
#define MT_COUNT(array) (sizeof(array) / sizeof((array)[0]))

constexpr size_t kMax2(size_t a, size_t b) { return a > b ? a : b; }

/* Identify rides on every device type; Descriptor contributes nothing. */
constexpr size_t kIdentifySlots = MT_COUNT(identifyAttrs);
constexpr size_t kSensorSlots =
    kMax2(kMax2(kMax2(MT_COUNT(tempAttrs), MT_COUNT(booleanStateAttrs)),
                kMax2(MT_COUNT(occupancyAttrs), MT_COUNT(humidityAttrs))),
          kMax2(kMax2(MT_COUNT(pressureAttrs), MT_COUNT(illuminanceAttrs)),
                kMax2(MT_COUNT(flowAttrs), MT_COUNT(airQualityAttrs))));
constexpr size_t kActuatorSlots =
    kMax2(kMax2(kMax2(MT_COUNT(thermostatAttrs), MT_COUNT(fanControlAttrs)),
                kMax2(MT_COUNT(windowCoveringAttrs), MT_COUNT(switchAttrs))),
          kMax2(kMax2(MT_COUNT(doorLockAttrs), MT_COUNT(valveAttrs)),
                kMax2(MT_COUNT(smokeCoAlarmAttrs),
                      kMax2(MT_COUNT(opStateAttrs), MT_COUNT(modeSelectAttrs)))));

/* The widest of the light/plug family. The on/off light and plug (OnOff
 * alone) and the dimmable light and plug (OnOff + LevelControl) are strict
 * prefixes of this, so covering the wider colour light covers them all. */
constexpr size_t kLightSlots = MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) +
    kMax2(MT_COUNT(colorTempAttrs), MT_COUNT(extendedColorAttrs));

/* Catalogue batch 5's two-app-cluster appliances: the pump (racOnOffAttrs
 * + pumpAttrs) and the room air conditioner (racOnOffAttrs +
 * thermostatAttrs), both far under kLightSlots today, computed anyway so
 * the floor keeps deriving from the same declarations the registry is
 * built from. */
constexpr size_t kOnOffApplianceSlots =
    MT_COUNT(racOnOffAttrs) + kMax2(MT_COUNT(pumpAttrs), MT_COUNT(thermostatAttrs));

/* Catalogue batch 4 brought the first device types WITHOUT Identify (the
 * power source and, later in the batch, the chime), so the widest-endpoint
 * computation gained a second family: types that ride without the
 * kIdentifySlots term. MT_COUNT over an attr list counts DECLARED entries
 * plus the LIST_END ClusterRevision, which over-counts a list whose
 * metadata-only members (strings, structs, arrays beyond the LIST_END
 * convention) get no slot; that over-count only ever makes the asserted
 * floor MORE conservative, so it is left uncorrected. */
constexpr size_t kNoIdentifySlots = kMax2(MT_COUNT(powerSourceAttrs), MT_COUNT(chimeAttrs));

constexpr size_t kWidestEndpointSlots =
    kMax2(kIdentifySlots +
              kMax2(kMax2(kSensorSlots, kActuatorSlots), kMax2(kLightSlots, kOnOffApplianceSlots)),
          kNoIdentifySlots);

/* Deliberately partial: every sensor and actuator device type shares the
 * same three-cluster shape, so temperatureSensorClusters stands in for all
 * of them rather than listing seventeen identical counts. */
constexpr size_t kWidestClusterList = kMax2(
    kMax2(kMax2(kMax2(MT_COUNT(onOffLightClusters), MT_COUNT(dimmableLightClusters)),
                kMax2(MT_COUNT(colorTemperatureLightClusters),
                      MT_COUNT(extendedColorLightClusters))),
          kMax2(MT_COUNT(rvcClusters), MT_COUNT(wiredElectricalSensorClusters))),
    kMax2(kMax2(MT_COUNT(onOffPlugInUnitClusters), MT_COUNT(dimmablePlugInUnitClusters)),
          kMax2(kMax2(MT_COUNT(thermostatClusters), MT_COUNT(fanClusters)),
                kMax2(MT_COUNT(windowCoveringClusters), MT_COUNT(temperatureSensorClusters)))));

/*
 * Store reclaim round: the floor must price the trailing stores too, or a
 * grown store could quietly out-size the widest colour light and the
 * assertion below would be guarding the wrong number. Every store-bearing
 * type is computed as its own candidate, each from its OWN cluster
 * list and attribute lists plus its store's sizeof, rather than folding
 * store bytes into the shared cluster/slot maxima (which would charge
 * every candidate for a store only these types carry). The MT_COUNT
 * over-count note above applies here the same way: it only pushes the
 * asserted floor higher, never lower. The chime candidate repeats
 * MT_COUNT(chimeAttrs) rather than reusing kNoIdentifySlots on purpose:
 * that constant is the no-Identify FAMILY maximum (power source included)
 * and this candidate must price the chime's own count.
 */
constexpr size_t kModeSelectBlockBytes =
    block_bytes(MT_COUNT(modeSelectClusters), kIdentifySlots + MT_COUNT(modeSelectAttrs)) +
    sizeof(mt_mode_store_t);
constexpr size_t kChimeBlockBytes =
    block_bytes(MT_COUNT(chimeClusters), MT_COUNT(chimeAttrs)) + sizeof(mt_chime_store_t);

/* Catalogue batch 5: the RVC candidate, the same own-lists-plus-stores
 * shape, with TWO trailing stores (the first type to carry more than
 * one). The MT_COUNT over-count (each list's metadata-only members plus
 * the LIST_END ClusterRevision) again only pushes the asserted floor
 * higher than the real 864 B block, never lower. */
constexpr size_t kRvcBlockBytes =
    block_bytes(MT_COUNT(rvcClusters),
                kIdentifySlots + 2 * MT_COUNT(modeBaseAttrs) + MT_COUNT(opStateAttrs)) +
    2 * sizeof(mt_mb_store_t);

/* Catalogue batch 7a fix round (review B7A-3): the DEM device type is the
 * fourth store-bearing block shape, so it is a candidate like the other
 * three, not a silent bystander: the whole point of deriving the floor
 * from the declarations is that a grown store or attribute list cannot
 * out-size the guarded widest type unnoticed. The variant-0 list is the
 * wider one (demAttrs strictly contains demReportOnlyAttrs); MT_COUNT
 * over-counts the metadata-only members as ever, only pushing the
 * asserted floor higher than the real 462 B payload. It loses to the RVC
 * today; batch 7b's EVSE (two stores, eight clusters) is the candidate
 * expected to take the title and force the heap decision the audit
 * records. */
constexpr size_t kDemBlockBytes =
    block_bytes(MT_COUNT(demClusters), MT_COUNT(demAttrs) + MT_COUNT(modeBaseAttrs)) +
    sizeof(mt_mb_store_t);

/*
 * Catalogue batch 7b: the MT_COUNT over-count convention STOPS at these two
 * candidates, deliberately. Every earlier candidate tolerated MT_COUNT's
 * counting of metadata-only members ("only pushes the asserted floor
 * higher, never lower") because the over-count was a handful of entries.
 * The two 7b shapes compose epmAttrs and eemAttrs, whose DE407 option-C
 * declarations are MOSTLY metadata-only (eight of epmAttrs' twelve entries
 * and three of eemAttrs' five take no slot), so the naive sums would be
 * 1,006 B for the water heater and about 1,150 B for battery storage:
 * both spuriously past the RVC's 936 B candidate, and the water heater's
 * kHeapCostOf x 8 alone would fail the floor assertion below for a block
 * that in truth costs 808 B. A floor tripped by an arithmetic convention
 * rather than by real bytes would force a real HEARTH_EP_HEAP_BYTES
 * decision on false evidence, the exact opposite of what the assertion is
 * for.
 *
 * So these candidates count EXACTLY: MT_COUNT per list minus a NAMED
 * constant for that list's metadata-only members, each constant carrying
 * the members it stands for. (Counting through attr_gets_slot() itself at
 * compile time would be better still, and does not compile: the lists are
 * const but not constexpr, and a const object of class type is not usable
 * in a constant expression, so attr_gets_slot() cannot read one at compile
 * time. Memory reclaim round A moved them from .data to .rodata with
 * HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN; that does not change this.)
 * Drift-proofing is the equality assert under each candidate, pinning the
 * whole derivation to the sizing table's payload row: ANY edit to any
 * involved list moves the sum off the pinned value and fails the build,
 * so a stale subtraction constant cannot survive a list change silently,
 * which is exactly the property the MT_COUNT over-count bought the older
 * candidates.
 */
/* Metadata-only (slotless) members per composed list, from the audit
 * notes at each declaration: epmAttrs has eight (Accuracy plus the seven
 * DE407 push fields), eemAttrs three (the three structs), whmAttrs one
 * (EstimatedHeatRequired), modeBaseAttrs one (SupportedModes);
 * thermostatAttrs and ptopAttrs have none. */
constexpr size_t kEpmNoSlot      = 8;
constexpr size_t kEemNoSlot      = 3;
constexpr size_t kWhmNoSlot      = 1;
constexpr size_t kModeBaseNoSlot = 1;

/* Water heater v0, the wider variant list (whmAttrs strictly contains
 * whmBareAttrs and the v1 list drops three whole clusters): 7 clusters,
 * 11 + 7 + 3 + 2 + 4 + 2 = 29 slots, one ModeBase store. */
constexpr size_t kWaterHeaterBlockBytes =
    block_bytes(MT_COUNT(waterHeaterClusters),
                MT_COUNT(thermostatAttrs) + (MT_COUNT(whmAttrs) - kWhmNoSlot) +
                    (MT_COUNT(modeBaseAttrs) - kModeBaseNoSlot) + MT_COUNT(ptopAttrs) +
                    (MT_COUNT(epmAttrs) - kEpmNoSlot) + (MT_COUNT(eemAttrs) - kEemNoSlot)) +
    sizeof(mt_mb_store_t);
/* Payload pinned here (kHeapCostOf is not declared yet at this point in
 * the file); the 808 B HEAP figure the tables quote is pinned too, below
 * the kHeapCostOf definition beside the floor assertion (fix round M5),
 * so a formula change cannot stale the tables silently. */
static_assert(kWaterHeaterBlockBytes == 798,
              "the water heater block changed size; redo the sizing table rows and the "
              "batch 7b capacity arithmetic");

/* More metadata-only counts for the battery candidate: the rechargeable
 * PowerSource list has four (Description, EndpointList and the two fault
 * arrays), demAttrs three (PowerAdjustmentCapability and the two power_mw
 * scalars). */
constexpr size_t kRechargeablePsNoSlot = 4;
constexpr size_t kDemNoSlot            = 3;

/* Battery storage v0, the wider variant list (v1 drops the DEM pair
 * whole): 7 clusters, 15 + 2 + 4 + 2 + 6 + 3 = 32 slots, one ModeBase
 * store. The catalogue's second-widest block: 846 B payload against the
 * RVC's real 856 (heap cost 856 against 864), ten bytes shy of the
 * title. */
constexpr size_t kBatteryStorageBlockBytes =
    block_bytes(MT_COUNT(batteryStorageClusters),
                (MT_COUNT(rechargeableBatteryPowerSourceAttrs) - kRechargeablePsNoSlot) +
                    MT_COUNT(ptopAttrs) + (MT_COUNT(epmAttrs) - kEpmNoSlot) +
                    (MT_COUNT(eemAttrs) - kEemNoSlot) + (MT_COUNT(demAttrs) - kDemNoSlot) +
                    (MT_COUNT(modeBaseAttrs) - kModeBaseNoSlot)) +
    sizeof(mt_mb_store_t);
/* 846 B payload; the 856 B heap figure is pinned below the kHeapCostOf
 * definition (fix round M5, the water heater pin's note). The audit's
 * 3.6 estimate (830/832) under-counted this list by one slot and
 * mis-rounded, re-derived per the batch brief (the section comment). */
static_assert(kBatteryStorageBlockBytes == 846,
              "the battery storage block changed size; redo the sizing table rows and the "
              "batch 7b capacity arithmetic");

/*
 * Catalogue batch 8. Seven new device types and thirteen new realised block
 * shapes, and ONE new candidate, which needs its argument made rather than
 * assumed: the store-reclaim round's rule is that every store-bearing type
 * is its own candidate, and eight of the thirteen carry a store.
 *
 * The cook surface variant 1 (381 B), the unparented cabinet variant 1
 * (329), the refrigerator (514), the microwave (530) and the cooler and
 * heater cabinets in every combination are ALL strict subsets of the heater
 * cabinet variant 1 in both dimensions that matter: it has the most clusters
 * (4), the most slots of any TemperatureLevel shape, and it is the only
 * shape in the batch that carries BOTH new store types at once (a ModeBase
 * store and a label store). So growing either store, or any list either
 * candidate composes, moves THIS candidate's pinned value and fails the
 * build, which is exactly the drift-proofing the per-type rule buys, without
 * eight near-identical constants that would each have to be kept honest.
 *
 * Exact counting, the batch-7b convention rather than the older MT_COUNT
 * over-count: these lists are small enough that over-counting would not have
 * tripped the floor, but the convention is the newer one and mixing them
 * across neighbouring candidates is how a reader stops trusting either.
 * Metadata-only members, from each declaration's own audit note:
 * temperatureControlLevelAttrs has one (SupportedTemperatureLevels, the
 * ARRAY that doubles as the store discriminator), opStateAttrs three
 * (PhaseList and OperationalStateList, both ARRAY, and OperationalError, a
 * STRUCT), modeBaseAttrs one (SupportedModes, already named above).
 *
 */
constexpr size_t kTcLevelNoSlot = 1;
constexpr size_t kOpStateNoSlot = 3;

/* Cabinet, Heater, variant 1: 4 clusters (TemperatureControl, OvenMode,
 * OvenCavityOperationalState, Descriptor), 3 + 3 + 4 = 10 slots, one
 * ModeBase store and one label store. */
constexpr size_t kCabinetHeaterLevelBlockBytes =
    block_bytes(MT_COUNT(cabinetHeaterLevelClusters),
                (MT_COUNT(temperatureControlLevelAttrs) - kTcLevelNoSlot) +
                       (MT_COUNT(modeBaseAttrs) - kModeBaseNoSlot) +
                       (MT_COUNT(opStateAttrs) - kOpStateNoSlot)) +
    sizeof(mt_mb_store_t) + sizeof(mt_temp_levels_store_t);
static_assert(kCabinetHeaterLevelBlockBytes == 755,
              "the heater cabinet variant 1 block changed size; redo the sizing table rows and "
              "the batch 8 capacity arithmetic");

/* The microwave oven, the batch's other store-bearing candidate worth naming
 * separately (fix round N3). It loses to the heater cabinet v1 by 225 bytes
 * and so cannot move kWidestUncappedBlockBytes, but its block cost is the divisor in
 * a capacity claim the README makes out loud, "15 microwave ovens fit and 16
 * do not", and in kObjMicrowaveEndpointLimit over in mt_matter_zephyr.cpp.
 * Pinning it here is what stops those three drifting apart: this is the only
 * translation unit that knows both the block cost and the heap's usable size.
 * 4 clusters (OperationalState, MicrowaveOvenMode, MicrowaveOvenControl,
 * Descriptor), 5 + 3 + 5 = 13 slots, one ModeBase store. */
constexpr size_t kMicrowaveBlockBytes =
    block_bytes(MT_COUNT(microwaveOvenClusters),
                (MT_COUNT(microwaveOpStateAttrs) - kOpStateNoSlot) +
                    (MT_COUNT(modeBaseAttrs) - kModeBaseNoSlot) + MT_COUNT(mwocAttrs)) +
    sizeof(mt_mb_store_t);
static_assert(kMicrowaveBlockBytes == 530,
              "the microwave oven block changed size; redo the sizing table row, the "
              "README's 15-of-16 capacity claim and kObjMicrowaveEndpointLimit");

/*
 * The UNCAPPED maximum: every candidate here is a device type a host may
 * declare as many of as the endpoint table and the block heap allow, so the
 * floor below demands kMinWidestEndpoints of whichever is widest. This is the
 * constant the pre-EVSE assertion called kWidestBlockBytes, unchanged in
 * membership and value; only the name says which half of the floor it feeds.
 */
/*
 * The EVSE round's candidate, and the first CAPPED one: MT_EVSE_MAX bounds
 * how many of these a composition can hold, so it is asserted against
 * kEvseEndpointCap below rather than joining the uncapped maximum. Exact
 * counting, the batch-7b convention: 8 clusters, 12 + 3 + 5 + 3 + 2 + 4 + 2 =
 * 31 slots and TWO ModeBase stores. The EnergyEvse list has six metadata-only
 * members (the three amperage_ma currents, NextChargeRequiredEnergy,
 * BatteryCapacity and SessionEnergyCharged, the quiet-table rows above), the
 * DEM report-only list two (its two power_mw scalars), and modeBaseAttrs one
 * each, all named as constants so a list edit moves the pinned sum instead of
 * going unnoticed.
 *
 * The variant-0 list is the wider one (evseAttrs strictly contains
 * evseNoSocAttrs), so this candidate covers both: v1 is 1,128 B of heap
 * against v0's 1,144.
 */
constexpr size_t kEvseNoSlot = 6;

constexpr size_t kEvseBlockBytes =
    block_bytes(MT_COUNT(energyEvseClusters),
                (MT_COUNT(evseAttrs) - kEvseNoSlot) +
                    (MT_COUNT(modeBaseAttrs) - kModeBaseNoSlot) +
                    (MT_COUNT(demReportOnlyAttrs) - kDemNoSlot + 1) +
                    (MT_COUNT(modeBaseAttrs) - kModeBaseNoSlot) + MT_COUNT(ptopAttrs) +
                    (MT_COUNT(epmAttrs) - kEpmNoSlot) + (MT_COUNT(eemAttrs) - kEemNoSlot)) +
    2 * sizeof(mt_mb_store_t);
/* Payload pinned here; the 1,144 B HEAP figure the tables quote is pinned
 * below the kHeapCostOf definition with the other four. kDemNoSlot is
 * demAttrs' count (three: PowerAdjustmentCapability and the two power_mw
 * scalars) and the report-only list drops PowerAdjustmentCapability with the
 * feature, so it has two: the "+ 1" is that difference, spelled out rather
 * than given a near-duplicate constant of its own. */
static_assert(kEvseBlockBytes == 1140,
              "the energy EVSE block changed size; redo the sizing table rows and the EVSE "
              "round's capacity arithmetic");

constexpr size_t kWidestUncappedBlockBytes =
    kMax2(block_bytes(kWidestClusterList, kWidestEndpointSlots),
          kMax2(kMax2(kModeSelectBlockBytes, kChimeBlockBytes),
                kMax2(kMax2(kRvcBlockBytes, kDemBlockBytes),
                      kMax2(kMax2(kWaterHeaterBlockBytes, kBatteryStorageBlockBytes),
                            kMax2(kCabinetHeaterLevelBlockBytes, kMicrowaveBlockBytes)))));

/*
 * Zephyr charges roundup(payload + 4, 8) per allocation on a heap this size,
 * modelled here so the floor is checked against the real cost rather than
 * the bare payload. The 4 is the SMALL-heap chunk header
 * (heap.h chunk_header_bytes()); a big heap would charge 8 and every number
 * in the table above would be wrong.
 *
 * Small-heap-ness is not a Kconfig here, it is derived, so the two things
 * that would flip it are asserted rather than assumed. big_heap_chunks()
 * (heap.h:80-90) returns true if CONFIG_SYS_HEAP_BIG_ONLY is set, or if
 * pointers are wider than 4 bytes, or if the heap exceeds 0x7fff chunks. The
 * third is checked by arithmetic below (8192/8 = 1024, far under); the first
 * two are checked here.
 */
#ifdef CONFIG_SYS_HEAP_BIG_ONLY
#error "CONFIG_SYS_HEAP_BIG_ONLY makes chunk headers 8 bytes; redo the sizing table"
#endif
BUILD_ASSERT(sizeof(void *) == 4,
             "a 64-bit target forces a big heap (8-byte chunk headers); redo the sizing table");
BUILD_ASSERT(HEARTH_EP_HEAP_BYTES / 8 <= 0x7fff,
             "this heap is large enough to be a big heap (8-byte chunk headers); redo the "
             "sizing table");

constexpr size_t kHeapCostOf(size_t payload) { return ((payload + 4 + 7) / 8) * 8; }

/*
 * Usable bytes, not the gross define: sys_heap_init() spends some of the
 * buffer on itself before any allocation happens, and asserting the floor
 * against the gross number would quietly over-promise by that much.
 *
 * For this configuration (8192 B, small heap, CONFIG_SYS_HEAP_RUNTIME_STATS
 * on, 32-bit) the overhead is exactly 80 bytes:
 *
 *   4   end-marker chunk header      heap_footer_bytes(8192), heap.c:538-539
 *   4   lost rounding 8188 down to a CHUNK_UNIT boundary      heap.c:542-544
 *  72   chunk 0, which holds struct z_heap itself: 28 bytes
 *       (chunk0_hdr[2] + end_chunk + avail_buckets + the three runtime-stats
 *       counters) plus 10 buckets x 4, so 68 rounded up to 9 chunks
 *                                                            heap.c:564-566
 *
 * leaving 1023 - 9 = 1014 chunks, 8,112 bytes, which is the figure the
 * README's capacity table is built on.
 */
constexpr size_t kHeapOverheadBytes = 80;
constexpr size_t kHeapUsableBytes = HEARTH_EP_HEAP_BYTES - kHeapOverheadBytes;

constexpr size_t kMinWidestEndpoints = 8;

/* Fix round M5: the HEAP figures the in-file sizing table, the README
 * rows and the capacity arithmetic actually quote for the two batch 7b
 * shapes, pinned through the same kHeapCostOf the runtime charge model
 * uses (their PAYLOADS are pinned at the candidates above, where this
 * function was not yet declared). A change to kHeapCostOf's formula, or
 * to either block, now fails the build instead of silently staling three
 * tables. */
static_assert(kHeapCostOf(kWaterHeaterBlockBytes) == 808,
              "the water heater heap cost moved off the tables' 808; redo the sizing rows");
static_assert(kHeapCostOf(kBatteryStorageBlockBytes) == 856,
              "the battery storage heap cost moved off the tables' 856; redo the sizing rows");
static_assert(kHeapCostOf(kCabinetHeaterLevelBlockBytes) == 760,
              "the heater cabinet variant 1 heap cost moved off the tables' 760; redo the "
              "sizing rows");
static_assert(kHeapCostOf(kMicrowaveBlockBytes) == 536,
              "the microwave oven heap cost moved off the tables' 536; redo the sizing rows");
static_assert(kHeapCostOf(kEvseBlockBytes) == 1144,
              "the energy EVSE heap cost moved off the tables' 1,144; redo the sizing rows");
/* Fix round N3: the README's "15 microwave ovens fit, 16 do not" row and
 * kObjMicrowaveEndpointLimit in mt_matter_zephyr.cpp both rest on this exact
 * division, so it is asserted rather than divided by hand in a comment. Both
 * halves matter: the first is the capacity promise, the second is what makes
 * 15 the LIMIT rather than merely a number that fits. */
static_assert(15 * kHeapCostOf(kMicrowaveBlockBytes) <= kHeapUsableBytes &&
                  16 * kHeapCostOf(kMicrowaveBlockBytes) > kHeapUsableBytes,
              "the endpoint block heap no longer admits exactly fifteen microwave ovens; "
              "redo the README capacity row and kObjMicrowaveEndpointLimit");

/*
 * ---- the floor, in its cap-aware form (the block comment above) ----------
 *
 * kFloorDemand(cap) is the whole of the change: how many blocks of a
 * candidate the heap must hold is the smaller of that candidate's own
 * capacity cap and kMinWidestEndpoints. Written as a function rather than
 * spelled out per candidate so the two arms below cannot drift into meaning
 * different things.
 */
constexpr size_t kMin2(size_t a, size_t b) { return a < b ? a : b; }

constexpr size_t kFloorDemand(size_t cap) { return kMin2(cap, kMinWidestEndpoints); }

/*
 * An uncapped candidate's cap IS kServiceableEndpoints: sixteen is what the
 * endpoint table allows and nothing else refuses. Spelled out rather than
 * passing kMinWidestEndpoints straight in, so the expression says why the
 * demand is eight instead of asserting it.
 */
static_assert(kFloorDemand(kServiceableEndpoints) == kMinWidestEndpoints,
              "kServiceableEndpoints dropped below kMinWidestEndpoints; the uncapped floor is "
              "no longer eight and the capacity table needs rewriting");
static_assert(kHeapCostOf(kWidestUncappedBlockBytes) * kFloorDemand(kServiceableEndpoints) <=
                  kHeapUsableBytes,
              "the endpoint heap no longer holds eight of the widest uncapped device type; redo "
              "the sizing table beside K_HEAP_DEFINE and raise HEARTH_EP_HEAP_BYTES");

/*
 * The capped candidates. One so far: the Energy EVSE, whose delegate carries
 * the whole charging-target store, which is why MT_EVSE_MAX
 * (core/include/mt_matter.h) is 2 and why mt_matter_evse_reserve() refuses a
 * third before anything is built. The block candidate itself arrives with the
 * device type's declarations; what belongs HERE, ahead of it, is the cap and
 * the two properties that keep the floor honest.
 */
constexpr size_t kEvseEndpointCap = MT_EVSE_MAX;

/* NON-VACUITY, half one: a cap of zero would make this candidate's demand
 * zero and its floor an assertion about nothing. */
static_assert(kEvseEndpointCap >= 1,
              "MT_EVSE_MAX is zero; the EVSE floor below would assert nothing at all");
/* NON-VACUITY, half two, and the one that matters. A candidate only belongs
 * in this half while its cap is genuinely BELOW the uncapped demand. Raise
 * MT_EVSE_MAX to eight or more and this fires, which is the instruction to
 * move the EVSE candidate into kWidestUncappedBlockBytes above, where it must
 * then fit eight times over or force a real HEARTH_EP_HEAP_BYTES decision.
 * Without this, raising the cap would silently keep the weaker floor. */
static_assert(kEvseEndpointCap < kMinWidestEndpoints,
              "MT_EVSE_MAX has reached kMinWidestEndpoints; the EVSE is no longer a capped "
              "candidate and its block must join kWidestUncappedBlockBytes above");
/* The floor itself, on the EVSE's own demand. 1,144 x 2 = 2,288 B of 8,112,
 * against the 9,152 an uncapped eight would have asked for and the heap does
 * not have: this is the whole reason the assertion above the candidates was
 * reworked, and the one line that would have to be argued with before
 * MT_EVSE_MAX could rise. */
static_assert(kHeapCostOf(kEvseBlockBytes) * kFloorDemand(kEvseEndpointCap) <= kHeapUsableBytes,
              "the endpoint heap no longer holds MT_EVSE_MAX energy EVSE blocks; redo the "
              "sizing table beside K_HEAP_DEFINE and raise HEARTH_EP_HEAP_BYTES");

dyn_endpoint s_dyn[kServiceableEndpoints];

uint16_t live_endpoints()
{
    uint16_t n = 0;
    for (auto &d : s_dyn) {
        if (d.used) {
            n++;
        }
    }
    return n;
}

/*
 * Where emberAfDoorLockClusterInitCallback() (below, outside this
 * namespace) leaves its result for mt_devtype_create() to check.
 *
 * A void callback is the only shape the SDK offers for an init whose
 * failure genuinely means the endpoint is broken, so the result is parked
 * here instead of being logged and forgotten. Two fields rather than one:
 * the callback also fires for the catalogue endpoint at boot, so
 * mt_devtype_create() must be able to tell "the init ran for MY endpoint
 * and succeeded" from "it ran for some other endpoint" and from "it never
 * ran at all", which is itself a failure worth catching: this file's audit
 * note asserts that ember drives the init for dynamic endpoints, and that
 * claim is now checked at runtime rather than only in a comment.
 *
 * No locking of its own: written from the init callback, which runs inside
 * emberAfSetDynamicEndpoint(), and read immediately after that call
 * returns, both under the StackLock mt_devtype_create() holds across its
 * whole body.
 */
CHIP_ERROR s_lock_init_err = CHIP_NO_ERROR;
EndpointId s_lock_init_ep = kInvalidEndpointId;

/* Endpoint ids run 1..N in composition order and are reassigned from 1 on
 * every boot. The composition itself is what persists, so replaying it in
 * order reproduces the same ids: that is the property AT+MTEP? and any
 * commissioned fabric depend on, and it is why the C6 builds its endpoints
 * before esp_matter::start(). */
uint16_t s_next_ep_id = 1;

/*
 * Boot values for attributes where zero is the wrong answer, little-endian
 * as the attribute store holds them. Two kinds of entry live here:
 *
 *   Functional. LevelControl reads MinLevel and MaxLevel out of the store
 *   when the endpoint is enabled (level-control.cpp:1480-1481) and keeps
 *   them as the movement bounds for the life of the endpoint. Left at zero,
 *   a dimmable light clamps every MoveToLevel to 0 and never lights.
 *
 *   Conformance. FeatureMap and ClusterRevision must describe the cluster
 *   the endpoint actually presents. Revisions are this tree's own
 *   zzz_generated/app-common/clusters/<Cluster>/Metadata.h kRevision
 *   (OnOff 6, LevelControl 6, TemperatureMeasurement 4, Identify 6);
 *   feature bits are from the matching Enums.h (OnOff Feature::kLighting
 *   0x1; LevelControl kOnOff 0x1 | kLighting 0x2 = 0x3).
 *
 * Nullable attributes whose correct boot value is null carry the type's
 * null sentinel rather than a zero: NumericAttributeTraits::GetNullValue()
 * (attribute-storage-null-handling.h:77-81) is the type maximum for
 * unsigned and enum types (0xFF for a 1-byte one) and the type minimum for
 * signed ones (0x8000 for INT16S, stored little-endian as 00 80).
 *
 * Catalogue batch 2 added the trailing `devtype` qualifier. Until then one
 * (cluster, attribute) pair had exactly one boot value across every device
 * type carrying it, which stopped being true the moment two device types
 * shared ColorControl with different feature sets: 0x010C advertises CT
 * alone and 0x010D advertises HS|XY|CT, so FeatureMap and ColorCapabilities
 * have to differ per device type while the other eleven ColorControl seeds
 * stay shared. devtype 0 means "any device type" and is what every seed
 * written before this batch means; a row naming a specific device type wins
 * over the wildcard for that type (see seed_slots()). It is the LAST member
 * on purpose: aggregate initialization zero-fills members a brace list does
 * not reach, so every pre-existing row keeps its exact original text and
 * gets devtype 0 for free.
 *
 * Catalogue batch 8 added the `variant` qualifier by the identical trick,
 * one member further out. It exists because TemperatureControl's FeatureMap
 * is variant-dependent AND shared by two device types (0x0071 and 0x0077),
 * so neither the wildcard nor the devtype qualifier can split it: variant 0
 * is TemperatureNumber|TemperatureStep (0x05) and variant 1 is
 * TemperatureLevel (0x02) on BOTH types. Batches 7a and 7b had already met
 * this problem twice (the DEM and WHM FeatureMaps) and each solved it with a
 * hand-written special case ahead of the table lookup in seed_slots(); this
 * column is the general fix, and both of those special cases were RETIRED
 * into ordinary rows when it landed. AirQuality's special case stays, and is
 * not the same problem: its value comes from a shared accessor function
 * rather than from a literal, which no table column can express.
 *
 * Why TemperatureControl is what forced the general fix rather than a fourth
 * special case: it is the first variant-dependent FeatureMap that is
 * FUNCTIONALLY load-bearing rather than merely descriptive.
 * emberAfTemperatureControlClusterSetTemperatureCallback() reads the
 * FeatureMap back to pick SetTemperature's branch
 * (temperature-control-server.cpp:117-124, :183), so a wrong bit does not
 * just misreport a capability, it makes a healthy endpoint answer Failure or
 * InvalidCommand to a valid command.
 *
 * THE ENCODING, because the zero-fill has to keep meaning "any variant":
 * 0 is the wildcard and a row naming a variant stores variant + 1.
 * seed_variant() is the only place that arithmetic appears.
 *
 * PRECEDENCE, generalising batch 2's rule rather than replacing it: the most
 * specific matching row wins, scored devtype-qualified 2, variant-qualified
 * 1, both 3, neither 0. A row whose qualifier is present but does not match
 * is skipped outright. Every pre-batch-8 row is variant-wildcard, so the
 * relative order of any two of them is exactly what it was and every
 * existing seed resolves to the identical value. Ranking a devtype match
 * above a variant match is a choice, not a derivation; no pair in this table
 * makes the two compete today, and if one ever does the comment is here to
 * be argued with.
 */
constexpr uint8_t kSeedAnyVariant = 0;
constexpr uint8_t seed_variant(uint8_t v) { return (uint8_t)(v + 1); }

struct attr_seed {
    ClusterId cluster;
    AttributeId attr;
    uint8_t size;
    uint8_t bytes[4];
    uint32_t devtype;
    uint8_t variant;
};

const attr_seed s_seeds[] = {
    /* OnOff */
    { OnOff::Id, OnOff::Attributes::GlobalSceneControl::Id, 1, { 0x01 } },     /* TRUE, cluster spec default */
    { OnOff::Id, OnOff::Attributes::StartUpOnOff::Id, 1, { 0xFF } },           /* null */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 } },
    { OnOff::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x06, 0x00 } },

    /* LevelControl */
    { LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, 1, { 0xFE } },
    { LevelControl::Id, LevelControl::Attributes::MinLevel::Id, 1, { 0x01 } },
    { LevelControl::Id, LevelControl::Attributes::MaxLevel::Id, 1, { 0xFE } },
    { LevelControl::Id, LevelControl::Attributes::OnLevel::Id, 1, { 0xFF } },  /* null */
    { LevelControl::Id, LevelControl::Attributes::StartUpCurrentLevel::Id, 1, { 0xFF } }, /* null */
    { LevelControl::Id, LevelControl::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 } },
    { LevelControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x06, 0x00 } },

    /* TemperatureMeasurement: no features, and no reading until the host
     * writes one, so all three values start null. */
    { TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MinMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MaxMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { TemperatureMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x04, 0x00 } },

    /* Identify: no features; IdentifyType 0 (None) is the zero-fill. */
    { Identify::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x06, 0x00 } },

    /* BooleanState: no features; StateValue false is the zero-fill (spelled
     * out anyway, since it is the attribute this cluster exists for). */
    { BooleanState::Id, BooleanState::Attributes::StateValue::Id, 1, { 0x00 } },
    { BooleanState::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* OccupancySensing: fix round 2, C2. The device-type XML alone was the
     * wrong source for FeatureMap: it names no <features> block, but the
     * CLUSTER XML (data_model/1.5/clusters/OccupancySensing.xml) declares
     * all eight Feature bits as `optionalConform choice="a" min="1"` --
     * choice group "a" requires AT LEAST ONE of them set, so FeatureMap 0
     * does not conform. Seeding a PIR sensor means Feature::
     * kPassiveInfrared (0x2) must be set.
     *
     * Trap worth naming explicitly: kPassiveInfrared is BIT 1 of Feature
     * (Enums.h:46-56, value 0x2), but OccupancySensorTypeBitmap::kPir is
     * BIT 0 of a DIFFERENT bitmap (Enums.h:64-70 -> Bitmap for
     * OccupancySensorTypeBitmap, value 0x1). The two "PIR" bits live in
     * unrelated attributes at different bit positions by design -- do not
     * copy one value into the other.
     *
     * Sensor-type fields seed a PIR default (type 0 = kPir, bitmap 0x01 =
     * kPir); see the comment above occupancyAttrs for why the seed rows
     * below are the only writer of these fields on this (dynamic)
     * endpoint. */
    { OccupancySensing::Id, OccupancySensing::Attributes::OccupancySensorType::Id, 1, { 0x00 } },
    { OccupancySensing::Id, OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id, 1, { 0x01 } },
    { OccupancySensing::Id, OccupancySensing::Attributes::FeatureMap::Id, 4,
      { 0x02, 0x00, 0x00, 0x00 } },
    { OccupancySensing::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* RelativeHumidityMeasurement: no features, and no reading until the
     * host writes one, so all three values start null (uint16, sentinel is
     * the type maximum 0xFFFF). */
    { RelativeHumidityMeasurement::Id, RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { RelativeHumidityMeasurement::Id, RelativeHumidityMeasurement::Attributes::MinMeasuredValue::Id,
      2, { 0xFF, 0xFF } },
    { RelativeHumidityMeasurement::Id, RelativeHumidityMeasurement::Attributes::MaxMeasuredValue::Id,
      2, { 0xFF, 0xFF } },
    { RelativeHumidityMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* PressureMeasurement: no features mandated, and no reading until the
     * host writes one, so all three values start null (int16s, sentinel is
     * the type minimum 0x8000, same convention as TemperatureMeasurement
     * above). */
    { PressureMeasurement::Id, PressureMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { PressureMeasurement::Id, PressureMeasurement::Attributes::MinMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { PressureMeasurement::Id, PressureMeasurement::Attributes::MaxMeasuredValue::Id, 2,
      { 0x00, 0x80 } },
    { PressureMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* IlluminanceMeasurement: no features, and no reading until the host
     * writes one, so all three values start null. uint16, NOT int16s like
     * temperature/pressure above: the sentinel is the type maximum 0xFFFF,
     * not the type minimum -- see the comment on illuminanceAttrs. */
    { IlluminanceMeasurement::Id, IlluminanceMeasurement::Attributes::MeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { IlluminanceMeasurement::Id, IlluminanceMeasurement::Attributes::MinMeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { IlluminanceMeasurement::Id, IlluminanceMeasurement::Attributes::MaxMeasuredValue::Id, 2,
      { 0xFF, 0xFF } },
    { IlluminanceMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* FlowMeasurement: no features, and no reading until the host writes
     * one, so all three values start null (uint16, sentinel 0xFFFF). */
    { FlowMeasurement::Id, FlowMeasurement::Attributes::MeasuredValue::Id, 2, { 0xFF, 0xFF } },
    { FlowMeasurement::Id, FlowMeasurement::Attributes::MinMeasuredValue::Id, 2, { 0xFF, 0xFF } },
    { FlowMeasurement::Id, FlowMeasurement::Attributes::MaxMeasuredValue::Id, 2, { 0xFF, 0xFF } },
    { FlowMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* On/Off and dimmable plug-in units reuse OnOff/LevelControl/Identify
     * verbatim (same FeatureMap and ClusterRevision seeds as the lights
     * above), so they need no seed rows of their own. */

    /* ---- catalogue batch 2 ------------------------------------------- */

    /* ColorControl, shared by both color lights. Values are esp-matter's own
     * feature-config defaults (esp_matter_feature.h color_temperature:291-293,
     * xy:306, hue_saturation:275), so a host library written against the C6
     * sees the same boot state here.
     *
     * ColorMode and EnhancedColorMode are seeded kColorTemperatureMireds (2)
     * rather than esp-matter's constructor default of 1. Note this is NOT a
     * conformance requirement: all three ColorModeEnum values are
     * mandatoryConform and ungated in ColorControl.xml, so 0, 1 and 2 are
     * each a legal value of the attribute in the abstract. The reason is the
     * power-up rule. StartUpColorTemperatureMireds is seeded non-null (250,
     * C6 parity), and the cluster spec says a lamp with a non-null startup
     * color temperature powers up in color-temperature mode with ColorMode
     * and EnhancedColorMode reflecting that; startUpColorTempCommand()
     * (color-control-server.cpp:2577-2624) writes exactly that pair at
     * endpoint create. Seeding the same value means the arena is right
     * whether or not that init runs. For 0x010C there is a second reason:
     * it declares neither CurrentHue/CurrentSaturation nor CurrentX/CurrentY,
     * so ColorMode 0 or 1 would name a mode whose defining attributes the
     * endpoint does not present.
     *
     * NumberOfPrimaries is null: this firmware drives no primaries of its
     * own and has no way to know what the host's lamp has. */
    { ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id, 2, { 0xFA, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, 2,
      { 0x01, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, 2,
      { 0xFF, 0xFE } },
    { ColorControl::Id, ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, 2,
      { 0x01, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::StartUpColorTemperatureMireds::Id, 2,
      { 0xFA, 0x00 } },
    { ColorControl::Id, ColorControl::Attributes::ColorMode::Id, 1, { 0x02 } },
    { ColorControl::Id, ColorControl::Attributes::EnhancedColorMode::Id, 1, { 0x02 } },
    { ColorControl::Id, ColorControl::Attributes::NumberOfPrimaries::Id, 1, { 0xFF } }, /* null */
    { ColorControl::Id, ColorControl::Attributes::CurrentX::Id, 2, { 0x6B, 0x61 } },
    { ColorControl::Id, ColorControl::Attributes::CurrentY::Id, 2, { 0x7D, 0x60 } },
    { ColorControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x07, 0x00 } },
    /* CurrentHue, CurrentSaturation and Options are all zero at boot, which
     * is the zero-fill, so they carry no row. */

    /* The two per-device-type ColorControl seeds. ColorCapabilities must
     * mirror FeatureMap bits 0..4 (ColorControl.xml constrains it to
     * 0x001F), so the pairs move together: 0x010C is CT alone
     * (Feature::kColorTemperature 0x10), 0x010D is HS|XY|CT (0x1|0x8|0x10 =
     * 0x19). Bit values from ColorControl/Enums.h:148-155 and the matching
     * ColorCapabilitiesBitmap. */
    { ColorControl::Id, ColorControl::Attributes::FeatureMap::Id, 4, { 0x10, 0x00, 0x00, 0x00 },
      0x010C },
    { ColorControl::Id, ColorControl::Attributes::ColorCapabilities::Id, 2, { 0x10, 0x00 }, 0x010C },
    { ColorControl::Id, ColorControl::Attributes::FeatureMap::Id, 4, { 0x19, 0x00, 0x00, 0x00 },
      0x010D },
    { ColorControl::Id, ColorControl::Attributes::ColorCapabilities::Id, 2, { 0x19, 0x00 }, 0x010D },

    /* Thermostat. LocalTemperature is a `temperature` (int16s), so its null
     * sentinel is the signed-type minimum, the same 00 80 the temperature
     * and pressure sensors use. Setpoints are hundredths of a degree:
     * 1600 = 16 C, 2400 = 24 C (the C6's cross-layer I1 values, not
     * esp-matter's 2000/2600), limits are the spec defaults 700/3000 heating
     * and 1600/3200 cooling. ControlSequenceOfOperation 4 is
     * CoolingAndHeating, matching FeatureMap Heating|Cooling; SystemMode 0
     * is Off, spelled out because esp-matter's default is 1 (Auto) and Auto
     * is not a mode this endpoint can honour without the AutoMode feature.
     * Revision 9 is Thermostat/Metadata.h kRevision, which is also what the
     * cluster's own AttributeAccessInterface answers for ClusterRevision. */
    { Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, 2, { 0x00, 0x80 } }, /* null */
    { Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, 2, { 0x40, 0x06 } },
    { Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, 2, { 0x60, 0x09 } },
    { Thermostat::Id, Thermostat::Attributes::MinHeatSetpointLimit::Id, 2, { 0xBC, 0x02 } },
    { Thermostat::Id, Thermostat::Attributes::MaxHeatSetpointLimit::Id, 2, { 0xB8, 0x0B } },
    { Thermostat::Id, Thermostat::Attributes::MinCoolSetpointLimit::Id, 2, { 0x40, 0x06 } },
    { Thermostat::Id, Thermostat::Attributes::MaxCoolSetpointLimit::Id, 2, { 0x80, 0x0C } },
    { Thermostat::Id, Thermostat::Attributes::ControlSequenceOfOperation::Id, 1, { 0x04 } },
    { Thermostat::Id, Thermostat::Attributes::SystemMode::Id, 1, { 0x00 } },
    { Thermostat::Id, Thermostat::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 } },
    { Thermostat::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x09, 0x00 } },
    /* Catalogue batch 7b: the water heater's HEATING-ONLY thermostat,
     * keyed per device type so the Heating|Cooling wildcard rows above
     * keep winning for the standalone thermostat (0x0301) and the room
     * air conditioner (0x0072), the ColorControl 0x010C/0x010D mechanism.
     * 0x1 is Feature::kHeating alone, WaterHeater.xml's Thermostat(HEAT)
     * mandate (the waterHeaterClusters audit note traces it);
     * ControlSequenceOfOperation 0x02 is HeatingOnly, because the wildcard
     * 0x04 (CoolingAndHeating) would advertise cooling this endpoint does
     * not have (a DELIBERATE divergence from the C6, which serves
     * esp-matter's inherited config default 4 here; the FanModeSequence
     * precedent). The setpoint seeds, SystemMode 0 and the revision row
     * stay shared: same cluster, same AAI answer. */
    { Thermostat::Id, Thermostat::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 },
      0x050F },
    { Thermostat::Id, Thermostat::Attributes::ControlSequenceOfOperation::Id, 1, { 0x02 },
      0x050F },

    /* FanControl. FeatureMap 0 conforms (FanControl.xml declares every
     * feature optionalConform with no choice group), and FanModeSequence
     * must therefore be one of the !AUT values 0, 1 or 5:
     * kOffLowMedHigh (0) is the widest of them. FanMode Off, PercentSetting
     * and PercentCurrent 0 are the zero-fill, spelled out because they are
     * the attributes this cluster exists for. Revision 5 is
     * FanControl/Metadata.h kRevision.
     *
     * Fix round 2 re-examined PercentSetting's 0, since it is a NULLABLE
     * attribute seeded with a real value rather than the null sentinel and
     * a bench reader saw that 0 come back from chip-tool before any write.
     * The 0 is deliberate and stays, for two independent reasons.
     *
     * First, it IS the cluster's declared default. Mind which XML: the one
     * this build's zap actually consumes is
     * src/app/zap-templates/zcl/data-model/chip/fan-control-cluster.xml,
     * reached through zcl.json's xmlRoot (['.', './data-model/chip']) and
     * its xmlFile list, NOT the data_model/1.x spec snapshots this file
     * cites for conformance and feature questions. Line 102 of that XML
     * declares PercentSetting `default="0" isNullable="true"`, and that is
     * where hearth.zap's generated defaultValue of 0x00 comes from. (The
     * 1.5 snapshot carries no default for this attribute, which an earlier
     * version of this comment wrongly read as "no XML default to defer to".
     * Right seed, wrong reason; the build consumes the zap-templates copy.)
     *
     * Second, the spec binds its pairing with FanMode and the server
     * implements it: setting FanMode to Off SHALL set PercentSetting,
     * PercentCurrent, SpeedSetting and SpeedCurrent to 0
     * (fan-control-server.cpp:337-361). FanMode is seeded Off one row
     * below, so 0 is the only value consistent with it, and a null seed
     * would contradict that pairing at boot. Null is the
     * Auto-mode answer instead (:363-380 sets PercentSetting null when
     * FanMode goes to Auto), and Auto sits behind the AUT feature this
     * endpoint does not advertise. It also matches esp-matter's own
     * fan_control config defaults (esp_matter_cluster.h:377-392, fan_mode 0
     * and percent_setting 0), so the C6 boots the same pair. The bench's
     * chip-tool 0 was this seed read back correctly: FanControl registers
     * no AttributeAccessInterface at all, so nothing intercepts the read. */
    { FanControl::Id, FanControl::Attributes::FanMode::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::FanModeSequence::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::PercentSetting::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::PercentCurrent::Id, 1, { 0x00 } },
    { FanControl::Id, FanControl::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { FanControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* WindowCovering. Target and current lift positions are percent100ths
     * (uint16), so their null sentinel is the type maximum 0xFFFF, not the
     * signed minimum: nothing is known about the covering's position until
     * the host says so. ConfigStatus 0x09 is Operational|LiftPositionAware
     * (ConfigStatus bitmap, WindowCovering/Enums.h:88-97). FeatureMap 0x05 is
     * Lift|PositionAwareLift. Revision 5 is WindowCovering/Metadata.h
     * kRevision, which is also what the cluster's AttributeAccessInterface
     * answers for ClusterRevision (window-covering-server.cpp:122-123).
     * Type, OperationalStatus, EndProductType and Mode are all zero at boot
     * (roller shade, idle, no mode bits), which is the zero-fill. */
    { WindowCovering::Id, WindowCovering::Attributes::ConfigStatus::Id, 1, { 0x09 } },
    { WindowCovering::Id, WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id, 2,
      { 0xFF, 0xFF } },
    { WindowCovering::Id, WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, 2,
      { 0xFF, 0xFF } },
    { WindowCovering::Id, WindowCovering::Attributes::FeatureMap::Id, 4,
      { 0x05, 0x00, 0x00, 0x00 } },
    { WindowCovering::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* AirQuality. The AirQuality attribute is kUnknown (0) at boot, the
     * zero-fill. FeatureMap deliberately has NO row here: seed_slots() fills
     * it from mt_air_quality_feature_mask() instead, so this file and any
     * future server Instance read the enabled feature set from one accessor
     * rather than two transcribed literals (mt_matter.h's contract for that
     * function). Revision 1 is AirQuality/Metadata.h kRevision. */
    { AirQuality::Id, AirQuality::Attributes::AirQuality::Id, 1, { 0x00 } },
    { AirQuality::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* DoorLock. LockState is seeded to its ENUM8 null sentinel (0xFF)
     * rather than to a state, because that is what the endpoint genuinely
     * knows at boot: the firmware never actuates a lock itself, and only
     * the host's AT+MTLOCK can report a real one (AT_MT_SPEC.md 3.18). It
     * is also what the SDK writes over the seed a moment later regardless,
     * from DoorLockServer::InitEndpoint() (door-lock-server.cpp:101), which
     * calls Attributes::LockState::SetNull() and SetActuatorEnabled(true).
     * The C6 reaches the same pair by the same route, through esp-matter's
     * own emberAfDoorLockClusterInitCallback registration, even though its
     * door_lock::config_t nominally defaults lock_state to 0: the init runs
     * after the attribute is created and wins. So these two rows are seeded
     * to agree with what InitEndpoint will write, the same discipline the
     * ColorControl rows follow: called so the agreement stays true if the
     * seeds change.
     *
     * LockType 0 is kDeadBolt, OperatingMode 0 is kNormal, both zero-fill
     * and both spelled out because they are attributes a controller reads.
     * SupportedOperatingModes 0xFFF6 is the XML's own declared default
     * (door-lock-cluster.xml:272) and esp-matter's, so it is what both
     * platforms present. Its bits are inverted-sense per the cluster spec
     * (a CLEAR bit means the mode IS supported), which makes 0xFFF6 read
     * as Normal (0x01) and NoRemoteLockUnlock (0x08) supported. Nothing in
     * the SDK reads this attribute, so that reading is the spec's, not
     * something the server enforces; the value is taken from the XML rather
     * than computed. AutoRelockTime 0 disables auto-relock and, more
     * importantly, exists at all: see the B129 note on doorLockAttrs.
     * FeatureMap 0, no PIN/USER/schedule/Aliro features this round.
     * Revision 7 is DoorLock/Metadata.h kRevision in THIS tree; the C6's
     * esp-matter pins 10, and the rule here is the tree the build consumes,
     * not the sibling platform's. */
    { DoorLock::Id, DoorLock::Attributes::LockState::Id, 1, { 0xFF } }, /* null */
    { DoorLock::Id, DoorLock::Attributes::LockType::Id, 1, { 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::ActuatorEnabled::Id, 1, { 0x01 } },
    { DoorLock::Id, DoorLock::Attributes::AutoRelockTime::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::OperatingMode::Id, 1, { 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::SupportedOperatingModes::Id, 2, { 0xF6, 0xFF } },
    { DoorLock::Id, DoorLock::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { DoorLock::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x07, 0x00 } },

    /* ValveConfigurationAndControl. All five state attributes boot null,
     * which is both the XML's declared default and the truthful answer for
     * a valve this firmware has never actuated: the host owns actuation and
     * reports it back with AT+MTVALVE (AT_MT_SPEC.md 3.19). ELAPSED_S is an
     * alias for INT32U, so its null sentinel is the unsigned type maximum
     * (four 0xFF bytes); CurrentState and TargetState are ENUM8, so 0xFF.
     *
     * RemainingDuration is seeded here for completeness but a controller
     * never reads this slot: the cluster's wildcard AttributeAccessInterface
     * answers that one attribute from its own gRemainingDuration[] shadow
     * ahead of ember (see the audit note on valveAttrs). The split is the
     * BooleanState shape, and unlike BooleanState it is deliberately NOT
     * bridged: RemainingDuration is a countdown the server itself owns and
     * ticks, with no host write in the AT contract to push into it. The
     * consequence to know is that an AT+MTATTR read of it answers from this
     * arena, i.e. +MTERR:5 for the null seeded here, while a subscribed
     * controller sees the server's live countdown. The C6 has the same
     * split for the same reason.
     *
     * FeatureMap 0, no TS and no LVL: see the valveAttrs comment for why TS
     * in particular would be actively harmful. Revision 2 is
     * ValveConfigurationAndControl/Metadata.h kRevision and the XML's
     * globalAttribute value in this tree; the C6's esp-matter pins 1, and
     * the SDK's own chef reference also says 1, which is stale against its
     * own XML. This tree's 2 is what this build serves. */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::OpenDuration::Id,
      4, { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id,
      ValveConfigurationAndControl::Attributes::DefaultOpenDuration::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id,
      ValveConfigurationAndControl::Attributes::RemainingDuration::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::CurrentState::Id,
      1, { 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::TargetState::Id, 1,
      { 0xFF } }, /* null */
    { ValveConfigurationAndControl::Id, ValveConfigurationAndControl::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 } },
    { ValveConfigurationAndControl::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x02, 0x00 } },

    /* ---- catalogue batch 4 ------------------------------------------- */

    /* PowerSource. Status 0 (Unspecified), Order 0, BatChargeLevel 0 (OK),
     * BatReplacementNeeded false and BatReplaceability 0 (Unspecified) are
     * all the zero-fill and all match the C6's config defaults
     * (esp_matter_cluster.h power_source config, all-zero; the battery
     * sub-config likewise), so they carry no rows. BatPercentRemaining
     * boots null: this firmware measures no battery, and only a host
     * AT+MTATTR write can report a real reading (INT8U, so the unsigned
     * 1-byte sentinel 0xFF; ember's MIN_MAX check admits it past the
     * 0..200 bounds because null is always in-range for a nullable
     * attribute, attribute-table.cpp:401-403). FeatureMap 0x02 is
     * Feature::kBattery (PowerSource/Enums.h:282-283), the exact-one
     * Wired/Battery choice matching the C6. Revision 3 is
     * PowerSource/Metadata.h:20 kRevision, which is also what the
     * cluster's wildcard AttributeAccessInterface answers for
     * ClusterRevision (power-source-server.cpp:131), the same
     * seed-agrees-with-AAI discipline as the Thermostat and
     * WindowCovering rows above. Description gets no slot and no row. */
    { PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id, 1, { 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x02, 0x00, 0x00, 0x00 } },
    { PowerSource::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },
    /* Catalogue batch 7a: the WIRED PowerSource FeatureMap for the heat
     * pump and solar endpoints, keyed per device type so the battery
     * wildcard row above keeps winning for the standalone power source
     * (0x0011), the ColorControl 0x010C/0x010D mechanism. 0x1 is
     * Feature::kWired (PowerSource/Enums.h:282). Status 0, Order 0 and
     * WiredCurrentType 0 (kAc, WiredCurrentTypeEnum) are the zero-fill;
     * the four battery-list seeds above target attributes
     * wiredPowerSourceAttrs does not declare and never match here. The
     * revision row (3) is shared: same cluster, same AAI answer. */
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 },
      0x0309 },
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 },
      0x0017 },
    /* Catalogue batch 7b: battery storage's Battery|Rechargeable
     * FeatureMap (0x2 | 0x4 = 0x6, PowerSource/Enums.h:280-286), the
     * RECHG conformance fix's feature half, keyed per device type like
     * the wired rows above. The four nullable uint32 battery attributes
     * boot null (4-byte unsigned sentinel; this firmware measures no
     * battery and only a host AT+MTATTR write can report a reading, the
     * BatPercentRemaining rule); the rows are wildcard because only the
     * rechargeable list declares these attributes, so no other type can
     * match them. BatCapacity 0, BatChargeState 0 (kUnknown) and
     * BatFunctionalWhileCharging false are the zero-fill matching the C6
     * config defaults (the rechargeableBatteryPowerSourceAttrs audit
     * note). */
    { PowerSource::Id, PowerSource::Attributes::FeatureMap::Id, 4, { 0x06, 0x00, 0x00, 0x00 },
      0x0018 },
    { PowerSource::Id, PowerSource::Attributes::BatVoltage::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::BatTimeRemaining::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::BatTimeToFullCharge::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { PowerSource::Id, PowerSource::Attributes::BatChargingCurrent::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */

    /* SmokeCoAlarm. Every state boots at its enum's zero: ExpressedState 0
     * (kNormal), SmokeState/COState/BatteryAlert 0 (kNormal),
     * TestInProgress and HardwareFaultAlert false, EndOfServiceAlert 0
     * (kNormal): the truthful boot state for an alarm this firmware has
     * never been told anything about, and all the zero-fill, so only
     * ExpressedState is spelled out (it is the attribute the cluster
     * derives everything toward; see mt_matter_alarm_set()'s B165
     * recompute in mt_matter_zephyr.cpp). FeatureMap 3 is
     * SmokeAlarm|CoAlarm (SmokeCoAlarm/Enums.h:115-119), both features
     * because AT+MTALARM's field table has no single-feature variant.
     * Revision 1 is SmokeCoAlarm/Metadata.h:20 kRevision and the XML's
     * globalAttribute value (smoke-co-alarm-cluster.xml:35). */
    { SmokeCoAlarm::Id, SmokeCoAlarm::Attributes::ExpressedState::Id, 1, { 0x00 } },
    { SmokeCoAlarm::Id, SmokeCoAlarm::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 } },
    { SmokeCoAlarm::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* OperationalState (the washer/dishwasher/dryer trio). Every cluster
     * attribute is served by the per-endpoint Instance's AAI, so these
     * slots are INERT on the fabric and exist for AT+MTATTR arena
     * consistency and AttributeList truthfulness (see the opStateAttrs
     * audit note). Seeded to agree with the Instance's own boot state so
     * the two stores at least start in agreement: OperationalState 0
     * (kStopped, the Instance's member default,
     * operational-state-server.h:224) is the zero-fill and carries no
     * row; CurrentPhase boots null (nullable INT8U, sentinel 0xFF).
     * FeatureMap 0 (the cluster defines no features). Revision 1 is
     * OperationalState/Metadata.h:20 kRevision, which is also what the
     * Instance's AAI answers for ClusterRevision
     * (operational-state-server.cpp:408). */
    { OperationalState::Id, OperationalState::Attributes::CurrentPhase::Id, 1, { 0xFF } }, /* null */
    { OperationalState::Id, OperationalState::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 } },
    { OperationalState::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ModeSelect. StandardNamespace boots null (nullable ENUM16, unsigned
     * 2-byte sentinel 0xFFFF): this co-processor cannot know which
     * standard namespace the host's modes belong to, and the C6's config
     * default is the same null. CurrentMode 0 is the zero-fill and
     * matches the C6's config default; the host's AT+MTMODES list
     * conventionally starts at mode 0. FeatureMap 0 (no DEPONOFF; the
     * cluster's only feature needs an OnOff server on the endpoint).
     * Revision 2 is ModeSelect/Metadata.h:20 kRevision and the XML's
     * globalAttribute value (mode-select-cluster.xml:45). Description
     * gets no slot and no row. */
    { ModeSelect::Id, ModeSelect::Attributes::StandardNamespace::Id, 2, { 0xFF, 0xFF } }, /* null */
    { ModeSelect::Id, ModeSelect::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { ModeSelect::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* Chime. SelectedChime 0 is the zero-fill and the XML default;
     * Enabled seeds TRUE, the XML default (chime-cluster.xml:42) and the
     * ChimeServer's own constructed state (mEnabled true,
     * chime-server.cpp:44). Both slots are fully inert since fix round 2
     * (the server's AAI shadows them on the fabric, and DE397's carve-out
     * routes both the AT+MTATTR read AND write legs to the live server,
     * mt_matter_zephyr.cpp), so these seeds are never observable
     * anywhere; kept truthful to a fresh server anyway, the arena
     * discipline every other row follows. FeatureMap 0
     * (the cluster declares no features). Revision 1 is
     * Chime/Metadata.h:20 kRevision and the XML's globalAttribute value
     * (chime-cluster.xml:39). */
    { Chime::Id, Chime::Attributes::Enabled::Id, 1, { 0x01 } },
    { Chime::Id, Chime::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { Chime::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- catalogue batch 5 ------------------------------------------- */

    /* Switch. NumberOfPositions 2 is the cluster's own declared default
     * and minimum (switch-cluster.xml:83, min="2") and esp-matter's config
     * default, so the C6 boots the same pair; CurrentPosition 0 is the
     * zero-fill and carries no row. FeatureMap 0x02 is MomentarySwitch
     * (Switch/Enums.h:35; see the switchAttrs audit note for why exactly
     * that bit). Revision 2 is Switch/Metadata.h:20 kRevision in THIS
     * tree, the batch 3 rule (the tree the build consumes, not the
     * sibling platform's pins). */
    { Switch::Id, Switch::Attributes::NumberOfPositions::Id, 1, { 0x02 } },
    { Switch::Id, Switch::Attributes::FeatureMap::Id, 4, { 0x02, 0x00, 0x00, 0x00 } },
    { Switch::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* The featureless-OnOff pair (racOnOffAttrs): the wildcard OnOff
     * FeatureMap row above says 0x01 (Lighting) for the light and plug
     * family, so the pump and the RAC each need a per-device-type row
     * that wins over it (the ColorControl 0x010C/0x010D mechanism). The
     * pump's OnOff has no features at all; the RAC's carries exactly
     * DeadFrontBehavior (0x02, OnOff/Enums.h:84), mandatory per the RAC's
     * element requirements. The wildcard OnOff ClusterRevision row (6)
     * stays correct for both, and the four LT-gated seeds above target
     * attributes racOnOffAttrs does not declare, so they simply never
     * match on these endpoints. */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 }, 0x0303 },
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x02, 0x00, 0x00, 0x00 }, 0x0072 },

    /* PumpConfigurationAndControl. The five capability attributes boot
     * null, the truthful answer for hardware this co-processor has never
     * seen: MaxPressure and Capacity are int16s (sentinel 0x8000, stored
     * 00 80, the temperature/pressure convention), MaxSpeed, MaxFlow,
     * MinConstSpeed and MaxConstSpeed are int16u (sentinel 0xFFFF). All
     * six match the C6's config defaults (esp_matter_cluster.h pump
     * config, max_* and capacity nullable-null; constant_speed::config_t
     * both null). EffectiveOperationMode, EffectiveControlMode and
     * OperationMode are all 0, the zero-fill, and all three are LEGAL
     * zeros under a ConstantSpeed-only feature map because
     * OperationModeEnum::kNormal and ControlModeEnum::kConstantSpeed are
     * both 0x00 (PumpConfigurationAndControl/Enums.h:50, :34), so they
     * carry no rows. FeatureMap 0x8 is Feature::kConstantSpeed
     * (Enums.h:67). Revision 4 is PumpConfigurationAndControl/
     * Metadata.h:20 kRevision in THIS tree. */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxPressure::Id, 2,
      { 0x00, 0x80 } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxSpeed::Id, 2,
      { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxFlow::Id, 2,
      { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MinConstSpeed::Id,
      2, { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::MaxConstSpeed::Id,
      2, { 0xFF, 0xFF } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::Capacity::Id, 2,
      { 0x00, 0x80 } }, /* null */
    { PumpConfigurationAndControl::Id, PumpConfigurationAndControl::Attributes::FeatureMap::Id, 4,
      { 0x08, 0x00, 0x00, 0x00 } },
    { PumpConfigurationAndControl::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x04, 0x00 } },

    /* The RVC's ModeBase pair. CurrentMode 0 and FeatureMap 0 are the
     * zero-fill (both slots are inert AAI shadows anyway; DirectModeChange
     * is the aliases' only feature and is optional, so 0 is passed to the
     * Instance constructor too and the two stay in agreement). The
     * ClusterRevision seeds are LIVE, unlike every other Instance-served
     * cluster in this file: ModeBase's Instance::Read() has no default
     * arm, so revision reads fall through to ember (mode-base-server.cpp:
     * 325-347, and CodegenDataModelProvider_Read.cpp treats a no-encode
     * AAI return as "continue to ember"). RvcRunMode/Metadata.h:20 says 4,
     * RvcCleanMode/Metadata.h:20 says 5, per cluster in THIS tree. */
    { RvcRunMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x04, 0x00 } },
    { RvcCleanMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x05, 0x00 } },

    /* RvcOperationalState. Same split as the base trio's rows above
     * (CurrentPhase boots null, sentinel 0xFF; OperationalState 0 kStopped
     * and FeatureMap 0 are the zero-fill), keyed to the DERIVED cluster id
     * since seed matching is per cluster. The revision seed is INERT here,
     * unlike the ModeBase pair's: the Instance's AAI answers
     * ClusterRevision itself, hardcoded to the BASE cluster's
     * OperationalState::kRevision constant even on the derived cluster
     * (operational-state-server.cpp:408-409); both constants are 1 in this
     * tree (OperationalState/Metadata.h:20, RvcOperationalState/
     * Metadata.h:20), so there is no observable divergence, and the seed
     * is written 1 anyway so the arena's metadata stays truthful. */
    { RvcOperationalState::Id, OperationalState::Attributes::CurrentPhase::Id, 1,
      { 0xFF } }, /* null */
    { RvcOperationalState::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- catalogue batch 7a: the energy foundation ------------------- */

    /* ElectricalPowerMeasurement. PowerMode and NumberOfMeasurementTypes
     * ARE the AT-served truth (slot-served; the fabric side reads the
     * identical constants from HearthEpmDelegate::GetPowerMode()/
     * GetNumberOfMeasurementTypes(), so the two agree by the FanControl
     * discipline): kAc = 2 (ElectricalPowerMeasurement/Enums.h PowerMode
     * Enum), one measurement type (ActivePower, the mandatory accuracy
     * entry). FeatureMap 0x2 is Feature::kAlternatingCurrent, the inert
     * shadow of the BitMask every EPM Instance is constructed with
     * (mt_matter_meas_delegate_set_endpoint, cross-referenced there).
     * ClusterRevision 3 is LIVE (Instance::Read() has no revision case)
     * and is ElectricalPowerMeasurement/Metadata.h:20 in THIS tree. The
     * seven 64-bit fields carry no rows: slotless (DE407 option C), so a
     * seed would have nothing to land in. */
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::PowerMode::Id, 1,
      { 0x02 } },
    { ElectricalPowerMeasurement::Id,
      ElectricalPowerMeasurement::Attributes::NumberOfMeasurementTypes::Id, 1, { 0x01 } },
    { ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::FeatureMap::Id, 4,
      { 0x02, 0x00, 0x00, 0x00 } },
    { ElectricalPowerMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x03, 0x00 } },

    /* ElectricalEnergyMeasurement. FeatureMap 0x7 is the inert shadow of
     * the ONE wildcard AttrAccess's fixed Imported|Exported|Cumulative
     * mask (mt_matter_eem_register, mt_matter_zephyr.cpp: that object
     * answers FeatureMap for every EEM endpoint, so this seed may never
     * diverge from its construction). ClusterRevision 2 is LIVE (the
     * wildcard AAI serves no revision case) and is
     * ElectricalEnergyMeasurement/Metadata.h:20 in THIS tree. The three
     * struct attributes are metadata-only and carry no rows. */
    { ElectricalEnergyMeasurement::Id, ElectricalEnergyMeasurement::Attributes::FeatureMap::Id, 4,
      { 0x07, 0x00, 0x00, 0x00 } },
    { ElectricalEnergyMeasurement::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x02, 0x00 } },

    /* PowerTopology. FeatureMap 0x1 is Feature::kNodeTopology, the inert
     * shadow of the Instance's construction mask; ClusterRevision 1 is
     * LIVE (Instance::Read() serves FeatureMap and the two absent
     * endpoint lists only, power-topology-server.cpp:64-77) and is
     * PowerTopology/Metadata.h:20 in THIS tree. */
    { PowerTopology::Id, PowerTopology::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { PowerTopology::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* MeterIdentification. MeterType boots null (Instance::Init() nulls
     * all five attributes; the inert shadow carries the 1-byte unsigned
     * sentinel so the arena agrees at boot; reads answer the live
     * Instance through the carve-out either way). FeatureMap 0x1 is
     * Feature::kPowerThreshold, the inert shadow of
     * mt_meter_feature_mask(), the one-accessor-two-callers discipline.
     * ClusterRevision 1 is LIVE (no Read() case) and is
     * MeterIdentification/Metadata.h:20 in THIS tree. The strings and
     * the struct are metadata-only and carry no rows. */
    { MeterIdentification::Id, MeterIdentification::Attributes::MeterType::Id, 1,
      { 0xFF } }, /* null */
    { MeterIdentification::Id, MeterIdentification::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { MeterIdentification::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* DeviceEnergyManagement. ESAState's shadow seeds kOnline (1), the
     * delegate default and the spec's pre-first-push answer
     * (AT_MT_SPEC.md 3.25); ESAType 0 (kEvse), ESACanGenerate false and
     * OptOutState 0 (kNoOptOut) are the zero-fill matching the delegate
     * defaults, all Instance-served with carve-out reads either way.
     * ClusterRevision 4 is LIVE (Instance::Read() falls through for it) and
     * is DeviceEnergyManagement/Metadata.h:20 in THIS tree. The DEMMode
     * revision is LIVE too (the ModeBase no-default-arm rule the RVC rows
     * above document): DeviceEnergyManagementMode/Metadata.h:20 says 2.
     *
     * FeatureMap is the catalogue's FIRST variant-dependent boot value.
     * Until batch 8 it had no row here at all and was written by a special
     * case in seed_slots(); the variant column retired that (the s_seeds
     * header comment). Variant 0 is Feature::kPowerAdjustment (0x1) and
     * variant 1 is 0, the same two values, and the create path still hands
     * the identical `variant == 0` predicate to mt_matter_dem_register() so
     * the seeded shadow and the Instance's own mask agree by construction.
     * Both variants share device type 0x050D, which is why the devtype
     * column could never have split them. */
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::ESAState::Id, 1, { 0x01 } },
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 }, 0, seed_variant(0) },
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 }, 0, seed_variant(1) },
    { DeviceEnergyManagement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x04, 0x00 } },
    { DeviceEnergyManagementMode::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x02, 0x00 } },

    /* ---- catalogue batch 7b: the delegate-served energy pair --------- */

    /* WaterHeaterManagement. THE ONE INERT REVISION SEED IN THE ENERGY
     * FAMILY: Instance::Read() serves ClusterRevision itself
     * (water-heater-management-server.cpp:146-147, kClusterRevision = 2 at
     * :40), so this row is a shadow kept equal to that served value, the
     * Thermostat discipline; every OTHER energy revision seed is live.
     * HeaterTypes 0, HeatDemand 0, TankVolume 0, TankPercentage 0 and
     * BoostState 0 (kInactive) are the zero-fill matching the
     * HearthWhmDelegate defaults (all Instance-served with carve-out reads
     * either way).
     *
     * FeatureMap is the catalogue's SECOND variant-dependent boot value and
     * had no row here either until batch 8's variant column retired its
     * special case. Variant 0 is Feature::kEnergyManagement |
     * Feature::kTankPercent (0x3, the value the C6 hand-sets around its two
     * broken feature helpers) and variant 1 is 0; the create path still
     * hands the identical `variant == 0` predicate to
     * mt_matter_whm_register(), whose Instance snapshots mFeature at
     * construction and answers the fabric-side read from it, so seed and
     * Instance agree by construction. Both variants share device type
     * 0x050F. */
    { WaterHeaterManagement::Id, WaterHeaterManagement::Attributes::FeatureMap::Id, 4,
      { 0x03, 0x00, 0x00, 0x00 }, 0, seed_variant(0) },
    { WaterHeaterManagement::Id, WaterHeaterManagement::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 }, 0, seed_variant(1) },
    { WaterHeaterManagement::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },
    /* WaterHeaterMode. ClusterRevision 1 is LIVE (the ModeBase AAI has no
     * revision case, the RVC rows' rule on the fourth alias) and is
     * WaterHeaterMode/Metadata.h:20 in THIS tree. CurrentMode 0 and
     * FeatureMap 0 are the zero-fill, the DEMMode shape. */
    { WaterHeaterMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- catalogue batch 8: the composed appliances ------------------- */

    /* Cooktop's OnOff FeatureMap: Feature::kOffOnly (0x4, OnOff/Enums.h:85),
     * device-type-qualified against the lighting types' wildcard 0x01, the
     * pump and room air conditioner rows' exact mechanism. Mandatory rather
     * than optional here: Cooktop.xml:69-74 makes OFFONLY mandatory inside a
     * mandatory OnOff, which is also why cooktopClusters declares its own
     * incoming command list (the cooktop audit note). ClusterRevision rides
     * the shared OnOff row (6) and OnOff itself is the zero-fill false. */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x04, 0x00, 0x00, 0x00 }, kDtCooktop },

    /* RefrigeratorAlarm. Mask 1 and Supported 1 are the C6's config
     * defaults (esp_matter_cluster.h:926-930) and the values AT_MT_SPEC.md
     * 2364-2366 documents to hosts: bit 0 (DoorOpen) supported and unmasked,
     * every other bit neither. State 0 and FeatureMap 0 are the zero-fill,
     * spelled out anyway for State because it is the attribute this cluster
     * exists for. All three are LIVE arena values, not shadows: the server
     * is a singleton that reads and writes them through the generated
     * Accessors, so nothing intercepts them. ClusterRevision 1 is
     * RefrigeratorAlarm/Metadata.h:20 in THIS tree. */
    { RefrigeratorAlarm::Id, RefrigeratorAlarm::Attributes::Mask::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { RefrigeratorAlarm::Id, RefrigeratorAlarm::Attributes::State::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 } },
    { RefrigeratorAlarm::Id, RefrigeratorAlarm::Attributes::Supported::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { RefrigeratorAlarm::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* RefrigeratorAndTemperatureControlledCabinetMode. ClusterRevision 2 is
     * LIVE (the ModeBase AAI has no revision case, the RVC rows' standing
     * rule) and is this tree's own Metadata.h:20. It DIVERGES from the C6,
     * which says 3 (esp_matter_cluster_revisions.h:270-272); the port's
     * convention is this tree's own kRevision and has been since batch 2, so
     * 2 is right here and the divergence is disclosable rather than a bug.
     * CurrentMode 0 and FeatureMap 0 are the zero-fill, the DEMMode
     * shape. */
    { RefrigeratorAndTemperatureControlledCabinetMode::Id,
      Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* Cook surface's OnOff FeatureMap: kOffOnly again, the cooktop's row one
     * endpoint down, and the second consumer of that device-type
     * qualifier. */
    { OnOff::Id, OnOff::Attributes::FeatureMap::Id, 4, { 0x04, 0x00, 0x00, 0x00 },
      kDtCookSurface },

    /* TemperatureControl, and the reason attr_seed grew a variant column.
     * The FeatureMap rows are variant-qualified and device-type-WILDCARD,
     * because 0x0071 and 0x0077 need the identical pair of values: variant 0
     * is kTemperatureNumber | kTemperatureStep (0x05) and variant 1 is
     * kTemperatureLevel (0x02), from TemperatureControl::Feature
     * (Enums.h:34-36). Getting one of these wrong does not merely misreport
     * a capability, it makes SetTemperature answer Failure or InvalidCommand
     * on a healthy endpoint (the temperatureControl*Attrs audit note).
     *
     * The four number-variant values are esp-matter's own feature-config
     * defaults (esp_matter_feature.h:970-975, :997-1001) so the two
     * platforms boot a cabinet identically: setpoint 1, min 0, max 10, step
     * 1, all INT16S little-endian. SelectedTemperatureLevel 1 is the
     * level-variant default (esp_matter_feature.h:985-988). ClusterRevision
     * 1 is TemperatureControl/Metadata.h:20 in THIS tree and esp-matter
     * agrees for once (esp_matter_cluster_revisions.h:262-264). Every one of
     * these is a LIVE arena value: this cluster has no Instance, and its one
     * AAI serves SupportedTemperatureLevels alone. */
    { TemperatureControl::Id, TemperatureControl::Attributes::FeatureMap::Id, 4,
      { 0x05, 0x00, 0x00, 0x00 }, 0, seed_variant(0) },
    { TemperatureControl::Id, TemperatureControl::Attributes::FeatureMap::Id, 4,
      { 0x02, 0x00, 0x00, 0x00 }, 0, seed_variant(1) },
    { TemperatureControl::Id, TemperatureControl::Attributes::TemperatureSetpoint::Id, 2,
      { 0x01, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::MinTemperature::Id, 2,
      { 0x00, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::MaxTemperature::Id, 2,
      { 0x0A, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::Step::Id, 2, { 0x01, 0x00 } },
    { TemperatureControl::Id, TemperatureControl::Attributes::SelectedTemperatureLevel::Id, 1,
      { 0x01 } },
    { TemperatureControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* OvenMode. ClusterRevision 1 is LIVE (the ModeBase AAI has no revision
     * case, the standing rule on the sixth alias) and is
     * OvenMode/Metadata.h:20 in THIS tree; the C6 hand-set 2 from the 1.5.1
     * XML, a disclosable divergence. CurrentMode 0 and FeatureMap 0 are the
     * zero-fill.
     *
     * OvenCavityOperationalState. It reuses opStateAttrs, so it also reuses
     * that list's seed values, but NOT through the same rows: every
     * OperationalState seed here is keyed on cluster id, and the cavity is a
     * different cluster id, so it needs its own three. CurrentPhase 0xFF is
     * null (these cavities publish no phases, so PhaseList reads null and
     * CurrentPhase with it), OperationalState 0 is kStopped, FeatureMap 0 is
     * the zero-fill. ClusterRevision 1 is this tree's Metadata.h and is
     * INERT: Instance::Read() answers ClusterRevision itself
     * (operational-state-server.cpp:408-409), so this row is a shadow kept
     * equal to the served value, the Thermostat and WHM discipline. */
    { OvenMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },
    { OvenCavityOperationalState::Id, OperationalState::Attributes::CurrentPhase::Id, 1,
      { 0xFF } }, /* null */
    { OvenCavityOperationalState::Id, OperationalState::Attributes::OperationalState::Id, 1,
      { 0x00 } },
    { OvenCavityOperationalState::Id, Globals::Attributes::ClusterRevision::Id, 2,
      { 0x01, 0x00 } },

    /* MicrowaveOvenMode. ClusterRevision 1 is LIVE (the ModeBase AAI has no
     * revision case, the standing rule on the seventh alias) and is
     * MicrowaveOvenMode/Metadata.h:20 in THIS tree; the C6 says 2, a
     * disclosable divergence. CurrentMode 0 and FeatureMap 0 are the
     * zero-fill.
     *
     * MicrowaveOvenControl. All four declared values are Instance-served
     * shadows kept equal to what Instance::Read() answers at boot, the
     * FanControl agreement discipline (the mwocAttrs audit note explains why
     * there is no k_instance_served carve-out and why the shadow is expected
     * to go stale after the first cooking command):
     *   CookTime 30, kDefaultCookTimeSec: the constant at
     *     microwave-oven-control-server.h:34 and the Instance's own
     *     mCookTimeSec initialiser at :88, which Instance::Read() answers
     *     CookTime from. The audit's estimate said 0; 30 is what the server
     *     actually serves from the first read onward, and a shadow that
     *     disagrees with its source on boot would be wrong for no gain.
     *     (Fix round M6: an earlier version of this note also claimed the
     *     cluster XML gives CookTime a default of 30. It does not:
     *     data_model/1.5/clusters/MicrowaveOvenControl.xml declares CookTime
     *     with a constraint and no default. The SDK evidence alone carries
     *     the decision and the XML claim is withdrawn.)
     *   MaxCookTime 86400, HearthMwocDelegate::GetMaxCookTimeSec(), the C6's
     *     same constant and the XML's own maximum.
     *   PowerSetting 100, kDefaultMaxPowerNum and the delegate's
     *     m_power_setting initialiser.
     *   FeatureMap 0x01, Feature::kPowerAsNumber, MANDATORY rather than
     *     chosen (the mwocAttrs note), and the value Init() will refuse to
     *     start without.
     * ClusterRevision 1 is MicrowaveOvenControl/Metadata.h:20.
     *
     * The microwave's OperationalState seeds ride the existing 0x0060 rows
     * except CountdownTime, which no earlier device type declares: 0xFFFFFFFF
     * is the uint32 null sentinel, matching the NullNullable the delegate's
     * GetCountdownTime() answers, so the inert shadow and the served value
     * agree on null. */
    { MicrowaveOvenMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },
    { OperationalState::Id, OperationalState::Attributes::CountdownTime::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::CookTime::Id, 4,
      { 0x1E, 0x00, 0x00, 0x00 } },
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::MaxCookTime::Id, 4,
      { 0x80, 0x51, 0x01, 0x00 } },
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::PowerSetting::Id, 1, { 0x64 } },
    { MicrowaveOvenControl::Id, MicrowaveOvenControl::Attributes::FeatureMap::Id, 4,
      { 0x01, 0x00, 0x00, 0x00 } },
    { MicrowaveOvenControl::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x01, 0x00 } },

    /* ---- the EVSE round: the last device type ------------------------- */

    /* EnergyEvse. Every one of these slots is an INERT SHADOW: Instance::Read()
     * serves all 23 attributes from the delegate (energy-evse-server.cpp:68-133)
     * and the DE397 carve-out serves the AT side, so nothing reads these values
     * after boot. They are seeded anyway, and correctly, for the reason the
     * MeterIdentification MeterType row is: the arena and the served value must
     * agree at boot, or a future reader comparing them has to work out which
     * one lied.
     *
     * The three enums (State kNotPluggedIn, SupplyState kDisabled, FaultState
     * kNoError) are all zero and ride the zero-fill; the seven nullables carry
     * their type's null sentinel, which is what the delegate's default-
     * constructed Nullable<> members answer, so shadow and cache agree on
     * "nothing pushed yet". The six 8-byte declarations have no slot at all
     * (the quiet table) and so no row here.
     *
     * FeatureMap is the catalogue's FOURTH variant-dependent boot value, after
     * DEM's, WHM's and TemperatureControl's, and it uses the same variant
     * column: variant 0 is kChargingPreferences | kSoCReporting (0x3) and
     * variant 1 is kChargingPreferences alone (0x1). PREF is on in BOTH, which
     * is the whole point of the pair: the C6's esp-matter adds charging
     * preferences unconditionally, and mt_matter_evse_register() hands the
     * Instance the identical predicate, so the seeded shadow and the
     * Instance's own mFeature snapshot agree by construction. Both variants
     * share device type 0x050C, which is why the devtype column could not have
     * split them.
     *
     * ClusterRevision 3 is LIVE (Read() has no case for it and falls through
     * to ember, energy-evse-server.cpp:131-133) and is
     * EnergyEvse/Metadata.h:20 in THIS tree. */
    { EnergyEvse::Id, EnergyEvse::Attributes::ChargingEnabledUntil::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeStartTime::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeTargetTime::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::NextChargeTargetSoC::Id, 1, { 0xFF } },  /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::StateOfCharge::Id, 1, { 0xFF } },        /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::SessionID::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::SessionDuration::Id, 4,
      { 0xFF, 0xFF, 0xFF, 0xFF } }, /* null */
    { EnergyEvse::Id, EnergyEvse::Attributes::FeatureMap::Id, 4, { 0x03, 0x00, 0x00, 0x00 }, 0,
      seed_variant(0) },
    { EnergyEvse::Id, EnergyEvse::Attributes::FeatureMap::Id, 4, { 0x01, 0x00, 0x00, 0x00 }, 0,
      seed_variant(1) },
    { EnergyEvse::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x03, 0x00 } },

    /* EnergyEvseMode. ClusterRevision 2 is LIVE (the ModeBase AAI has no
     * revision case, the standing rule on the eighth and last alias) and is
     * EnergyEvseMode/Metadata.h:20 in THIS tree. CurrentMode 0 and FeatureMap 0
     * are the zero-fill, the DEMMode shape. */
    { EnergyEvseMode::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x02, 0x00 } },

    /* THE EVSE'S OWN DEM FEATUREMAP, and the one row in this table whose
     * whole job is to override two others. The DEM rows above are
     * variant-qualified (variant 0 is kPowerAdjustment, variant 1 is 0)
     * because the DEM and battery-storage device types put PA on their
     * variant 0. The EVSE's variant axis is SOC, not PA: it carries the
     * over-delivered DEM cluster with NO PowerAdjustment on EITHER variant
     * (mt_add_dem_triple(ep, false, ...) on the C6, and demReportOnlyAttrs in
     * both cluster lists here), so a variant-0 EVSE inheriting the wildcard
     * variant-0 row would advertise PA with none of its four obligations met:
     * the iron rule (ARCHITECTURE.md 8.12) broken by a table lookup.
     *
     * One devtype-qualified row settles it for both variants, because the
     * precedence rule scores a devtype match (2) above a variant match (1).
     * That is the first time in this table those two qualifiers compete, which
     * the batch-8 precedence comment said would need arguing when it happened:
     * the argument is that "which device type is this" is a stronger statement
     * about an endpoint than "which variant is this", since a variant only
     * means anything relative to a device type in the first place. */
    { DeviceEnergyManagement::Id, DeviceEnergyManagement::Attributes::FeatureMap::Id, 4,
      { 0x00, 0x00, 0x00, 0x00 }, 0x050C },
};

/* Fills this endpoint's block. Walks the same two predicates count_slots()
 * used to size it, so slot_count lands on slot_capacity exactly; the bound
 * below is a backstop against those two drifting, not a normal path. */
void seed_slots(dyn_endpoint *d)
{
    attr_slot *slots = block_slots(*d);
    d->slot_count = 0;
    for (uint8_t c = 0; c < d->ep_type->clusterCount; c++) {
        const EmberAfCluster &cl = d->ep_type->cluster[c];
        if (!cluster_gets_slots(cl)) {
            continue;
        }
        for (uint16_t a = 0; a < cl.attributeCount; a++) {
            const EmberAfAttributeMetadata &md = cl.attributes[a];
            if (!attr_gets_slot(md)) {
                /* Named here rather than in the predicate so an attribute
                 * too wide for a slot is visible in the log instead of
                 * silently unserved; the ARRAY globals are expected and
                 * stay quiet. Catalogue batch 4 widened the quiet set to
                 * CHAR_STRING and STRUCT: those are deliberate
                 * metadata-only declarations (PowerSource/ModeSelect
                 * Description, OperationalState's OperationalError), made
                 * so AttributeList stays truthful for attributes this
                 * arena cannot hold; each carries its own audit note at
                 * the declaration. Anything ELSE too wide is still a
                 * mistake worth shouting about. */
                if (md.attributeType != ZAP_TYPE(ARRAY) &&
                    md.attributeType != ZAP_TYPE(CHAR_STRING) &&
                    md.attributeType != ZAP_TYPE(STRUCT) &&
                    !attr_quiet_no_slot(cl.clusterId, md.attributeId)) {
                    /* Catalogue batch 7a: the quiet set gained the narrow
                     * per-(cluster, attribute) table above for the proven
                     * Instance-served 64-bit declarations, DE407 option C.
                     * Any over-wide scalar NOT in that table still shouts:
                     * that shout is the ruling's drift alarm, never
                     * silence it by type. */
                    LOG_ERR("attr 0x%08X on cluster 0x%08X is %u bytes, a slot holds %u",
                            (unsigned)md.attributeId, (unsigned)cl.clusterId, (unsigned)md.size,
                            (unsigned)kSlotDataBytes);
                }
                continue;
            }
            if (d->slot_count >= d->slot_capacity) {
                LOG_ERR("devtype 0x%04X block holds %u slots, seeding wanted more",
                        (unsigned)d->type->id, (unsigned)d->slot_capacity);
                return;
            }
            attr_slot &s = slots[d->slot_count++];
            s.cluster = cl.clusterId;
            s.attr = md.attributeId;
            s.size = (uint8_t)md.size;
            memset(s.data, 0, sizeof(s.data));

            /* AirQuality's FeatureMap is the one boot value that is not a
             * literal in s_seeds. mt_matter.h makes
             * mt_air_quality_feature_mask() the single source of truth for
             * which of Fair/Moderate/VeryPoor/ExtremelyPoor are enabled,
             * precisely so an ember feature map and a server Instance's
             * BitMask<Feature> cannot be edited apart; honour that here
             * rather than transcribing the bits a second time. Written
             * little-endian, the same convention as every seed row. */
            if (cl.clusterId == AirQuality::Id &&
                md.attributeId == Globals::Attributes::FeatureMap::Id) {
                uint32_t mask = mt_air_quality_feature_mask();
                for (uint8_t b = 0; b < s.size && b < sizeof(s.data); b++) {
                    s.data[b] = (uint8_t)(mask >> (8 * b));
                }
                continue;
            }

            /* Catalogue batch 7a's DEM FeatureMap special case and batch
             * 7b's WHM one both stood HERE until batch 8 gave attr_seed a
             * variant column and retired them into ordinary rows (the
             * s_seeds comment). Nothing about their contract changed: the
             * create path still hands the identical variant predicate to
             * mt_matter_dem_register() / mt_matter_whm_register(), whose
             * Instance masks are what the fabric-side FeatureMap reads
             * actually answer, so the seed and the Instance still agree by
             * construction with the variant as the single shared source.
             * Only where the literal lives moved. */

            /* The most specific matching seed row wins: a row naming this
             * device type AND this variant beats one naming only the device
             * type, which beats one naming only the variant, which beats the
             * bare wildcard. A row whose qualifier is present but does not
             * match is skipped. Scored rather than short-circuited so the
             * table's row order stays irrelevant (batch 2's loop stopped
             * early on a devtype hit, which was the same property with two
             * levels instead of four). */
            const attr_seed *chosen = nullptr;
            int chosen_score = -1;
            for (auto &seed : s_seeds) {
                if (seed.cluster != cl.clusterId || seed.attr != md.attributeId) {
                    continue;
                }
                if (seed.devtype != 0 && seed.devtype != d->type->id) {
                    continue;
                }
                if (seed.variant != kSeedAnyVariant && seed.variant != seed_variant(d->variant)) {
                    continue;
                }
                const int score = (seed.devtype != 0 ? 2 : 0) +
                                  (seed.variant != kSeedAnyVariant ? 1 : 0);
                if (score > chosen_score) {
                    chosen = &seed;
                    chosen_score = score;
                }
            }
            if (chosen != nullptr) {
                memcpy(s.data, chosen->bytes, (chosen->size < s.size) ? chosen->size : s.size);
            }
        }
    }
}

} /* namespace */

/*
 * Strong override of the generated weak stub in
 * zap-generated/callback-stub.cpp. Nothing in the SDK calls
 * DoorLockServer::InitEndpoint() on its own; this hook is where an app is
 * expected to, and ember dispatches it per endpoint by cluster id from
 * emberAfClusterInitCallback() inside initializeEndpoint()
 * (attribute-storage.cpp:480), for dynamic endpoints as well as fixed ones.
 * See the audit note on doorLockAttrs above for why the similarly-named
 * emberAfDoorLockClusterServerInitCallback is NOT the hook to define, and
 * why this does not belong at the B388 call site.
 *
 * InitEndpoint() rather than InitServer(). InitServer() is explicitly "a
 * deprecated alias for InitEndpoint with no delegate"
 * (door-lock-server.h:121) whose whole body is a call to InitEndpoint()
 * followed by "We have no way to communicate this error, so just log it"
 * (door-lock-server.cpp:87-95). We do have a way: mt_devtype_create() is
 * still on the stack. The C6 and the nRF lock sample both call the alias;
 * that is not a parity constraint worth keeping, because the alias's only
 * difference is throwing away the one thing this port can act on.
 *
 * The error is worth acting on. InitEndpoint() fails when getContext()
 * cannot resolve a slot (:105-110), and without that context
 * HandleRemoteLockOperation() has nowhere to record wrong-code state and
 * the lock does not work at all: an endpoint that presents a DoorLock
 * cluster a controller can invoke and that cannot actually operate is
 * exactly the create failure the composition's stop-at-failure abort exists
 * for. It also sets LockState null and ActuatorEnabled true (:102-106),
 * which the seeds above are chosen to agree with so no +MTATTR URC fires
 * at rebuild time.
 *
 * The result is parked in s_lock_init_* rather than acted on here because
 * this callback returns void; mt_devtype_create() reads it back the moment
 * emberAfSetDynamicEndpoint() returns.
 */
void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
    s_lock_init_ep = endpoint;
    s_lock_init_err = DoorLockServer::Instance().InitEndpoint(endpoint);
    if (s_lock_init_err != CHIP_NO_ERROR) {
        LOG_ERR("DoorLock InitEndpoint(%u) failed: %" CHIP_ERROR_FORMAT, (unsigned)endpoint,
                s_lock_init_err.Format());
    }
}

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
 * The store accessors (store reclaim round; contract in mt_dyn_store.h).
 * Same shape as mt_dyn_attr_slot() above: walk the header table for the
 * live endpoint, then follow its block pointer, under the caller's
 * StackLock or on the CHIP thread. The type check answers nullptr for an
 * endpoint whose device type carries no such store, so the AT bridges'
 * defensive arms and the SDK readers' no-store answers stay exactly what
 * the exhausted/unclaimed .bss pool used to produce. The offset arithmetic
 * is the block layout's third knower (the note beside K_HEAP_DEFINE):
 * stores sit past the dv and slot regions, at the offset store_offset()
 * answers, the same alignment-correct walk (store_walk, fix round 2) the
 * create path constructed them through.
 */
mt_mode_store_t *mt_dyn_mode_store(EndpointId ep)
{
    for (auto &d : s_dyn) {
        if (!d.used || d.ep_id != ep) {
            continue;
        }
        if (!type_has_cluster(d.ep_type, ModeSelect::Id)) {
            return nullptr;
        }
        return reinterpret_cast<mt_mode_store_t *>(
            static_cast<uint8_t *>(d.block) +
            block_bytes(d.ep_type->clusterCount, d.slot_capacity) +
            store_offset(d.ep_type, ModeSelect::Id));
    }
    return nullptr;
}

mt_chime_store_t *mt_dyn_chime_store(EndpointId ep)
{
    for (auto &d : s_dyn) {
        if (!d.used || d.ep_id != ep) {
            continue;
        }
        if (!type_has_cluster(d.ep_type, Chime::Id)) {
            return nullptr;
        }
        return reinterpret_cast<mt_chime_store_t *>(
            static_cast<uint8_t *>(d.block) +
            block_bytes(d.ep_type->clusterCount, d.slot_capacity) +
            store_offset(d.ep_type, Chime::Id));
    }
    return nullptr;
}

/* Catalogue batch 5: cluster-keyed, unlike the two accessors above,
 * because the RVC block carries two ModeBase stores and store_offset()
 * places each by its own cluster id. The cluster check doubles as the
 * only-these guard: any cluster this type does not carry (including a
 * non-ModeBase id) answers nullptr, the callers' defensive arm.
 *
 * THIS LIST IS THE SECOND COPY OF THE MODEBASE ID SET, and it has to be kept
 * in step with mt_matter_modebase_set()'s accept set in mt_matter_zephyr.cpp
 * by hand, because the two guards mean different things (this one is "does a
 * store exist at an offset for this id", that one is "does the wire contract
 * admit this id") and neither can be derived from the other. When the EVSE
 * round added the EnergyEvseMode row to kStoreWalk, this list was NOT
 * updated, which was invisible: the store was allocated and constructed in
 * the block, every accessor answered nullptr for it, and the delegate's
 * count-0 arm served the placeholder exactly as it would have with no store
 * at all. A composition paid 306 bytes an endpoint for a region nothing
 * could reach. It became visible only when 0x009D was admitted to
 * AT+MTMODES and the feed had somewhere to fail. There is no build check for
 * this; the third copy, if one ever appears, should be derived from
 * kStoreWalk instead. */
mt_mb_store_t *mt_dyn_mb_store(EndpointId ep, ClusterId cluster)
{
    if (cluster != RvcRunMode::Id && cluster != RvcCleanMode::Id &&
        cluster != DeviceEnergyManagementMode::Id && cluster != WaterHeaterMode::Id &&
        cluster != RefrigeratorAndTemperatureControlledCabinetMode::Id &&
        cluster != OvenMode::Id && cluster != MicrowaveOvenMode::Id &&
        cluster != EnergyEvseMode::Id) {
        return nullptr;
    }
    for (auto &d : s_dyn) {
        if (!d.used || d.ep_id != ep) {
            continue;
        }
        if (!type_has_cluster(d.ep_type, cluster)) {
            return nullptr;
        }
        return reinterpret_cast<mt_mb_store_t *>(
            static_cast<uint8_t *>(d.block) +
            block_bytes(d.ep_type->clusterCount, d.slot_capacity) +
            store_offset(d.ep_type, cluster));
    }
    return nullptr;
}

/* Catalogue batch 8: the label store. The one accessor whose presence test
 * is not "does the type carry the cluster" but "does the type declare
 * SupportedTemperatureLevels", the same question store_walk() and the
 * create-path construction ask, because both variants of both carrying
 * device types have the cluster and only one variant has the store. A
 * TemperatureNumber cabinet therefore answers nullptr here with its
 * TemperatureControl cluster fully live, which is exactly the state
 * AT+MTTEMPLEVELS renders as +MTERR:4 (the header's contract). */
mt_temp_levels_store_t *mt_dyn_temp_levels_store(EndpointId ep)
{
    for (auto &d : s_dyn) {
        if (!d.used || d.ep_id != ep) {
            continue;
        }
        if (!type_has_cluster(d.ep_type, TemperatureControl::Id) ||
            !type_has_attr(d.ep_type, TemperatureControl::Id,
                           TemperatureControl::Attributes::SupportedTemperatureLevels::Id)) {
            return nullptr;
        }
        return reinterpret_cast<mt_temp_levels_store_t *>(
            static_cast<uint8_t *>(d.block) +
            block_bytes(d.ep_type->clusterCount, d.slot_capacity) +
            store_offset(d.ep_type, TemperatureControl::Id));
    }
    return nullptr;
}

/* ---- mt_devtypes.h --------------------------------------------------- */

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
 * Catalogue batch 8 made this real. The rule itself is parent_policy_ok()
 * beside the registry, constexpr so the shape maps can be checked against it
 * at compile time; this is only the extern "C" door core/mt/mt_at.c knocks
 * on, called on the AT+MTEP= line itself (mt_at.c:1897-1905) so a bad
 * pairing answers +MTERR:1 on the line that proposed it and consumes no
 * staging slot.
 */
extern "C" bool mt_devtype_parent_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    return parent_policy_ok(devtype_id, variant, parent_devtype);
}

extern "C" int mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype,
                                 uint16_t parent_ep_id, uint16_t *out_ep_id)
{
    if (!out_ep_id) {
        return -1;
    }

    /* The lock covers the whole function, so every s_dyn, s_ep_heap_used and
     * s_next_ep_id mutation happens under it and the invariant is statable:
     * the dynamic endpoint header table, the endpoint block heap and the
     * blocks themselves are only ever touched with the CHIP stack lock held.
     * emberAfSetDynamicEndpoint() below enables the endpoint synchronously
     * and runs cluster init callbacks that read this endpoint's block back
     * through the external-storage callbacks, so the two must not be
     * lockable separately. */
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

    /* Re-checked here, not just at AT+MTEP staging time: a composition blob
     * persisted by a wider build can name a variant this build does not
     * implement, and silently dropping it would hand a commissioned device
     * an endpoint that is not what its stored composition says. Fail, and
     * let the rebuild loop abort. */
    if (variant > type->max_variant) {
        LOG_ERR("devtype 0x%04X variant %u not supported by this build", (unsigned)devtype_id,
                (unsigned)variant);
        return -1;
    }

    /* Catalogue batch 7a: the variant's own cluster list. A max_variant-1
     * row carries a second declared EmberAfEndpointType (ep_type_v1, the
     * registry comment) because this port cannot tear a cluster out of a
     * built list at runtime; every walk below and every block-layout
     * accessor later reads this chosen list, never type->ep_type
     * directly.
     *
     * Catalogue batch 8: a row whose cluster set depends on its PARENT
     * carries a shape map instead, and the pair (variant, parent_devtype)
     * selects from it. The map's domain is proven equal to
     * parent_policy_ok()'s accept set at compile time
     * (shape_domain_matches_policy() beside the registry), so the no-match
     * arm below cannot be reached by any composition AT+MTEP accepted; it is
     * the drift alarm for the day someone edits one encoding and not the
     * other in a way the assertion somehow admits, and it aborts rather than
     * falling back, because a fallback would serve a cabinet whose stored
     * composition says something else.
     *
     * A DELIBERATE DIVERGENCE FROM THE C6, and the better half of it.
     * The C6 builds the base endpoint, calls set_parent_endpoint(), and only
     * THEN augments the cabinet with its parent-conditional clusters
     * (mt_devtypes.cpp:2802-2837), so a failed augment leaves a half-built
     * child already attached in the parent's tree (ARCHITECTURE.md:1447-1452
     * records the consequence). This port SELECTS the whole cluster set
     * here, before emberAfSetDynamicEndpoint() is called at all, so a
     * composed endpoint appears complete or does not appear: there is no
     * moment at which a parent's PartsList names a child that is still being
     * assembled. Same derived-never-stored rule as the C6 (the blob carries
     * device type, variant and parent index only, AT_MT_SPEC.md 495-505);
     * only the ordering differs. */
    const EmberAfEndpointType *ep_type = nullptr;
    if (!type->shapes.empty()) {
        for (auto &s : type->shapes) {
            if (s.variant == variant && s.parent_devtype == parent_devtype) {
                ep_type = s.ep_type;
                break;
            }
        }
        if (ep_type == nullptr) {
            LOG_ERR("devtype 0x%04X variant %u under parent 0x%04X has no declared cluster set; "
                    "the shape map and mt_devtype_parent_ok() have drifted apart",
                    (unsigned)devtype_id, (unsigned)variant, (unsigned)parent_devtype);
            return -1;
        }
    } else {
        ep_type = (variant == 1 && type->ep_type_v1 != nullptr) ? type->ep_type_v1 : type->ep_type;
    }

    /*
     * ---- the per-endpoint delegate handout (catalogue batch 3) ---------
     *
     * The pattern batch 4 reuses for every cluster whose server needs one
     * object per endpoint. It has two halves, and WHERE each half goes is
     * dictated by the SDK, not by taste:
     *
     *   BEFORE anything is consumed: take a pool slot. A pool exhaustion
     *   must abort this create BEFORE an endpoint id, a header entry or a
     *   heap block is spent, because a served endpoint whose command
     *   adjudication has nowhere to go is worse than no endpoint at all
     *   (the boot rebuild aborts the whole composition on any single
     *   failure, CLAUDE.md, "A failed endpoint::create() consumes no
     *   endpoint ID").
     *
     *   AFTER emberAfSetDynamicEndpoint() succeeds: register it. This is
     *   the ordering DELTA from the C6, and it is forced.
     *   ValveConfigurationAndControl::SetDefaultDelegate() resolves its
     *   table slot with emberAfGetClusterServerEndpointIndex()
     *   (valve-configuration-and-control-cluster.cpp:264), which answers
     *   kEmberInvalidEndpointIndex for an endpoint that is not yet
     *   configured AND enabled (attribute-storage.cpp:923-935). On the C6
     *   the delegate goes the other way round, INTO the config struct
     *   before esp_matter's create() consumes it, because esp_matter defers
     *   the registration itself. Here there is no config struct and no
     *   deferral: register too early and the call is dropped.
     *
     *   Dropped SILENTLY, which is why the registration is read back below
     *   rather than trusted. SetDefaultDelegate() returns void and its only
     *   guard is `if (ep < kValveConfigurationAndControlDelegateTableSize)`
     *   with no else and no log (:267). A valve whose delegate never landed
     *   still answers Open and Close with Success, still updates its
     *   attributes and still looks healthy on a controller, while every
     *   +MTCMD the host is waiting for is simply never raised, and the
     *   server's own 1 Hz tick never runs either (:243 returns early with
     *   no delegate). That is precisely the failure this firmware must not
     *   ship silently.
     *
     * ---- what each of the two checks is actually for --------------------
     *
     * Neither is expected to fire, and they are not the same kind of guard.
     * Saying so plainly, because "abort here, keep going there" reads like
     * an inconsistency otherwise:
     *
     *   The pre-create pool check ABORTS. Its job is to keep two numbers
     *   tied: the pool is sized kServiceableEndpoints and so is the header
     *   table, so exhaustion cannot be reached while that holds. If a later
     *   round shrinks the pool, or gives one device type two delegates,
     *   this becomes reachable immediately, and then it is a genuine
     *   create failure. Aborting costs nothing here because nothing has
     *   been spent yet, and a served endpoint whose commands are never
     *   adjudicated is worse than no endpoint.
     *
     *   The post-create read-back does NOT abort, and its failure is
     *   unreachable BY ORDERING rather than by arithmetic: it runs below a
     *   successful emberAfSetDynamicEndpoint(), so the endpoint is
     *   configured and enabled and the index resolves, and the delegate
     *   table is sized fixed + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT so
     *   that index is in range. It is belt and braces against the silent
     *   drop described above. If it ever did fire, the endpoint is by then
     *   live, correctly seeded and correct in every other respect, and
     *   there is no clean way to take it back: emberAfClearDynamicEndpoint()
     *   would leave CHIP's own error paths holding the dataVersions span
     *   (see the allocate-only note beside K_HEAP_DEFINE) and the block is
     *   never freed either way. Keeping a valve that reports state
     *   correctly but does not adjudicate, and shouting about it, is the
     *   lesser evil against tearing down a healthy endpoint and renumbering
     *   everything after it.
     *
     * mt_matter.h's contract for the pair is honoured as written: alloc
     * hands out an opaque void*, set_endpoint fixes the endpoint once it is
     * known. On this platform set_endpoint does the SetDefaultDelegate()
     * call as well, which is exactly what "fix the real endpoint after
     * create" means here; this file still never names a CHIP delegate type.
     */
    /* Catalogue batch 4: the OperationalState trio reuses the two-halves
     * pattern verbatim. The alloc takes the CLUSTER id (mt_matter.h fixes
     * it at alloc time; only the base 0x0060 exists in this catalogue),
     * and set_endpoint below does more than the valve's: it
     * placement-constructs the per-endpoint Instance and runs Init(),
     * which is only legal below a successful emberAfSetDynamicEndpoint()
     * because Init() bails on emberAfContainsServer (see the opStateAttrs
     * audit note). Pool exhaustion aborts here, before anything is spent,
     * for the valve's exact reasons. */
    /* Catalogue batch 8: the pool now serves TWO cluster ids. The heater
     * cabinet carries OvenCavityOperationalState (0x0048), a derived cluster
     * whose Instance is a public subclass of the base one with no data
     * members of its own, so it shares this pool, this delegate class and
     * this raw storage; only the cluster id, fixed at alloc, differs. No
     * device type carries both, so one variable still suffices. */
    /*
     * ---- the EVSE's reserve, and why it is FIRST in the claim block -----
     *
     * mt_matter_evse_reserve() is a count-only gate with no endpoint id, and
     * it is the C6's reserve-before-create fix rendered here. On that
     * platform energy_evse::create() allocates the endpoint, marks it enabled
     * and appends it to the node's own list before the delegate handout can
     * run, so a pool-exhausted third EVSE left a LIVE, delegate-less endpoint
     * behind: reachable through provider::Endpoints(), absent from AT+MTEP?
     * and failing every attribute read. This port's create path has the same
     * shape and the same hazard, one function further along: the Instance is
     * built by mt_matter_evse_register() below a successful
     * emberAfSetDynamicEndpoint(), so the register cannot be the gate either.
     * Hence the split gate, and hence this claim before anything is spent.
     *
     * It is first among the claims for a second reason of its own: the
     * reserve also COMMITS the inbound row staging session (ruling DE419), a
     * 5,608-byte allocation from the staging arena that can genuinely fail,
     * unlike every pool claim below it whose depth is arithmetic. Failing
     * early costs nothing; failing late would strand the claims above it for
     * the rest of the boot.
     */
    if (type_has_cluster(ep_type, EnergyEvse::Id)) {
        if (!mt_matter_evse_reserve()) {
            LOG_ERR("devtype 0x%04X: EVSE delegate pool exhausted (MT_EVSE_MAX %u), or the "
                    "inbound row stage could not be committed; %u of %u serviceable endpoints "
                    "in use, endpoint heap %zu of %zu usable B used; host may declare %u, this "
                    "build serves %u",
                    (unsigned)devtype_id, (unsigned)MT_EVSE_MAX, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints, s_ep_heap_used, kHeapUsableBytes,
                    (unsigned)MT_COMP_MAX_ENDPOINTS, (unsigned)kServiceableEndpoints);
            return -1;
        }
    }

    void *opstate_delegate = nullptr;
    ClusterId opstate_cluster = kInvalidClusterId;
    if (type_has_cluster(ep_type, OperationalState::Id)) {
        opstate_cluster = OperationalState::Id;
    } else if (type_has_cluster(ep_type, OvenCavityOperationalState::Id)) {
        opstate_cluster = OvenCavityOperationalState::Id;
    }
    if (opstate_cluster != kInvalidClusterId) {
        opstate_delegate = mt_matter_opstate_delegate_alloc(opstate_cluster);
        if (opstate_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: opstate delegate pool exhausted (%u slots, one per "
                    "serviceable endpoint); %u of %u serviceable endpoints in use, endpoint "
                    "heap %zu of %zu usable B used; host may declare %u, this build serves %u",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints, s_ep_heap_used,
                    kHeapUsableBytes, (unsigned)MT_COMP_MAX_ENDPOINTS,
                    (unsigned)kServiceableEndpoints);
            return -1;
        }
    }

    /* Catalogue batch 4: mode select's manager handout is the two-halves
     * pattern collapsed to one half. There is no per-endpoint object at
     * all, only the ONE process-global SupportedModesManager, and
     * mt_matter_mode_select_manager() both registers it with the SDK
     * (setSupportedModesManager, an idempotent bare-pointer store, so a
     * second mode select endpoint re-registering is harmless) and returns
     * it for this null check. Nothing to do after create: the manager
     * dispatches on endpoint id internally, and the endpoint learns
     * nothing the manager needs. Cannot fail today (the accessor returns
     * a static object's address); checked anyway so a future refactor
     * that CAN fail aborts before anything is spent, the pool checks'
     * rule. */
    if (type_has_cluster(ep_type, ModeSelect::Id)) {
        if (mt_matter_mode_select_manager() == nullptr) {
            LOG_ERR("devtype 0x%04X: mode select manager unavailable", (unsigned)devtype_id);
            return -1;
        }
    }

    /* Catalogue batch 4: the chime's handout, the trio's pattern with a
     * ChimeServer in place of an Instance. Same two halves, same
     * exhaustion-aborts-before-spending rule. Fix round M3: unlike the
     * opstate and valve claims (which keep batch 4's shape), this claim
     * is handed back through mt_matter_chime_delegate_unclaim() on every
     * failure path below, so a -1 between here and set_endpoint no
     * longer strands a pool slot. */
    void *chime_delegate = nullptr;
    if (type_has_cluster(ep_type, Chime::Id)) {
        chime_delegate = mt_matter_chime_delegate_alloc();
        if (chime_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: chime delegate pool exhausted (%u slots, one per "
                    "serviceable endpoint); %u of %u serviceable endpoints in use, endpoint "
                    "heap %zu of %zu usable B used; host may declare %u, this build serves %u",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints, s_ep_heap_used,
                    kHeapUsableBytes, (unsigned)MT_COMP_MAX_ENDPOINTS,
                    (unsigned)kServiceableEndpoints);
            return -1;
        }
    }

    /* Catalogue batch 5, the RVC's three delegates: two ModeBase (one per
     * (endpoint, cluster) pair, RvcRunMode and RvcCleanMode both on this
     * one endpoint) and one RvcOperationalState (its own pool: the base
     * OperationalState Delegate::SetInstance() VerifyOrDies on sharing,
     * so the trio's pool cannot serve it, and the RVC delegate class
     * additionally carries the GoHome hook). All claimed HERE, before
     * anything is spent, the two-halves rule: for the ModeBase pair the
     * pre-create half matters MORE than for any earlier pool, because the
     * second half's Instance::Init() VerifyOrDies (panics) rather than
     * soft-bailing, so the only acceptable failure mode is this abort
     * before an endpoint id, header entry or block exists. The cluster id
     * is fixed at alloc time (mt_matter.h's contract: the delegate must
     * answer GetModeValueByIndex(0, ...) for its OWN cluster the moment
     * Init() asks, which is before set_endpoint could run a second
     * setter). Like the opstate and valve claims, and unlike the chime's
     * (fix round M3 scoped the unclaim to the chime), a claim stranded by
     * a later failure path is bounded at one create per boot: the rebuild
     * stops at the failure. */
    void *mb_run_delegate = nullptr;
    void *mb_clean_delegate = nullptr;
    if (type_has_cluster(ep_type, RvcRunMode::Id)) {
        mb_run_delegate = mt_matter_modebase_delegate_alloc(RvcRunMode::Id);
        if (mb_run_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (run mode); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    if (type_has_cluster(ep_type, RvcCleanMode::Id)) {
        mb_clean_delegate = mt_matter_modebase_delegate_alloc(RvcCleanMode::Id);
        if (mb_clean_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (clean mode); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    void *rvc_opstate_delegate = nullptr;
    if (type_has_cluster(ep_type, RvcOperationalState::Id)) {
        rvc_opstate_delegate = mt_matter_rvc_opstate_delegate_alloc();
        if (rvc_opstate_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: rvc opstate delegate pool exhausted (%u slots); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* Catalogue batch 7a: the measurement handout's first half. One EPM
     * delegate for any type carrying ElectricalPowerMeasurement, one
     * PowerTopology delegate for any carrying PowerTopology, both from the
     * MT_MEAS_MAX (8) pools in mt_matter_zephyr.cpp, the C6's exact pool
     * depths (DE407). Exhaustion aborts HERE, before an endpoint id, a
     * header entry or a heap block is spent, the two-halves rule; the
     * established unwind applies (chime unclaimed; a claim stranded by a
     * LATER failure is bounded at one create per boot, the rebuild stops
     * at the failure, the modebase claims' standing policy). */
    void *epm_delegate = nullptr;
    if (type_has_cluster(ep_type, ElectricalPowerMeasurement::Id)) {
        epm_delegate = mt_matter_epm_delegate_alloc();
        if (epm_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: EPM delegate pool exhausted (MT_MEAS_MAX %u); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)MT_MEAS_MAX, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    void *ptop_delegate = nullptr;
    if (type_has_cluster(ep_type, PowerTopology::Id)) {
        ptop_delegate = mt_matter_ptop_delegate_alloc();
        if (ptop_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: PowerTopology delegate pool exhausted (MT_MEAS_MAX %u); "
                    "%u of %u serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)MT_MEAS_MAX, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    /* The EEM measurement table's capacity, claimed here for the same
     * two-halves reason as the two above: mt_matter_eem_register() below
     * cannot be the gate, because it needs an endpoint id that only a
     * successful create() produces, so a pool-exhausted endpoint would
     * already be live, and answering Failure on the MANDATORY Accuracy
     * attribute of a cluster it advertises, by the time anyone could
     * notice. The pool itself is inside the SDK (../sdk-patches), which is
     * why this port has to hold the count; mt_matter_eem_reserve()'s
     * comment carries the reasoning and why this is unreachable today. */
    if (type_has_cluster(ep_type, ElectricalEnergyMeasurement::Id)) {
        if (!mt_matter_eem_reserve()) {
            LOG_ERR("devtype 0x%04X: EEM measurement pool exhausted "
                    "(CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES %u, "
                    "src/chip_project_config.h); %u of %u serviceable endpoints in use",
                    (unsigned)devtype_id,
                    (unsigned)CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* Catalogue batch 7a: the meter capacity claim, the C6's
     * reserve-before-create fix rendered in this port's claim block. Only
     * a count is claimed here (MT_METER_MAX, mt_matter_zephyr.cpp's
     * pool); the Instance itself is constructed later by
     * mt_meter_register_all()'s post-rebuild scan, because its Init()
     * wants the endpoint's cluster to exist first and because a scan
     * discovered shortfall could not abort this create retroactively (the
     * meterIdAttrs audit note). A claim stranded by a later failure in
     * this function is bounded at one per boot, the modebase claims'
     * standing policy. */
    if (type_has_cluster(ep_type, MeterIdentification::Id)) {
        if (!mt_meter_reserve()) {
            LOG_ERR("devtype 0x%04X: MeterIdentification instance pool exhausted "
                    "(MT_METER_MAX %u); %u of %u serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)MT_METER_MAX, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* Catalogue batch 7a: the DEM handout's first half. The alloc takes
     * the endpoint id per core/include/mt_matter.h's alloc(ep) contract,
     * and since the batch 7b fix round (review M1) the pool DISCARDS it
     * until the success-only second half: s_next_ep_id is only the id
     * this create assigns IF IT SUCCEEDS, and a stamp on a claim
     * stranded by a later failure would alias the NEXT successful
     * create's id into the dead delegate (the stranded claim itself
     * stays bounded at one per boot, the standing policy; the full
     * reasoning is at the pool). The DEMMode ModeBase claim is the RVC
     * pair's
     * discipline verbatim, one slot from the shared pool with the cluster
     * id fixed at alloc; its second half's Instance::Init() VerifyOrDies
     * on ordering, so the only acceptable failure is this abort before
     * anything is spent. */
    void *dem_delegate = nullptr;
    if (type_has_cluster(ep_type, DeviceEnergyManagement::Id)) {
        dem_delegate = mt_matter_dem_delegate_alloc(s_next_ep_id);
        if (dem_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: DEM delegate pool exhausted (MT_DEM_MAX %u); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)MT_DEM_MAX, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    void *dem_mode_delegate = nullptr;
    if (type_has_cluster(ep_type, DeviceEnergyManagementMode::Id)) {
        dem_mode_delegate = mt_matter_modebase_delegate_alloc(DeviceEnergyManagementMode::Id);
        if (dem_mode_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (DEM mode); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* Catalogue batch 7b: the water heater handout's first half, the DEM
     * pair's discipline verbatim: the WHM alloc takes the endpoint id per
     * the header's alloc(ep) contract (core/include/mt_matter.h:1003) and
     * discards it until the success-only second half (fix round M1, the
     * DEM claim's note above and the pool's own comment), the
     * WaterHeaterMode ModeBase claim is one more slot from the shared pool
     * with the cluster id fixed at alloc, and its second half's
     * Instance::Init() VerifyOrDies on ordering, so the only acceptable
     * failure is this abort before anything is spent. The established
     * unwind applies (chime unclaimed; a claim stranded by a LATER failure
     * is bounded at one create per boot, the standing policy). */
    void *whm_delegate = nullptr;
    if (type_has_cluster(ep_type, WaterHeaterManagement::Id)) {
        whm_delegate = mt_matter_whm_delegate_alloc(s_next_ep_id);
        if (whm_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: WHM delegate pool exhausted (MT_WHM_MAX %u); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)MT_WHM_MAX, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    void *wh_mode_delegate = nullptr;
    if (type_has_cluster(ep_type, WaterHeaterMode::Id)) {
        wh_mode_delegate = mt_matter_modebase_delegate_alloc(WaterHeaterMode::Id);
        if (wh_mode_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (water heater mode); "
                    "%u of %u serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* Catalogue batch 8: the refrigerator's ModeBase claim, and from the
     * cabinet commit the cooler cabinet's too. One more slot from the shared
     * pool with the cluster id fixed at alloc, the RVC pair's discipline
     * verbatim; RefrigeratorAlarm needs nothing here at all (a process
     * singleton with no per-endpoint object and an empty plugin init, the
     * refrigeratorAlarmAttrs audit note). */
    void *fridge_mode_delegate = nullptr;
    if (type_has_cluster(ep_type, RefrigeratorAndTemperatureControlledCabinetMode::Id)) {
        fridge_mode_delegate =
            mt_matter_modebase_delegate_alloc(RefrigeratorAndTemperatureControlledCabinetMode::Id);
        if (fridge_mode_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (refrigerator/cabinet "
                    "mode); %u of %u serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* Catalogue batch 8: the heater cabinet's OvenMode claim, the seventh
     * ModeBase consumer. Same discipline as every other. */
    void *oven_mode_delegate = nullptr;
    if (type_has_cluster(ep_type, OvenMode::Id)) {
        oven_mode_delegate = mt_matter_modebase_delegate_alloc(OvenMode::Id);
        if (oven_mode_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (oven mode); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* Catalogue batch 8: the microwave's TWO claims, on top of the
     * OperationalState claim it already made above. Three per-endpoint
     * object pairs on one endpoint, the most in the catalogue, and the
     * reason HEARTH_OBJ_HEAP_BYTES moved this batch. Both abort before
     * anything is spent, the standing rule; the second halves are NOT
     * independent and are handed to one ordering function below. */
    void *mwoc_delegate = nullptr;
    void *microwave_mode_delegate = nullptr;
    if (type_has_cluster(ep_type, MicrowaveOvenControl::Id)) {
        mwoc_delegate = mt_matter_mwoc_delegate_alloc();
        if (mwoc_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: MicrowaveOvenControl delegate pool exhausted (%u slots, "
                    "one per serviceable endpoint); %u of %u serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    /* Fix round M5: this claim is made on the CLUSTER but consumed only
     * inside the mwoc_delegate arm of the second half, so a future device
     * type carrying MicrowaveOvenMode WITHOUT MicrowaveOvenControl would take
     * a pool slot and never construct or Init() its Instance: the cluster
     * would be declared, unregistered and silent, which is the exact class of
     * quiet cripple this file alarms on. Unreachable today (only 0x0079
     * carries either), and alarmed anyway, because mt_matter_mwoc_register()
     * already carries the alarm for the opposite pairing. */
    if (type_has_cluster(ep_type, MicrowaveOvenMode::Id) &&
        !type_has_cluster(ep_type, MicrowaveOvenControl::Id)) {
        LOG_ERR("devtype 0x%04X carries MicrowaveOvenMode without "
                "MicrowaveOvenControl; its ModeBase Instance would never be constructed "
                "and the cluster would answer nothing on the fabric",
                (unsigned)devtype_id);
        if (chime_delegate != nullptr) {
            mt_matter_chime_delegate_unclaim(chime_delegate);
        }
        return -1;
    }
    if (type_has_cluster(ep_type, MicrowaveOvenMode::Id)) {
        microwave_mode_delegate = mt_matter_modebase_delegate_alloc(MicrowaveOvenMode::Id);
        if (microwave_mode_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (microwave oven mode); "
                    "%u of %u serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /* The EVSE round: the delegate handout's first half, CONSUMING the
     * reservation taken at the top of this block (mt_matter_evse_reserve()
     * refuses a handout with no outstanding reservation, so this can no
     * longer be the first place a full pool is discovered). Unlike the DEM
     * and WHM allocs this one really uses the id it is passed, because it
     * also LOADS this endpoint's stored charging schedule, satisfying the
     * SDK's unenforced "LoadTargets before GetTargets" contract; the fix
     * round M1 aliasing hazard those two avoid by discarding the id does not
     * arise, since a claim stranded by a later failure cannot alias a LATER
     * create's id when the rebuild stops at the first failure. Its
     * EnergyEvseMode ModeBase claim is the RVC pair's discipline verbatim,
     * one slot from the shared pool with the cluster id fixed at alloc, and
     * its second half's Instance::Init() VerifyOrDies on ordering, so the
     * only acceptable failure is this abort before anything is spent. */
    void *evse_delegate = nullptr;
    if (type_has_cluster(ep_type, EnergyEvse::Id)) {
        evse_delegate = mt_matter_evse_delegate_alloc(s_next_ep_id);
        if (evse_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: EVSE delegate pool exhausted (MT_EVSE_MAX %u); %u of %u "
                    "serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)MT_EVSE_MAX, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }
    void *evse_mode_delegate = nullptr;
    if (type_has_cluster(ep_type, EnergyEvseMode::Id)) {
        evse_mode_delegate = mt_matter_modebase_delegate_alloc(EnergyEvseMode::Id);
        if (evse_mode_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: modebase delegate pool exhausted (energy EVSE mode); %u of "
                    "%u serviceable endpoints in use",
                    (unsigned)devtype_id, (unsigned)live_endpoints(),
                    (unsigned)kServiceableEndpoints);
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    void *valve_delegate = nullptr;
    if (type_has_cluster(ep_type, ValveConfigurationAndControl::Id)) {
        valve_delegate = mt_matter_valve_delegate_alloc();
        if (valve_delegate == nullptr) {
            /* Both walls named, the rule the two capacity logs below
             * follow: an integrator reading this needs to know which
             * resource ran out and how far away the others were. */
            LOG_ERR("devtype 0x%04X: valve delegate pool exhausted (%u slots, one per "
                    "serviceable endpoint); %u of %u serviceable endpoints in use, endpoint "
                    "heap %zu of %zu usable B used; host may declare %u, this build serves %u",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints, s_ep_heap_used,
                    kHeapUsableBytes, (unsigned)MT_COMP_MAX_ENDPOINTS,
                    (unsigned)kServiceableEndpoints);
            /* Fix round M3: no catalogue type carries both Valve and
             * Chime, so chime_delegate is always null here today; the
             * unwind is written anyway so a future type that did could
             * not leak. Same on every -1 below. */
            if (chime_delegate != nullptr) {
                mt_matter_chime_delegate_unclaim(chime_delegate);
            }
            return -1;
        }
    }

    /*
     * The two capacity limits, checked in order and both named in whichever
     * log fires. They are different resources and a composition can exhaust
     * either one first: sixteen sensors exhaust the header table with the
     * heap barely touched, while fourteen extended colour lights exhaust the
     * heap with headers to spare. An integrator reading the log needs to
     * know which wall was hit and how far away the other one was, so both
     * messages carry both numbers.
     */
    uint16_t index = 0;
    while (index < kServiceableEndpoints && s_dyn[index].used) {
        index++;
    }
    if (index == kServiceableEndpoints) {
        LOG_ERR("devtype 0x%04X: all %u serviceable endpoints in use "
                "(endpoint heap %zu of %zu usable B used); host may declare %u, "
                "this build serves %u",
                (unsigned)devtype_id, (unsigned)kServiceableEndpoints, s_ep_heap_used,
                kHeapUsableBytes, (unsigned)MT_COMP_MAX_ENDPOINTS,
                (unsigned)kServiceableEndpoints);
        if (chime_delegate != nullptr) {
            mt_matter_chime_delegate_unclaim(chime_delegate);
        }
        return -1;
    }

    /* Sized for THIS device type, not for the widest in the catalogue. The
     * store bytes (store reclaim round) are zero for every type but mode
     * select and chime. */
    const uint16_t n_clusters = ep_type->clusterCount;
    const uint16_t n_slots = count_slots(ep_type);
    const size_t base = block_bytes(n_clusters, n_slots);
    const size_t store = store_bytes(ep_type);
    const size_t want = base + store;

    void *block = k_heap_alloc(&hearth_ep_heap, want, K_NO_WAIT);
    if (block == nullptr) {
        size_t free_bytes = 0;
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
        struct sys_memory_stats st = {};
        if (sys_heap_runtime_stats_get(&hearth_ep_heap.heap, &st) == 0) {
            free_bytes = st.free_bytes;
        }
#endif
        /* Fix round M1: every figure on this line is chunk-rounded and
         * shares the usable-bytes denominator, so the bench's 13-prefix
         * leg reads coherently: cost + handed out + free reconcile
         * (584 wanted, 7,592 of 8,112 used, 520 free). The raw payload and
         * its breakdown stay in the parenthesis for the sizing table's
         * arithmetic. */
        LOG_ERR("devtype 0x%04X: endpoint block costing %zu B (payload %zu: %u clusters, "
                "%u slots, %zu store B) does not fit; "
                "%zu of %zu usable B handed out, %zu B free; this is endpoint %u of %u serviceable",
                (unsigned)devtype_id, kHeapCostOf(want), want, (unsigned)n_clusters,
                (unsigned)n_slots, store, s_ep_heap_used, kHeapUsableBytes, free_bytes,
                (unsigned)(index + 1), (unsigned)kServiceableEndpoints);
        if (chime_delegate != nullptr) {
            mt_matter_chime_delegate_unclaim(chime_delegate);
        }
        return -1;
    }
    s_ep_heap_used += kHeapCostOf(want);

    /* Construct the trailing store(s) before the endpoint can be served:
     * heap bytes arrive uninitialized where the old .bss stores were
     * zeroed, and value-initialization () is exactly "count 0, entries
     * zeroed, ModeOptionStructs default-constructed", the empty-list state
     * every reader maps to the pre-reclaim no-slot answers (mt_dyn_store.h).
     * Never destroyed, the block policy; offsets come from store_offset(),
     * the alignment-correct store_walk (fix round 2) shared with the
     * accessors, so writer and readers place every store identically. */
    {
        uint8_t *region = static_cast<uint8_t *>(block) + base;
        if (type_has_cluster(ep_type, ModeSelect::Id)) {
            new (region + store_offset(ep_type, ModeSelect::Id)) mt_mode_store_t();
        }
        if (type_has_cluster(ep_type, Chime::Id)) {
            new (region + store_offset(ep_type, Chime::Id)) mt_chime_store_t();
        }
        /* Catalogue batch 5: the RVC's two ModeBase stores, the first
         * type with more than one trailing store in a block. count 0 is
         * the pre-feed state the delegate's placeholder-mode-0 policy
         * answers for; value-initialization is what produces it, and it
         * MUST be in place before emberAfSetDynamicEndpoint() below,
         * because the ModeBase Instance::Init() run in the handout's
         * second half reads the delegate's index 0 as its first act. */
        if (type_has_cluster(ep_type, RvcRunMode::Id)) {
            new (region + store_offset(ep_type, RvcRunMode::Id)) mt_mb_store_t();
        }
        if (type_has_cluster(ep_type, RvcCleanMode::Id)) {
            new (region + store_offset(ep_type, RvcCleanMode::Id)) mt_mb_store_t();
        }
        /* Catalogue batch 7a: the DEM mode list, same before-the-endpoint
         * ordering obligation as the RVC pair (the ModeBase Init reads
         * index 0 as its first act). */
        if (type_has_cluster(ep_type, DeviceEnergyManagementMode::Id)) {
            new (region + store_offset(ep_type, DeviceEnergyManagementMode::Id)) mt_mb_store_t();
        }
        /* Catalogue batch 7b: the water heater's mode list, same
         * obligation. */
        if (type_has_cluster(ep_type, WaterHeaterMode::Id)) {
            new (region + store_offset(ep_type, WaterHeaterMode::Id)) mt_mb_store_t();
        }
        /* Catalogue batch 8: the refrigerator's (and, from the cabinet
         * commit, the cooler cabinet's) mode list, same obligation. */
        if (type_has_cluster(ep_type, RefrigeratorAndTemperatureControlledCabinetMode::Id)) {
            new (region + store_offset(ep_type,
                                       RefrigeratorAndTemperatureControlledCabinetMode::Id))
                mt_mb_store_t();
        }
        /* Catalogue batch 8: the heater cabinet's mode list, same
         * obligation. */
        if (type_has_cluster(ep_type, OvenMode::Id)) {
            new (region + store_offset(ep_type, OvenMode::Id)) mt_mb_store_t();
        }
        /* Catalogue batch 8: the microwave's mode list. The obligation is
         * SHARPER here than anywhere else it appears, because the ModeBase
         * Init()'s index-0 read is not the only reader of the placeholder:
         * MicrowaveOvenControl's very first SetCookingParameters resolves
         * its default cookMode through GetModeValueByModeTag(kNormal), which
         * walks this delegate's tags. An unconstructed store there would not
         * merely fail an Init, it would refuse every cooking command with a
         * status the server also produces legitimately. */
        if (type_has_cluster(ep_type, MicrowaveOvenMode::Id)) {
            new (region + store_offset(ep_type, MicrowaveOvenMode::Id)) mt_mb_store_t();
        }
        /* The EVSE round: the EnergyEvseMode list, the eighth and last
         * ModeBase store and the second one in THIS block (the DEM mode's
         * construction above is the other). Same before-the-endpoint
         * obligation as every other: the ModeBase Init() reads the
         * delegate's index 0 as its first act. */
        if (type_has_cluster(ep_type, EnergyEvseMode::Id)) {
            new (region + store_offset(ep_type, EnergyEvseMode::Id)) mt_mb_store_t();
        }
        /* Catalogue batch 8: the temperature-level label store, the first
         * store whose presence is finer than a cluster id (the kStoreWalk
         * struct comment). The condition below must be the SAME question
         * store_walk() asks, or the writer and the readers would disagree
         * about whether this block has one; asking type_has_attr() here,
         * exactly as the walk does, is what keeps them tied. Unlike the
         * ModeBase stores there is no Init() waiting on it, but it must
         * still be constructed before the endpoint is served, because the
         * SDK's wildcard AAI is already registered and a controller could
         * read SupportedTemperatureLevels the moment the endpoint enables. */
        if (type_has_cluster(ep_type, TemperatureControl::Id) &&
            type_has_attr(ep_type, TemperatureControl::Id,
                          TemperatureControl::Attributes::SupportedTemperatureLevels::Id)) {
            new (region + store_offset(ep_type, TemperatureControl::Id))
                mt_temp_levels_store_t();
        }
    }

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

    /* Cleared so the check below cannot read a result left by an earlier
     * endpoint's init, or by the catalogue endpoint's at boot. */
    s_lock_init_ep = kInvalidEndpointId;
    s_lock_init_err = CHIP_NO_ERROR;

    /* Catalogue batch 7b: the variant's own device-type span, the
     * ep_type_v1 fallback rule's twin (the registry struct comment): a
     * variant whose cluster set loses a grafted device type must not keep
     * advertising its id. Rows without a v1 span (everything before 7b)
     * get the shared one for free via the empty-span fallback. */
    const Span<const EmberAfDeviceType> &device_types =
        (variant == 1 && !type->device_types_v1.empty()) ? type->device_types_v1
                                                         : type->device_types;

    CHIP_ERROR err = emberAfSetDynamicEndpoint(
        index, d.ep_id, ep_type, Span<DataVersion>(block_dv(d), n_clusters),
        device_types, (parent_devtype != 0) ? parent_ep_id : kInvalidEndpointId);
    if (err != CHIP_NO_ERROR) {
        /* The header goes back on the free list, the block does not: see the
         * allocate-only note beside K_HEAP_DEFINE. This path returns -1 and
         * the rebuild stops here, so nothing allocates again before the
         * reboot that resets the heap: the orphaned block is bounded at one
         * per boot and unreachable. Freeing it would be worse than keeping
         * it, because CHIP may still hold this span in emAfEndpoints[index]
         * on its own error paths. */
        d.used = false;
        d.block = nullptr;
        LOG_ERR("emberAfSetDynamicEndpoint(0x%04X) failed: %" CHIP_ERROR_FORMAT,
                (unsigned)devtype_id, err.Format());
        if (chime_delegate != nullptr) {
            mt_matter_chime_delegate_unclaim(chime_delegate);
        }
        return -1;
    }

    /*
     * The door lock's per-endpoint init has to have RUN and SUCCEEDED for
     * this endpoint, and this is the first moment both are knowable.
     * emberAfSetDynamicEndpoint() enables the endpoint, which dispatches
     * emberAfDoorLockClusterInitCallback() (above), which leaves its result
     * in s_lock_init_*. Two ways to fail:
     *
     *   InitEndpoint() returned an error, i.e. no mEndpointCtx slot. The
     *   endpoint would present a DoorLock cluster a controller can invoke
     *   while HandleRemoteLockOperation() has nowhere to keep its state.
     *
     *   The callback never fired for this endpoint at all. That would mean
     *   ember stopped driving cluster inits for dynamic endpoints, which is
     *   the load-bearing assumption in the doorLockAttrs audit note. Better
     *   caught here on the next SDK bump than found on a bench.
     *
     * Unlike the valve's read-back, this one ABORTS, and the endpoint is
     * taken back down first: it is live in CHIP by now, so leaving it there
     * with its header freed would present a data model entry this file can
     * no longer serve attributes for. emberAfClearDynamicEndpoint() also
     * runs the cluster shutdown callbacks, which releases whatever context
     * the failed init did claim. The block is not freed, the allocate-only
     * invariant beside K_HEAP_DEFINE; the rebuild stops here, so it is
     * bounded at one per boot.
     */
    if (type_has_cluster(ep_type, DoorLock::Id) &&
        (s_lock_init_ep != d.ep_id || s_lock_init_err != CHIP_NO_ERROR)) {
        LOG_ERR("devtype 0x%04X: DoorLock init did not succeed for endpoint %u "
                "(init ran for endpoint %u, result %" CHIP_ERROR_FORMAT ")",
                (unsigned)devtype_id, (unsigned)d.ep_id, (unsigned)s_lock_init_ep,
                s_lock_init_err.Format());
        emberAfClearDynamicEndpoint(index);
        d.used = false;
        d.block = nullptr;
        if (chime_delegate != nullptr) {
            mt_matter_chime_delegate_unclaim(chime_delegate);
        }
        return -1;
    }

    /* Dynamic endpoints are declared with DECLARE_DYNAMIC_CLUSTER, which
     * hardcodes functions=NULL: the per-endpoint ServerInit callbacks that
     * static endpoints get for free via GENERATED_FUNCTION_ARRAYS never run
     * here. That is harmless for a cluster whose server logic reads
     * attributes directly (OnOff) or whose init is a no-op against our
     * seeds (Occupancy), but LevelControl's ServerInitCallback caches
     * Min/MaxLevel into EmberAfLevelControlState, and moveToLevelHandler()
     * clamps every target against that cached state, not the attribute. Left
     * uninitialized, the cache reads 0/0 and every level transition clamps
     * to 0 (B388). Call it by hand for any devtype carrying LevelControl.
     * A future cluster with its own cached-state ServerInit joins this call
     * site.
     *
     * Catalogue batch 2: ColorControl joined it. Its ServerInit
     * (color-control-server.cpp:3291) calls startUpColorTempCommand(), which
     * applies a non-null StartUpColorTemperatureMireds to
     * ColorTemperatureMireds and forces ColorMode/EnhancedColorMode to
     * kColorTemperatureMireds. That is not a cache like LevelControl's, but
     * it is per-endpoint boot state the endpoint would otherwise never get,
     * which is the same class of defect. With this batch's seeds it writes
     * the value already there; it is called so that stays true when the
     * seeds change. Everything else in this batch was audited and has no
     * ServerInit worth running: Thermostat's is an empty TODO body
     * (thermostat-server.cpp:866), and FanControl, WindowCovering and
     * AirQuality define none at all.
     *
     * Catalogue batch 3 did NOT join it, and the door lock is the
     * interesting case: it has per-endpoint init that is genuinely
     * required (DoorLockServer::InitEndpoint), and it still does not belong
     * here, because ember already runs it. The hook is
     * emberAfDoorLockClusterInitCallback, dispatched by cluster id from
     * emberAfClusterInitCallback() inside initializeEndpoint()
     * (attribute-storage.cpp:480), which emberAfSetDynamicEndpoint() above
     * reaches through emberAfEndpointEnableDisable(). That is a different
     * mechanism from the function-array INIT_FUNCTION slot this call site
     * exists to substitute for, and unlike that slot it is NOT bypassed by
     * DECLARE_DYNAMIC_CLUSTER's functions=NULL. Calling InitServer() again
     * from here would be a second init on one endpoint, which the SDK
     * explicitly requires a ShutdownEndpoint() between
     * (door-lock-server.h:110-113). See the audit note on doorLockAttrs.
     * The valve has no per-endpoint init at all; what it needs instead is a
     * delegate registration, which is below rather than here because it is
     * not an init.
     *
     * The loop no longer stops at the first match: the color lights carry
     * BOTH LevelControl and ColorControl, and breaking after LevelControl
     * would silently skip the second init.
     *
     * Running these AFTER emberAfSetDynamicEndpoint() is load-bearing for
     * memory safety, not just ordering. startUpColorTempCommand() indexes
     * quietTemperatureMireds[getEndpointIndex(endpoint)] with NO bounds
     * check (color-control-server.cpp:2609-2612), and
     * emberAfGetClusterServerEndpointIndex() returns kEmberInvalidEndpointIndex
     * (0xFFFF) for an endpoint that is not yet configured and enabled
     * (attribute-storage.cpp:923-935). Called before the endpoint exists,
     * that is a wild write far past what is now a 17-entry array (one fixed
     * catalogue endpoint plus kServiceableEndpoints). The command handlers
     * are safer than this (they all go through the bounds-checked
     * get*TransitionStateByIndex() getters), but the init is not, so the
     * call must stay below the successful emberAfSetDynamicEndpoint(). */
    for (uint8_t i = 0; i < ep_type->clusterCount; i++) {
        if (ep_type->cluster[i].clusterId == LevelControl::Id) {
            emberAfLevelControlClusterServerInitCallback(d.ep_id);
        } else if (ep_type->cluster[i].clusterId == ColorControl::Id) {
            emberAfColorControlClusterServerInitCallback(d.ep_id);
        } else if (ep_type->cluster[i].clusterId == ModeSelect::Id) {
            /* Catalogue batch 4: ModeSelect joined. Its ServerInit is a
             * real strong body reached only through the nulled functions
             * array; on this composition it verifiably does nothing (the
             * StartUpMode read answers UnsupportedAttribute and the body
             * skips), and it is called anyway so the day StartUpMode is
             * declared a dynamic endpoint boots the right CurrentMode.
             * See the modeSelectAttrs audit note. */
            emberAfModeSelectClusterServerInitCallback(d.ep_id);
        }
    }

    /* The second half of the delegate handout. Must be HERE, below a
     * successful emberAfSetDynamicEndpoint(), because the endpoint has to
     * be both configured and enabled for SetDefaultDelegate() to resolve an
     * index; that ordering is the whole argument in the comment above.
     *
     * mt_matter_valve_delegate_set_endpoint() reads the registration back
     * and logs loudly if it did not land, and deliberately does NOT abort
     * the create. See "what each of the two checks is actually for" above
     * for why that is the same story as the pool check aborting and not an
     * inconsistency: this failure is unreachable by ordering, and if it
     * ever fired the endpoint would already be live and correct in every
     * respect but adjudication, with no clean way to take it back. The log
     * is the bench's signal that the SDK's silent-drop path fired. */
    if (valve_delegate != nullptr) {
        mt_matter_valve_delegate_set_endpoint(valve_delegate, d.ep_id);
    }

    /* The trio's second half: constructs the OperationalState::Instance in
     * its slot's raw storage and runs Init(), which registers the
     * endpoint-scoped command handler and AAI. Below the successful
     * emberAfSetDynamicEndpoint() by necessity (Init() checks
     * emberAfContainsServer) and below the B388 inits for tidiness. An
     * Init() failure is logged loudly inside and does not abort, the
     * valve read-back's reasoning: unreachable by ordering, and the
     * endpoint is live and correct in every other respect by then. */
    /* Catalogue batch 8: the microwave takes the ordered path instead, and
     * the else here is the whole guard against constructing its
     * OperationalState Instance twice. Every other type's second halves are
     * independent and may run in any order among themselves; this one's are
     * a three-way construction ORDER whose violations are all silent, so
     * they are handed to a single function that owns the sequence. See
     * mt_matter_mwoc_register() in mt_matter_zephyr.cpp for the contract;
     * the two ModeBase and OperationalState claims it consumes were made in
     * the claim block above like everyone else's. */
    if (mwoc_delegate != nullptr) {
        mt_matter_mwoc_register(mwoc_delegate, opstate_delegate, microwave_mode_delegate,
                                d.ep_id);
    } else if (opstate_delegate != nullptr) {
        mt_matter_opstate_delegate_set_endpoint(opstate_delegate, d.ep_id);
    }

    /* The chime's second half: placement-constructs the ChimeServer
     * (endpoint id plus delegate reference) and runs Init(), whose AAI
     * and command-handler registrations are endpoint-scoped and whose
     * LoadPersistentAttributes() keys on GetEndpointId(), so it belongs
     * below the successful emberAfSetDynamicEndpoint() like the trio's.
     * Failure is logged loudly inside and does not abort, the same
     * reasoning. */
    if (chime_delegate != nullptr) {
        mt_matter_chime_delegate_set_endpoint(chime_delegate, d.ep_id);
    }

    /* Catalogue batch 5, the RVC's second halves. THE ORDER OF THIS CALL
     * RELATIVE TO emberAfSetDynamicEndpoint() IS LOAD-BEARING AND WRONG
     * ORDER IS A PANIC, not a log line: each ModeBase set_endpoint
     * placement-constructs its Instance and runs Init(), whose second act
     * is VerifyOrDie(emberAfContainsServer(ep, cluster))
     * (mode-base-server.cpp:77). Run above the successful
     * emberAfSetDynamicEndpoint() and the board aborts; every earlier
     * pool in this function merely soft-bails on the same mistake. Its
     * FIRST act is reading the delegate's mode index 0 (:74), which is
     * why the mt_mb_store_t construction above sits before the endpoint
     * is served and why the delegate's placeholder-mode-0 policy exists:
     * a failure there returns early and the Instance silently never
     * registers, no abort, no diagnostic. Both set_endpoint
     * implementations check Init()'s return and log loudly
     * (mt_matter_zephyr.cpp), unlike the C6 whose SDK init-callback path
     * discards it. The RVC opstate half is the trio's shape verbatim
     * (soft-bail Init, logged inside). */
    if (mb_run_delegate != nullptr) {
        mt_matter_modebase_delegate_set_endpoint(mb_run_delegate, d.ep_id);
    }
    if (mb_clean_delegate != nullptr) {
        mt_matter_modebase_delegate_set_endpoint(mb_clean_delegate, d.ep_id);
    }
    if (rvc_opstate_delegate != nullptr) {
        mt_matter_rvc_opstate_delegate_set_endpoint(rvc_opstate_delegate, d.ep_id);
    }

    /* Catalogue batch 7a: the measurement second halves. Each setter
     * placement-constructs its cluster Instance and runs Init(), which is
     * SOFT for both (AAI registration only, no VerifyOrDie: electrical-
     * power-measurement-server.cpp:43-47, power-topology-server.cpp:42-46),
     * so ordering mistakes here cannot panic, unlike the ModeBase block
     * above; below the successful create so the registration serves a live
     * endpoint. mt_matter_eem_register() is the EEM equivalent for a
     * cluster that has no delegate at all: the one-time wildcard AAI (its
     * mask is load-bearing for FeatureMap truth on every EEM endpoint, the
     * eemAttrs audit note) plus this endpoint's SetMeasurementAccuracy(),
     * which resolves through emberAfGetClusterServerEndpointIndex() and so
     * REQUIRES the endpoint configured and enabled first. */
    if (epm_delegate != nullptr) {
        mt_matter_meas_delegate_set_endpoint(epm_delegate, d.ep_id);
    }
    if (ptop_delegate != nullptr) {
        mt_matter_meas_delegate_set_endpoint(ptop_delegate, d.ep_id);
    }
    if (type_has_cluster(ep_type, ElectricalEnergyMeasurement::Id)) {
        mt_matter_eem_register(d.ep_id);
    }

    /* Catalogue batch 7a: the DEM second halves. The ModeBase setter
     * carries the RVC block's panic warning verbatim (Init() VerifyOrDies
     * above a successful create); the DEM register is soft (CHI then AAI,
     * no contains-server check) and takes the variant's PA predicate, the
     * single source both the FeatureMap seed and the Instance mask derive
     * from (seed_slots()'s special case). */
    /* THE PA PREDICATE IS READ FROM THE DECLARED LIST, not from the variant,
     * since the EVSE round. It used to be `variant == 0`, which was exactly
     * right while the only DEM-bearing types were the standalone DEM and
     * battery storage, whose variant 0 carries PowerAdjustment and whose
     * variant 1 does not. The EVSE breaks that: its variant axis is SOC, and
     * it carries the over-delivered DEM with NO PowerAdjustment on either
     * variant, so `variant == 0` would have handed a variant-0 EVSE's
     * Instance a PA mask over a list declaring none of PA's four obligations.
     * type_has_attr() asks the list itself, which is the honest source and
     * cannot disagree with what the endpoint advertises.
     *
     * PURELY ADDITIVE ON THE EXISTING TYPES, and checkable by reading three
     * declarations: demAttrs (0x050D variant 0) declares
     * PowerAdjustmentCapability, demReportOnlyAttrs (0x050D variant 1) does
     * not, batteryStorageClusters (0x0018 variant 0) uses demAttrs and
     * batteryStorageNoDemClusters (variant 1) has no DEM cluster at all. So
     * the new predicate answers exactly what `variant == 0` answered for
     * every row that existed before this one. The seeded FeatureMap follows
     * the same split through its own devtype-qualified row. */
    if (dem_delegate != nullptr) {
        mt_matter_dem_register(dem_delegate, d.ep_id,
                               type_has_attr(ep_type, DeviceEnergyManagement::Id,
                                             DeviceEnergyManagement::Attributes::
                                                 PowerAdjustmentCapability::Id));
    }
    if (dem_mode_delegate != nullptr) {
        mt_matter_modebase_delegate_set_endpoint(dem_mode_delegate, d.ep_id);
    }

    /* Catalogue batch 7b: the water heater second halves, the DEM pair's
     * shape one cluster over. The WHM register is soft (CHI then AAI, no
     * contains-server check, water-heater-management-server.cpp:94-100)
     * and takes the variant's EM|TP predicate, the single source both the
     * FeatureMap seed and the Instance mask derive from (seed_slots()'s
     * WHM special case); the ModeBase setter carries the RVC block's panic
     * warning verbatim. */
    if (whm_delegate != nullptr) {
        mt_matter_whm_register(whm_delegate, d.ep_id, variant == 0);
    }
    if (wh_mode_delegate != nullptr) {
        mt_matter_modebase_delegate_set_endpoint(wh_mode_delegate, d.ep_id);
    }

    /* Catalogue batch 8: the refrigerator/cooler-cabinet mode's second half,
     * carrying the RVC block's panic warning verbatim (Init() VerifyOrDies
     * unless it runs below a successful emberAfSetDynamicEndpoint()). */
    if (fridge_mode_delegate != nullptr) {
        mt_matter_modebase_delegate_set_endpoint(fridge_mode_delegate, d.ep_id);
    }
    if (oven_mode_delegate != nullptr) {
        mt_matter_modebase_delegate_set_endpoint(oven_mode_delegate, d.ep_id);
    }

    /* The EVSE round's second halves. mt_matter_evse_register() constructs
     * the Instance with the variant's feature mask and Init()s it, SOFT on
     * both registrations (CHI then AAI, energy-evse-server.cpp:40-46: no
     * emberAfContainsServer check and no VerifyOrDie), so an ordering mistake
     * here cannot panic; it is below the successful create so the
     * registration serves a live endpoint, and because the Instance's own
     * constructor is what stamps the delegate's endpoint id and the back
     * pointer the delegate needs. The ModeBase setter carries the RVC block's
     * panic warning verbatim.
     *
     * with_soc READS THE DECLARED LIST, the same source and the same argument
     * as the DEM PowerAdjustment predicate two blocks above, and it was
     * `variant == 0` until the EVSE round's fix pass. Both answers are
     * identical today, because the registry selects evseAttrs for variant 0
     * and evseNoSocAttrs for variant 1 and StateOfCharge is exactly what
     * separates them. The reason to ask the list anyway is that the identity
     * is a property of one registry row rather than of anything the compiler
     * or this call site can see, and the stamped mask feeds THREE consumers
     * (the Instance's own mFeature, the AT+MTMEAS existence gates, and the
     * merge's SOC-variant rule), so a future edit that split the variant from
     * the list would have been silent in all three at once. Asking the list
     * makes "the declared list and the served feature cannot disagree" true
     * rather than merely observed. */
    if (evse_delegate != nullptr) {
        mt_matter_evse_register(
            evse_delegate, d.ep_id,
            type_has_attr(ep_type, EnergyEvse::Id, EnergyEvse::Attributes::StateOfCharge::Id));
    }
    if (evse_mode_delegate != nullptr) {
        mt_matter_modebase_delegate_set_endpoint(evse_mode_delegate, d.ep_id);
    }

    /* Catalogue batch 8: hand the SDK the one global TemperatureControl
     * iterator delegate. Idempotent (a bare pointer store), takes no
     * endpoint, and has no ordering requirement of its own; it sits here
     * with the other second halves so every registration this file owns is
     * in one place. Registered for BOTH variants: a TemperatureNumber
     * endpoint never reaches the iterator, and making the registration
     * conditional on the level variant would make it depend on composition
     * order for no gain. */
    if (type_has_cluster(ep_type, TemperatureControl::Id)) {
        mt_matter_temp_levels_register();
    }

    s_next_ep_id++;
    *out_ep_id = d.ep_id;
    return 0;
}

/* ---- CHIP external attribute storage --------------------------------- */

/*
 * Strong definitions overriding the weak ones in CHIP's
 * app/util/generic-callback-stubs.cpp. Signatures are copied from
 * app/util/generic-callbacks.h in the NCS v3.3.4 tree: this tree returns
 * Protocols::InteractionModel::Status, not the older EmberAfStatus.
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
