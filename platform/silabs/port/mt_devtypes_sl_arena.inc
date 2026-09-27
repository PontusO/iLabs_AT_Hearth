/*
 * mt_devtypes_sl_arena.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds the external attribute store, the endpoint arena and its trailing
 * store walk, its sizing and floor (nRF 4642-5978 and 5032-5220).
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
 *     | host-fed store(s), only    |     store_bytes(): mode select 436 B,
 *     | for types that carry one   |     chime 273 B; 0 otherwise
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
 * header, no bucket table, so usable bytes are gross bytes). The types of
 * batches 1 to 3 and batch 4's mode select, the widest; batch 4's other
 * types (power source, smoke/CO, the laundry trio, the chime) are narrower
 * than 608 and priced by the candidates below, not tabled here: batch 5a's
 * pump and room air conditioner (304 B) and generic switch (144 B) are
 * priced by the candidates too, not tabled:
 *
 *   device type                        clusters  slots  payload  arena cost
 *   mode select           0x0027            3     10      608         608  * over-count price
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
 * cluster carries, Identify's four included and Descriptor's none. The
 * mode select's block is the over-count price 608 (3 clusters, 10 slots:
 * 12 + 160 + the host-fed mode store's 436); its exact runtime block is 3
 * clusters, 8 slots (the 5 modeSelect plus the 3 identify, no descriptor
 * slots), 140 + 436 = 576.
 *
 * HEARTH_EP_ARENA_BYTES is 9,728, which is 16 x 608: this arena holds
 * kServiceableEndpoints of the WIDEST type it can build, the mode select's
 * 608-byte block, with nothing left over and nothing wasted. Catalogue batch
 * 2 kept batch 1's strong promise (every composition the build accepts, it
 * can build) by raising HEARTH_EP_ARENA_BYTES from 5,376 to 9,600 rather
 * than lowering the floor to the nRF's eight (the ruling of 2026-09-24), and
 * catalogue batch 4 raised it from 9,600 to 9,728 when the mode select
 * became the widest (the ruling of 2026-09-26). The RVC (batch 5b) is the
 * one admitted type below that sixteen: up to nine, because its 424 B of
 * cluster objects per endpoint is what the cluster-object arena caps (DE566)
 * rather than this arena, which would hold ten RVC blocks (10 x 936 = 9,360
 * of 9,728). It is still a stronger
 * promise than the nRF's, whose 8,112 usable bytes hold only 13 of the same
 * extended colour light. The floor below asserts the strong form
 * deliberately: the batch that adds a wider type will fail this build and
 * have to make the same trade the nRF made, in the open, rather than inherit
 * a weaker floor by accident.
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
 * The type-conditional trailing stores (nRF 5032-5197). store_walk() over a
 * twelve-row kStoreWalk table, placing each host-fed store (mode select's
 * 436 B, chime's 273, the ModeBase families' 306 each, the temperature-level
 * labels' 273) at its own alignment behind the slot region. Catalogue batch
 * 4's ruling copied the table WHOLE, including the batch 5, 7 and 8 rows for
 * device types this build does not declare yet: type_has_cluster() is false
 * for their clusters on every declared type, so those rows walk as dead
 * entries and only their sizeof/alignof assertions cost anything, which is
 * what the store shapes in mt_stores.h exist for.
 */

/* Does this endpoint type carry cluster id as a server? nRF type_has_cluster.
 * Defined here (not in the .cpp) because store_walk() below calls it and the
 * .cpp includes this file before its own definitions; the .cpp's create and
 * seeding paths use the same file-scope function. */
static bool type_has_cluster(const EmberAfEndpointType *ep_type, ClusterId id)
{
    for (uint8_t i = 0; i < ep_type->clusterCount; i++) {
        if (ep_type->cluster[i].clusterId == id) {
            return true;
        }
    }
    return false;
}
struct store_desc {
    ClusterId cluster;
    AttributeId attr;
    size_t size;
    size_t align;
};

/* Does this device type declare `attr` on `cluster`? Asked by the store
 * walk and, since catalogue batch 7a-2, by the create path's DEM
 * PowerAdjustment predicate; it walks the declared list rather than
 * consulting the block. */
bool type_has_attr(const EmberAfEndpointType *t, ClusterId cluster, AttributeId attr)
{
    for (uint8_t c = 0; c < t->clusterCount; c++) {
        if (t->cluster[c].clusterId != cluster) {
            continue;
        }
        for (uint16_t a = 0; a < t->cluster[c].attributeCount; a++) {
            if (t->cluster[c].attributes[a].attributeId == attr) {
                return true;
            }
        }
        return false;
    }
    return false;
}

constexpr store_desc kStoreWalk[] = {
    { ModeSelect::Id, kInvalidAttributeId, sizeof(mt_mode_store_t), alignof(mt_mode_store_t) },
    { Chime::Id, kInvalidAttributeId, sizeof(mt_chime_store_t), alignof(mt_chime_store_t) },
    /* Catalogue batch 5: the RVC's two host-fed ModeBase lists. Both rows
     * are PRESENT on one type (0x0074 carries RvcRunMode and RvcCleanMode
     * at once), the first time the walk places two stores in one block;
     * store_walk() already iterates the whole table and align_up() makes
     * the order taste, so the mechanism extends without change. */
    { RvcRunMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    { RvcCleanMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 7a: the DEM device type's mode list, the third
     * ModeBase store row; the walk mechanism is unchanged (the batch 5
     * note above), only membership grows. */
    { DeviceEnergyManagementMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t),
      alignof(mt_mb_store_t) },
    /* Catalogue batch 7b: the water heater's mode list, the fourth. Same
     * mechanism, same 306 B shape. */
    { WaterHeaterMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the refrigerator's mode list, the fifth. */
    { RefrigeratorAndTemperatureControlledCabinetMode::Id, kInvalidAttributeId,
      sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the heater cabinet's mode list, the sixth. */
    { OvenMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the microwave's mode list, the seventh. */
    { MicrowaveOvenMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* The EVSE round: the eighth and last ModeBase store row. The EVSE is the
     * SECOND type after the RVC to carry two stores in one block, and unlike
     * the RVC's pair these come from two different rows of this table
     * (EnergyEvseMode here, DeviceEnergyManagementMode above), which the walk
     * has handled since batch 5 without change. */
    { EnergyEvseMode::Id, kInvalidAttributeId, sizeof(mt_mb_store_t), alignof(mt_mb_store_t) },
    /* Catalogue batch 8: the temperature-level label store, and the ONLY row
     * with an attribute discriminator (the struct comment). Present on a
     * variant-1 cabinet or cook surface, absent on their variant-0 forms,
     * which carry the same cluster id. */
    { TemperatureControl::Id, TemperatureControl::Attributes::SupportedTemperatureLevels::Id,
      sizeof(mt_temp_levels_store_t), alignof(mt_temp_levels_store_t) },
};

constexpr size_t align_up(size_t off, size_t align) { return ((off + align - 1) / align) * align; }

/* stop_at = a store's cluster id answers that store's aligned offset
 * within the region (the caller has established the type carries it);
 * stop_at = kInvalidClusterId (matching no store) walks to the end and
 * answers the region's total size. */
size_t store_walk(const EmberAfEndpointType *t, ClusterId stop_at)
{
    size_t off = 0;
    for (auto &s : kStoreWalk) {
        if (!type_has_cluster(t, s.cluster)) {
            continue;
        }
        /* Catalogue batch 8: the finer key. A row naming an attribute is
         * present only when the declared list carries that attribute too;
         * kInvalidAttributeId is every older row's "cluster presence
         * alone". */
        if (s.attr != kInvalidAttributeId && !type_has_attr(t, s.cluster, s.attr)) {
            continue;
        }
        off = align_up(off, s.align);
        if (s.cluster == stop_at) {
            return off;
        }
        off += s.size;
    }
    return off;
}

size_t store_bytes(const EmberAfEndpointType *t)
{
    return store_walk(t, kInvalidClusterId);
}

size_t store_offset(const EmberAfEndpointType *t, ClusterId which)
{
    return store_walk(t, which);
}

/* The sizing table's two store figures, pinned so the table cannot go
 * stale without the build noticing (the same discipline as the widest-type
 * floor below); recompute the mode select and chime rows if either
 * fires. */
static_assert(sizeof(mt_mode_store_t) == 436,
              "mt_mode_store_t changed size; redo the sizing table's mode select row");
static_assert(sizeof(mt_chime_store_t) == 273,
              "mt_chime_store_t changed size; redo the sizing table's chime row");
static_assert(sizeof(mt_mb_store_t) == 306,
              "mt_mb_store_t changed size; redo the sizing table's rvc row");
static_assert(sizeof(mt_temp_levels_store_t) == 273,
              "mt_temp_levels_store_t changed size; redo the sizing table's cabinet and cook "
              "surface variant-1 rows");

/* The trailing store begins at 4 * clusterCount + 16 * n_slots, a multiple
 * of 4, and a hearth_arena block is 8-aligned (hearth_arena_cost rounds the
 * payload up to 8, the layout note beside s_obj_arena in mt_matter_sl.cpp),
 * so nothing wanting more than DataVersion's alignment may ever live there.
 * The conclusion stays conservative: the old note said 4-byte aligned, and
 * the 8-aligned figure only tightens the bound. */
static_assert(alignof(mt_mode_store_t) <= alignof(DataVersion),
              "mt_mode_store_t must not out-align the block's store offset");
static_assert(alignof(mt_chime_store_t) <= alignof(DataVersion),
              "mt_chime_store_t must not out-align the block's store offset");
static_assert(alignof(mt_mb_store_t) <= alignof(DataVersion),
              "mt_mb_store_t must not out-align the block's store offset");
static_assert(alignof(mt_temp_levels_store_t) <= alignof(DataVersion),
              "mt_temp_levels_store_t must not out-align the block's store offset");

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
 * THE nRF'S FLOOR ALSO PRICES THE STORE-BEARING CANDIDATES (kModeSelectBlockBytes,
 * kChimeBlockBytes, kRvcBlockBytes, kDemBlockBytes, kWaterHeaterBlockBytes,
 * kBatteryStorageBlockBytes, kCabinetHeaterLevelBlockBytes, kMicrowaveBlockBytes,
 * kEvseBlockBytes), which this one does not: catalogue batch 4's ruling priced
 * the mode select with the MT_COUNT over-count instead of the nRF's
 * exact-counting convention (metadata-only members excluded, as count_slots()
 * does), and the candidates for the types this build does not declare yet
 * (the non-RVC ModeBase families, the temperature-level labels, and the other
 * store-bearing types) come with their batches; the chime (batch 4), the
 * RVC (batch 5b) and the DEM (batch 7a-2) candidates live below. And the
 * nRF's floor carries a capped half (kFloorDemand, kEvseEndpointCap and its
 * two non-vacuity assertions) that this one does not: it exists because the
 * EVSE's own MT_EVSE_MAX of 2 makes "room for eight of it" a demand no
 * composition can make. The caps this build does carry (the RVC's
 * nine, the measurement family's eight, the DEM's four, the meter's two:
 * DE566, DE584) are admission limits on the cluster-object arena, not floor
 * demands, so every candidate is uncapped and the floor is
 * the single inequality the nRF had before the EVSE round. The capped half
 * comes back with the EVSE.
 */
#define MT_COUNT(array) (sizeof(array) / sizeof((array)[0]))

constexpr size_t kMax2(size_t a, size_t b) { return a > b ? a : b; }

/* Identify rides on all thirty-five device types; Descriptor contributes
 * nothing.
 * MT_COUNT over an attr list counts DECLARED entries plus the LIST_END
 * ClusterRevision, which over-counts a list whose metadata-only members get
 * no slot; batch 4's mode select and chime lists have one (their
 * Description), so an over-count would only ever make the asserted floor
 * MORE conservative. */
constexpr size_t kIdentifySlots = MT_COUNT(identifyAttrs);
/* Catalogue batch 4 brought the first device types WITHOUT Identify (the
 * power source and, later in the batch, the chime), so the widest-endpoint
 * computation gained a second family: types that ride without the
 * kIdentifySlots term. Its MT_COUNT over powerSourceAttrs over-counts by one
 * (Description is a metadata-only member, no slot), the same conservative
 * direction as the note above. */
constexpr size_t kNoIdentifySlots = kMax2(MT_COUNT(powerSourceAttrs), MT_COUNT(chimeAttrs));
/* Catalogue batch 5's two-app-cluster appliances: the pump (racOnOffAttrs
 * + pumpAttrs) and the room air conditioner (racOnOffAttrs +
 * thermostatAttrs), both far under kLightSlots today, computed anyway so
 * the floor keeps deriving from the same declarations the registry is
 * built from. */
constexpr size_t kOnOffApplianceSlots =
    MT_COUNT(racOnOffAttrs) + kMax2(MT_COUNT(pumpAttrs), MT_COUNT(thermostatAttrs));
/* The widest endpoint is the one with the most slots: the colour lights
 * carry OnOff AND LevelControl AND ColorControl, and the extended one
 * declares sixteen ColorControl attributes; the mode select's eleven tie the
 * on/off pair. Each MT_COUNT counts declared entries plus the LIST_END
 * ClusterRevision; the mode select's Description is metadata-only, so its
 * count over-counts, the conservative direction. Catalogue batch 5a adds
 * the generic switch and the two OnOff appliances (the pump and the room air
 * conditioner), both far narrower than the extended colour light. Catalogue
 * batch 7a-1 adds the wired electrical sensor (the heat pump and solar
 * power), the widest of the measurement family, still narrower than the
 * extended colour light. Catalogue batch 7a-2 adds the utility meter
 * (Identify and MeterIdentification), far narrower again; the DEM is a
 * store-bearing candidate of its own below. */
constexpr size_t kWidestEndpointSlots =
    kMax2(kIdentifySlots + MT_COUNT(meterIdAttrs), kMax2(MT_COUNT(wiredPowerSourceAttrs) + MT_COUNT(ptopAttrs) + MT_COUNT(epmAttrs) + MT_COUNT(eemAttrs), kMax2(kMax2(kIdentifySlots + kOnOffApplianceSlots,
                kIdentifySlots + MT_COUNT(switchAttrs)),
          kMax2(kIdentifySlots + MT_COUNT(modeSelectAttrs), kMax2(kIdentifySlots + kMax2(kMax2(kMax2(MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) + MT_COUNT(colorTempAttrs), MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs) + MT_COUNT(extendedColorAttrs)), kMax2(MT_COUNT(onOffAttrs) + MT_COUNT(levelAttrs), kMax2(kMax2(kMax2(MT_COUNT(tempAttrs), MT_COUNT(booleanStateAttrs)), kMax2(MT_COUNT(occupancyAttrs), MT_COUNT(humidityAttrs))), kMax2(MT_COUNT(pressureAttrs), kMax2(MT_COUNT(illuminanceAttrs), MT_COUNT(flowAttrs)))))), kMax2(kMax2(kMax2(MT_COUNT(thermostatAttrs), MT_COUNT(fanControlAttrs)), kMax2(kMax2(MT_COUNT(windowCoveringAttrs), MT_COUNT(airQualityAttrs)), kMax2(MT_COUNT(doorLockAttrs), MT_COUNT(valveAttrs)))), kMax2(kIdentifySlots + MT_COUNT(smokeCoAlarmAttrs), kIdentifySlots + MT_COUNT(opStateAttrs)))), kNoIdentifySlots)))));
/* The widest cluster list is a max over every declared cluster list, not
 * just the dimmable light's: a future device type with a wider list must
 * fail the floor's static_assert below at build time, not surface later as a
 * runtime allocation refusal when its endpoint overruns block_bytes()'s
 * sizing. Catalogue batch 2's six five- and three-cluster lists join it, as
 * does batch 4's mode select (four, tied with the dimmable light and the
 * plug); the two five-cluster colour light lists are the max. Batch 5a's
 * switch (three clusters) and pump and room air conditioner (four) join it.
 * Batch 7a-1's six measurement lists (the wired sensor's five clusters the
 * widest) join it, and batch 7a-2's utility meter and two DEM lists (three
 * clusters each). */
constexpr size_t kWidestClusterList = kMax2(kMax2(MT_COUNT(utilityMeterClusters), kMax2(MT_COUNT(demClusters), MT_COUNT(demReportOnlyClusters))), kMax2(kMax2(kMax2(MT_COUNT(electricalSensorClusters), MT_COUNT(electricalSensorPowerOnlyClusters)), kMax2(MT_COUNT(electricalMeterClusters), MT_COUNT(electricalMeterPowerOnlyClusters))), kMax2(kMax2(MT_COUNT(wiredElectricalSensorClusters), MT_COUNT(wiredElectricalSensorPowerOnlyClusters)), kMax2(
    kMax2(MT_COUNT(genericSwitchClusters),
          kMax2(MT_COUNT(pumpClusters), MT_COUNT(roomAirConditionerClusters))),
    kMax2(MT_COUNT(modeSelectClusters), kMax2(
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
            kMax2(kMax2(MT_COUNT(powerSourceClusters), MT_COUNT(smokeCoAlarmClusters)),
                  kMax2(MT_COUNT(applianceOpStateClusters), MT_COUNT(chimeClusters))))))))));

constexpr size_t kModeSelectBlockBytes =
    block_bytes(MT_COUNT(modeSelectClusters), kIdentifySlots + MT_COUNT(modeSelectAttrs)) +
    sizeof(mt_mode_store_t);
/* Catalogue batch 4, the chime candidate: the same own-lists-plus-stores
 * shape as the mode select's, with NO kIdentifySlots term (the chime has no
 * Identify, like the power source) and its own MT_COUNT(chimeAttrs) rather
 * than the no-Identify family maximum, so the candidate prices the chime's
 * own count. */
constexpr size_t kChimeBlockBytes =
    block_bytes(MT_COUNT(chimeClusters), MT_COUNT(chimeAttrs)) + sizeof(mt_chime_store_t);

/* Catalogue batch 5b, the robotic vacuum cleaner, under admission limits
 * (graph DE566): its block is wider than the 608 B every other type is
 * sixteen-of, so it is NOT folded into kWidestEndpointSlots or
 * kWidestClusterList. It gets its own candidate (Identify, the two ModeBase
 * lists, the operational state list and the two trailing ModeBase stores,
 * the MT_COUNT over-count as everywhere here) and a documented maximum of
 * ten per composition (nine until the object arena grew, DE603); a
 * composition past that is refused at create. This arena holds ten RVC
 * blocks (10 x 936 = 9,360 of 9,728), asserted below; the eleventh is
 * refused by the 20-slot ModeBase pool and could not fit the 4,256 B
 * cluster-object arena either (11 x 424 = 4,664), pinned by the
 * static_asserts beside the RvcOperationalState pair in
 * mt_matter_sl_b5.inc. */
constexpr size_t kRvcMaxEndpoints = 10;
constexpr size_t kRvcBlockBytes =
    block_bytes(MT_COUNT(rvcClusters),
                kIdentifySlots + 2 * MT_COUNT(modeBaseAttrs) + MT_COUNT(opStateAttrs)) +
    2 * sizeof(mt_mb_store_t);
static_assert(hearth_arena_cost(kRvcBlockBytes) == 936,
              "the RVC block moved off the 936 B the admission limit was sized with");
static_assert(kRvcMaxEndpoints * hearth_arena_cost(kRvcBlockBytes) <= kEpArenaBytes,
              "ten RVC blocks no longer fit the endpoint arena");

/* Catalogue batch 7a-2 (nRF 5446-5459, its batch 7a fix round, review
 * B7A-3): the DEM device type is the fourth store-bearing block shape, so
 * it is a candidate like the other three, not a silent bystander: the whole point of deriving the floor
 * from the declarations is that a grown store or attribute list cannot
 * out-size the guarded widest type unnoticed. The variant-0 list is the
 * wider one (demAttrs strictly contains demReportOnlyAttrs); MT_COUNT
 * over-counts the metadata-only members as ever, only pushing the
 * asserted floor higher than the real 462 B payload. On this port it is
 * 526 B, 528 of arena, and loses to the mode select's 608, so the widest
 * block below does not move; the EVSE round's EVSE (two stores, eight
 * clusters) is the candidate expected to take the title. */
constexpr size_t kDemBlockBytes =
    block_bytes(MT_COUNT(demClusters), MT_COUNT(demAttrs) + MT_COUNT(modeBaseAttrs)) +
    sizeof(mt_mb_store_t);
static_assert(hearth_arena_cost(kDemBlockBytes) == 528,
              "the DEM block moved off the 528 B this batch was sized with");

/*
 * Catalogue batch 7b: the MT_COUNT over-count convention STOPS at these two
 * candidates, deliberately. Every earlier candidate tolerated MT_COUNT's
 * counting of metadata-only members ("only pushes the asserted floor
 * higher, never lower") because the over-count was a handful of entries.
 * The two 7b shapes compose epmAttrs and eemAttrs, whose DE407 option-C
 * declarations are MOSTLY metadata-only (eight of epmAttrs' twelve entries
 * and three of eemAttrs' five take no slot), so the naive sums would be
 * 1,006 B for the water heater and about 1,150 B for battery storage:
 * both spuriously past the RVC's 936 B candidate. On the nRF the water
 * heater's over-count would even fail its floor assertion; on this port
 * both are admission candidates, not floor terms (like the RVC), and an
 * over-count would overstate their admission arithmetic against the
 * fixed 9,728 B endpoint arena, the opposite of what the asserts are for.
 * Catalogue batch 7b, 2026-09-27: the exact count departs from this
 * port's MT_COUNT convention for these two candidates only.
 *
 * So these candidates count EXACTLY: MT_COUNT per list minus a NAMED
 * constant for that list's metadata-only members, each constant carrying
 * the members it stands for. (Counting through attr_gets_slot() itself at
 * compile time would be better still, and does not compile: the lists are
 * const but not constexpr, and a const object of class type is not usable
 * in a constant expression, so attr_gets_slot() cannot read one at compile
 * time. The nRF's memory reclaim round A moved them from .data to .rodata
 * with HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN, the form this port
 * declares them in; that does not change this.)
 * Drift-proofing is the equality assert under each candidate, pinning the
 * whole derivation to the sizing table's payload row: ANY edit to any
 * involved list moves the sum off the pinned value and fails the build,
 * so a stale subtraction constant cannot survive a list change silently,
 * which is exactly the property the MT_COUNT over-count bought the older
 * candidates.
 */
/* Metadata-only (slotless) members per composed list, from the audit
 * notes at each declaration: epmAttrs has eight (Accuracy plus the seven
 * DE407 push fields), eemAttrs three (the three structs), whmAttrs one
 * (EstimatedHeatRequired), modeBaseAttrs one (SupportedModes);
 * thermostatAttrs and ptopAttrs have none. */
constexpr size_t kEpmNoSlot      = 8;
constexpr size_t kEemNoSlot      = 3;
constexpr size_t kWhmNoSlot      = 1;
constexpr size_t kModeBaseNoSlot = 1;

/* Water heater v0, the wider variant list (whmAttrs strictly contains
 * whmBareAttrs and the v1 list drops three whole clusters): 7 clusters,
 * 11 + 7 + 3 + 2 + 4 + 2 = 29 slots, one ModeBase store. */
constexpr size_t kWaterHeaterBlockBytes =
    block_bytes(MT_COUNT(waterHeaterClusters),
                MT_COUNT(thermostatAttrs) + (MT_COUNT(whmAttrs) - kWhmNoSlot) +
                    (MT_COUNT(modeBaseAttrs) - kModeBaseNoSlot) + MT_COUNT(ptopAttrs) +
                    (MT_COUNT(epmAttrs) - kEpmNoSlot) + (MT_COUNT(eemAttrs) - kEemNoSlot)) +
    sizeof(mt_mb_store_t);
/* The payload is pinned here, and its arena cost (800) and admission
 * below it, so a formula change cannot stale the tables silently. */
static_assert(kWaterHeaterBlockBytes == 798,
              "the water heater block changed size; redo the sizing table rows and the "
              "batch 7b capacity arithmetic");
static_assert(hearth_arena_cost(kWaterHeaterBlockBytes) == 800,
              "the water heater block's arena cost moved off the 800 B this batch was sized with");
static_assert(MT_WHM_MAX * hearth_arena_cost(kWaterHeaterBlockBytes) <= kEpArenaBytes,
              "MT_WHM_MAX water heater blocks no longer fit the endpoint arena (DE584)");

constexpr size_t kWidestBlockBytes =
    kMax2(block_bytes(kWidestClusterList, kWidestEndpointSlots),
          kMax2(kModeSelectBlockBytes, kMax2(kChimeBlockBytes, kDemBlockBytes)));

/* Pinned, so the sizing table above cannot go stale without the build
 * noticing: the mode select is the widest type this build declares, at 608
 * payload bytes and 608 of arena. */
static_assert(kWidestBlockBytes == 608,
              "the widest declared block changed size; redo the sizing table above and the "
              "README's capacity rows");
static_assert(hearth_arena_cost(kWidestBlockBytes) == 608,
              "the widest declared block's arena cost moved off the table's 608");

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
 * catalogue stays small enough that the strong form holds: sixteen mode
 * selects are 9,728 bytes, so the floor stays the full
 * kServiceableEndpoints and the capacity claim is "every composition this
 * build accepts, it can build". Catalogue batch 1 kept that promise by
 * raising HEARTH_EP_ARENA_BYTES from 3,072 to 5,376 rather than lowering the
 * floor (the ruling of 2026-09-22); catalogue batch 2's wider types revisit
 * the choice and keep the same strong form by raising it from 5,376 to
 * 9,600 (the ruling of 2026-09-24); catalogue batch 4's store-bearing mode
 * select revisits the choice and keeps the same strong form by raising it
 * from 9,600 to 9,728 (the ruling of 2026-09-26). The RVC (batch 5b) stays
 * below this floor by design: its admission limit is up to nine (DE566),
 * capped by the cluster-object arena, not by this endpoint floor, which
 * would hold ten of its 936-byte blocks.
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

