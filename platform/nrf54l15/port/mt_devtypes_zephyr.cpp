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
#include <app-common/zap-generated/callback.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <lib/core/DataModelTypes.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/interaction_model/StatusCode.h>

#include <string.h>

#include <zephyr/logging/log.h>

extern "C" {
#include "mt_composition.h"
#include "mt_devtypes.h"
}
#include "mt_dyn_store.h"

using namespace chip;
using namespace chip::app::Clusters;

using chip::Protocols::InteractionModel::Status;

LOG_MODULE_REGISTER(hearth_devtypes, LOG_LEVEL_INF);

/* The dynamic endpoint table must be as deep as the composition the AT
 * contract can stage, or a legal AT+MTEP sequence would be silently
 * truncated at boot. chip_project_config.h cannot include core headers
 * (CHIP pulls it in everywhere), so the two constants are tied here. */
static_assert(CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT == MT_COMP_MAX_ENDPOINTS,
              "chip_project_config.h must mirror MT_COMP_MAX_ENDPOINTS");

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
};

/* ---- the external attribute store ------------------------------------ */

/* One attribute-value arena per dynamic slot. Every dynamic attribute
 * declared above is 4 bytes or fewer (BITMAP32 is the widest), so 4-byte
 * slots hold all of them; seed_slots() refuses anything larger rather than
 * overrunning. 28 endpoints times this arena is the dominant RAM cost of
 * the whole file, so the bounds are tight on purpose. */
struct attr_slot {
    ClusterId cluster;
    AttributeId attr;
    uint8_t size;
    uint8_t data[4];
};

/* Two slots of headroom over the widest endpoint type. The dimmable light
 * uses 20: OnOff 6 + auto ClusterRevision, LevelControl 8 + 1, Identify
 * 3 + 1, Descriptor skipped. Sizing this to exactly 20 would make the
 * overflow guard in seed_slots() the normal path the moment anyone adds an
 * attribute, and a skipped slot shows up only as a wrong value at runtime.
 * The guard stays a backstop; raise this number when 22 is reached.
 *
 * Catalogue batch 1 (nRF): the dimmable plug-in unit (0x010B) ties the
 * dimmable light at 20 slots (same OnOff + LevelControl + Identify set);
 * every other new devtype is narrower (the widest sensor is occupancy
 * sensing at 9). 22 still holds. */
constexpr uint8_t kMaxSlots = 22;

/* One data version per cluster; the widest endpoint type has exactly four.
 * emberAfSetDynamicEndpoint() returns CHIP_ERROR_NO_MEMORY outright if the
 * span is shorter than the server-cluster count
 * (attribute-storage.cpp:319-323), so a short array fails loudly.
 *
 * Catalogue batch 1: every new devtype has at most three server clusters
 * (measurement/boolean-state/occupancy + Identify + Descriptor, or OnOff +
 * LevelControl + Identify + Descriptor for the dimmable plug), so 4 still
 * holds. */
constexpr uint8_t kMaxClusters = 4;

struct dyn_endpoint {
    bool used;
    EndpointId ep_id;
    const hearth_devtype *type;
    DataVersion dv[kMaxClusters];
    attr_slot slots[kMaxSlots];
    uint8_t slot_count;
};

dyn_endpoint s_dyn[MT_COMP_MAX_ENDPOINTS];

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
 */
struct attr_seed {
    ClusterId cluster;
    AttributeId attr;
    uint8_t size;
    uint8_t bytes[4];
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
};

void seed_slots(dyn_endpoint *d)
{
    d->slot_count = 0;
    for (uint8_t c = 0; c < d->type->ep_type->clusterCount; c++) {
        const EmberAfCluster &cl = d->type->ep_type->cluster[c];
        /* Descriptor is served by CHIP's own DescriptorCluster server
         * object, registered per endpoint by the cluster init callback
         * that emberAfSetDynamicEndpoint() fires; its reads never reach
         * the external-storage callbacks, so it gets no slots. */
        if (cl.clusterId == Descriptor::Id) {
            continue;
        }
        for (uint16_t a = 0; a < cl.attributeCount; a++) {
            const EmberAfAttributeMetadata &md = cl.attributes[a];
            if (md.attributeType == ZAP_TYPE(ARRAY)) {
                continue;
            }
            if (md.size > sizeof(d->slots[0].data)) {
                LOG_ERR("attr 0x%08X on cluster 0x%08X is %u bytes, arena holds %u",
                        (unsigned)md.attributeId, (unsigned)cl.clusterId, (unsigned)md.size,
                        (unsigned)sizeof(d->slots[0].data));
                continue;
            }
            if (d->slot_count >= kMaxSlots) {
                LOG_ERR("slot arena full for devtype 0x%04X", (unsigned)d->type->id);
                return;
            }
            attr_slot &s = d->slots[d->slot_count++];
            s.cluster = cl.clusterId;
            s.attr = md.attributeId;
            s.size = (uint8_t)md.size;
            memset(s.data, 0, sizeof(s.data));
            for (auto &seed : s_seeds) {
                if (seed.cluster == cl.clusterId && seed.attr == md.attributeId) {
                    memcpy(s.data, seed.bytes, (seed.size < s.size) ? seed.size : s.size);
                    break;
                }
            }
        }
    }
}

} /* namespace */

bool mt_dyn_attr_slot(EndpointId ep, ClusterId cluster, AttributeId attr, uint8_t **data,
                      uint8_t *size)
{
    for (auto &d : s_dyn) {
        if (!d.used || d.ep_id != ep) {
            continue;
        }
        for (uint8_t i = 0; i < d.slot_count; i++) {
            if (d.slots[i].cluster == cluster && d.slots[i].attr == attr) {
                *data = d.slots[i].data;
                *size = d.slots[i].size;
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

    /* The lock covers the whole function, so every s_dyn and s_next_ep_id
     * mutation happens under it and the invariant is statable: the dynamic
     * endpoint table and the slot arena are only ever touched with the CHIP
     * stack lock held. emberAfSetDynamicEndpoint() below enables the
     * endpoint synchronously and runs cluster init callbacks that read this
     * arena back, so the two must not be lockable separately. */
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

    /* Checked before the dv span below is built: emberAfSetDynamicEndpoint()
     * takes Span<DataVersion>(d.dv, type->ep_type->clusterCount), and d.dv
     * is exactly kMaxClusters wide. A devtype with more clusters than that
     * would hand the span a length past the end of d.dv, the same
     * silent-overrun kMaxSlots guards against above. Raise kMaxClusters
     * when a devtype needs more. */
    if (type->ep_type->clusterCount > kMaxClusters) {
        LOG_ERR("devtype 0x%04X has %u clusters, kMaxClusters is %u; raise it",
                (unsigned)devtype_id, (unsigned)type->ep_type->clusterCount, (unsigned)kMaxClusters);
        return -1;
    }

    uint16_t index = 0;
    while (index < MT_COMP_MAX_ENDPOINTS && s_dyn[index].used) {
        index++;
    }
    if (index == MT_COMP_MAX_ENDPOINTS) {
        return -1;
    }

    dyn_endpoint &d = s_dyn[index];
    d.type = type;
    d.ep_id = s_next_ep_id;
    seed_slots(&d);
    /* Marked live BEFORE the call, not after: emberAfSetDynamicEndpoint()
     * enables the endpoint on the spot, which runs every cluster's init
     * callback, and those read attributes straight back through the
     * external-storage callbacks. A slot that is not findable yet reads
     * as unsupported and the cluster inits against garbage. */
    d.used = true;

    CHIP_ERROR err = emberAfSetDynamicEndpoint(
        index, d.ep_id, type->ep_type, Span<DataVersion>(d.dv, type->ep_type->clusterCount),
        type->device_types, (parent_devtype != 0) ? parent_ep_id : kInvalidEndpointId);
    if (err != CHIP_NO_ERROR) {
        d.used = false;
        LOG_ERR("emberAfSetDynamicEndpoint(0x%04X) failed: %" CHIP_ERROR_FORMAT,
                (unsigned)devtype_id, err.Format());
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
     * site. */
    for (uint8_t i = 0; i < type->ep_type->clusterCount; i++) {
        if (type->ep_type->cluster[i].clusterId == LevelControl::Id) {
            emberAfLevelControlClusterServerInitCallback(d.ep_id);
            break;
        }
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
