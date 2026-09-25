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
 * The cluster-object heap (nRF mt_matter_zephyr.cpp 133-386 and 9137-9472)
 * is absent from this port for the same reason one level up: every one of
 * its fourteen pools serves a per-endpoint Delegate or Instance belonging to
 * a device type this round does not build, so carrying it would be a static
 * arena with no allocation site. The note in port/mt_matter_sl.cpp beside
 * the live endpoint table says what the batch that needs it has to bring.
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

/* ---- shared cluster building blocks (nRF 276-320) --------------------- */

/*
 * Descriptor carries no attribute metadata of its own beyond the
 * ClusterRevision that DECLARE_DYNAMIC_ATTRIBUTE_LIST_END() appends.
 *
 * Its four lists (DeviceTypeList, ServerList, ClientList, PartsList) are
 * served by CHIP's registered DescriptorCluster object, which wins before
 * ember storage is ever consulted (CodegenDataModelProvider_Read.cpp checks
 * the cluster registry ahead of the metadata path), so declaring them here
 * buys nothing. It costs correctness: emberAfSetDynamicEndpoint validates
 * every declared attribute size against the ember attribute IO buffer
 * (attribute-storage.cpp), and that buffer is ATTRIBUTE_LARGEST from our
 * generated endpoint_config.h:623, which is 66. A 254-byte ARRAY
 * declaration therefore fails the check and makes every single endpoint
 * creation return CHIP_ERROR_NO_MEMORY. This was the nRF's as-built finding
 * and it is checked again for this image: 66 is the figure task 2 recorded
 * and the widest attribute any list below declares is 4 bytes.
 */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Identify's attributes DO reach the external store below: unlike
 * Descriptor, MatterIdentifyClusterInitCallback (identify-server/
 * CodegenIntegration.cpp) only logs, and the IdentifyCluster object is built
 * solely by constructing a legacy Identify instance, which nothing does for
 * a dynamic endpoint.
 *
 * The incoming command list is nullptr for the same reason: with no cluster
 * object there is no handler, and this tree has no ember fallback for
 * Identify either (our generated IMClusterCommandHandler.cpp dispatches only
 * the clusters hearth.zap enables on the fixed endpoints). Advertising
 * Identify in AcceptedCommandList would be a lie. Per-endpoint
 * IdentifyCluster instances are a later round's work. */
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

/* ---- on/off light (0x0100) (nRF 321-348) ------------------------------ */

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

/* ---- temperature sensor (0x0302) (nRF 388-416) ------------------------ */

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

/* data_model/1.5/device_types/TemperatureSensor.xml revision is 3, not 2
 * (nRF fix round 2, M1). */
constexpr EmberAfDeviceType kTemperatureSensorTypes[] = { { 0x0302, 3 } };

/* ---- dimmable light (0x0101) (nRF 349-387) ---------------------------- */

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

/* ---- boolean-state sensors: contact (0x0015), rain (0x0044), water
 * freeze (0x0041), water leak (0x0043) (nRF 417-479) -------------------- */

/*
 * BooleanState (0x0045) is one of the clusters CHIP has migrated to the
 * newer code-driven ServerClusterInterface path
 * (src/app/clusters/boolean-state-server/CodegenIntegration.cpp):
 * MatterBooleanStateClusterInitCallback fires for every endpoint carrying
 * the cluster, dynamic ones included (its instance pool is explicitly sized
 * kFixedClusterCount + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT), and
 * constructs a BooleanStateCluster object whose ReadAttribute() answers
 * StateValue from its own mStateValue member, never consulting this arena:
 * the Descriptor situation above, but for a value the host is meant to
 * update live rather than a static list. The nRF arm confirmed the split
 * both on the bench (an AT+MTATTR write of StateValue=1 fired its URC, but
 * chip-tool still read FALSE) and mechanically (CodegenDataModelProvider_
 * Read.cpp:116 checks that registry before ember's external-storage
 * fallback is ever consulted).
 *
 * The split is BRIDGED on this arm too, not left as a gap.
 * mt_matter_attr_read/write (port/mt_matter_sl.cpp) call the classic
 * emberAfReadAttribute/WriteAttribute path and always reach this arena, so
 * AT+MTATTR against StateValue reads and writes correctly here;
 * MatterPostAttributeChangeCallback additionally looks up the registered
 * BooleanStateCluster object via BooleanState::FindClusterOnEndpoint() and
 * calls SetStateValue() on it, so a host write also reaches a real Matter
 * controller's read AND emits the cluster's StateChange event. See the
 * comment at that call site for the full mechanism and why it cannot
 * recurse. Declared here regardless: the AT+MTATTR contract must still
 * resolve the attribute against this arena, and every other cluster in this
 * file besides Descriptor is a plain ember external-storage cluster with no
 * such split.
 */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(booleanStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::StateValue::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

/* Contact/Rain/Water Freeze/Water Leak all compose to the exact same cluster
 * set (BooleanState + Identify + Descriptor, mandatory clusters only), so one
 * cluster list and one EmberAfEndpointType serve all four: the same "one
 * metadata array per cluster, shared by every endpoint type that carries that
 * cluster" principle stated above for onOffAttrs, extended here to the whole
 * cluster list since the whole composition, not just one cluster, is identical
 * across these four. Only the EmberAfDeviceType (id, revision) differs per
 * device type, and that is what s_registry keys off. */
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

/* ---- occupancy sensor (0x0107) (nRF 480-538) -------------------------- */

/*
 * Occupancy, OccupancySensorType and OccupancySensorTypeBitmap are none of
 * them nullable (controller-clusters.matter: plain, non-nullable
 * attributes).
 *
 * THE INIT CALLBACK DOES NOT RUN FOR A DYNAMIC ENDPOINT, which is why the
 * seed row is the only writer of OccupancySensorType and
 * OccupancySensorTypeBitmap. emberAfOccupancySensingClusterServerInitCallback
 * (occupancy-sensor-server.cpp) is reached only through the per-cluster
 * "functions" array on EmberAfCluster (MATTER_CLUSTER_FLAG_INIT_FUNCTION,
 * checked by emberAfFindClusterFunction() in attribute-storage.cpp's
 * initializeEndpoint()). endpoint_config.h wires that array
 * (chipFuncArrayOccupancySensingServer) for the STATIC, ZAP-declared cluster
 * on endpoint 240 only; DECLARE_DYNAMIC_CLUSTER below hardcodes
 * `.functions = NULL` for every dynamic cluster it builds, with no way to
 * attach one. (This is a DIFFERENT, unrelated function from
 * emberAfOccupancySensingClusterInitCallback, no "Server" in the name: since
 * ca05afa the committed zap-generated/app/cluster-init-callback.cpp:49-50 is
 * the live route for it, the same generic per-endpoint dispatch the
 * LevelControl hook below uses (see "the ember cluster init hook" section),
 * so it IS called, on endpoint 240 and on any dynamic endpoint that carries
 * OccupancySensing. It stays the weak no-op stub because OccupancySensing
 * does not need a strong override the way LevelControl does: its server
 * init, occupancy-sensor-server.cpp:216-246, only writes
 * OccupancySensorType and OccupancySensorTypeBitmap, and the seed row above
 * already writes both.)
 *
 * occupancy-sensor-server.cpp does define an AttributeAccessInterface Instance
 * class and its object file is linked into this build, but nothing in this
 * firmware instantiates one. With no Instance registered, reads and writes for
 * this cluster are plain ember external storage, exactly like every other
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

/* ---- humidity sensor (0x0307) (nRF 539-563) --------------------------- */

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

/* ---- pressure sensor (0x0305) (nRF 564-588) --------------------------- */

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

/* ---- light (illuminance) sensor (0x0106) (nRF 589-620) ----------------
 *
 * IlluminanceMeasurement's MeasuredValue is uint16 (INT16U), unlike
 * TemperatureMeasurement/PressureMeasurement's signed int16s: the null
 * sentinel is therefore the type MAXIMUM (0xFFFF, NumericAttributeTraits::
 * GetNullValue() for an unsigned type), not the signed-type minimum 0x8000
 * the temperature/pressure seeds use. Same attr_null_sentinel() convention
 * (port/mt_matter_sl.cpp), different type -> different sentinel bytes. */

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

/* ---- flow sensor (0x0306) (nRF 621-644) ------------------------------- */

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

/* ---- on/off plug-in unit (0x010A) (nRF 645-670) -----------------------
 *
 * Reuses onOffAttrs/kOnOffIncoming verbatim: OnOffPlug-inUnit.xml mandates
 * the SAME OnOff feature (LT, "Lighting") as OnOffLight.xml, surprising for
 * a plug, but confirmed against both the device-type XML (mandatoryConform
 * on feature LT for the On/Off cluster) and the C6's own esp_matter build
 * (esp_matter_endpoint.cpp on_off_plug_in_unit::add() calls
 * on_off::feature::lighting::add() for this same device type), so the plug's
 * OnOff FeatureMap seed is 0x01, identical to the light's, not 0. Groups and
 * Scenes Management are also mandatoryConform in the XML for this device
 * type, matching OnOffLight.xml exactly, and are left out here for the same
 * reason the on/off light above leaves them out: this catalogue serves
 * attribute-only clusters this build declares, and Groups/Scenes were never
 * added for the lights either. */
HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(onOffPlugInUnitClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(onOffPlugInUnitEndpoint, onOffPlugInUnitClusters);

constexpr EmberAfDeviceType kOnOffPlugInUnitTypes[] = { { 0x010A, 4 } };

/* ---- dimmable plug-in unit (0x010B) (nRF 700-721) ---------------------
 *
 * Reuses onOffAttrs/levelAttrs/kOnOffIncoming/kLevelIncoming verbatim:
 * DimmablePlug-InUnit.xml mandates OnOff feature LT and LevelControl
 * features OO+LT, the identical set DimmableLight.xml mandates, so this
 * cluster list and its seeds (FeatureMap 0x01 for OnOff, 0x03 for
 * LevelControl) are the same as the dimmable light above; see that device
 * type's seed rows in s_seeds, none of which are duplicated here. */
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

/* ---- color temperature light (0x010C) and extended color light (0x010D)
 * (nRF 745-946)
 *
 * Catalogue batch 2 audit, ColorControl (0x0300). Three questions, three
 * answers from this tree:
 *
 *   Code-driven? No. There is no CodegenIntegration.cpp under
 *   src/app/clusters/color-control-server/, and adding the cluster to
 *   hearth.zap emitted no case in zap-generated/CodeDrivenInitShutdown.cpp
 *   (that file still lists exactly the clusters it did before this
 *   batch, BooleanState last). So no registered ServerCluster object wins
 *   ahead of ember, unlike BooleanState/Descriptor: reads and writes land in
 *   the arena below.
 *
 *   AAI? No. MatterColorControlPluginServerInitCallback() is empty
 *   (color-control-server.cpp:3345); nothing registers an
 *   AttributeAccessInterface for this cluster.
 *
 *   ServerInit? YES, and it does observable work, so this cluster joins the
 *   init-hook section below.
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

/* ---- thermostat (0x0301) (nRF 947-1040) -------------------------------
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
 *   this cluster does NOT join the init-hook section.
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

/* ---- fan (0x002B) (nRF 1041-1100) -------------------------------------
 *
 * Catalogue batch 2 audit, FanControl (0x0202). The simplest of the batch:
 * no CodegenIntegration.cpp and no CodeDrivenInitShutdown case, no
 * AttributeAccessInterface at all, and MatterFanControlPluginServerInit
 * Callback() is an empty definition in CHIP's own src/app/util/util.cpp:109.
 * There is no emberAfFanControlClusterServerInitCallback either, so nothing
 * to join the init-hook section with. Plain ember external storage.
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

/* ---- window covering (0x0202) (nRF 1125-1199) -------------------------
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
 *   NOT join the init-hook section. The only per-endpoint state is the delegate
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

/* ---- air quality sensor (0x002C) (nRF 1200-1246) ----------------------
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
 *   definition in src/app/util/util.cpp:112, not something the cluster
 *   provides, and this port has no equivalent of the C6's
 *   mt_air_quality_register_all(). With no Instance registered, reads and
 *   writes are plain ember external storage against the arena below - the
 *   same shape as OccupancySensing in batch 1.
 *
 *   ServerInit? None exists; nothing joins the init-hook section.
 *
 * The four optional features (Fair, Moderate, VeryPoor, ExtremelyPoor) are
 * all advertised, so the host library's seven-value AirQuality_t enum can
 * never report a value this endpoint's feature map does not admit. The mask
 * is NOT a literal in s_seeds: seed_slots() reads it from
 * mt_air_quality_feature_mask() (mt_matter.h), the single accessor whose
 * whole point is that the ember feature map and any future Instance's
 * BitMask<Feature> cannot drift apart. On this platform that function was a
 * stub returning 0 until this batch; it now lives in mt_matter_sl.cpp
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

/* ---- door lock (0x000A) (nRF 1248-1357) ------------------------
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

/* ---- water valve (0x0042) (nRF 1358-1484) ---------------------
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
 *   Delegate*) (cluster.h:40, impl cluster.cpp:262). Task 3 adds this
 *   port's mt_devtype_create() claim and SetDefaultDelegate() call, and
 *   with them the ordering rule this forces and why it differs from the
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

/* ---- the parenting policy (nRF 3325-3391) ----------------------------- */

/*
 * The five device type ids the PARENTING POLICY names, given names here
 * because they appear in two places that must agree: parent_policy_ok()
 * below and the registry rows themselves. (On the nRF there is a third, the
 * shape maps that render the same policy as cluster sets; those come with
 * the batch that ports the composed appliances.) Every other row in this
 * file keeps its bare hex literal, because no other row's id is read by
 * anything but the lookup.
 */
constexpr uint32_t kDtRefrigerator          = 0x0070;
constexpr uint32_t kDtTempControlledCabinet = 0x0071;
constexpr uint32_t kDtCookSurface           = 0x0077;
constexpr uint32_t kDtCooktop               = 0x0078;
constexpr uint32_t kDtOven                  = 0x007B;

/*
 * THE POLICY IS PORTED IN FULL EVEN THOUGH NEITHER RESTRICTED DEVICE TYPE IS
 * BUILT THIS ROUND, and that is the registry policy's whole point: the
 * predicate answers on the AT+MTEP= line, from the row's identity, so a host
 * gets the same +MTERR:1 for a cook surface with no cooktop parent on this
 * image as on the nRF's, months before either image can stand one up.
 *
 * variant is a parameter of the contract (core/include/mt_devtypes.h) and is
 * deliberately unused: neither rule depends on it today. It stays in the
 * signature, and in the compile-time check's enumeration, so a future rule
 * that DOES depend on it needs no plumbing.
 *
 * constexpr for the reason it is on the nRF: shape_domain_matches_policy()
 * below evaluates it over the whole registry at compile time.
 */
constexpr bool parent_policy_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    (void)variant;
    /* Cook Surface: legal ONLY under a Cooktop. The unparented form is
     * rejected by the same expression, since no device type id is 0 and
     * parent_devtype 0 is how the AT layer spells "no parent"
     * (core/mt/mt_at.c:1903-1905). This is the catalogue's only device type
     * that REQUIRES a parent. */
    if (devtype_id == kDtCookSurface) {
        return parent_devtype == kDtCooktop;
    }
    /* Temperature Controlled Cabinet: legal unparented (the standalone rows
     * earlier compositions declared keep working), or under a Refrigerator
     * or an Oven and nothing else. Those two are the appliances that give a
     * child cabinet a coherent conditional cluster set (Cooler, Heater); a
     * third parent would be a cabinet with no defined set at all, which is
     * why the policy refuses rather than falling back to the bare shape. */
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
 * A shape is one realised cluster set for one (variant, parent devtype)
 * pair (nRF 3392-3428). Rows whose cluster set does not depend on the parent
 * carry no shapes at all and keep the ep_type / ep_type_v1 pair the registry
 * struct describes; only a row the policy RESTRICTS needs a shape map.
 *
 * NO ROW CARRIES A SHAPE MAP IN THIS BUILD, because the only two rows that
 * ever do are the cabinet and the cook surface, whose cluster sets arrive
 * with nRF batch 8. The struct, the registry member and the selection arm in
 * mt_devtype_create() are carried anyway so that batch adds a TABLE and not
 * a mechanism, and so the two arms stay diffable.
 */
struct hearth_shape {
    uint8_t variant;
    uint32_t parent_devtype;
    const EmberAfEndpointType *ep_type;
};

/* ---- the registry (nRF 4402-4641) ------------------------------------- */

/*
 * ep_type_v1 is the port's rendering of the C6's max_variant semantics: a
 * max_variant-1 row's variant 1 is a SECOND declared cluster list, because
 * this port has no cluster::destroy() analogue to carve one list down at
 * runtime. It is the LAST member but one on purpose: aggregate
 * initialization zero-fills members a brace list does not reach, so every
 * max_variant-0 row keeps its exact original text and gets nullptr for free,
 * and mt_devtype_create() falls back to ep_type whenever ep_type_v1 is null.
 * device_types_v1 exists by the identical mechanism, for the rows whose
 * variant 1 advertises fewer device type ids than its variant 0 (the water
 * heater and battery storage, nRF batch 7b), and `shapes` (nRF batch 8) for
 * the rows whose cluster set depends on their parent: a non-empty shapes
 * span REPLACES the ep_type/ep_type_v1 selection entirely.
 *
 * In THIS build every one of those four members is null or empty on every
 * row but the twenty ported ones, which carry ep_type and device_types
 * alone.
 */
struct hearth_devtype {
    uint32_t id;
    uint8_t max_variant;
    const EmberAfEndpointType *ep_type;
    Span<const EmberAfDeviceType> device_types;
    const EmberAfEndpointType *ep_type_v1;
    Span<const EmberAfDeviceType> device_types_v1;
    Span<const hearth_shape> shapes;
};

/*
 * constexpr, not merely const: shape_domain_matches_policy() below has to
 * READ this table at compile time to prove the parenting policy and the
 * cluster sets agree, and a const object of class type is not usable in a
 * constant expression.
 *
 * ALL 52 CATALOGUE ROWS ARE HERE, in the nRF's order, so the two arms diff
 * row for row. Each row carries its id, its max_variant and (implicitly,
 * through parent_policy_ok() keying on the id) its parenting rule. A row
 * whose cluster set this build does not declare carries a trailing comment
 * naming the nRF batch that ports it, and nothing else: no ep_type, no
 * device types, no shapes. mt_devtype_create() refuses such a row at its
 * first check.
 *
 * The max_variant column is NOT a placeholder and must not be zeroed for an
 * unported row: mt_devtype_variant_ok() answers from it on the AT+MTEP=
 * line, so AT+MTEP=0x050F,1 has to be accepted by this image exactly as the
 * nRF accepts it. What differs is only what happens at the rebuild.
 */
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
    { 0x0011, 0 },                                                      /* batch 4: power source */
    { 0x0076, 0 },                                                      /* batch 4: smoke/co alarm */
    { 0x0073, 0 },                                                      /* batch 4: laundry washer */
    { 0x0075, 0 },                                                      /* batch 4: dishwasher */
    { 0x007C, 0 },                                                      /* batch 4: laundry dryer */
    { 0x0027, 0 },                                                      /* batch 4: mode select */
    { 0x0146, 0 },                                                      /* batch 4: chime */
    /* Catalogue batch 5: the standalone remainder. */
    { 0x002D, 0 },                                                      /* batch 5: air purifier */
    { 0x010F, 0 },                                                      /* batch 5: mounted on/off control */
    { 0x0110, 0 },                                                      /* batch 5: mounted dimmable load control */
    { 0x000F, 0 },                                                      /* batch 5: generic switch */
    { 0x0303, 0 },                                                      /* batch 5: pump */
    { 0x0072, 0 },                                                      /* batch 5: room air conditioner */
    { 0x0074, 0 },                                                      /* batch 5: robotic vacuum cleaner */
    /* Catalogue batch 7a: the energy foundation types, the registry's first
     * variant-carrying rows (variant 0 = power + energy, variant 1 = power
     * only). */
    { 0x0510, 1 },                                                      /* batch 7a: electrical sensor */
    { 0x0514, 1 },                                                      /* batch 7a: electrical meter */
    { 0x0309, 0 },                                                      /* batch 7a: heat pump */
    { 0x0017, 1 },                                                      /* batch 7a: solar power */
    { 0x0511, 0 },                                                      /* batch 7a: electrical utility meter */
    { 0x050D, 1 },                                                      /* batch 7a: device energy management */
    /* Catalogue batch 7b: the delegate-served energy pair, the first rows
     * with a variant-dependent device-type span. */
    { 0x050F, 1 },                                                      /* batch 7b: water heater */
    { 0x0018, 1 },                                                      /* batch 7b: battery storage */
    /* Catalogue batch 8: the composed appliances. The first three carry no
     * shapes even on the nRF: the policy is permissive for all of them, and
     * a cooktop's or an oven's own cluster set does not change when a child
     * is composed under it. */
    { kDtCooktop, 0 },                                                  /* batch 8: cooktop */
    { kDtOven, 0 },                                                     /* batch 8: oven */
    { 0x007A, 0 },                                                      /* batch 8: extractor hood */
    { kDtRefrigerator, 0 },                                             /* batch 8: refrigerator */
    /* The two shape-bearing rows. Their PARENTING RULE is live in this image
     * (parent_policy_ok() above keys on exactly these two ids); only their
     * cluster sets are batch 8's. */
    { kDtTempControlledCabinet, 1 },                                    /* batch 8: temperature controlled cabinet */
    { kDtCookSurface, 1 },                                              /* batch 8: cook surface */
    { 0x0079, 0 },                                                      /* batch 8: microwave oven */
    /* The EVSE round: the fifty-second and last row, closing the catalogue. */
    { 0x050C, 1 },                                                      /* EVSE round: energy EVSE */
};

static_assert(sizeof(s_registry) / sizeof(s_registry[0]) == 52,
              "the catalogue is 52 device types; a row was added or lost");

/*
 * ---- the tie between the two encodings of the parenting policy --------
 *
 * nRF 4543-4641, recast for a registry most of whose rows have no cluster
 * set. This walks the whole registry at compile time and proves, for every
 * row THIS BUILD CAN CREATE, every variant it accepts and every device type
 * in the catalogue used as a parent (plus the unparented form), that:
 *
 *   a row the policy RESTRICTS carries a shape map, and its domain is
 *   EXACTLY the policy's accept set: one shape for every accepted pair, no
 *   shape for any rejected pair, and never two shapes for one pair;
 *
 *   a row the policy leaves PERMISSIVE carries no shape map, because it
 *   needs none: its cluster set is chosen by variant alone.
 *
 * WHAT THE RECAST CHANGED, and what it did not. The nRF's version runs this
 * check over every row unconditionally, because on that arm every row has a
 * cluster set. Here a row with neither an ep_type nor a shape map is a row
 * this build cannot create at all, and there is nothing to check: it has no
 * cluster set for any (variant, parent) pair, the policy cannot leave one
 * unserved, and mt_devtype_create() refuses it before the parent is even
 * looked at. So the per-row body is skipped for those, and what the
 * assertion now proves is the same property over the rows that can be
 * created. It does NOT weaken into vacuity: the twenty ported rows are checked
 * in full, the parent universe is still the whole 52-row registry (so a
 * future restricted row is enumerated against every catalogue id from the
 * moment it gains a cluster set), and the day a batch gives the cabinet or
 * the cook surface an ep_type without a shape map, this fails the build
 * exactly as it would on the nRF.
 *
 * Which direction is load-bearing, said plainly. "Every shape is accepted"
 * catches a dead shape row, which is confusing but harmless. "Every accepted
 * pair has a shape" catches the dangerous one: a pairing AT+MTEP accepts at
 * staging time whose cluster set does not exist, which would be discovered
 * only at the next boot, as a create failure that aborts the whole rebuild
 * and truncates the composition.
 */
constexpr bool shape_domain_matches_policy()
{
    constexpr size_t kRows = sizeof(s_registry) / sizeof(s_registry[0]);
    for (size_t r = 0; r < kRows; r++) {
        const hearth_devtype &e = s_registry[r];
        /* An unported row: no cluster set for any pairing, refused by
         * mt_devtype_create() at its first check. Nothing to prove. */
        if (e.ep_type == nullptr && e.shapes.empty()) {
            continue;
        }
        for (size_t s = 0; s < e.shapes.size(); s++) {
            if (e.shapes.data()[s].variant > e.max_variant) {
                return false;
            }
            if (e.shapes.data()[s].ep_type == nullptr) {
                return false;
            }
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
              "this build CREATES has no shape map, a shape map has a hole or a duplicate, or "
              "a permissive row grew a shape map");

/* ---- the external attribute store (nRF 4642-4658) --------------------- */

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
 * ---- the endpoint block arena (nRF 4659-5272) ------------------------
 *
 * A slim static header table (below) plus exactly ONE arena block per
 * created endpoint, holding that endpoint's DataVersion array and its
 * attribute slots, sized for ITS device type:
 *
 *     +----------------------------+  <- dyn_endpoint::block
 *     | DataVersion dv[n_clusters] |     4 * clusterCount bytes
 *     +----------------------------+
 *     | attr_slot slots[n_slots]   |     16 * count_slots() bytes
 *     +----------------------------+
 *     | host-fed store(s), only    |     store_bytes(): nothing on any
 *     | for types that carry one   |     device type this build declares
 *     +----------------------------+
 *
 * The nRF's alternative was a FLAT arena: one dyn_endpoint holding
 * DataVersion dv[kMaxClusters] and attr_slot slots[kMaxSlots] sized for the
 * WIDEST device type in the catalogue, MT_COMP_MAX_ENDPOINTS deep, which
 * cost it 18,144 bytes of .bss whether or not a single endpoint was ever
 * created and made an on/off light pay the extended colour light's bill.
 * The per-endpoint block is that defect's fix and it is transferred whole,
 * even though a twenty-device-type catalogue would barely notice the flat
 * form today: the shape is what the later batches need.
 *
 * dv first is what keeps the layout alignment-free. Arena blocks are
 * 8-aligned (hearth_arena_alloc), which is MORE than every region needs:
 * DataVersion is uint32_t and attr_slot's leading members are uint32_t, so
 * both want 4-byte alignment, and an integral number of uint32_t of dv can
 * never leave the slots misaligned. The trailing region starts at
 * 4 * clusterCount + 16 * n_slots, a multiple of 4. block_dv() and
 * block_slots() are the only places that know this layout.
 *
 * A DEDICATED arena, not the one system heap. Three reasons, all of which
 * matter more than the handful of bytes an allocator's metadata would cost
 * (and here it costs none at all):
 *
 *   - Failure is CONTAINED. An oversized composition cannot starve the CHIP
 *     stack, mbedTLS or OpenThread, all of which draw on
 *     sl_memory_manager's pool; it fails here, at the endpoint that does not
 *     fit, and nowhere else.
 *   - Failure is MEASURABLE. The cap is one number in one place, so the
 *     failure log in mt_devtype_create() can name exactly what was asked for
 *     and what was left, rather than reporting a generic allocation failure
 *     whose real cause is somewhere else entirely.
 *   - The budget is auditable. "3 KB of endpoint blocks" is a line item in
 *     the README's capacity table; "some of the system heap" is not.
 *
 * ALLOCATE-ONLY, and that is what makes fragmentation a non-question rather
 * than a risk to be managed. Blocks are allocated in exactly one place,
 * mt_devtype_create(), which is reached from exactly one caller,
 * src/main.cpp's rebuild_composition(), which runs once at boot before the
 * AT link carries a byte. Nothing frees. There is no AT command that
 * destroys an endpoint: the composition is edited by AT+MTEP and applied by
 * a reboot, and a reboot resets this arena wholesale.
 *
 * The one deliberate leak: if emberAfSetDynamicEndpoint() fails AFTER a
 * successful allocation, the block is not returned. That path returns -1
 * and the rebuild stops there, so no further endpoint is created and the
 * block can never be reached again before the reboot that resets the arena.
 * Bounded at one block per boot. Freeing it would be actively worse, not
 * merely pointless: CHIP keeps the dataVersions span it was handed in
 * emAfEndpoints[index], and its own failure path does not always clear that
 * slot, so handing the memory back would turn that into a dangling pointer
 * for the next allocation to reuse. (A bump arena has no free at all, which
 * is the same conclusion reached by construction.)
 *
 * ---- sizing -----------------------------------------------------------
 *
 * Block payload is 4 * clusterCount + 16 * slots. The cost is that rounded
 * up to 8 and nothing more (hearth_arena_cost, mt_dyn_store.h: no chunk
 * header, no bucket table, so usable bytes are gross bytes). The whole
 * catalogue this build declares:
 *
 *   device type                        clusters  slots  payload  arena cost
 *   extended colour light   0x010D            5     36      596         600
 *   colour temperature lt   0x010C            5     32      532         536
 *   dimmable light / plug  0x0101 0x010B       4     20      336         336
 *   thermostat              0x0301            3     15      252         256
 *   window covering         0x0202            3     13      220         224
 *   on/off light / plug    0x0100 0x010A       3     11      188         192
 *   fan                     0x002B            3     10      172         176
 *   temp/humidity/pressure/light/flow
 *     0x0302 0x0307 0x0305 0x0106 0x0306       3      9      156         160
 *   occupancy sensor       0x0107             3      9      156         160
 *   door lock              0x000A            3     12      204         208
 *   water valve            0x0042            3     11      188         192
 *   air quality sensor     0x002C            3      7      124         128
 *   boolean-state sensors
 *     0x0015 0x0044 0x0041 0x0043             3      7      124         128
 *
 * Slots are the declared attributes plus the LIST_END ClusterRevision each
 * cluster carries, Identify's four included and Descriptor's none.
 *
 * HEARTH_EP_ARENA_BYTES is 9,600, which is 16 x 600: this arena holds
 * kServiceableEndpoints of the WIDEST type it can build, the extended colour
 * light, with nothing left over and nothing wasted. Catalogue batch 2 kept
 * batch 1's strong promise (every composition the build accepts, it can
 * build) by raising HEARTH_EP_ARENA_BYTES from 5,376 to 9,600 rather than
 * lowering the floor to the nRF's eight (the ruling of 2026-09-24). It is
 * still a stronger promise than the nRF's, whose 8,112 usable bytes hold
 * only 13 of the same extended colour light. The floor below asserts the
 * strong form deliberately: the batch that adds a wider type will fail this
 * build and have to make the same trade the nRF made, in the open, rather
 * than inherit a weaker floor by accident.
 */
constexpr size_t kEpArenaBytes = HEARTH_EP_ARENA_BYTES;

alignas(8) uint8_t s_ep_arena_mem[kEpArenaBytes];
hearth_arena s_ep_arena = { s_ep_arena_mem, kEpArenaBytes, 0 };

/*
 * Which attributes get a slot. The single predicate seed_slots() and
 * count_slots() both consult, so the size counted at allocation time and
 * the number actually written can never disagree: a mismatch would either
 * overrun the block or leave an attribute unserved.
 *
 * ARRAY-typed attributes are the list globals (AttributeList,
 * AcceptedCommandList, GeneratedCommandList), which CHIP's own machinery
 * answers and which would not fit a 4-byte slot anyway. STRUCT-typed ones
 * are served entirely by a cluster's own AttributeAccessInterface and
 * declared only so AttributeList is truthful. Anything wider than the slot
 * payload is refused rather than truncated; seed_slots() logs it, this
 * predicate stays silent so the two loops agree exactly.
 */
bool attr_gets_slot(const EmberAfAttributeMetadata &md)
{
    return md.attributeType != ZAP_TYPE(ARRAY) && md.attributeType != ZAP_TYPE(STRUCT) &&
           md.size <= kSlotDataBytes;
}

/* Descriptor is served by CHIP's own DescriptorCluster server object,
 * registered per endpoint by the cluster init callback that
 * emberAfSetDynamicEndpoint() fires; its reads never reach the
 * external-storage callbacks, so it gets no slots and no block space. */
bool cluster_gets_slots(const EmberAfCluster &cl)
{
    return cl.clusterId != Descriptor::Id;
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
 * The type-conditional trailing stores (nRF 5032-5197), reduced to the one
 * question the block layout asks. On the nRF this is store_walk() over an
 * eleven-row kStoreWalk table, placing each host-fed store (mode select's
 * 436 B, chime's 273, the ModeBase families' 306 each, the temperature-level
 * labels' 273) at its own alignment behind the slot region. Not one of those
 * device types is built by this round, so every one of those rows would be
 * dead data keyed on a cluster no list here declares, and the four
 * sizeof/alignof assertions that keep the table honest would be assertions
 * about structures nothing allocates.
 *
 * What is kept is the CALL: the block layout asks store_bytes() how many
 * trailing bytes this device type wants, exactly as the nRF's does, so the
 * batch that ports the first store-bearing type replaces this function with
 * the table and changes nothing else about how a block is sized, placed or
 * constructed.
 */
constexpr size_t store_bytes(const EmberAfEndpointType *)
{
    return 0;
}

/*
 * The header table. Four fields plus the two counts the block walk needs;
 * everything else about an endpoint lives in its arena block, which `block`
 * points at.
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
    /* The cluster list this endpoint was actually created with: the
     * variant's own list, or the shape's, chosen once in
     * mt_devtype_create(). Every block-layout walk reads it from here so the
     * layout can never be computed against the wrong variant's list. The
     * variant is kept beside it for the seeds that depend on it. */
    const EmberAfEndpointType *ep_type;
    uint8_t variant;
    void *block;
    uint16_t slot_capacity;
    uint16_t slot_count;
};

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
 * ---- the compiler-checked floor under the arena sizing (nRF 5273-5978) --
 *
 * count_slots() runs at create time, so no device type's size is a
 * compile-time constant any more, and the old kMaxSlots / kMaxClusters
 * ceilings no longer bound any array: dv and slots are sized exactly. What
 * DOES still need guarding is the sizing story. HEARTH_EP_ARENA_BYTES was
 * chosen against the table above, and that table holds only while the
 * catalogue's widest device type stays roughly the size it is now. Add a
 * device type with 80 slots and the capacity line goes quietly wrong: no
 * build error, no runtime error, until a real composition hits the wall on
 * someone's bench.
 *
 * So the widest type is computed at compile time, from the same
 * DECLARE_DYNAMIC_* arrays the registry is built from, and asserted against
 * a floor.
 *
 * WHAT THIS DOES NOT PROTECT, said once and plainly, because it reads more
 * confidently than the mechanism deserves: the candidate set is
 * HAND-MAINTAINED, exactly as it is on the nRF. Nothing ties the chain below
 * to s_registry, so a device type added with no candidate of its own is not
 * guarded. What the assertions guarantee is that a candidate which IS
 * declared cannot be quietly weakened. Deriving the candidate set from
 * s_registry the way shape_domain_matches_policy() derives the parent
 * universe would close the gap; count_slots() is not constexpr, which is
 * what has stopped it on both arms.
 *
 * TWO THINGS THE nRF'S VERSION HAS AND THIS ONE DOES NOT, both because the
 * thing they price does not exist here:
 *
 *   - The store-bearing candidates (kModeSelectBlockBytes, kChimeBlockBytes,
 *     kRvcBlockBytes, kDemBlockBytes, kWaterHeaterBlockBytes,
 *     kBatteryStorageBlockBytes, kCabinetHeaterLevelBlockBytes,
 *     kMicrowaveBlockBytes, kEvseBlockBytes) and the exact-counting
 *     convention that keeps their pinned payloads honest. Every one is a
 *     sum over attribute lists this file does not declare.
 *   - The capped half of the floor (kFloorDemand, kEvseEndpointCap and its
 *     two non-vacuity assertions). It exists because the EVSE's own
 *     MT_EVSE_MAX of 2 makes "room for eight of it" a demand no composition
 *     can make. No device type in this build carries a capacity cap of its
 *     own, so every candidate is uncapped and the floor is the single
 *     inequality the nRF had before the EVSE round. The capped half comes
 *     back with the EVSE.
 */
#define MT_COUNT(array) (sizeof(array) / sizeof((array)[0]))

constexpr size_t kMax2(size_t a, size_t b) { return a > b ? a : b; }

/* Identify rides on all twenty device types; Descriptor contributes nothing.
 * MT_COUNT over an attr list counts DECLARED entries plus the LIST_END
 * ClusterRevision, which over-counts a list whose metadata-only members get
 * no slot; no list here has one, so these counts are exact, and an
 * over-count would only ever make the asserted floor MORE conservative. */
constexpr size_t kIdentifySlots = MT_COUNT(identifyAttrs);
/* The widest endpoint is the one with the most slots: the colour lights
 * carry OnOff AND LevelControl AND ColorControl, and the extended one
 * declares sixteen ColorControl attributes. Each MT_COUNT counts declared
 * entries plus the LIST_END ClusterRevision; no list here has a
 * metadata-only member, so the counts are exact. */
constexpr size_t kWidestEndpointSlots =
    kIdentifySlots +
    kMax2(
        kMax2(
            kMax2(MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) + MT_COUNT(colorTempAttrs),
                  MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) + MT_COUNT(extendedColorAttrs)),
            kMax2(MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs),
                  kMax2(kMax2(kMax2(MT_COUNT(tempAttrs), MT_COUNT(booleanStateAttrs)),
                              kMax2(MT_COUNT(occupancyAttrs), MT_COUNT(humidityAttrs))),
                        kMax2(MT_COUNT(pressureAttrs),
                              kMax2(MT_COUNT(illuminanceAttrs), MT_COUNT(flowAttrs)))))),
        kMax2(
            kMax2(MT_COUNT(thermostatAttrs), MT_COUNT(fanControlAttrs)),
            kMax2(kMax2(MT_COUNT(windowCoveringAttrs), MT_COUNT(airQualityAttrs)),
                  kMax2(MT_COUNT(doorLockAttrs), MT_COUNT(valveAttrs)))));
/* The widest cluster list is a max over every declared cluster list, not
 * just the dimmable light's: a future device type with a wider list must
 * fail the floor's static_assert below at build time, not surface later as a
 * runtime allocation refusal when its endpoint overruns block_bytes()'s
 * sizing. Catalogue batch 2's six five- and three-cluster lists join it; the
 * two five-cluster colour light lists are the max. */
constexpr size_t kWidestClusterList = kMax2(
    kMax2(
        kMax2(kMax2(MT_COUNT(onOffLightClusters), MT_COUNT(dimmableLightClusters)),
              kMax2(MT_COUNT(colorTemperatureLightClusters), MT_COUNT(extendedColorLightClusters))),
        kMax2(MT_COUNT(onOffPlugInUnitClusters), MT_COUNT(dimmablePlugInUnitClusters))),
    kMax2(
        kMax2(kMax2(MT_COUNT(thermostatClusters), MT_COUNT(fanClusters)),
              kMax2(kMax2(MT_COUNT(windowCoveringClusters), MT_COUNT(airQualitySensorClusters)),
                    kMax2(MT_COUNT(doorLockClusters), MT_COUNT(waterValveClusters)))),
        kMax2(MT_COUNT(temperatureSensorClusters),
              kMax2(kMax2(MT_COUNT(booleanStateSensorClusters), MT_COUNT(occupancySensorClusters)),
                    kMax2(MT_COUNT(humiditySensorClusters),
                          kMax2(MT_COUNT(pressureSensorClusters),
                                kMax2(MT_COUNT(lightSensorClusters), MT_COUNT(flowSensorClusters))))))));

constexpr size_t kWidestBlockBytes =
    block_bytes(kWidestClusterList, kWidestEndpointSlots) +
    /* store_bytes() is 0 for every declared type; written as a term so the
     * batch that ports a store-bearing type has a place to put it. */
    0;

/* Pinned, so the sizing table above cannot go stale without the build
 * noticing: the extended colour light is the widest type this build
 * declares, at 596 payload bytes and 600 of arena. */
static_assert(kWidestBlockBytes == 596,
              "the widest declared block changed size; redo the sizing table above and the "
              "README's capacity rows");
static_assert(hearth_arena_cost(kWidestBlockBytes) == 600,
              "the widest declared block's arena cost moved off the table's 600");

/*
 * Usable bytes ARE gross bytes on a bump arena: nothing is spent on an
 * allocator before the first allocation. This constant exists so the
 * capacity logs and the floor read like the nRF's, where it is the gross
 * define minus 80.
 */
constexpr size_t kArenaUsableBytes = kEpArenaBytes;

/*
 * The floor. The nRF demands room for kMinWidestEndpoints = 8 of its widest
 * uncapped type, eight being "the point below which the capacity table would
 * be describing a different device"; sizing for sixteen of its heaviest is
 * precisely the trade that round declined, and its own arena serves only
 * thirteen of the extended colour light this catalogue now builds. This
 * catalogue stays small enough that the strong form holds: sixteen extended
 * colour lights are 9,600 bytes, so the floor stays the full
 * kServiceableEndpoints and the capacity claim is "every composition this
 * build accepts, it can build". Catalogue batch 1 kept that promise by
 * raising HEARTH_EP_ARENA_BYTES from 3,072 to 5,376 rather than lowering the
 * floor (the ruling of 2026-09-22); catalogue batch 2's wider types revisit
 * the choice and keep the same strong form by raising it from 5,376 to
 * 9,600 (the ruling of 2026-09-24).
 *
 * That is deliberate rather than incidental. The next batch that adds a
 * wider device type fails THIS assertion and has to choose, with the numbers
 * in front of it, between raising HEARTH_EP_ARENA_BYTES again and dropping
 * to a floor of eight the way the nRF did. Asserting eight today would let
 * that batch inherit the weaker promise without anyone deciding to.
 */
constexpr size_t kMinWidestEndpoints = kServiceableEndpoints;

static_assert(hearth_arena_cost(kWidestBlockBytes) * kMinWidestEndpoints <= kArenaUsableBytes,
              "the endpoint arena no longer holds kServiceableEndpoints of the widest declared "
              "device type; either raise HEARTH_EP_ARENA_BYTES or lower kMinWidestEndpoints to "
              "the nRF's eight and redo the sizing table and the README's capacity rows");

dyn_endpoint s_dyn[kServiceableEndpoints];

/* How many header slots are currently taken. Only ever used to make a
 * failure log name both walls rather than one; the header-table check in
 * mt_devtype_create() does its own scan because it needs the free index,
 * not the count. */
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

/* Endpoint ids run 1..N in composition order and are reassigned from 1 on
 * every boot. The composition itself is what persists, so replaying it in
 * order reproduces the same ids: that is the property AT+MTEP? and any
 * commissioned fabric depend on. */
uint16_t s_next_ep_id = 1;

/*
 * Boot values for attributes where zero is the wrong answer, little-endian
 * as the attribute store holds them (nRF 5905-6889). Two kinds of entry live
 * here:
 *
 *   Functional. LevelControl reads MinLevel and MaxLevel out of the store
 *   when the endpoint is enabled and keeps them as the movement bounds for
 *   the life of the endpoint. Left at zero, a dimmable light clamps every
 *   MoveToLevel to 0 and never lights.
 *
 *   Conformance. FeatureMap and ClusterRevision must describe the cluster
 *   the endpoint actually presents. Revisions are the tree's own
 *   zzz_generated/app-common/clusters/<Cluster>/Metadata.h kRevision; feature
 *   bits are from the matching Enums.h.
 *
 * Nullable attributes whose correct boot value is null carry the type's
 * null sentinel rather than a zero: NumericAttributeTraits::GetNullValue()
 * is the type maximum for unsigned and enum types (0xFF for a 1-byte one)
 * and the type minimum for signed ones (0x8000 for INT16S, stored
 * little-endian as 00 80).
 *
 * THE WHOLE TABLE IS TRANSFERRED, not just the rows the twenty ported
 * device types consult, and that is a deliberate departure from this file's own
 * "only what is ported" rule. Three reasons. It is const data keyed by
 * (cluster, attribute), so a row for a cluster no list declares is never
 * looked up and costs flash only. It compiles against the app-common id
 * headers, which the SDK generates for all 145 clusters whether or not their
 * servers are in the image, so nothing here depends on a cluster component.
 * And the seeds are the part of a device type most easily got wrong and
 * least visible when wrong: a batch that ports a device type and forgets its
 * FeatureMap row ships an endpoint that misreports its own capabilities, so
 * having the rows already here, already reviewed on the other arm, is worth
 * more than the flash.
 *
 * The `devtype` qualifier (nRF batch 2) exists because one (cluster,
 * attribute) pair does not always have one boot value across every device
 * type carrying it: 0x010C advertises ColorControl's CT alone and 0x010D
 * advertises HS|XY|CT. devtype 0 means "any device type". The `variant`
 * qualifier (nRF batch 8) is the same trick one member further out, for a
 * FeatureMap that is variant-dependent AND shared by two device types.
 * Both are LAST members, so a row that names neither keeps its exact
 * original text and gets the wildcard for free.
 *
 * THE ENCODING, because the zero-fill has to keep meaning "any variant":
 * 0 is the wildcard and a row naming a variant stores variant + 1.
 * seed_variant() is the only place that arithmetic appears.
 *
 * PRECEDENCE: the most specific matching row wins, scored devtype-qualified
 * 2, variant-qualified 1, both 3, neither 0. A row whose qualifier is
 * present but does not match is skipped outright.
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
     * Revision 9 is DoorLock/Metadata.h kRevision in THIS tree
     * (third_party/matter_sdk/zzz_generated/app-common/clusters/DoorLock/
     * Metadata.h, Silabs 2.8.1); the nRF port seeds 7, which is its own
     * NCS tree's value, so the two ports differ on purpose (graph F540):
     * the rule is the tree the build consumes. */
    { DoorLock::Id, DoorLock::Attributes::LockState::Id, 1, { 0xFF } }, /* null */
    { DoorLock::Id, DoorLock::Attributes::LockType::Id, 1, { 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::ActuatorEnabled::Id, 1, { 0x01 } },
    { DoorLock::Id, DoorLock::Attributes::AutoRelockTime::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::OperatingMode::Id, 1, { 0x00 } },
    { DoorLock::Id, DoorLock::Attributes::SupportedOperatingModes::Id, 2, { 0xF6, 0xFF } },
    { DoorLock::Id, DoorLock::Attributes::FeatureMap::Id, 4, { 0x00, 0x00, 0x00, 0x00 } },
    { DoorLock::Id, Globals::Attributes::ClusterRevision::Id, 2, { 0x09, 0x00 } },

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

/* Fills this endpoint's block (nRF 6891-7005). Walks the same two
 * predicates count_slots() used to size it, so slot_count lands on
 * slot_capacity exactly; the bound below is a backstop against those two
 * drifting, not a normal path. */
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
                 * stay quiet, as are CHAR_STRING and STRUCT, which are
                 * deliberate metadata-only declarations made so
                 * AttributeList stays truthful for attributes this arena
                 * cannot hold. Anything ELSE too wide is still a mistake
                 * worth shouting about.
                 *
                 * The nRF additionally consults attr_quiet_no_slot() here,
                 * a narrow per-(cluster, attribute) table (DE407 option C)
                 * that silences the shout for over-wide scalars PROVEN to
                 * be served by a cluster Instance instead. Every row in it
                 * belongs to an energy cluster this build does not declare,
                 * so the table is absent and the shout is unconditional.
                 * That is the ruling's intent, not a gap: the table admits
                 * one proven pair at a time and never a whole type, so it
                 * arrives with the batch that declares the first such
                 * attribute. */
                if (md.attributeType != ZAP_TYPE(ARRAY) &&
                    md.attributeType != ZAP_TYPE(CHAR_STRING) &&
                    md.attributeType != ZAP_TYPE(STRUCT)) {
                    HEARTH_LOGE("devtypes", "attr 0x%08X on cluster 0x%08X is %u bytes, a slot "
                                            "holds %u",
                                (unsigned)md.attributeId, (unsigned)cl.clusterId,
                                (unsigned)md.size, (unsigned)kSlotDataBytes);
                }
                continue;
            }
            if (d->slot_count >= d->slot_capacity) {
                HEARTH_LOGE("devtypes", "devtype 0x%04X block holds %u slots, seeding wanted more",
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
             * little-endian, the same convention as every seed row.
             * Catalogue batch 2's air quality sensor (0x002C) is the first
             * device type in this build to reach this arm. */
            if (cl.clusterId == AirQuality::Id &&
                md.attributeId == Globals::Attributes::FeatureMap::Id) {
                uint32_t mask = mt_air_quality_feature_mask();
                for (uint8_t b = 0; b < s.size && b < sizeof(s.data); b++) {
                    s.data[b] = (uint8_t)(mask >> (8 * b));
                }
                continue;
            }

            /* The most specific matching seed row wins: a row naming this
             * device type AND this variant beats one naming only the device
             * type, which beats one naming only the variant, which beats the
             * bare wildcard. A row whose qualifier is present but does not
             * match is skipped. Scored rather than short-circuited so the
             * table's row order stays irrelevant. */
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
 * ColorControl hook take. Endpoint 240's own functions array runs its
 * init, so it returns at once (nRF mt_devtypes_zephyr.cpp 7025-7049 is
 * the model).
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
