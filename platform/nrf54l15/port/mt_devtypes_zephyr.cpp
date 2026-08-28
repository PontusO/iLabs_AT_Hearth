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

/* Descriptor's four lists are ARRAY-typed. The declared size is the byte
 * budget CHIP checks against its attribute IO buffer, not storage we own:
 * the values come from CHIP's own DescriptorCluster server object. */
constexpr uint16_t kDescriptorArraySize = 254;

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::DeviceTypeList::Id, ARRAY, kDescriptorArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ServerList::Id, ARRAY, kDescriptorArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ClientList::Id, ARRAY, kDescriptorArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::PartsList::Id, ARRAY, kDescriptorArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(identifyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyTime::Id, INT16U, 2,
                          ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kIdentifyIncoming[] = { Identify::Commands::Identify::Id, kInvalidCommandId };

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
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId kOnOffIncoming[] = { OnOff::Commands::Off::Id, OnOff::Commands::On::Id,
                                         OnOff::Commands::Toggle::Id, kInvalidCommandId };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(onOffLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), kOnOffIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), kIdentifyIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(onOffLightEndpoint, onOffLightClusters);

constexpr EmberAfDeviceType kOnOffLightTypes[] = { { 0x0100, 3 } };

/* ---- dimmable light (0x0101) ----------------------------------------- */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(levelAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::CurrentLevel::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::RemainingTime::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MinLevel::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MaxLevel::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::Options::Id, BITMAP8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::OnLevel::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::StartUpCurrentLevel::Id, INT8U, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
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
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), kIdentifyIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(dimmableLightEndpoint, dimmableLightClusters);

constexpr EmberAfDeviceType kDimmableLightTypes[] = { { 0x0101, 3 } };

/* ---- temperature sensor (0x0302) ------------------------------------- */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(tempAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MinMeasuredValue::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(TemperatureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(temperatureSensorClusters)
DECLARE_DYNAMIC_CLUSTER(TemperatureMeasurement::Id, tempAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), kIdentifyIncoming,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(temperatureSensorEndpoint, temperatureSensorClusters);

constexpr EmberAfDeviceType kTemperatureSensorTypes[] = { { 0x0302, 2 } };

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
};

/* ---- the external attribute store ------------------------------------ */

/* One attribute-value arena per dynamic slot. Every dynamic attribute
 * declared above is 4 bytes or fewer, so 8-byte slots are generous and
 * uniform; the widest endpoint type (dimmable light) needs 19 slots. */
struct attr_slot {
    ClusterId cluster;
    AttributeId attr;
    uint8_t size;
    uint8_t data[8];
};
constexpr uint8_t kMaxSlots = 24;

/* Eight data versions is one per cluster, and no endpoint type above has
 * more than four clusters; emberAfSetDynamicEndpoint() refuses the call
 * outright if the span is short, so the headroom is cheap insurance. */
constexpr uint8_t kMaxClusters = 8;

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
            if (d->slot_count >= kMaxSlots) {
                LOG_ERR("slot arena full for devtype 0x%04X", (unsigned)d->type->id);
                return;
            }
            attr_slot &s = d->slots[d->slot_count++];
            s.cluster = cl.clusterId;
            s.attr = md.attributeId;
            s.size = (uint8_t)md.size;
            memset(s.data, 0, sizeof(s.data));
            if (md.attributeId == Globals::Attributes::ClusterRevision::Id) {
                s.data[0] = 1;
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
    /* None of the three milestone device types requires a parent or
     * restricts which device type may parent it, so every combination is
     * legal, including the unparented case (parent_devtype 0). Same answer
     * as the C6 table gives for these three. */
    return true;
}

extern "C" int mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype,
                                 uint16_t parent_ep_id, uint16_t *out_ep_id)
{
    /* No milestone device type has variants, so variant carries no
     * construction difference yet; mt_devtype_variant_ok() has already
     * rejected anything but 0 by the time the rebuild loop gets here. */
    (void)variant;

    if (!out_ep_id) {
        return -1;
    }

    const hearth_devtype *type = nullptr;
    for (auto &e : s_registry) {
        if (e.id == devtype_id) {
            type = &e;
        }
    }
    if (!type) {
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

    chip::DeviceLayer::StackLock lock;
    CHIP_ERROR err = emberAfSetDynamicEndpoint(
        index, d.ep_id, type->ep_type, Span<DataVersion>(d.dv, type->ep_type->clusterCount),
        type->device_types, (parent_devtype != 0) ? parent_ep_id : kInvalidEndpointId);
    if (err != CHIP_NO_ERROR) {
        d.used = false;
        LOG_ERR("emberAfSetDynamicEndpoint(0x%04X) failed: %" CHIP_ERROR_FORMAT,
                (unsigned)devtype_id, err.Format());
        return -1;
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
