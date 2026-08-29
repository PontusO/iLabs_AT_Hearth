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
/* For mt_air_quality_feature_mask(), the single accessor the AirQuality
 * FeatureMap seed reads instead of transcribing the bits again. */
#include "mt_matter.h"
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
 * sensing at 9). 22 still holds.
 *
 * Catalogue batch 2: 22 no longer holds, and this is the moment the guard
 * exists for. The extended color light (0x010D) is now the widest endpoint
 * type at 36 slots: OnOff 6 declared + 1 auto ClusterRevision = 7,
 * LevelControl 8 + 1 = 9, ColorControl 15 + 1 = 16, Identify 3 + 1 = 4,
 * Descriptor skipped = 36. The color temperature light (0x010C) is 32 by the
 * same arithmetic with an 11-attribute ColorControl; every other new devtype
 * is far narrower (thermostat 15, window covering 13, fan 10, air quality
 * sensor 7). Raised to 36 + 2 = 38, keeping the same two-slot headroom rule
 * as before: raise it again when 38 is reached.
 *
 * RAM cost of the raise: attr_slot is 16 bytes (two 4-byte ids, a size byte
 * and a 4-byte payload, padded to the id alignment), so 16 more slots per
 * dynamic endpoint times MT_COMP_MAX_ENDPOINTS (28) is 7,168 bytes of .bss
 * on top of the 22-slot arena's 9,856. This flat per-endpoint arena is the
 * design's known cost: every endpoint pays for the widest device type
 * whether it uses those slots or not. Sizing the arena per devtype would
 * reclaim most of it and is the obvious next move if RAM gets tight. */
constexpr uint8_t kMaxSlots = 38;

/* The arithmetic in the comment above, checked by the compiler rather than
 * trusted. Each DECLARE_DYNAMIC_ATTRIBUTE_LIST array already includes the
 * ClusterRevision entry LIST_END() appends, and Descriptor contributes no
 * slots (seed_slots() skips it), so a per-devtype sum IS that device type's
 * slot count.
 *
 * Fix round: written as a max over EVERY device type rather than as the
 * single sum for today's widest one. The earlier form only tripped if the
 * extended color light grew; widening the thermostat or window covering
 * lists past 38 would have sailed past it and shown up as seed_slots()'
 * runtime LOG_ERR and an endpoint quietly missing its last attributes. */
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
    kMax2(kMax2(MT_COUNT(thermostatAttrs), MT_COUNT(fanControlAttrs)),
          MT_COUNT(windowCoveringAttrs));

/* The widest of the light/plug family. The on/off light and plug (OnOff
 * alone) and the dimmable light and plug (OnOff + LevelControl) are strict
 * prefixes of this, so covering the wider color light covers them all. */
constexpr size_t kLightSlots = MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) +
    kMax2(MT_COUNT(colorTempAttrs), MT_COUNT(extendedColorAttrs));

constexpr size_t kWidestEndpointSlots =
    kIdentifySlots + kMax2(kMax2(kSensorSlots, kActuatorSlots), kLightSlots);

static_assert(kWidestEndpointSlots <= kMaxSlots,
              "some device type's attribute lists no longer fit the slot arena; raise kMaxSlots");

/* One data version per cluster; the widest endpoint type has exactly four.
 * emberAfSetDynamicEndpoint() returns CHIP_ERROR_NO_MEMORY outright if the
 * span is shorter than the server-cluster count
 * (attribute-storage.cpp:319-323), so a short array fails loudly.
 *
 * Catalogue batch 1: every new devtype has at most three server clusters
 * (measurement/boolean-state/occupancy + Identify + Descriptor, or OnOff +
 * LevelControl + Identify + Descriptor for the dimmable plug), so 4 still
 * holds.
 *
 * Catalogue batch 2: the two color lights carry five (OnOff + LevelControl +
 * ColorControl + Identify + Descriptor), so 4 does not hold. Raised to
 * 5 + 2 = 7, the same widest-plus-two headroom rule kMaxSlots uses, so the
 * next cluster added to a device type does not have to touch this constant
 * as well. RAM cost: DataVersion is 4 bytes, so three extra entries per
 * dynamic endpoint times MT_COMP_MAX_ENDPOINTS (28) is 336 bytes of .bss. */
constexpr uint8_t kMaxClusters = 7;

/* Same idea as the kMaxSlots assertion, and a max over every cluster list
 * for the same reason: naming only today's longest one would let a sixth
 * cluster on some other device type slip past to mt_devtype_create()'s
 * runtime check. Sensors all share the three-cluster shape, so one
 * representative stands for them. */
constexpr size_t kWidestClusterList = kMax2(
    kMax2(kMax2(MT_COUNT(onOffLightClusters), MT_COUNT(dimmableLightClusters)),
          kMax2(MT_COUNT(colorTemperatureLightClusters), MT_COUNT(extendedColorLightClusters))),
    kMax2(kMax2(MT_COUNT(onOffPlugInUnitClusters), MT_COUNT(dimmablePlugInUnitClusters)),
          kMax2(kMax2(MT_COUNT(thermostatClusters), MT_COUNT(fanClusters)),
                kMax2(MT_COUNT(windowCoveringClusters), MT_COUNT(temperatureSensorClusters)))));

static_assert(kWidestClusterList <= kMaxClusters,
              "some device type's cluster list no longer fits dyn_endpoint::dv; raise kMaxClusters");

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
     * The 0 is deliberate and stays. PercentSetting has NO default in
     * FanControl.xml (neither a default= nor a <default> child), so there is
     * no XML default to defer to. What the spec does bind is its pairing
     * with FanMode, and the server implements it: setting FanMode to Off
     * SHALL set PercentSetting, PercentCurrent, SpeedSetting and
     * SpeedCurrent to 0 (fan-control-server.cpp:337-361). FanMode is seeded
     * Off one row below, so 0 is the only value consistent with it, and a
     * null seed would contradict that pairing at boot. Null is the
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
     * that is a wild write far past a 29-entry array. The command handlers
     * are safer than this (they all go through the bounds-checked
     * get*TransitionStateByIndex() getters), but the init is not, so the
     * call must stay below the successful emberAfSetDynamicEndpoint(). */
    for (uint8_t i = 0; i < type->ep_type->clusterCount; i++) {
        if (type->ep_type->cluster[i].clusterId == LevelControl::Id) {
            emberAfLevelControlClusterServerInitCallback(d.ep_id);
        } else if (type->ep_type->cluster[i].clusterId == ColorControl::Id) {
            emberAfColorControlClusterServerInitCallback(d.ep_id);
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
