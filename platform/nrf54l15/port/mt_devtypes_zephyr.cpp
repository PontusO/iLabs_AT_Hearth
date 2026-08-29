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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(identifyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyTime::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

/* One metadata array per cluster, shared by every endpoint type that
 * carries that cluster: EmberAfCluster holds a const pointer to it and
 * nothing ever writes through that pointer, so a copy per devtype would
 * only cost flash. */

/* ---- on/off light (0x0100) ------------------------------------------- */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::GlobalSceneControl::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnTime::Id, INT16U, 2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OffWaitTime::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::StartUpOnOff::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kOnOffIncoming[] = { OnOff::Commands::Off::Id, OnOff::Commands::On::Id,
                                         OnOff::Commands::Toggle::Id, kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(onOffLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(onOffLightEndpoint, onOffLightClusters);

constexpr EmberAfDeviceType kOnOffLightTypes[] = { { 0x0100, 3 } };

/* ---- dimmable light (0x0101) ----------------------------------------- */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(levelAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kLevelIncoming[] = {
    LevelControl::Commands::MoveToLevel::Id,          LevelControl::Commands::Move::Id,
    LevelControl::Commands::Step::Id,                 LevelControl::Commands::Stop::Id,
    LevelControl::Commands::MoveToLevelWithOnOff::Id, LevelControl::Commands::MoveWithOnOff::Id,
    LevelControl::Commands::StepWithOnOff::Id,        LevelControl::Commands::StopWithOnOff::Id,
    kInvalidCommandId
};

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(dimmableLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(dimmableLightEndpoint, dimmableLightClusters);

constexpr EmberAfDeviceType kDimmableLightTypes[] = { { 0x0101, 3 } };

/* ---- temperature sensor (0x0302) ------------------------------------- */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(tempAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MinMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(temperatureSensorClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureMeasurement::Id, tempAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(temperatureSensorEndpoint, temperatureSensorClusters);

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(booleanStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::StateValue::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

/* Fix round 2, M2: Contact/Rain/Water Freeze/Water Leak all compose to the
 * exact same cluster set (BooleanState + Identify + Descriptor, mandatory
 * clusters only), so one cluster list and one EmberAfEndpointType serve
 * all four -- the same "one metadata array per cluster, shared by every
 * endpoint type that carries that cluster" principle the top of this file
 * states for onOffAttrs, extended here to the whole cluster list since the
 * whole composition, not just one cluster, is identical across these four.
 * Only the EmberAfDeviceType (id, revision) differs per device type, and
 * that is what s_registry keys off. */
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(booleanStateSensorClusters)
DECLARE_DYNAMIC_CLUSTER(BooleanState::Id, booleanStateAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(booleanStateSensorEndpoint, booleanStateSensorClusters);

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(occupancyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::Occupancy::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::OccupancySensorType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id, BITMAP8, 1,
                              0),
    DECLARE_DYNAMIC_ATTRIBUTE(OccupancySensing::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(occupancySensorClusters)
DECLARE_DYNAMIC_CLUSTER(OccupancySensing::Id, occupancyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(occupancySensorEndpoint, occupancySensorClusters);

constexpr EmberAfDeviceType kOccupancySensorTypes[] = { { 0x0107, 4 } };

/* ---- humidity sensor (0x0307) ------------------------------------------ */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(humidityAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(RelativeHumidityMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(humiditySensorClusters)
DECLARE_DYNAMIC_CLUSTER(RelativeHumidityMeasurement::Id, humidityAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(humiditySensorEndpoint, humiditySensorClusters);

constexpr EmberAfDeviceType kHumiditySensorTypes[] = { { 0x0307, 2 } };

/* ---- pressure sensor (0x0305) ------------------------------------------ */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(pressureAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::MinMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(PressureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(pressureSensorClusters)
DECLARE_DYNAMIC_CLUSTER(PressureMeasurement::Id, pressureAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(pressureSensorEndpoint, pressureSensorClusters);

constexpr EmberAfDeviceType kPressureSensorTypes[] = { { 0x0305, 2 } };

/* ---- light (illuminance) sensor (0x0106) -------------------------------
 *
 * IlluminanceMeasurement's MeasuredValue is uint16 (INT16U), unlike
 * TemperatureMeasurement/PressureMeasurement's signed int16s: the null
 * sentinel is therefore the type MAXIMUM (0xFFFF, NumericAttributeTraits::
 * GetNullValue() for an unsigned type), not the signed-type minimum 0x8000
 * the temperature/pressure seeds use. Same attr_null_sentinel() convention
 * (mt_matter_zephyr.cpp), different type -> different sentinel bytes. */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(illuminanceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::MeasuredValue::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(IlluminanceMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(lightSensorClusters)
DECLARE_DYNAMIC_CLUSTER(IlluminanceMeasurement::Id, illuminanceAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(lightSensorEndpoint, lightSensorClusters);

constexpr EmberAfDeviceType kLightSensorTypes[] = { { 0x0106, 3 } };

/* ---- flow sensor (0x0306) ----------------------------------------------- */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(flowAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::MeasuredValue::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FlowMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(flowSensorClusters)
DECLARE_DYNAMIC_CLUSTER(FlowMeasurement::Id, flowAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(flowSensorEndpoint, flowSensorClusters);

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
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(onOffPlugInUnitClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(onOffPlugInUnitEndpoint, onOffPlugInUnitClusters);

constexpr EmberAfDeviceType kOnOffPlugInUnitTypes[] = { { 0x010A, 4 } };

/* ---- dimmable plug-in unit (0x010B) -------------------------------------
 *
 * Reuses onOffAttrs/levelAttrs/kOnOffIncoming/kLevelIncoming verbatim:
 * DimmablePlug-InUnit.xml mandates OnOff feature LT and LevelControl
 * features OO+LT, the identical set DimmableLight.xml mandates, so this
 * cluster list and its seeds (FeatureMap 0x01 for OnOff, 0x03 for
 * LevelControl) are the same as the dimmable light above; see that
 * device type's seed rows in s_seeds, none of which are duplicated here. */
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(dimmablePlugInUnitClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(dimmablePlugInUnitEndpoint, dimmablePlugInUnitClusters);

constexpr EmberAfDeviceType kDimmablePlugInUnitTypes[] = { { 0x010B, 5 } };

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

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(colorTempAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

/* The extended light's list is colorTempAttrs plus the HS and XY quartet.
 * Spelled out rather than composed: DECLARE_DYNAMIC_ATTRIBUTE_LIST_* builds a
 * plain array and there is no concatenation macro. */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(extendedColorAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

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

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(colorTemperatureLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, colorTempAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kColorTempIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(colorTemperatureLightEndpoint, colorTemperatureLightClusters);

constexpr EmberAfDeviceType kColorTemperatureLightTypes[] = { { 0x010C, 4 } };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(extendedColorLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelAttrs, ZAP_CLUSTER_MASK(SERVER), kLevelIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, extendedColorAttrs, ZAP_CLUSTER_MASK(SERVER),
                            kExtendedColorIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(extendedColorLightEndpoint, extendedColorLightClusters);

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(thermostatAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kThermostatIncoming[] = { Thermostat::Commands::SetpointRaiseLower::Id,
                                              kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(thermostatClusters)
DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kThermostatIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(thermostatEndpoint, thermostatClusters);

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(fanControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanMode::Id, ENUM8, 1,
                          ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanModeSequence::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentSetting::Id, PERCENT, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) | ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentCurrent::Id, PERCENT, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(fanClusters)
DECLARE_DYNAMIC_CLUSTER(FanControl::Id, fanControlAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(fanEndpoint, fanClusters);

constexpr EmberAfDeviceType kFanTypes[] = { { 0x002B, 4 } };

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(windowCoveringAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kWindowCoveringIncoming[] = { WindowCovering::Commands::UpOrOpen::Id,
                                                  WindowCovering::Commands::DownOrClose::Id,
                                                  WindowCovering::Commands::StopMotion::Id,
                                                  WindowCovering::Commands::GoToLiftPercentage::Id,
                                                  kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(windowCoveringClusters)
DECLARE_DYNAMIC_CLUSTER(WindowCovering::Id, windowCoveringAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kWindowCoveringIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(windowCoveringEndpoint, windowCoveringClusters);

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(airQualityAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(AirQuality::Attributes::AirQuality::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(AirQuality::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(airQualitySensorClusters)
DECLARE_DYNAMIC_CLUSTER(AirQuality::Id, airQualityAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(airQualitySensorEndpoint, airQualitySensorClusters);

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(doorLockAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kDoorLockIncoming[] = { DoorLock::Commands::LockDoor::Id,
                                            DoorLock::Commands::UnlockDoor::Id,
                                            kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(doorLockClusters)
DECLARE_DYNAMIC_CLUSTER(DoorLock::Id, doorLockAttrs, ZAP_CLUSTER_MASK(SERVER), kDoorLockIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(doorLockEndpoint, doorLockClusters);

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(valveAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kWaterValveIncoming[] = { ValveConfigurationAndControl::Commands::Open::Id,
                                              ValveConfigurationAndControl::Commands::Close::Id,
                                              kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(waterValveClusters)
DECLARE_DYNAMIC_CLUSTER(ValveConfigurationAndControl::Id, valveAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kWaterValveIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(waterValveEndpoint, waterValveClusters);

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

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(powerSourceAttrs)
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
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(powerSourceClusters)
DECLARE_DYNAMIC_CLUSTER(PowerSource::Id, powerSourceAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(powerSourceEndpoint, powerSourceClusters);

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
 * type (matter-devices.xml:2523-2546, MA-smokecoalarm), so the port
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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(smokeCoAlarmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::ExpressedState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::SmokeState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::COState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::BatteryAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::TestInProgress::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::HardwareFaultAlert::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::EndOfServiceAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(SmokeCoAlarm::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kSmokeCoAlarmIncoming[] = { SmokeCoAlarm::Commands::SelfTestRequest::Id,
                                                kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(smokeCoAlarmClusters)
DECLARE_DYNAMIC_CLUSTER(SmokeCoAlarm::Id, smokeCoAlarmAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kSmokeCoAlarmIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(smokeCoAlarmEndpoint, smokeCoAlarmClusters);

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
 * Start 0x02, Resume 0x03; the generated OperationalCommandResponse is
 * the server-to-client response and deliberately NOT in the incoming
 * list.
 *
 * The delegate publishes exactly Stopped/Running/Paused/Error in
 * OperationalStateList and answers CHIP_ERROR_NOT_FOUND at phase index 0
 * so PhaseList reads null; the verdict mapping (allow kNoError, deny
 * kUnableToCompleteOperation, and why the delegate never calls
 * SetOperationalState on allow) is on HearthOpStateDelegate in
 * mt_matter_zephyr.cpp. */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(opStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::PhaseList::Id, ARRAY, 0,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::CurrentPhase::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalStateList::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::OperationalError::Id, STRUCT, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(OperationalState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kOpStateIncoming[] = { OperationalState::Commands::Pause::Id,
                                           OperationalState::Commands::Stop::Id,
                                           OperationalState::Commands::Start::Id,
                                           OperationalState::Commands::Resume::Id,
                                           kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(applianceOpStateClusters)
DECLARE_DYNAMIC_CLUSTER(OperationalState::Id, opStateAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kOpStateIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(applianceOpStateEndpoint, applianceOpStateClusters);

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
 *   (emberAfIsKnownVolatileAttribute, util.cpp:155-165, answers false both
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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(modeSelectAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::Description::Id, CHAR_STRING, 65, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::StandardNamespace::Id, ENUM16, 2,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::SupportedModes::Id, ARRAY, 0, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::CurrentMode::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ModeSelect::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kModeSelectIncoming[] = { ModeSelect::Commands::ChangeToMode::Id,
                                              kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(modeSelectClusters)
DECLARE_DYNAMIC_CLUSTER(ModeSelect::Id, modeSelectAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kModeSelectIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(modeSelectEndpoint, modeSelectClusters);

constexpr EmberAfDeviceType kModeSelectTypes[] = { { 0x0027, 1 } };

/* ---- the registry ---------------------------------------------------- */

struct hearth_devtype {
    uint32_t id;
    uint8_t max_variant;
    const EmberAfEndpointType *ep_type;
    Span<const EmberAfDeviceType> device_types;
};

const hearth_devtype s_registry[] = {
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
};

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
 * created endpoint, holding that endpoint's DataVersion array and its
 * attribute slots, sized for ITS device type:
 *
 *     +----------------------------+  <- dyn_endpoint::block
 *     | DataVersion dv[n_clusters] |     4 * clusterCount bytes
 *     +----------------------------+
 *     | attr_slot slots[n_slots]   |     16 * count_slots() bytes
 *     +----------------------------+
 *
 * dv first is what keeps the layout alignment-free. k_heap_alloc() routes
 * through sys_heap_noalign_alloc() (kheap.c:119-129), and a chunk's memory
 * starts one chunk header past an 8-byte-aligned chunk boundary
 * (chunk_mem(), heap.c:24-27), so on this build a block is 4-byte aligned,
 * NOT 8. That is exactly what both halves need and no more: DataVersion is
 * uint32_t and attr_slot's leading members are uint32_t, so both want 4-byte
 * alignment, and an integral number of uint32_t of dv can never leave the
 * slots misaligned. block_dv() and block_slots() are the only two places
 * that know this.
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
 * Block payload is 4 * clusterCount + 16 * slots. Zephyr's sys_heap adds a
 * 4-byte chunk header for a heap this size (heap.h chunk_header_bytes(),
 * small heap because 8192/8 = 1024 chunks is far below the 0x7fff big-heap
 * threshold) and rounds the total up to CHUNK_UNIT (8 bytes), so the true
 * cost per endpoint is roundup(payload + 4, 8):
 *
 *   device type                        clusters  slots  payload  heap cost
 *   extended colour light   0x010D            5     36      596        600
 *   colour temperature lt   0x010C            5     32      532        536
 *   dimmable light / plug   0x0101 0x010B     4     20      336        344
 *   thermostat              0x0301            3     15      252        256
 *   smoke/co alarm          0x0076            3     13      220        224
 *   window covering         0x0202            3     13      220        224
 *   door lock               0x000A            3     12      204        208
 *   on/off light / plug     0x0100 0x010A     3     11      188        192
 *   water valve             0x0042            3     11      188        192
 *   fan                     0x002B            3     10      172        176
 *   temp/humidity/pressure/light/flow         3      9      156        160
 *   occupancy sensor        0x0107            3      9      156        160
 *   washer/dishwasher/dryer 0x0073 75 7C     3      8      140        144
 *   mode select             0x0027            3      8      140        144
 *   power source            0x0011            2      8      136        144
 *   boolean-state sensors, air quality        3      7      124        128
 *
 * HEARTH_EP_HEAP_BYTES is 8192, which leaves roughly 8,100 usable after the
 * heap's own header and bucket table. Against kServiceableEndpoints = 16:
 *
 *   16 x anything but a colour light   <= 16 x 344 = 5,504    fits
 *   16 x colour temperature light           15 fit (8,040)    ONE SHORT
 *   16 x extended colour light              13 fit (7,800)    THREE SHORT
 *   a realistic mixed composition, say
 *     2 extended colour + 2 dimmable +
 *     12 assorted sensors               1,200+688+1,920 = 3,808   fits easily
 *
 * That is the deliberate trade the round was asked for: sizing for 16 of
 * the heaviest type would want 9,600 bytes and buy a composition nobody
 * builds, when the same RAM is worth more elsewhere. A composition that
 * does ask for more gets the loud, specific failure in mt_devtype_create()
 * rather than a silent truncation, and the README's "Endpoint capacity"
 * section tells an integrator the numbers up front.
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

/* Payload bytes handed out so far. k_heap tracks its own occupancy, but
 * this is the number the failure log wants: what the compositions asked
 * for, independent of per-chunk rounding. */
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
                                         sizeof(DataVersion) * d.type->ep_type->clusterCount);
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
 * heaviest device type. Eight rather than sixteen because sizing for
 * sixteen of the heaviest is precisely the trade this round declined;
 * eight is the point below which the capacity table would be describing a
 * different device.
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
                MT_COUNT(windowCoveringAttrs)),
          kMax2(kMax2(MT_COUNT(doorLockAttrs), MT_COUNT(valveAttrs)),
                kMax2(MT_COUNT(smokeCoAlarmAttrs),
                      kMax2(MT_COUNT(opStateAttrs), MT_COUNT(modeSelectAttrs)))));

/* The widest of the light/plug family. The on/off light and plug (OnOff
 * alone) and the dimmable light and plug (OnOff + LevelControl) are strict
 * prefixes of this, so covering the wider colour light covers them all. */
constexpr size_t kLightSlots = MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) +
    kMax2(MT_COUNT(colorTempAttrs), MT_COUNT(extendedColorAttrs));

/* Catalogue batch 4 brought the first device types WITHOUT Identify (the
 * power source and, later in the batch, the chime), so the widest-endpoint
 * computation gained a second family: types that ride without the
 * kIdentifySlots term. MT_COUNT over an attr list counts DECLARED entries
 * plus the LIST_END ClusterRevision, which over-counts a list whose
 * metadata-only members (strings, structs, arrays beyond the LIST_END
 * convention) get no slot; that over-count only ever makes the asserted
 * floor MORE conservative, so it is left uncorrected. */
constexpr size_t kNoIdentifySlots = MT_COUNT(powerSourceAttrs);

constexpr size_t kWidestEndpointSlots =
    kMax2(kIdentifySlots + kMax2(kMax2(kSensorSlots, kActuatorSlots), kLightSlots),
          kNoIdentifySlots);

/* Deliberately partial: every sensor and actuator device type shares the
 * same three-cluster shape, so temperatureSensorClusters stands in for all
 * of them rather than listing seventeen identical counts. */
constexpr size_t kWidestClusterList = kMax2(
    kMax2(kMax2(MT_COUNT(onOffLightClusters), MT_COUNT(dimmableLightClusters)),
          kMax2(MT_COUNT(colorTemperatureLightClusters), MT_COUNT(extendedColorLightClusters))),
    kMax2(kMax2(MT_COUNT(onOffPlugInUnitClusters), MT_COUNT(dimmablePlugInUnitClusters)),
          kMax2(kMax2(MT_COUNT(thermostatClusters), MT_COUNT(fanClusters)),
                kMax2(MT_COUNT(windowCoveringClusters), MT_COUNT(temperatureSensorClusters)))));

constexpr size_t kWidestBlockBytes = block_bytes(kWidestClusterList, kWidestEndpointSlots);

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

static_assert(kHeapCostOf(kWidestBlockBytes) * kMinWidestEndpoints <= kHeapUsableBytes,
              "the endpoint heap no longer holds eight of the widest device type; redo the "
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
 */
struct attr_seed {
    ClusterId cluster;
    AttributeId attr;
    uint8_t size;
    uint8_t bytes[4];
    uint32_t devtype;
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
};

/* Fills this endpoint's block. Walks the same two predicates count_slots()
 * used to size it, so slot_count lands on slot_capacity exactly; the bound
 * below is a backstop against those two drifting, not a normal path. */
void seed_slots(dyn_endpoint *d)
{
    attr_slot *slots = block_slots(*d);
    d->slot_count = 0;
    for (uint8_t c = 0; c < d->type->ep_type->clusterCount; c++) {
        const EmberAfCluster &cl = d->type->ep_type->cluster[c];
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
                    md.attributeType != ZAP_TYPE(STRUCT)) {
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

            /* A seed row naming this device type wins over the wildcard row
             * (devtype 0) for the same cluster and attribute; the loop
             * handles either order in the table, since it only stops early
             * on an exact match. */
            const attr_seed *chosen = nullptr;
            for (auto &seed : s_seeds) {
                if (seed.cluster != cl.clusterId || seed.attr != md.attributeId) {
                    continue;
                }
                if (seed.devtype == d->type->id) {
                    chosen = &seed;
                    break;
                }
                if (seed.devtype == 0 && chosen == nullptr) {
                    chosen = &seed;
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

extern "C" bool mt_devtype_parent_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    (void)devtype_id;
    (void)variant;
    (void)parent_devtype;
    /* None of the device types in s_registry requires a parent or
     * restricts which device type may parent it, so every combination is
     * legal, including the unparented case (parent_devtype 0). Same answer
     * as the C6 table gives for all of these (all max_variant 0, generic
     * parenting). */
    return true;
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
    void *opstate_delegate = nullptr;
    if (type_has_cluster(type->ep_type, OperationalState::Id)) {
        opstate_delegate = mt_matter_opstate_delegate_alloc(OperationalState::Id);
        if (opstate_delegate == nullptr) {
            LOG_ERR("devtype 0x%04X: opstate delegate pool exhausted (%u slots, one per "
                    "serviceable endpoint); %u of %u serviceable endpoints in use, endpoint "
                    "heap %zu of %u B used; host may declare %u, this build serves %u",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints, s_ep_heap_used,
                    (unsigned)HEARTH_EP_HEAP_BYTES, (unsigned)MT_COMP_MAX_ENDPOINTS,
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
    if (type_has_cluster(type->ep_type, ModeSelect::Id)) {
        if (mt_matter_mode_select_manager() == nullptr) {
            LOG_ERR("devtype 0x%04X: mode select manager unavailable", (unsigned)devtype_id);
            return -1;
        }
    }

    void *valve_delegate = nullptr;
    if (type_has_cluster(type->ep_type, ValveConfigurationAndControl::Id)) {
        valve_delegate = mt_matter_valve_delegate_alloc();
        if (valve_delegate == nullptr) {
            /* Both walls named, the rule the two capacity logs below
             * follow: an integrator reading this needs to know which
             * resource ran out and how far away the others were. */
            LOG_ERR("devtype 0x%04X: valve delegate pool exhausted (%u slots, one per "
                    "serviceable endpoint); %u of %u serviceable endpoints in use, endpoint "
                    "heap %zu of %u B used; host may declare %u, this build serves %u",
                    (unsigned)devtype_id, (unsigned)kServiceableEndpoints,
                    (unsigned)live_endpoints(), (unsigned)kServiceableEndpoints, s_ep_heap_used,
                    (unsigned)HEARTH_EP_HEAP_BYTES, (unsigned)MT_COMP_MAX_ENDPOINTS,
                    (unsigned)kServiceableEndpoints);
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
                "(endpoint heap %zu of %u B used); host may declare %u, this build serves %u",
                (unsigned)devtype_id, (unsigned)kServiceableEndpoints, s_ep_heap_used,
                (unsigned)HEARTH_EP_HEAP_BYTES, (unsigned)MT_COMP_MAX_ENDPOINTS,
                (unsigned)kServiceableEndpoints);
        return -1;
    }

    /* Sized for THIS device type, not for the widest in the catalogue. */
    const uint16_t n_clusters = type->ep_type->clusterCount;
    const uint16_t n_slots = count_slots(type->ep_type);
    const size_t want = block_bytes(n_clusters, n_slots);

    void *block = k_heap_alloc(&hearth_ep_heap, want, K_NO_WAIT);
    if (block == nullptr) {
        size_t free_bytes = 0;
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
        struct sys_memory_stats st = {};
        if (sys_heap_runtime_stats_get(&hearth_ep_heap.heap, &st) == 0) {
            free_bytes = st.free_bytes;
        }
#endif
        LOG_ERR("devtype 0x%04X: endpoint block of %zu B (%u clusters, %u slots) does not fit; "
                "%zu of %u B handed out, %zu B free; this is endpoint %u of %u serviceable",
                (unsigned)devtype_id, want, (unsigned)n_clusters, (unsigned)n_slots,
                s_ep_heap_used, (unsigned)HEARTH_EP_HEAP_BYTES, free_bytes, (unsigned)(index + 1),
                (unsigned)kServiceableEndpoints);
        return -1;
    }
    s_ep_heap_used += want;

    dyn_endpoint &d = s_dyn[index];
    d.type = type;
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

    CHIP_ERROR err = emberAfSetDynamicEndpoint(
        index, d.ep_id, type->ep_type, Span<DataVersion>(block_dv(d), n_clusters),
        type->device_types, (parent_devtype != 0) ? parent_ep_id : kInvalidEndpointId);
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
    if (type_has_cluster(type->ep_type, DoorLock::Id) &&
        (s_lock_init_ep != d.ep_id || s_lock_init_err != CHIP_NO_ERROR)) {
        LOG_ERR("devtype 0x%04X: DoorLock init did not succeed for endpoint %u "
                "(init ran for endpoint %u, result %" CHIP_ERROR_FORMAT ")",
                (unsigned)devtype_id, (unsigned)d.ep_id, (unsigned)s_lock_init_ep,
                s_lock_init_err.Format());
        emberAfClearDynamicEndpoint(index);
        d.used = false;
        d.block = nullptr;
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
    for (uint8_t i = 0; i < type->ep_type->clusterCount; i++) {
        if (type->ep_type->cluster[i].clusterId == LevelControl::Id) {
            emberAfLevelControlClusterServerInitCallback(d.ep_id);
        } else if (type->ep_type->cluster[i].clusterId == ColorControl::Id) {
            emberAfColorControlClusterServerInitCallback(d.ep_id);
        } else if (type->ep_type->cluster[i].clusterId == ModeSelect::Id) {
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
    if (opstate_delegate != nullptr) {
        mt_matter_opstate_delegate_set_endpoint(opstate_delegate, d.ep_id);
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
