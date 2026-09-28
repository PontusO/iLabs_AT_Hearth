/*
 * mt_devtypes_sl_registry.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds the registry (nRF 4402-4641). The parenting policy and the shape
 * struct (nRF 3325-3428) moved to mt_devtypes_sl_tables_b8.inc with
 * catalogue batch 8, the nRF's order: they must precede the batch 8 tables.
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, so the whole is still one translation
 * unit: file-local names stay file-local and the generated code is the
 * same as when this text lived in the .cpp. Split out 2026-09-25 so each
 * file fits a small model's context (graph T549, DE550). Do not add it
 * to hearth.slcp.
 */

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
 * Since catalogue batch 8 every row carries a cluster set: ep_type and
 * device_types (on the variant rows also ep_type_v1 and device_types_v1),
 * or, on the two shape-bearing rows, device_types and shapes.
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
 * row for row, and since catalogue batch 8 every one of them has a cluster
 * set. Each row carries its id, its max_variant and (implicitly, through
 * parent_policy_ok() keying on the id) its parenting rule.
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
    /* Catalogue batch 7a: the energy foundation types, the registry's first
     * variant-carrying rows (variant 0 = power + energy, variant 1 = power
     * only). */
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
    /* Catalogue batch 8: the composed appliances. These four carry no
     * shapes (nor does the microwave below): the policy is permissive for
     * all of them, and a cooktop's or an oven's own cluster set does not
     * change when a child is composed under it (only the child's does, and
     * only PartsList moves on the parent, which the provider serves). */
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

static_assert(sizeof(s_registry) / sizeof(s_registry[0]) == 52,
              "the catalogue is 52 device types; a row was added or lost");

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

