/*
 * mt_devtypes_sl_tables_b4.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 4 (nRF 1485-2006).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, after mt_devtypes_sl_tables_b3.inc
 * and before mt_devtypes_sl_registry.inc, so the whole is still one
 * translation unit: file-local names stay file-local and the generated
 * code is the same as when this text lived in the .cpp. Split out
 * 2026-09-26 so each file fits a small model's context. Do not add it
 * to hearth.slcp.
 */

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
