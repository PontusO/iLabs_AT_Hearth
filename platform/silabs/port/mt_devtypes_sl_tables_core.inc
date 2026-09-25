/*
 * mt_devtypes_sl_tables_core.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: milestone types and catalogue batch 1 (nRF 276-721).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, so the whole is still one translation
 * unit: file-local names stay file-local and the generated code is the
 * same as when this text lived in the .cpp. Split out 2026-09-25 so each
 * file fits a small model's context (graph T549, DE550). Do not add it
 * to hearth.slcp.
 */

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

