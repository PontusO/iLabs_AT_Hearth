/*
 * mt_devtypes_sl_arena.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds the external attribute store, the endpoint arena, its sizing and floor (nRF 4642-5978).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, so the whole is still one translation
 * unit: file-local names stay file-local and the generated code is the
 * same as when this text lived in the .cpp. Split out 2026-09-25 so each
 * file fits a small model's context (graph T549, DE550). Do not add it
 * to hearth.slcp.
 */

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
/* Catalogue batch 4 brought the first device type WITHOUT Identify (the
 * power source), so the widest-endpoint computation gained a second family:
 * a type that rides without the kIdentifySlots term. Its MT_COUNT over
 * powerSourceAttrs over-counts by one (Description is a metadata-only
 * member, no slot), the same conservative direction as the note above. */
constexpr size_t kNoIdentifySlots = MT_COUNT(powerSourceAttrs);
/* The widest endpoint is the one with the most slots: the colour lights
 * carry OnOff AND LevelControl AND ColorControl, and the extended one
 * declares sixteen ColorControl attributes. Each MT_COUNT counts declared
 * entries plus the LIST_END ClusterRevision; no list here has a
 * metadata-only member, so the counts are exact. */
constexpr size_t kWidestEndpointSlots =
    kMax2(kIdentifySlots + kMax2(kMax2(kMax2(MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) + MT_COUNT(colorTempAttrs), MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) + MT_COUNT(extendedColorAttrs)), kMax2(MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs), kMax2(kMax2(kMax2(MT_COUNT(tempAttrs), MT_COUNT(booleanStateAttrs)), kMax2(MT_COUNT(occupancyAttrs), MT_COUNT(humidityAttrs))), kMax2(MT_COUNT(pressureAttrs), kMax2(MT_COUNT(illuminanceAttrs), MT_COUNT(flowAttrs)))))), kMax2(kMax2(kMax2(MT_COUNT(thermostatAttrs), MT_COUNT(fanControlAttrs)), kMax2(kMax2(MT_COUNT(windowCoveringAttrs), MT_COUNT(airQualityAttrs)), kMax2(MT_COUNT(doorLockAttrs), MT_COUNT(valveAttrs)))), kIdentifySlots + MT_COUNT(smokeCoAlarmAttrs))), kNoIdentifySlots);
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
        kMax2(
            kMax2(kMax2(MT_COUNT(thermostatClusters), MT_COUNT(fanClusters)),
                  kMax2(kMax2(MT_COUNT(windowCoveringClusters), MT_COUNT(airQualitySensorClusters)),
                        kMax2(MT_COUNT(doorLockClusters), MT_COUNT(waterValveClusters)))),
            kMax2(MT_COUNT(temperatureSensorClusters),
                  kMax2(kMax2(MT_COUNT(booleanStateSensorClusters), MT_COUNT(occupancySensorClusters)),
                        kMax2(MT_COUNT(humiditySensorClusters),
                              kMax2(MT_COUNT(pressureSensorClusters),
                                    kMax2(MT_COUNT(lightSensorClusters), MT_COUNT(flowSensorClusters))))))),
        kMax2(MT_COUNT(powerSourceClusters), MT_COUNT(smokeCoAlarmClusters))));

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

