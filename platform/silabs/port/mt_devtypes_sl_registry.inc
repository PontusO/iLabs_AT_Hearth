/*
 * mt_devtypes_sl_registry.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds the parenting policy and the registry (nRF 3325-3428, 4402-4641).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, so the whole is still one translation
 * unit: file-local names stay file-local and the generated code is the
 * same as when this text lived in the .cpp. Split out 2026-09-25 so each
 * file fits a small model's context (graph T549, DE550). Do not add it
 * to hearth.slcp.
 */

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
 * row but the twenty-four ported ones, which carry ep_type and device_types
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
    { 0x0011, 0, &powerSourceEndpoint, Span<const EmberAfDeviceType>(kPowerSourceTypes) },
    { 0x0076, 0, &smokeCoAlarmEndpoint, Span<const EmberAfDeviceType>(kSmokeCoAlarmTypes) },
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
 * created. It does NOT weaken into vacuity: the twenty-four ported rows are checked
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

