/*
 * mt_matter_sl_evse.inc: a fragment of mt_matter_sl.cpp, NOT a compile unit.
 *
 * Holds bridge code: the EVSE round (nRF mt_matter_zephyr.cpp 6938-9135):
 * HearthEvseDelegate with its charging-target store and its NVM3
 * persistence, the EVSE pool, reserve and register, the host-side target
 * store and the AT+MTROW dispatchers, the AT+MTMEAS 0x0099 bridge, the
 * mt_matter_evse_set() and the EnergyEvse live read. Included at the end of
 * mt_matter_sl.cpp after mt_matter_sl_b7.inc, so the whole is still one
 * translation unit. Do not add it to hearth.slcp.
 */

/*
 * ---- energy EVSE: HearthEvseDelegate, its target store and its pool -------
 *
 * Catalogue batch EVSE, the last device type. The C6's mt_evse.cpp
 * (platform/esp32c6/main/mt_evse.cpp) ported whole: the host-pushed scalar
 * cache the Instance's AAI reads, the charging-target store with its own
 * persistence, and the two SDK lifetime contracts nothing enforces. It stays
 * in mt_matter_sl.cpp's translation unit (this fragment is included at its
 * end) rather than earning its own the way the C6's does, because the
 * cluster-object arena and obj_pair_new() live in that file's anonymous
 * namespace; a second translation unit would have to export them, and the
 * port has no per-cluster translation unit for any other family either.
 *
 * ---- THE PURE-VIRTUAL COUNT ----
 *
 * 34, counted from THIS tree's own header rather than transcribed:
 * energy-evse-server.h's class Delegate is 8 command handlers, 23 attribute
 * getters and 3 attribute setters. A missing one makes this class abstract
 * and obj_pair_new<HearthEvseDelegate, Instance>() fails to compile, so the
 * build is the arbiter.
 *
 * ---- THE TWO LIFETIME CONTRACTS, both documented and enforced by nothing --
 *
 * 1. LoadTargets() MUST run before GetTargets(). Nothing in the server calls
 *    it: Instance::Init() (energy-evse-server.cpp:40-46) registers the CHI
 *    and the AAI and nothing else, and HandleGetTargets() goes straight to
 *    the delegate. Satisfied in mt_matter_evse_delegate_alloc() below, during
 *    the boot composition rebuild, and again lazily in GetTargets() itself so
 *    no future caller can reintroduce the gap.
 *
 * 2. GetTargets() hands back a NON-OWNING VIEW. Its out parameter is a
 *    DataModel::List over storage this delegate owns, and the encode happens
 *    LATER, at ctx.mCommandHandler.AddResponse(). Both the schedule array AND
 *    every per-day target array must still be alive and unmodified at that
 *    point, which is why the whole store is MEMBERS here and nothing ever
 *    builds a schedule on a stack and returns a view over it: that would be a
 *    use-after-free on the SUCCESS path, invisible to every test.
 *
 *    THE STORE STAYS IN THE POOL OBJECT, NOT IN THE ENDPOINT BLOCK, and that
 *    is this port's own decision rather than an inherited one. The C6's
 *    justification ("1680 bytes against ~106 KB free heap is not the
 *    constraint it is for a general-purpose reference") does not transfer to
 *    a 256 KB part. The arithmetic that does: at MT_EVSE_MAX 2 the store is
 *    about 3.4 KB of the 8,464 B cluster-object arena, which DE608 grew by
 *    4,208 B of .bss for it (about 67 KB of heap stays free at +MTREADY),
 *    while moving it into the endpoint block would take that block from
 *    1,144 B to roughly 2,900 B of the fixed 9,728 B endpoint arena for no
 *    gain at a cap of two. The endpoint-block technique pays when a store is
 *    charged per POSSIBLE endpoint (the mode and chime stores were 16-deep
 *    .bss); this one occupies the shared object arena only while an EVSE is
 *    composed, and other families draw on that space otherwise.
 *
 * The mutating side of contract 2 is the stack lock. Instance::HandleGetTargets
 * calls GetTargets() and AddResponse() back to back on the CHIP event loop
 * task, which holds the stack lock throughout; every mutation from the AT
 * parser task goes through a bridge below that takes the same lock, so no
 * host apply can land between the view being handed out and the encoder
 * reading it. The other mutator, the adjudicated SetTargets, runs on the CHIP
 * task itself, so it and the encode are one thread of execution.
 *
 * ---- THE MERGE RULE ----
 *
 * An apply MERGES BY DAY; it does not replace wholesale. Days present in the
 * applied set replace those days ENTIRELY, days absent are UNCHANGED. CHIP's
 * own reference store splits day bitmasks the same way
 * (EnergyEvseTargetsStore.cpp), and a wholesale replace passes every
 * apply-then-read-back test anyone would write while silently destroying days
 * a controller never mentioned. The one exception: a count of 0 with no days
 * named clears the ENTIRE stored payload, because that is how a host issues
 * the ClearTargets equivalent and merging zero rows would otherwise leave no
 * way to empty a schedule at all.
 *
 * ---- PERSISTENCE, and its deliberate asymmetry with the composition ----
 *
 * The schedule lives in this port's own KV namespace (MT_EVSE_KV_NS) through
 * hearth_kv_*, i.e. NVM3 (hearth_port_sl.c; hearth_kv_id() hashes the
 * (namespace, key) pair into the Hearth key range 0x0A000..0x0AFFF), one
 * blob per endpoint. It is
 * USER DATA: it survives AT+MTRESET and a power cycle, and only AT+MTFRESET
 * erases it. That is the exact opposite of the endpoint composition, which
 * survives a factory reset because it is a product definition rather than
 * something an end user chose. Same store, opposite lifetimes.
 */

/* The merge both row paths commit through, defined below beside the store
 * because it needs the pool lookup; declared here because the delegate's
 * SetTargets() calls it and a member function body is compiled as if written
 * right after the class. */
static int evse_targets_apply_locked(uint16_t ep, const mt_row_stage_t *stage,
                                     uint8_t clear_days);

/* Field order of MT_ROW_KIND_EVSE_TARGET, from mt_rows.c's own field table
 * (day bitmap, minutes past midnight, SoC, added energy). Named here rather
 * than repeated as literals; mt_rows.h publishes only the field COUNT,
 * because the codec is the single definition of the rules. */
constexpr uint8_t kRowDay    = 0;
constexpr uint8_t kRowTime   = 1;
constexpr uint8_t kRowSoC    = 2;
constexpr uint8_t kRowEnergy = 3;
constexpr uint8_t kRowFields = 4;

/* Spec-defined store bounds, from the SDK header (energy-evse-server.h:40-41;
 * :38 is kMinimumChargeCurrentLimit and :39 kMaxRandomizationDelayWindow,
 * which the push branch below reads from the same header), never
 * transcribed. */
constexpr uint8_t kEvseMaxDays   = chip::app::Clusters::EnergyEvse::kEvseTargetsMaxNumberOfDays;
constexpr uint8_t kEvseMaxPerDay = chip::app::Clusters::EnergyEvse::kEvseTargetsMaxTargetsPerDay;

/* Every day bit the cluster defines, bits 0..6 (TargetDayOfWeekBitmap
 * kSunday..kSaturday). The same mask mt_rows.c's field table enforces at
 * stage time. */
constexpr uint8_t kEvseAllDaysMask = 0x7F;

/*
 * ---- the stored blob ------------------------------------------------------
 *
 * One blob per EVSE endpoint, key "t<ep>" in namespace MT_EVSE_KV_NS, BYTE FOR
 * BYTE the C6's format (mt_evse.cpp) so the two platforms describe a charging
 * schedule the same way:
 *
 *   u8  version (kEvseBlobV1)
 *   u8  schedule count, 0..7
 *   per schedule:
 *     u8  day bitmap, 1..0x7F
 *     u8  target count, 0..10
 *     per target:
 *       u8  flags: bit 0 SoC present, bit 1 added energy present
 *       u16 minutes past midnight, little endian
 *       u8  SoC percent (0 when absent)
 *       i64 added energy mWh, little endian (0 when absent)
 *
 * Hand-rolled and fixed-width for the same reason mt_composition.c is: the
 * format is small, it has to be readable by a maintainer holding only this
 * comment, and a decoder that validates every field can treat a corrupt blob
 * as "no schedule" instead of trusting flash.
 *
 * Per-endpoint keys, not one blob for the device, so an apply rewrites only
 * the endpoint it touched. Known residue: changing the composition so a former
 * EVSE endpoint id becomes something else leaves that key behind. It is inert
 * (only an endpoint that HAS a delegate ever reads one) and
 * mt_matter_evse_targets_erase_all() sweeps every id this port can assign.
 */
#define MT_EVSE_KV_NS "mt_evse"
constexpr uint8_t kEvseBlobV1 = 1;

constexpr size_t kEvseTargetBlobSize   = 1 + 2 + 1 + 8;                            /* 12  */
constexpr size_t kEvseScheduleBlobSize = 2 + kEvseMaxPerDay * kEvseTargetBlobSize; /* 122 */
constexpr size_t kEvseBlobMax          = 2 + kEvseMaxDays * kEvseScheduleBlobSize; /* 856 */

/*
 * The one scratch buffer every encode and decode uses. File-static rather
 * than a local because 856 bytes is a seventh of the AT parser task's
 * 6144-byte stack, and every caller is serialised by the CHIP stack lock:
 * the mutating bridges take it, the fabric handlers already run on the CHIP
 * task holding it, and mt_matter_evse_delegate_alloc() runs inside
 * mt_devtype_create(), which holds it for the whole create.
 */
static uint8_t s_evse_blob[kEvseBlobMax];

static void evse_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t evse_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void evse_put_i64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
    }
}

static int64_t evse_get_i64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= ((uint64_t)p[i]) << (8 * i);
    }
    return (int64_t)u;
}

static void evse_key_for(uint16_t ep, char *out, size_t len)
{
    snprintf(out, len, "t%u", (unsigned)ep);
}

/*
 * The delegate. Every attribute of EnergyEvse (0x0099) is served by the
 * cluster's Instance from the cache below (energy-evse-server.cpp:68-133
 * covers all 23 and falls through to ember only for ClusterRevision), so
 * AT+MTMEAS cluster 0x0099 is the ONLY way a host sets any of them and none
 * of them ever raises a +MTATTR URC.
 *
 * Command handlers take NO stack lock: they are invoked by CHIP from the
 * Instance's command dispatch, already on the CHIP event loop task holding
 * it, the standing rule HearthWhmDelegate::HandleBoost and
 * HearthDemDelegate::PowerAdjustRequest carry.
 */
class HearthEvseDelegate : public chip::app::Clusters::EnergyEvse::Delegate
{
public:
    /* ---- host-pushed scalar cache -------------------------------------
     * Defaults are the pre-first-push answers: the three enums at their
     * zero (NotPluggedIn / Disabled / NoError), the currents and the delay
     * window at 0, every nullable NULL. The same "null until the host first
     * pushes it" contract the EPM measurement values ship with, and the
     * values the inert arena shadows seeded by mt_devtypes_sl_seeds.inc
     * carry. */
    chip::app::Clusters::EnergyEvse::StateEnum m_state =
        chip::app::Clusters::EnergyEvse::StateEnum::kNotPluggedIn;
    chip::app::Clusters::EnergyEvse::SupplyStateEnum m_supply_state =
        chip::app::Clusters::EnergyEvse::SupplyStateEnum::kDisabled;
    chip::app::Clusters::EnergyEvse::FaultStateEnum m_fault_state =
        chip::app::Clusters::EnergyEvse::FaultStateEnum::kNoError;

    chip::app::DataModel::Nullable<uint32_t> m_charging_enabled_until;
    chip::app::DataModel::Nullable<uint32_t> m_discharging_enabled_until;

    int64_t  m_circuit_capacity        = 0;
    int64_t  m_min_charge_current      = 0;
    int64_t  m_max_charge_current      = 0;
    int64_t  m_max_discharge_current   = 0;
    int64_t  m_user_max_charge_current = 0;
    uint32_t m_randomization_window    = 0;

    chip::app::DataModel::Nullable<uint32_t>      m_next_charge_start;
    chip::app::DataModel::Nullable<uint32_t>      m_next_charge_target;
    chip::app::DataModel::Nullable<int64_t>       m_next_charge_energy;
    chip::app::DataModel::Nullable<chip::Percent> m_next_charge_soc;
    chip::app::DataModel::Nullable<uint16_t>      m_approx_efficiency;

    chip::app::DataModel::Nullable<chip::Percent> m_state_of_charge;
    chip::app::DataModel::Nullable<int64_t>       m_battery_capacity;

    chip::app::DataModel::Nullable<uint32_t> m_session_id;
    chip::app::DataModel::Nullable<uint32_t> m_session_duration;
    chip::app::DataModel::Nullable<int64_t>  m_session_energy_charged;
    chip::app::DataModel::Nullable<int64_t>  m_session_energy_discharged;

    chip::EndpointId endpoint() const { return mEndpointId; }

    /* The endpoint's EnergyEvse feature bits, stamped by
     * mt_matter_evse_register() from the create path's declared-list
     * predicate, which the variant-qualified FeatureMap seed rows
     * (mt_devtypes_sl_seeds.inc) mirror
     * (the WHM and DEM registers' discipline: this port's ember FeatureMap
     * copy is a seed shadow, so the port record stamped from the same
     * predicate is the honest source, where the C6 reads its live
     * FeatureMap attribute back). Read by the 0x99 push branch's existence
     * gates and by the SOC-variant rule in the merge. */
    void set_features(uint32_t mask)
    {
        m_features       = mask;
        m_features_known = true;
    }
    bool has_feature(chip::app::Clusters::EnergyEvse::Feature f) const
    {
        return (m_features & chip::to_underlying(f)) != 0;
    }

    /* ---- the charging-target store -------------------------------------
     * Contract 2 in the section comment: this IS the memory GetTargets()
     * hands to the encoder, so it is held here and never rebuilt on a
     * stack. m_schedules[i].chargingTargets is a List view over
     * m_targets[i], which refresh_views() re-points after every mutation (a
     * List is a pointer and a length, so it goes stale the moment a count
     * changes). */
    chip::app::Clusters::EnergyEvse::Structs::ChargingTargetScheduleStruct::Type
        m_schedules[kEvseMaxDays];
    chip::app::Clusters::EnergyEvse::Structs::ChargingTargetStruct::Type
            m_targets[kEvseMaxDays][kEvseMaxPerDay];
    uint8_t m_target_count[kEvseMaxDays] = { 0 };
    uint8_t m_schedule_count             = 0;
    bool    m_loaded                     = false;

    void refresh_views()
    {
        for (uint8_t i = 0; i < m_schedule_count; i++) {
            m_schedules[i].chargingTargets = chip::app::DataModel::List<
                const chip::app::Clusters::EnergyEvse::Structs::ChargingTargetStruct::Type>(
                m_targets[i], m_target_count[i]);
        }
    }

    void clear_store()
    {
        using namespace chip::app::Clusters::EnergyEvse;
        for (uint8_t i = 0; i < kEvseMaxDays; i++) {
            m_schedules[i].dayOfWeekForSequence = chip::BitMask<TargetDayOfWeekBitmap>(0);
            m_schedules[i].chargingTargets =
                chip::app::DataModel::List<const Structs::ChargingTargetStruct::Type>();
            m_target_count[i] = 0;
            for (uint8_t j = 0; j < kEvseMaxPerDay; j++) {
                m_targets[i][j] = Structs::ChargingTargetStruct::Type();
            }
        }
        m_schedule_count = 0;
    }

    uint8_t day_bits(uint8_t i) const
    {
        using chip::app::Clusters::EnergyEvse::TargetDayOfWeekBitmap;
        return m_schedules[i].dayOfWeekForSequence.GetField(
            static_cast<TargetDayOfWeekBitmap>(kEvseAllDaysMask));
    }

    void set_day_bits(uint8_t i, uint8_t bits)
    {
        using chip::app::Clusters::EnergyEvse::TargetDayOfWeekBitmap;
        m_schedules[i].dayOfWeekForSequence = chip::BitMask<TargetDayOfWeekBitmap>(bits);
    }

    /* Total stored rows: the flattened (schedule, target) sequence. */
    uint16_t row_total() const
    {
        uint16_t n = 0;
        for (uint8_t i = 0; i < m_schedule_count; i++) {
            n = (uint16_t)(n + m_target_count[i]);
        }
        return n;
    }

    /*
     * Drop every stored schedule day that appears in bits, and delete any
     * schedule left with no days at all: the "days present in the applied
     * set replace those days entirely" half of the merge. The caller
     * appends the incoming groups afterwards. Deliberately NOT callable
     * before the caller has validated the incoming set, because it mutates.
     */
    void subtract_days(uint8_t bits)
    {
        uint8_t dst = 0;
        for (uint8_t src = 0; src < m_schedule_count; src++) {
            uint8_t left = (uint8_t)(day_bits(src) & (uint8_t)~bits);
            if (left == 0) {
                continue; /* every one of its days was replaced */
            }
            if (dst != src) {
                set_day_bits(dst, left);
                m_target_count[dst] = m_target_count[src];
                for (uint8_t j = 0; j < m_target_count[src]; j++) {
                    m_targets[dst][j] = m_targets[src][j];
                }
            } else {
                set_day_bits(dst, left);
            }
            dst++;
        }
        for (uint8_t i = dst; i < m_schedule_count; i++) {
            m_target_count[i] = 0;
            set_day_bits(i, 0);
        }
        m_schedule_count = dst;
        refresh_views();
    }

    /* ---- persistence --------------------------------------------------- */

    /* Encode the store into s_evse_blob. Returns the byte count. */
    size_t encode()
    {
        using namespace chip::app::Clusters::EnergyEvse;
        size_t n      = 0;
        s_evse_blob[n++] = kEvseBlobV1;
        s_evse_blob[n++] = m_schedule_count;
        for (uint8_t i = 0; i < m_schedule_count; i++) {
            s_evse_blob[n++] = day_bits(i);
            s_evse_blob[n++] = m_target_count[i];
            for (uint8_t j = 0; j < m_target_count[i]; j++) {
                const Structs::ChargingTargetStruct::Type &t = m_targets[i][j];
                uint8_t flags = 0;
                if (t.targetSoC.HasValue()) {
                    flags |= 0x01;
                }
                if (t.addedEnergy.HasValue()) {
                    flags |= 0x02;
                }
                s_evse_blob[n++] = flags;
                evse_put_u16(&s_evse_blob[n], t.targetTimeMinutesPastMidnight);
                n += 2;
                s_evse_blob[n++] = t.targetSoC.HasValue() ? t.targetSoC.Value() : 0;
                evse_put_i64(&s_evse_blob[n], t.addedEnergy.HasValue() ? t.addedEnergy.Value() : 0);
                n += 8;
            }
        }
        return n;
    }

    /*
     * Decode s_evse_blob[0..len) into the store. Returns false and leaves the
     * store EMPTY on anything unexpected: a truncated or corrupt blob means
     * "no schedule", never a half-loaded one, because a half-loaded schedule
     * is a charging plan nobody authored.
     */
    bool decode(size_t len)
    {
        using namespace chip::app::Clusters::EnergyEvse;
        clear_store();
        if (len < 2 || s_evse_blob[0] != kEvseBlobV1) {
            return false;
        }
        uint8_t sched = s_evse_blob[1];
        if (sched > kEvseMaxDays) {
            return false;
        }
        size_t  n         = 2;
        uint8_t seen_days = 0;
        for (uint8_t i = 0; i < sched; i++) {
            if (len - n < 2) {
                clear_store();
                return false;
            }
            uint8_t bits  = s_evse_blob[n++];
            uint8_t count = s_evse_blob[n++];
            if (bits == 0 || (bits & (uint8_t)~kEvseAllDaysMask) != 0 || (bits & seen_days) != 0 ||
                count > kEvseMaxPerDay) {
                clear_store();
                return false;
            }
            seen_days = (uint8_t)(seen_days | bits);
            if (len - n < (size_t)count * kEvseTargetBlobSize) {
                clear_store();
                return false;
            }
            set_day_bits(i, bits);
            m_target_count[i] = count;
            for (uint8_t j = 0; j < count; j++) {
                uint8_t  flags = s_evse_blob[n++];
                uint16_t time  = evse_get_u16(&s_evse_blob[n]);
                n += 2;
                uint8_t soc    = s_evse_blob[n++];
                int64_t energy = evse_get_i64(&s_evse_blob[n]);
                n += 8;
                if (time > 1439 || soc > 100 || energy < 0) {
                    clear_store();
                    return false;
                }
                Structs::ChargingTargetStruct::Type t;
                t.targetTimeMinutesPastMidnight = time;
                if (flags & 0x01) {
                    t.targetSoC.SetValue(soc);
                }
                if (flags & 0x02) {
                    t.addedEnergy.SetValue(energy);
                }
                if (!t.targetSoC.HasValue() && !t.addedEnergy.HasValue()) {
                    /* The XML's "choice a min 1": a target that says neither
                     * how full nor how much is not a target. */
                    clear_store();
                    return false;
                }
                m_targets[i][j] = t;
            }
            m_schedule_count = (uint8_t)(i + 1);
        }
        refresh_views();
        return true;
    }

    /*
     * THE SOC RULE APPLIES TO WHAT WAS LOADED, NOT ONLY TO WHAT ARRIVES, and
     * it has to run after EVERY load rather than after the first one.
     *
     * The case it closes. The blob is keyed on the endpoint id alone
     * ("t<ep>") and endpoint ids are assigned 1..N in composition order, so
     * re-declaring an EVSE at the same index with the other variant hands the
     * new endpoint the old one's schedule. decode() cannot see it: every
     * target in that schedule is well formed, in range, and was legal for the
     * variant that wrote it. Served unchanged, GetTargets() answers a
     * controller with targets carrying a targetSoC the cluster's SOC
     * conformance says may only be absent or 100, or with targets lacking one
     * where ValidateTargets() makes it mandatory. Either way GetTargets hands
     * back a schedule its own SetTargets would have been refused, which is
     * the one thing evse_targets_apply_locked()'s SOC check exists to prevent
     * on the other path.
     *
     * WHY IT IS A HELPER AND NOT AN INLINE BLOCK AT ONE CALL SITE. There are
     * THREE loads per endpoint, not one: the boot load from
     * mt_matter_evse_delegate_alloc(), the apply path's !m_loaded retry, and
     * GetTargets()'s own lazy retry. A boot load that hits a transient KV
     * failure leaves the store empty and m_loaded false, so a check placed at
     * the boot site passes over nothing; the later retry then succeeds, loads
     * the cross-variant blob and serves it unchecked, which is the exact
     * sentence this check is written against, reached by a different door.
     *
     * THE ONE ASYMMETRY WITH THE C6, and the reason this is not simply called
     * from LoadTargets() the way it is there: on that platform the variant is
     * an attribute of the live endpoint and is readable at every load. Here
     * the endpoint does not exist yet when the boot load runs
     * (mt_matter_evse_delegate_alloc() is called from mt_devtype_create()
     * BEFORE emberAfSetDynamicEndpoint()), so the first load genuinely cannot
     * know the variant. m_features_known is that fact made explicit: this
     * helper is a no-op until mt_matter_evse_register() stamps the mask, and
     * that function calls it once immediately afterwards, which covers the
     * boot load. LoadTargets() calls it too, which covers the other two.
     * Between them every load is validated, and the one load that cannot be
     * is validated the moment it becomes possible.
     *
     * THE DISCARD IS RAM-ONLY. The blob is not rewritten, so the log repeats
     * on every boot and the schedule returns intact if the endpoint is later
     * re-declared with its original variant. That is deliberate: a variant
     * flip is a composition edit, often a mistake, and destroying the user's
     * schedule to tidy up after it would be the worse failure. The store is
     * simply not served while it cannot be served correctly.
     */
    void drop_store_if_variant_mismatch()
    {
        using namespace chip::app::Clusters::EnergyEvse;
        if (!m_features_known || m_schedule_count == 0) {
            return;
        }
        const bool soc = has_feature(Feature::kSoCReporting);
        for (uint8_t i = 0; i < m_schedule_count; i++) {
            for (uint8_t j = 0; j < m_target_count[i]; j++) {
                const Structs::ChargingTargetStruct::Type &t = m_targets[i][j];
                if (soc ? !t.targetSoC.HasValue()
                        : (t.targetSoC.HasValue() && t.targetSoC.Value() != 100)) {
                    HEARTH_LOGE("matter", "ep %u: the stored charging schedule was written for the other "
                            "SOC variant and cannot be served here; not serving it (the "
                            "stored blob is left alone). A controller would otherwise be "
                            "handed a schedule its own SetTargets would be refused",
                            (unsigned)mEndpointId);
                    clear_store();
                    return;
                }
            }
        }
    }

    /* Write the store to the KV. Returns false on any failure. */
    bool save()
    {
        size_t len = encode();
        char   key[16];
        evse_key_for(mEndpointId, key, sizeof(key));
        if (hearth_kv_set_blob(MT_EVSE_KV_NS, key, s_evse_blob, len) != 0) {
            HEARTH_LOGE("matter", "ep %u: saving charging targets failed", (unsigned)mEndpointId);
            return false;
        }
        return true;
    }

    /* ---- the 8 command handlers ---------------------------------------- */

    /*
     * ---- Disable / EnableCharging: the EVSE's entire control surface ----
     *
     * Both are MANDATORY on this cluster and the Instance advertises both
     * unconditionally (RetrieveAcceptedCommands, energy-evse-server.cpp:186-211),
     * so a controller can invoke them the moment the endpoint exists.
     * Everything else on the cluster is a reading, so an EVSE that refused
     * both would be one with no control surface at all: they forward as
     * ordinary +MTCMD commands on the standard verdict path, the shape the
     * water heater's Boost uses.
     *
     * Neither one writes any attribute on allow, deliberately. SupplyState,
     * State and ChargingEnabledUntil are host-pushed (AT+MTMEAS 0x0099); the
     * host is the single authority on what the hardware actually did, and a
     * firmware that also wrote them would make two authorities disagree the
     * first time a relay failed to close. The host reports the resulting
     * state by pushing it, the split-ownership rule AT+MTLOCK, AT+MTVALVE and
     * the water heater's BoostState all keep.
     */
    chip::Protocols::InteractionModel::Status Disable() override
    {
        using namespace chip::app::Clusters::EnergyEvse;
        /* No fields: NULL reproduces mt_cmd_forward()'s exact four-field
         * +MTCMD line. */
        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::Disable::Id, NULL);
        return allow ? chip::Protocols::InteractionModel::Status::Success
                     : chip::Protocols::InteractionModel::Status::Failure;
    }

    /*
     * EnableCharging forwards three fields:
     * "<chargingEnabledUntil>,<minimumChargeCurrent>,<maximumChargeCurrent>"
     * (AT_MT_SPEC.md 3.17).
     *
     * chargingEnabledUntil is NULLABLE (null means "charge indefinitely") and
     * null renders as an EMPTY TOKEN between its commas, which is
     * mt_cmd_forward_fields()'s documented convention for a field position
     * carrying no value. The water heater's Boost uses a presence MASK
     * instead, but that exists to carry the VALUES of optional BOOLEANS,
     * which an empty token cannot express, and it costs a field of its own;
     * with one nullable number and no booleans here the empty token says the
     * same thing in the position the value would have occupied.
     *
     * No validation of the two currents: HandleEnableCharging
     * (energy-evse-server.cpp) has already rejected either below
     * kMinimumChargeCurrentLimit, and min > max, with ConstraintError before
     * this is reached.
     *
     * 64 bytes, not 48, and the arithmetic matters because snprintf truncates
     * in SILENCE and a truncated tail hands the host a wrong, smaller maximum
     * current, which is a safety-relevant number. The server bounds these
     * only from below, so the real worst case is a non-null
     * chargingEnabledUntil at UINT32_MAX with both currents at INT64_MAX:
     * 10 + 1 + 19 + 1 + 19 = 50 plus a NUL = 51. A negative current cannot
     * reach here but would only add two sign characters (53), so 64 has
     * headroom either way.
     */
    chip::Protocols::InteractionModel::Status EnableCharging(
        const chip::app::DataModel::Nullable<uint32_t> &enableChargeTime,
        const int64_t &minimumChargeCurrent, const int64_t &maximumChargeCurrent) override
    {
        using namespace chip::app::Clusters::EnergyEvse;
        char fields[64];
        if (enableChargeTime.IsNull()) {
            snprintf(fields, sizeof(fields), ",%lld,%lld", (long long)minimumChargeCurrent,
                     (long long)maximumChargeCurrent);
        } else {
            snprintf(fields, sizeof(fields), "%lu,%lld,%lld",
                     (unsigned long)enableChargeTime.Value(), (long long)minimumChargeCurrent,
                     (long long)maximumChargeCurrent);
        }
        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::EnableCharging::Id, fields);
        return allow ? chip::Protocols::InteractionModel::Status::Success
                     : chip::Protocols::InteractionModel::Status::Failure;
    }

    /*
     * EnableDischarging: V2X only, and V2X is NEVER BUILT on either platform
     * (the evseAttrs declaration note), so the Instance answers
     * UnsupportedCommand at the dispatch (energy-evse-server.cpp:227-232)
     * before ever reaching here. The override exists because the pure virtual
     * does.
     */
    chip::Protocols::InteractionModel::Status EnableDischarging(
        const chip::app::DataModel::Nullable<uint32_t> &enableDischargeTime,
        const int64_t &maximumDischargeCurrent) override
    {
        (void)enableDischargeTime;
        (void)maximumDischargeCurrent;
        return chip::Protocols::InteractionModel::Status::Failure;
    }

    /*
     * StartDiagnostics exists ONLY because the pure virtual does. The
     * Instance is constructed with an EMPTY OptionalCommands mask, so
     * StartDiagnostics never enters the advertised AcceptedCommandList
     * (RetrieveAcceptedCommands gates it on
     * OptionalCommands::kSupportsStartDiagnostics) and an invoke answers
     * UnsupportedCommand. Deliberate on both platforms: advertising it would
     * promise a diagnostic self-test this firmware cannot run.
     */
    chip::Protocols::InteractionModel::Status StartDiagnostics() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }

    /*
     * ---- SetTargets: the adjudicated path -------------------------------
     *
     * A controller's charging schedule does not become this device's schedule
     * until the host has seen it and said yes. The sequence, all of it on the
     * CHIP event loop task:
     *
     *   1. claim the inbound row stage and decode the schedule into it
     *   2. raise "+MTCMD:<seq>,<ep>,153,5,<rowcount>,<daymask>" and block
     *   3. the host pulls the rows with AT+MTROWGET=<ep>,1,,<seq> and answers
     *      AT+MTCMDRESP=<seq>,<0|1>
     *   4. on allow, merge through evse_targets_apply_locked(), the SAME
     *      function an AT+MTROWAPPLY commits through, so the two paths cannot
     *      store different things from the same rows
     *   5. release the claim, on every path out
     *
     * <daymask> is the set of days the proposal touches, which is NOT
     * derivable from the rows: a day being emptied carries none. See the
     * `affected` comment in the body.
     *
     * The rows never travel in the +MTCMD line. 70 rows is about 2,450 bytes,
     * MT_CMD_LINE_MAX is 112 and snprintf truncates in silence; the host
     * pulls instead, inside a command where it is doing nothing but draining
     * the response.
     *
     * ---- WHAT IS NOT RE-VALIDATED HERE ----
     *
     * Instance::ValidateTargets() has already run and returned Success, so
     * day-bit reuse across entries, a time past 1439, both SoC rules of the
     * SOC feature, a target with neither optional, a negative added energy
     * and more than ten targets in one day are all impossible by the time
     * this is called. None is checked again.
     *
     * mt_rows_stage() below is nonetheless the WRITER for this buffer and it
     * validates as it writes. That is not a second opinion on CHIP's work, it
     * is the codec's own field table doing what it does on the AT path too,
     * and it catches the three things ValidateTargets does NOT reject and
     * this firmware cannot represent: a schedule entry with a
     * dayOfWeekForSequence of ZERO (accepted there, since `bitmap & 0` never
     * collides and `|= 0` never marks a day), the unbounded NUMBER of such
     * entries that follows from it, and any total row count past the 70 a
     * stage holds.
     */
    chip::Protocols::InteractionModel::Status SetTargets(
        const chip::app::DataModel::DecodableList<
            chip::app::Clusters::EnergyEvse::Structs::ChargingTargetScheduleStruct::DecodableType>
            &chargingTargetSchedules) override
    {
        using chip::Protocols::InteractionModel::Status;
        using namespace chip::app::Clusters::EnergyEvse;

        mt_row_stage_t *stage = mt_rows_inbound_claim(mEndpointId, MT_ROW_KIND_EVSE_TARGET);
        if (stage == nullptr) {
            /* Another forward owns the stage, or the host is still streaming
             * the previous one out. Busy is retryable and true; Failure would
             * tell the controller its schedule was rejected. */
            HEARTH_LOGW("matter", "ep %u: SetTargets busy, inbound stage in use", (unsigned)mEndpointId);
            return Status::Busy;
        }

        /*
         * Flatten (schedule, target) into rows, the same flattening
         * evse_targets_get_locked() uses in the other direction: the day
         * bitmap is repeated on every row of its group, which is what lets
         * one row shape carry a nested payload.
         *
         * Re-iterating the DecodableList is sound: ValidateTargets() has
         * already walked this very list, begin() takes a copy of the TLV
         * reader, and the message buffer is alive for the whole invoke.
         */
        Status   decode_err = Status::Success;
        uint16_t n          = 0;
        /*
         * The affected-day mask, taken from the schedule ENTRIES and not from
         * the flattened rows. An entry may carry an EMPTY chargingTargets
         * list, which ValidateTargets() accepts (innerIdx starts at 0 and
         * only innerIdx > 10 is rejected) and which means "this day now has
         * no targets". It produces no row, so a mask derived from rows would
         * never mention that day, subtract_days() would leave it alone, and
         * the device would answer SUCCESS to a deletion it did not perform.
         * CHIP's own reference store clears the day in both of its arms.
         */
        uint8_t affected   = 0;
        auto    sched_iter = chargingTargetSchedules.begin();
        while (decode_err == Status::Success && sched_iter.Next()) {
            const auto &entry = sched_iter.GetValue();
            uint8_t     bits  = entry.dayOfWeekForSequence.GetField(
                static_cast<TargetDayOfWeekBitmap>(kEvseAllDaysMask));
            affected = (uint8_t)(affected | bits);

            auto tgt_iter = entry.chargingTargets.begin();
            while (tgt_iter.Next()) {
                if (n >= MT_ROW_MAX_ROWS) {
                    /* Only reachable through the zero-bitmap hole described
                     * above: seven real day groups cap at 7 x 10 = 70. */
                    decode_err = Status::ResourceExhausted;
                    break;
                }
                const auto &t = tgt_iter.GetValue();

                mt_row_t row;
                memset(&row, 0, sizeof(row));
                row.nfields            = kRowFields;
                row.present[kRowDay]   = true;
                row.value[kRowDay]     = bits;
                row.present[kRowTime]  = true;
                row.value[kRowTime]    = t.targetTimeMinutesPastMidnight;
                if (t.targetSoC.HasValue()) {
                    row.present[kRowSoC] = true;
                    row.value[kRowSoC]   = t.targetSoC.Value();
                }
                if (t.addedEnergy.HasValue()) {
                    row.present[kRowEnergy] = true;
                    row.value[kRowEnergy]   = t.addedEnergy.Value();
                }

                if (mt_rows_stage(stage, mEndpointId, MT_ROW_KIND_EVSE_TARGET, n, &row) !=
                    MT_ROW_OK) {
                    decode_err = Status::ConstraintError;
                    break;
                }
                n++;
            }
            if (decode_err == Status::Success && tgt_iter.GetStatus() != CHIP_NO_ERROR) {
                decode_err = Status::InvalidCommand;
            }
        }
        if (decode_err == Status::Success && sched_iter.GetStatus() != CHIP_NO_ERROR) {
            decode_err = Status::InvalidCommand;
        }

        if (decode_err != Status::Success) {
            HEARTH_LOGW("matter", "ep %u: SetTargets not representable, status 0x%02x", (unsigned)mEndpointId,
                    (unsigned)chip::to_underlying(decode_err));
            mt_rows_inbound_release();
            return decode_err;
        }

        /*
         * Blocks up to MT_CMD_VERDICT_ROWS_MS. The host fetches the rows and
         * answers in that window.
         *
         * The affected-day mask rides along as the +MTCMD line's second tail
         * field, after the row count, because the host cannot otherwise SEE a
         * day it is being asked to empty: there is no row to pull for it.
         * Without it a sketch adjudicating [{Mon, []}, {Tue, [t]}] would be
         * shown one Tuesday row and would have no idea Monday was being
         * deleted.
         */
        bool allow = mt_rows_inbound_forward(mEndpointId, Id, Commands::SetTargets::Id, affected);

        Status out;
        if (!allow) {
            /* Deny or timeout: nothing was touched, the stored schedule is
             * byte-identical, and the controller is told so. */
            out = Status::Failure;
        } else if (n == 0 && affected == 0) {
            /*
             * AN EMPTY TOP-LEVEL LIST IS A NO-OP HERE AND A CLEAR ON THE AT
             * PATH, and that asymmetry is deliberate.
             *
             * The fabric has a separate ClearTargets command, so an empty
             * SetTargets means "I am setting no days", which merges to
             * nothing; CHIP's own reference store behaves identically (its
             * merge loop simply never iterates). The AT surface has NO clear
             * verb, so AT+MTROWAPPLY=<ep>,1,0 had to become one, and
             * evse_targets_apply_locked() treats a rowless, dayless call as
             * "empty the store" for exactly that reason. Which means this
             * branch must NOT call the merge: handing it no rows and no days
             * would wipe the user's whole schedule because a controller sent
             * an empty list.
             *
             * The gate is "no rows AND no days", not "no rows". Those differ
             * precisely for the case above: a non-empty list whose entries
             * carry no targets has n == 0 but names real days, and it must
             * CLEAR those days rather than fall in here and do nothing.
             * "Empty request" and "request to empty" are different commands.
             *
             * n > 0 with affected == 0 is unrepresentable, which is what
             * makes this gate safe: a row only exists for an entry, and
             * mt_rows_stage() refuses a row whose day bitmap is 0.
             */
            HEARTH_LOGI("matter", "ep %u: SetTargets with no schedules, nothing to merge",
                    (unsigned)mEndpointId);
            out = Status::Success;
        } else {
            /* Already on the CHIP task inside a command dispatch, so the
             * stack lock is held and the _locked variant is the right call;
             * going through mt_matter_evse_targets_apply() would take
             * StackLock a second time and deadlock on it. */
            int rc = evse_targets_apply_locked(mEndpointId, stage, affected);
            if (rc == MT_ROW_OK) {
                out = Status::Success;
            } else {
                /* Includes MT_ROW_ERR_PERSIST, "merged but not saved":
                 * Failure, matching ClearTargets() below, so the cluster's
                 * two write commands report a durability failure the same
                 * way. The AT path reports it as +MTERR:7 instead, because it
                 * has a code for exactly that and the fabric does not. */
                HEARTH_LOGE("matter", "ep %u: SetTargets merge failed, rc %d", (unsigned)mEndpointId, rc);
                out = Status::Failure;
            }
        }

        mt_rows_inbound_release();
        return out;
    }

    /*
     * LoadTargets: contract 1. Reads this endpoint's blob out of the port's
     * own KV namespace into the member store. A missing key is success with
     * an empty schedule (a device that has never been given one), which is
     * why the not-found arm does not log.
     */
    chip::Protocols::InteractionModel::Status LoadTargets() override
    {
        using chip::Protocols::InteractionModel::Status;

        clear_store();

        char key[16];
        evse_key_for(mEndpointId, key, sizeof(key));
        size_t len = sizeof(s_evse_blob);
        int    rc  = hearth_kv_get_blob(MT_EVSE_KV_NS, key, s_evse_blob, &len);
        if (rc == 1) {
            m_loaded = true; /* no schedule for this endpoint: that is the truth */
            return Status::Success;
        }
        if (rc != 0) {
            /* m_loaded stays FALSE, and the distinction is the whole point:
             * "there is nothing stored" and "I could not find out what is
             * stored" are different facts, and only the first makes an empty
             * store authoritative. A transient read failure that marked the
             * store loaded would let the next apply merge into nothing and
             * then save that over a schedule which was intact all along,
             * which is silent data loss. */
            HEARTH_LOGE("matter", "ep %u: reading charging targets failed", (unsigned)mEndpointId);
            return Status::Failure;
        }
        if (!decode(len)) {
            /* CORRUPT is not transient: the bytes were read successfully and
             * they are not a schedule. Empty is then the only honest state,
             * re-reading would produce the same bytes for ever, and an apply
             * overwriting them loses nothing that was ever a schedule. So
             * this arm DOES mark the store loaded, unlike the one above. */
            m_loaded = true;
            HEARTH_LOGE("matter", "ep %u: stored charging targets are corrupt (%u bytes), ignored",
                    (unsigned)mEndpointId, (unsigned)len);
            return Status::Failure;
        }
        m_loaded = true;
        /* The apply path's retry and GetTargets()'s lazy retry both land
         * here, which is what covers them (the helper's own comment); the
         * boot load reaches the helper through mt_matter_evse_register()
         * instead, because the variant is not knowable yet at this point. */
        drop_store_if_variant_mismatch();
        HEARTH_LOGI("matter", "ep %u: loaded %u charging target schedule(s)", (unsigned)mEndpointId,
                (unsigned)m_schedule_count);
        return Status::Success;
    }

    /*
     * GetTargets: contract 2. Hands back a view over the member store, which
     * stays alive and unmodified until the encoder has run (the section
     * comment). The lazy LoadTargets() is defence in depth, not the primary
     * satisfaction of contract 1: mt_matter_evse_delegate_alloc() has already
     * loaded at boot, and this guard is here so a future path that reaches a
     * delegate before the rebuild has loaded it cannot encode a view over an
     * uninitialised store.
     *
     * A RETRY THAT STILL CANNOT READ THE STORE FAILS THE READ. m_loaded false
     * means a previous LoadTargets() hit a TRANSIENT failure, so this store
     * is empty only because nobody could find out what is in it (a corrupt
     * blob marks itself loaded, precisely so it does not land here). The
     * natural response to an empty schedule is to send a new one, and the
     * user would be re-authoring a plan that was never lost. An honest
     * failure is retryable; a confident empty list is not recoverable,
     * because nothing about it looks wrong. HandleGetTargets passes a
     * non-Success status straight through as the command status and sends no
     * response, which is exactly the shape wanted.
     */
    chip::Protocols::InteractionModel::Status GetTargets(
        chip::app::DataModel::List<
            const chip::app::Clusters::EnergyEvse::Structs::ChargingTargetScheduleStruct::Type>
            &chargingTargetSchedules) override
    {
        using namespace chip::app::Clusters::EnergyEvse;
        if (!m_loaded) {
            (void)LoadTargets();
            if (!m_loaded) {
                chargingTargetSchedules =
                    chip::app::DataModel::List<const Structs::ChargingTargetScheduleStruct::Type>();
                HEARTH_LOGE("matter", "ep %u: charging targets unreadable, failing GetTargets",
                        (unsigned)mEndpointId);
                return chip::Protocols::InteractionModel::Status::Failure;
            }
        }
        refresh_views();
        chargingTargetSchedules =
            chip::app::DataModel::List<const Structs::ChargingTargetScheduleStruct::Type>(
                m_schedules, m_schedule_count);
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /*
     * ClearTargets: empties the whole store, the same end state an
     * AT+MTROWAPPLY with count 0 reaches from the host side. Not adjudicated:
     * SetTargets is adjudicated because it installs a plan, while a clear
     * only removes one, and the host learns about it the way it learns about
     * any other fabric-side change, by reading back.
     */
    chip::Protocols::InteractionModel::Status ClearTargets() override
    {
        clear_store();
        m_loaded = true;
        if (!save()) {
            return chip::Protocols::InteractionModel::Status::Failure;
        }
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /* ---- the 23 attribute getters --------------------------------------- */

    chip::app::Clusters::EnergyEvse::StateEnum GetState() override { return m_state; }
    chip::app::Clusters::EnergyEvse::SupplyStateEnum GetSupplyState() override
    {
        return m_supply_state;
    }
    chip::app::Clusters::EnergyEvse::FaultStateEnum GetFaultState() override
    {
        return m_fault_state;
    }
    chip::app::DataModel::Nullable<uint32_t> GetChargingEnabledUntil() override
    {
        return m_charging_enabled_until;
    }
    chip::app::DataModel::Nullable<uint32_t> GetDischargingEnabledUntil() override
    {
        return m_discharging_enabled_until;
    }
    int64_t GetCircuitCapacity() override { return m_circuit_capacity; }
    int64_t GetMinimumChargeCurrent() override { return m_min_charge_current; }
    int64_t GetMaximumChargeCurrent() override { return m_max_charge_current; }
    int64_t GetMaximumDischargeCurrent() override { return m_max_discharge_current; }
    int64_t GetUserMaximumChargeCurrent() override { return m_user_max_charge_current; }
    uint32_t GetRandomizationDelayWindow() override { return m_randomization_window; }

    chip::app::DataModel::Nullable<uint32_t> GetNextChargeStartTime() override
    {
        return m_next_charge_start;
    }
    chip::app::DataModel::Nullable<uint32_t> GetNextChargeTargetTime() override
    {
        return m_next_charge_target;
    }
    chip::app::DataModel::Nullable<int64_t> GetNextChargeRequiredEnergy() override
    {
        return m_next_charge_energy;
    }
    chip::app::DataModel::Nullable<chip::Percent> GetNextChargeTargetSoC() override
    {
        return m_next_charge_soc;
    }
    chip::app::DataModel::Nullable<uint16_t> GetApproximateEVEfficiency() override
    {
        return m_approx_efficiency;
    }

    chip::app::DataModel::Nullable<chip::Percent> GetStateOfCharge() override
    {
        return m_state_of_charge;
    }
    chip::app::DataModel::Nullable<int64_t> GetBatteryCapacity() override
    {
        return m_battery_capacity;
    }

    /*
     * VehicleID is PlugAndCharge's, and PNC is never built (the evseAttrs
     * declaration note), so the attribute does not exist and this getter is
     * unreachable. Null rather than a dangling CharSpan: a Nullable<CharSpan>
     * over storage this delegate does not own would be the same non-owning-view
     * mistake contract 2 is about, one attribute down.
     */
    chip::app::DataModel::Nullable<chip::CharSpan> GetVehicleID() override
    {
        return chip::app::DataModel::Nullable<chip::CharSpan>();
    }

    chip::app::DataModel::Nullable<uint32_t> GetSessionID() override { return m_session_id; }
    chip::app::DataModel::Nullable<uint32_t> GetSessionDuration() override
    {
        return m_session_duration;
    }
    chip::app::DataModel::Nullable<int64_t> GetSessionEnergyCharged() override
    {
        return m_session_energy_charged;
    }
    chip::app::DataModel::Nullable<int64_t> GetSessionEnergyDischarged() override
    {
        return m_session_energy_discharged;
    }

    /* ---- the 3 attribute setters ----------------------------------------
     *
     * The fabric's write path: Instance::Write dispatches a controller write
     * straight into them, which is why they validate and report rather than
     * just assigning. They are the only writable attributes the cluster has.
     * The HOST pushes the same three fields through AT+MTMEAS (AT_MT_SPEC.md
     * 3.25), the C6's avoidance of DE270 kept as the contract; this port's
     * AT+MTATTR write has no bare-ERROR gap (the 0x0099 section comment
     * below).
     *
     * Instance::Write additionally gates all three on its OptionalAttributes
     * mask, which this port constructs EMPTY (the register below), matching
     * esp-matter, which creates none of the three. So on this firmware's
     * endpoints the attributes are absent from the metadata and these setters
     * are unreachable from the fabric; the cache is served either way, so the
     * moment the attributes are declared these become live with no further
     * change here.
     */
    CHIP_ERROR SetUserMaximumChargeCurrent(int64_t aNewValue) override
    {
        using namespace chip::app::Clusters::EnergyEvse;
        if (aNewValue < kMinimumChargeCurrentLimit) {
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        if (m_user_max_charge_current != aNewValue) {
            m_user_max_charge_current = aNewValue;
            MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                                   Attributes::UserMaximumChargeCurrent::Id);
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetRandomizationDelayWindow(uint32_t aNewValue) override
    {
        using namespace chip::app::Clusters::EnergyEvse;
        if (aNewValue > kMaxRandomizationDelayWindow) {
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        if (m_randomization_window != aNewValue) {
            m_randomization_window = aNewValue;
            MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                                   Attributes::RandomizationDelayWindow::Id);
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetApproximateEVEfficiency(
        chip::app::DataModel::Nullable<uint16_t> aNewValue) override
    {
        using namespace chip::app::Clusters::EnergyEvse;
        if (m_approx_efficiency != aNewValue) {
            m_approx_efficiency = aNewValue;
            MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                                   Attributes::ApproximateEVEfficiency::Id);
        }
        return CHIP_NO_ERROR;
    }

private:
    uint32_t m_features = 0;
    /* False until mt_matter_evse_register() stamps the mask, which is the one
     * window in which this delegate does not know its own variant: the boot
     * load runs before the endpoint exists. drop_store_if_variant_mismatch()
     * above is a no-op while it is false, and register() calls that helper
     * the moment it flips. */
    bool m_features_known = false;
};

/*
 * The pool, MT_EVSE_MAX (2, core/include/mt_matter.h:1292) delegates plus raw
 * aligned Instance storage, the WHM pool's exact shape. Two, not sixteen,
 * and the target store is the whole reason: each slot carries about 1.8 KB
 * of schedule, so this is the one pool in the port whose DEPTH is a memory
 * decision rather than an inherited C6 constant that happened to be small.
 * A third declared EVSE fails its create with the established loud line and
 * the composition serves the prefix before it, exactly as the measurement
 * pools do.
 *
 * The Instance is constructed in mt_matter_evse_register() with the
 * variant's own mask, where the C6 lets esp-matter's EnergyEvseDelegateInitCB
 * new it from a FeatureMap snapshot at endpoint enable. Never destroyed
 * (~Instance() would Shutdown() cleanly, but this platform has no teardown
 * path, the standing allocate-only policy).
 */
static HearthEvseDelegate *s_evse_delegates[MT_EVSE_MAX];
static size_t             s_evse_next;

/* The EVSE round's pins, the WHM pool's shape (DE608): the pair's arena
 * cost, and the EVSE admission maximum with its draw (the pair, the
 * EnergyEvseMode ModeBase pair, the DEM and DEMMode pairs and the EPM and
 * PowerTopology graft). A pin that fires with another value is a stop,
 * not a re-pin. */
static_assert(hearth_arena_cost(obj_pair_bytes<HearthEvseDelegate,
                                                chip::app::Clusters::EnergyEvse::Instance>()) == 2024,
              "the EVSE pair changed size; redo the EVSE admission arithmetic (DE608)");
static_assert(MT_EVSE_MAX * (2024 + 80 + 280 + 80 + 152 + 40) <= HEARTH_OBJ_ARENA_BYTES,
              "MT_EVSE_MAX energy EVSEs' objects no longer fit the cluster-object arena (DE584)");

/*
 * Reservations claimed by mt_matter_evse_reserve(), the count-only gate
 * mt_devtype_create()'s claim block calls before anything is spent.
 *
 * WHY A SECOND COUNTER RATHER THAN THE PLAIN POOL BOUND, and it is the same
 * hazard on both platforms in two different shapes. On the C6 the delegate
 * handout needs the endpoint id, which only energy_evse::create() can
 * produce, and by then create() has allocated the endpoint, marked it
 * enabled and appended it to the node's own endpoint list: a pool-exhausted
 * third EVSE left a LIVE, delegate-less EnergyEvse endpoint behind, visible
 * to a controller and invisible to AT+MTEP?. This port's create path has the
 * same shape and the same hazard: mt_matter_evse_register() (the second half)
 * is what constructs the Instance, and it must run BELOW a successful
 * emberAfSetDynamicEndpoint(), so it cannot be the capacity gate either.
 *
 * So the gate is SPLIT. reserve() takes the capacity decision before
 * anything is built; the alloc CONSUMES that reservation. The consumption
 * check (s_evse_next >= s_evse_reserved) is what makes the split safe: alloc
 * can never hand out a slot that was not reserved first, so the two counters
 * cannot drift into a state where the reservation said no and the alloc said
 * yes.
 *
 * Monotonic with no release, boot-scoped, the s_meter_reserved reasoning
 * verbatim: ANY create failure aborts the WHOLE composition rebuild for the
 * rest of this boot (main.cpp's rebuild loop breaks), so no later attempt in
 * the same boot can be blocked by a reservation an aborted endpoint never
 * consumed. A reboot reinitialises both counters with every other static
 * here.
 */
static size_t s_evse_reserved;

/*
 * The by-endpoint lookup every bridge below starts from, the whm_for() shape,
 * WITHOUT that pool's kInvalidEndpointId sentinel: this alloc stamps the real
 * id immediately, because LoadTargets() needs to know which endpoint's blob to
 * read (the alloc's own comment). So a delegate whose second half never ran IS
 * matchable here, and what makes that safe is one level up rather than in this
 * function: there are THREE callers, and each proves the endpoint exists and
 * carries the cluster before this lookup runs. evse_locate() does it for the
 * row bridges, mt_matter_meas_set() for the 0x0099 push branch, and
 * attr_locate() for mt_evse_attr_read_live() on the AT+MTATTR carve-out. A
 * create that failed after the alloc never produced an endpoint, so none of
 * the three can reach a stranded delegate. The rebuild also stops at the first
 * failure, so there is no later create to re-use the id and alias onto the
 * stranded delegate, which is the specific hazard the DEM and WHM pools'
 * sentinel exists to prevent and this pool does not have.
 */
static HearthEvseDelegate *evse_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_evse_next; i++) {
        if (s_evse_delegates[i]->endpoint() == ep) {
            return s_evse_delegates[i];
        }
    }
    return nullptr;
}

extern "C" bool mt_matter_evse_reserve(void)
{
    if (s_evse_reserved >= MT_EVSE_MAX) {
        return false;
    }
    /*
     * Commit the INBOUND row staging buffer here too (ruling DE419,
     * mt_at.h's mt_rows_inbound_commit()). This endpoint's SetTargets is the
     * only fabric-originated command in this firmware that carries rows, so
     * this reserve is the exact moment the buffer becomes reachable; a
     * composition with no EVSE never gets here and never pays for it. That
     * is the "committed per composition" half of DE419, and this port is the
     * first to reach it: until this device type landed, mt_rows_inbound_claim()
     * refused every claim here because nothing ever committed.
     *
     * It belongs in the RESERVE rather than in the delegate alloc for the
     * same reason the delegate count does: this runs before anything is
     * built, so a failure builds nothing at all. Idempotent, so a second EVSE
     * costs nothing more, and the reservation counter is not consumed when it
     * fails.
     */
    if (!mt_rows_inbound_commit()) {
        HEARTH_LOGE("matter", "inbound row stage could not be committed (%u B, a slot of the two-slot staging pool); "
                "an EVSE cannot adjudicate SetTargets without it",
                (unsigned)sizeof(mt_row_stage_t));
        return false;
    }
    s_evse_reserved++;
    return true;
}

/*
 * The handout's first half. Unlike the DEM and WHM pools this one DOES use
 * the endpoint id the header's alloc(ep) contract passes, and the reason is
 * the load: LoadTargets() needs to know which endpoint's blob to read, and
 * the load has to happen before the endpoint can serve GetTargets (contract
 * 1). The fix round M1 hazard the other two pools avoid by discarding the id
 * does not arise here, because the id stamped now is overwritten by the
 * Instance constructor in the success-only second half with the same value,
 * and a claim stranded by a later failure cannot alias a LATER create's id:
 * the rebuild stops at the first failure, so there is no later create in
 * this boot.
 */
extern "C" void *mt_matter_evse_delegate_alloc(uint16_t ep)
{
    /*
     * Consume a reservation mt_matter_evse_reserve() made before anything was
     * spent (the s_evse_reserved comment). This, not the MT_EVSE_MAX bound
     * below, is the live capacity check: reserved can never exceed
     * MT_EVSE_MAX, so a handout with no outstanding reservation is either a
     * caller that skipped reserve() or a pool that is genuinely full, and
     * both must fail here.
     */
    if (s_evse_next >= s_evse_reserved) {
        return nullptr;
    }
    /* Defensive backstop only, unreachable given the check above, kept for
     * the reason mt_meter_register_all()'s slot bound is: cheap, and worth
     * labelling as defensive rather than leaving a reader to wonder whether
     * it is still load-bearing. */
    if (s_evse_next >= MT_EVSE_MAX) {
        return nullptr;
    }
    HearthEvseDelegate *d =
        obj_pair_new<HearthEvseDelegate, chip::app::Clusters::EnergyEvse::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->SetEndpointId(ep);
    s_evse_delegates[s_evse_next++] = d;

    /*
     * Contract 1, satisfied here: LoadTargets() before anything can call
     * GetTargets(). This runs from the boot composition rebuild, inside
     * mt_devtype_create()'s stack lock, so the KV is reachable
     * (nvm3_ensure_init() is lazy inside hearth_kv_*, hearth_port_sl.c) and
     * no fabric
     * command can be in flight. A load failure is logged by LoadTargets()
     * itself and leaves an empty schedule; it must NOT abort the endpoint,
     * because a corrupt schedule is not a reason to refuse to be an EVSE.
     */
    (void)d->LoadTargets();
    return d;
}

/*
 * The EVSE second half: construct the Instance with the variant's feature
 * mask and Init() it (CHI registration then AAI, SOFT on both,
 * energy-evse-server.cpp:40-46: no emberAfContainsServer check anywhere, so
 * an ordering mistake here cannot panic, unlike the ModeBase halves; below
 * the successful create so the registration serves a live endpoint).
 *
 * THE FIVE-ARGUMENT CONSTRUCTOR, and what each argument is doing:
 *
 *   Feature: kChargingPreferences always (PREF is unconditional on both
 *     platforms, the evseAttrs note), plus kSoCReporting on variant 0. NOT
 *     kV2x and NOT kPlugAndCharge, ever: V2X would put EnableDischarging in
 *     the advertised AcceptedCommandList (RetrieveAcceptedCommands gates it
 *     on exactly that bit) with no attribute behind it, and PNC would
 *     advertise a VehicleID this firmware has no host story for.
 *   OptionalAttributes: EMPTY. The three optional writables
 *     (UserMaximumChargeCurrent, RandomizationDelayWindow,
 *     ApproximateEVEfficiency) are not declared on this endpoint, esp-matter
 *     creates none of them either, and Instance::Write refuses each one on
 *     this same mask, so an empty mask and an undeclared attribute agree.
 *   OptionalCommands: EMPTY, which is what keeps StartDiagnostics out of the
 *     advertised list (the delegate's own note).
 *
 * with_soc is READ FROM THE DECLARED CLUSTER LIST by the create path
 * (type_has_attr for StateOfCharge), not from the variant, and it is recorded
 * on the delegate for the 0x99 push branch's existence gates, the merge's
 * SOC-variant rule and drop_store_if_variant_mismatch(). The FeatureMap seed
 * in mt_devtypes_sl_seeds.inc is NOT the shared source, and this comment used
 * to say it was: the seed is a variant-keyed table row, so it is the derived
 * copy, and it is an inert shadow (the Instance serves FeatureMap from
 * mFeature), which is what would make it the one to go wrong quietly.
 */
extern "C" void mt_matter_evse_register(void *delegate, uint16_t ep, bool with_soc)
{
    using namespace chip::app::Clusters::EnergyEvse;
    auto    *d    = static_cast<HearthEvseDelegate *>(delegate);
    uint32_t mask = chip::to_underlying(Feature::kChargingPreferences);
    if (with_soc) {
        mask |= chip::to_underlying(Feature::kSoCReporting);
    }
    d->set_features(mask);
    /* The boot load ran before this endpoint existed, so the delegate could
     * not check its stored schedule against its own variant until now. The
     * helper is a no-op until set_features() above flips m_features_known,
     * which is exactly why it is called here and once; the other two load
     * sites reach it from inside LoadTargets(). Its own comment carries the
     * three-loads argument and why the discard is RAM-only. */
    d->drop_store_if_variant_mismatch();

    auto *inst = new (obj_inst_storage<HearthEvseDelegate, Instance>(d))
        Instance(ep, *d, static_cast<Feature>(mask), static_cast<OptionalAttributes>(0),
                 static_cast<OptionalCommands>(0));
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE("matter", "EnergyEvse Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; the cluster will serve nothing and its commands will not dispatch",
                (unsigned)ep, err.Format());
    }
}

/*
 * ---- the targets store, host side -----------------------------------------
 *
 * Locating an endpoint's store answers the two data-model error codes the AT
 * layer needs: an endpoint that is not in the composition at all
 * (MT_ROW_ERR_ENDPOINT, +MTERR:2) against one that exists but carries no
 * EnergyEvse cluster (MT_ROW_ERR_NO_PAYLOAD, +MTERR:4).
 */
static int evse_locate(uint16_t ep, HearthEvseDelegate **out)
{
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ROW_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::EnergyEvse::Id)) {
        return MT_ROW_ERR_NO_PAYLOAD;
    }
    HearthEvseDelegate *d = evse_for(ep);
    if (d == nullptr) {
        /* Cluster without a pool slot: unreachable after a successful boot
         * rebuild, since the create path allocates one per EnergyEvse
         * endpoint and aborts the whole composition when the pool is
         * exhausted. */
        return MT_ROW_ERR_NO_PAYLOAD;
    }
    *out = d;
    return MT_ROW_OK;
}

/*
 * The merge, and the one function BOTH row paths commit through: the host's
 * AT+MTROWAPPLY (via mt_matter_evse_targets_apply(), which takes the stack
 * lock) and the fabric's adjudicated SetTargets (which is already on the CHIP
 * task holding it, and calls this directly). Two callers, one merge, so the
 * two paths cannot end up storing different things from the same rows.
 *
 * clear_days is the set of days the caller is asking to REPLACE beyond the
 * ones its rows name. The AT path passes 0 (its rows are the whole request);
 * the fabric passes the full affected mask from the schedule entries,
 * row-bearing days included, which is why the fold happens after the grouping
 * loop rather than inside it.
 */
static int evse_targets_apply_locked(uint16_t ep, const mt_row_stage_t *stage, uint8_t clear_days)
{
    using namespace chip::app::Clusters::EnergyEvse;

    HearthEvseDelegate *d  = nullptr;
    int                 rc = evse_locate(ep, &d);
    if (rc != MT_ROW_OK) {
        return rc;
    }

    /*
     * The stage belongs to this (ep, kind) only when all three agree; when it
     * does not, the call carries ZERO rows. Do not read stage->row[] outside
     * that match: the pointer is one of the TWO staging sessions mt_at.c owns
     * (the AT parser task's s_row_stage, or the CHIP task's s_row_inbound,
     * which the fabric path passes), and either may hold a set for a
     * different endpoint entirely. Since ruling DE419 neither is file-static
     * storage: the host's is allocated per staging session and the inbound
     * one is committed per composition, so stage may also be NULL, which
     * mt_matter.h reads as the same instruction as a mismatch.
     */
    bool match = (stage != nullptr && stage->active && stage->ep == ep &&
                  stage->kind == MT_ROW_KIND_EVSE_TARGET && stage->count > 0);

    /*
     * No rows AND no days named: the documented clear-EVERYTHING request
     * (mt_matter.h's contract). This is the AT path's clear verb, and only
     * the AT path reaches it: the fabric passes clear_days from the schedule
     * entries it was actually sent, and its own "empty list" case never calls
     * here at all (SetTargets above documents why an empty SetTargets is a
     * no-op where an AT+MTROWAPPLY count of 0 is a clear).
     */
    if (!match && clear_days == 0) {
        d->clear_store();
        d->m_loaded = true; /* empty is now the truth, whatever a failed
                             * earlier load left behind */
        if (!d->save()) {
            return MT_ROW_ERR_PERSIST;
        }
        return MT_ROW_OK;
    }

    /*
     * Rows to merge, or none when only clear_days brought us here. Every loop
     * below is bounded by this rather than by stage->count, so the "day named
     * with no targets" case walks the stage zero times and is still a merge,
     * not a wholesale clear.
     */
    uint16_t nrows = match ? stage->count : 0;

    /*
     * A MERGE needs the stored schedule to be known, and m_loaded false means
     * a previous load hit a transient KV failure and this store is empty only
     * because nobody could read it (LoadTargets()). Retry once, and refuse if
     * it still cannot be read: merging into an assumed-empty store and saving
     * the result would overwrite a schedule that may be perfectly intact. The
     * clear path above is deliberately exempt, because there the host is
     * asking for the stored payload to go away regardless of what it says.
     */
    if (!d->m_loaded) {
        (void)d->LoadTargets();
        if (!d->m_loaded) {
            return MT_ROW_ERR_PERSIST;
        }
    }

    /*
     * Pass 1, PURELY READ-ONLY over the stage: group the incoming rows by day
     * bitmap in first-appearance order and check the two capacities the row
     * codec cannot see, because mt_rows.c knows nothing about schedules: at
     * most kEvseMaxDays distinct bitmaps, and at most kEvseMaxPerDay targets
     * sharing one. mt_rows_validate() has already proven the set dense, in
     * range, and free of a day bit appearing in two different bitmaps.
     *
     * No target storage is built here. 7 x 10 ChargingTargetStructs is about
     * 1.7 KB and the AT parser task has a 6144-byte stack, the same
     * arithmetic that made mt_at.c pass the stage by pointer in the first
     * place; the merge below writes straight into the member store instead.
     */
    uint8_t bitmaps[kEvseMaxDays];
    uint8_t counts[kEvseMaxDays];
    uint8_t groups   = 0;
    uint8_t all_bits = 0;

    /*
     * The SOC-variant rule, the one rule in this pass about a single target
     * rather than the shape of the set. Instance::ValidateTargets()
     * (energy-evse-server.cpp) enforces it on the FABRIC path: with the SOC
     * feature every target MUST carry targetSoC (InvalidCommand otherwise),
     * and without it targetSoC must be absent or exactly 100 (ConstraintError
     * otherwise). Nothing enforced it on the AT path, so a host could install
     * exactly the schedule a controller's SetTargets would have been refused,
     * and GetTargets would then hand that schedule back to the controller.
     *
     * Read from the delegate's stamped feature record, the same honest source
     * the push branch's gates use: it comes from the identical declared-list
     * predicate the Instance's own mFeature snapshot does, so this check and
     * ValidateTargets() cannot disagree. (The C6 asks its endpoint metadata
     * instead, because esp-matter's FeatureMap copy is authoritative there
     * and this port's is a seed shadow; the two questions are the same one.)
     *
     * THE SPEC SAYS THIS NOW, and it did not when the check was written.
     * AT_MT_SPEC.md 3.28's kind-1 rules carried only the choice-of-one rule
     * ("at least one of <soc> and <energy>"), so a host staging a row that
     * was legal by 3.28 and illegal by this one met an unexplained +MTERR:1.
     * The sentence was missing, not the check: 3.28 now carries a paragraph
     * of its own for the variant rule, naming the same +MTERR:1, the same
     * all-or-nothing timing, and AT+MTEP?'s fourth field as where a host
     * reads the variant back. The rule was never optional, because without it
     * a host could install over AT exactly the schedule a controller's
     * SetTargets would have been refused and GetTargets would hand that
     * schedule back to the controller; what was missing was saying so.
     */
    bool soc_feature = d->has_feature(Feature::kSoCReporting);

    for (uint16_t r = 0; r < nrows; r++) {
        const mt_row_t *row = &stage->row[r];
        if (row->nfields != kRowFields || !row->present[kRowDay] || !row->present[kRowTime]) {
            /* Defensive: mt_rows_stage() canonicalises nfields and refuses a
             * row missing a mandatory field, so this cannot fire from either
             * writer. */
            return MT_ROW_ERR_VALUE;
        }
        if (soc_feature) {
            if (!row->present[kRowSoC]) {
                return MT_ROW_ERR_VALUE; /* SOC endpoint: SoC is mandatory */
            }
        } else if (row->present[kRowSoC] && row->value[kRowSoC] != 100) {
            /* No SOC feature: the only SoC a target may state is "full",
             * which is the XML's way of saying "charge it completely" on a
             * device that cannot report state of charge. */
            return MT_ROW_ERR_VALUE;
        }
        uint8_t bits = (uint8_t)row->value[kRowDay];
        if (bits == 0 || (bits & (uint8_t)~kEvseAllDaysMask) != 0) {
            return MT_ROW_ERR_VALUE;
        }
        uint8_t g = 0;
        for (; g < groups; g++) {
            if (bitmaps[g] == bits) {
                break;
            }
        }
        if (g == groups) {
            if (groups >= kEvseMaxDays) {
                return MT_ROW_ERR_VALUE; /* more than 7 distinct day groups */
            }
            if ((bits & all_bits) != 0) {
                /* This day already belongs to a DIFFERENT bitmap in the same
                 * set (an identical bitmap would have matched a group above),
                 * which CHIP answers ConstraintError for. Checked again here
                 * rather than trusting mt_rows_validate(): this function is
                 * the last gate before the store, and it serves the fabric
                 * path too. */
                return MT_ROW_ERR_VALUE;
            }
            bitmaps[groups] = bits;
            counts[groups]  = 0;
            groups++;
        }
        if (counts[g] >= kEvseMaxPerDay) {
            return MT_ROW_ERR_VALUE; /* more than 10 targets for one day */
        }
        counts[g]++;
        all_bits = (uint8_t)(all_bits | bits);
    }

    /*
     * Fold in the days the caller named that carry no rows, and do it HERE,
     * after the loop, never inside it: the loop's "this day already belongs
     * to a different bitmap" check is about the ROWS, and the fabric passes
     * the full affected mask (row-bearing days included), so folding early
     * would make every fabric merge reject itself on its own days.
     *
     * These are days a controller asked to EMPTY: a schedule entry with a
     * dayOfWeekForSequence and an empty chargingTargets list, which
     * ValidateTargets() accepts and CHIP's own reference store clears in both
     * of its arms. Deriving the mask from the flattened ROWS instead would
     * drop such a day silently: the user deletes Monday's targets, the device
     * answers SUCCESS, GetTargets keeps returning them, and the car charges on
     * a schedule that was deleted.
     *
     * Masked to the seven real days so a caller cannot subtract a bit that is
     * not a day.
     */
    all_bits = (uint8_t)(all_bits | (clear_days & kEvseAllDaysMask));

    /*
     * Last read-only check: how many stored schedules survive the
     * subtraction, and does the merged result still fit. Provably it always
     * does (every surviving schedule and every appended group owns at least
     * one of the seven days, and they are pairwise disjoint), but the check
     * is made HERE, before anything is mutated, because the alternative to a
     * provable invariant is not an assertion halfway through a merge: that
     * would leave the store half-rewritten with no way back.
     */
    {
        uint8_t survivors = 0;
        for (uint8_t i = 0; i < d->m_schedule_count; i++) {
            if ((d->day_bits(i) & (uint8_t)~all_bits) != 0) {
                survivors++;
            }
        }
        if ((uint16_t)survivors + groups > kEvseMaxDays) {
            return MT_ROW_ERR_VALUE;
        }
    }

    /*
     * Pass 2, the mutation, which can no longer fail. Subtract every day the
     * incoming set mentions from what is stored (dropping a stored schedule
     * that loses all of its days), then append the incoming groups. Days the
     * set never mentioned are untouched, which is the whole point. A day in
     * all_bits with no group of its own is subtracted and never re-appended,
     * which is exactly how an emptied day ends up empty.
     */
    d->subtract_days(all_bits);

    uint8_t first_new = d->m_schedule_count;
    for (uint8_t g = 0; g < groups; g++) {
        uint8_t slot = (uint8_t)(first_new + g);
        d->set_day_bits(slot, bitmaps[g]);
        d->m_target_count[slot] = 0;
    }
    d->m_schedule_count = (uint8_t)(first_new + groups);

    for (uint16_t r = 0; r < nrows; r++) {
        const mt_row_t *row  = &stage->row[r];
        uint8_t         bits = (uint8_t)row->value[kRowDay];
        uint8_t         slot = first_new;
        for (uint8_t g = 0; g < groups; g++) {
            if (bitmaps[g] == bits) {
                slot = (uint8_t)(first_new + g);
                break;
            }
        }
        Structs::ChargingTargetStruct::Type t;
        t.targetTimeMinutesPastMidnight = (uint16_t)row->value[kRowTime];
        if (row->present[kRowSoC]) {
            t.targetSoC.SetValue((chip::Percent)row->value[kRowSoC]);
        }
        if (row->present[kRowEnergy]) {
            t.addedEnergy.SetValue(row->value[kRowEnergy]);
        }
        d->m_targets[slot][d->m_target_count[slot]] = t;
        d->m_target_count[slot]++;
    }
    d->refresh_views();

    /*
     * Persist. A failure here is reported rather than rolled back: the live
     * data model already carries the new schedule and a controller reading
     * GetTargets would see it, so the honest report is "applied but not
     * durable", which is what MT_ROW_ERR_PERSIST (+MTERR:7) says.
     */
    if (!d->save()) {
        return MT_ROW_ERR_PERSIST;
    }
    return MT_ROW_OK;
}

extern "C" int mt_matter_evse_targets_apply(uint16_t ep, const mt_row_stage_t *stage)
{
    chip::DeviceLayer::StackLock lock;
    /* clear_days 0: the host's rows ARE the whole request, so the only days
     * this merge replaces are the ones its rows name, and a stage that does
     * not match becomes the documented clear. */
    return evse_targets_apply_locked(ep, stage, 0);
}

extern "C" int mt_matter_evse_targets_total(uint16_t ep, uint16_t *total)
{
    chip::DeviceLayer::StackLock lock;
    if (total != nullptr) {
        *total = 0;
    }
    HearthEvseDelegate *d  = nullptr;
    int                 rc = evse_locate(ep, &d);
    if (rc != MT_ROW_OK) {
        return rc;
    }
    if (total != nullptr) {
        *total = d->row_total();
    }
    return MT_ROW_OK;
}

extern "C" int mt_matter_evse_targets_get(uint16_t ep, uint16_t idx, mt_row_t *out,
                                          uint16_t *total)
{
    using namespace chip::app::Clusters::EnergyEvse;
    chip::DeviceLayer::StackLock lock;

    if (total != nullptr) {
        *total = 0;
    }
    HearthEvseDelegate *d  = nullptr;
    int                 rc = evse_locate(ep, &d);
    if (rc != MT_ROW_OK) {
        return rc;
    }
    uint16_t n = d->row_total();
    if (total != nullptr) {
        *total = n;
    }
    if (out == nullptr) {
        return MT_ROW_OK;
    }
    if (idx >= n) {
        /* The caller checks idx against mt_matter_rows_total() first
         * (mt_matter.h), so this is defence against a caller that did not.
         * Value, not a data-model code: the endpoint and its payload both
         * exist, the index does not. */
        return MT_ROW_ERR_VALUE;
    }

    /* Walk the flattened (schedule, target) sequence to idx, the same
     * flattening SetTargets uses in the other direction. */
    uint16_t seen = 0;
    for (uint8_t i = 0; i < d->m_schedule_count; i++) {
        if (idx - seen >= d->m_target_count[i]) {
            seen = (uint16_t)(seen + d->m_target_count[i]);
            continue;
        }
        const Structs::ChargingTargetStruct::Type &t = d->m_targets[i][idx - seen];
        memset(out, 0, sizeof(*out));
        out->nfields            = kRowFields;
        out->present[kRowDay]   = true;
        out->value[kRowDay]     = d->day_bits(i);
        out->present[kRowTime]  = true;
        out->value[kRowTime]    = t.targetTimeMinutesPastMidnight;
        if (t.targetSoC.HasValue()) {
            out->present[kRowSoC] = true;
            out->value[kRowSoC]   = t.targetSoC.Value();
        }
        if (t.addedEnergy.HasValue()) {
            out->present[kRowEnergy] = true;
            out->value[kRowEnergy]   = t.addedEnergy.Value();
        }
        return MT_ROW_OK;
    }
    return MT_ROW_ERR_VALUE; /* unreachable: idx < n was checked above */
}

/*
 * AT+MTFRESET only. The schedule is USER DATA, so a factory reset is the one
 * thing that removes it (mt_matter.h's contract and the section comment's
 * asymmetry note).
 *
 * RAM first, so a failing KV erase still leaves the running device with no
 * schedule rather than one it cannot forget; AT+MTFRESET reboots immediately
 * afterwards either way.
 *
 * The KV sweep walks every endpoint id THIS PORT CAN EVER ASSIGN (1 through
 * kServiceableEndpoints, mt_devtype_create()'s counter) rather than only the
 * live delegates, because the residue this has to clear is exactly the key
 * left behind when a composition changed and a former EVSE endpoint id became
 * something else. The C6 erases its whole NVS namespace in one call; this
 * platform's KV port offers per-key delete only, and the id space is small
 * and bounded, so the sweep is the equivalent. A not-found key (return 1) is
 * not a failure.
 */
extern "C" int mt_matter_evse_targets_erase_all(void)
{
    chip::DeviceLayer::StackLock lock;

    for (size_t i = 0; i < s_evse_next; i++) {
        s_evse_delegates[i]->clear_store();
        s_evse_delegates[i]->m_loaded = true;
    }

    int rc = 0;
    for (uint16_t ep = 1; ep <= kServiceableEndpoints; ep++) {
        char key[16];
        evse_key_for(ep, key, sizeof(key));
        if (hearth_kv_delete(MT_EVSE_KV_NS, key) < 0) {
            HEARTH_LOGE("matter", "erasing charging targets for endpoint %u failed", (unsigned)ep);
            rc = -1;
        }
    }
    if (rc == 0) {
        HEARTH_LOGI("matter", "charging targets erased");
    }
    return rc;
}

/*
 * ---- the mt_matter_rows_* dispatchers -------------------------------------
 *
 * The per-kind switch mt_matter.h describes, and the reason it takes NO lock
 * of its own: kind 1 forwards to mt_matter_evse_targets_*() above, each of
 * which takes StackLock, and StackLock wraps a plain LockChipStack()/Unlock()
 * pair rather than a recursive mutex, so taking it here as well would
 * DEADLOCK against the callee rather than merely be redundant. A future row
 * kind whose handler calls into CHIP directly from inside one of these
 * switches, instead of forwarding to its own locking bridge, must take the
 * lock at that call site. Do not "fix" these by wrapping the switch.
 *
 * An unknown kind answers MT_ROW_ERR_KIND (+MTERR:3), which mt_at.c's own
 * codec check makes unreachable from the AT path; it is the drift alarm for
 * a kind added to mt_rows.c and not here.
 */
extern "C" int mt_matter_rows_apply(uint16_t ep, uint8_t kind, const mt_row_stage_t *stage)
{
    switch (kind) {
    case MT_ROW_KIND_EVSE_TARGET:
        return mt_matter_evse_targets_apply(ep, stage);
    default:
        return MT_ROW_ERR_KIND;
    }
}

extern "C" int mt_matter_rows_get(uint16_t ep, uint8_t kind, uint16_t idx, mt_row_t *out,
                                  uint16_t *total)
{
    switch (kind) {
    case MT_ROW_KIND_EVSE_TARGET:
        return mt_matter_evse_targets_get(ep, idx, out, total);
    default:
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        if (total) {
            *total = 0;
        }
        return MT_ROW_ERR_KIND;
    }
}

extern "C" int mt_matter_rows_total(uint16_t ep, uint8_t kind, uint16_t *total)
{
    switch (kind) {
    case MT_ROW_KIND_EVSE_TARGET:
        return mt_matter_evse_targets_total(ep, total);
    default:
        if (total) {
            *total = 0;
        }
        return MT_ROW_ERR_KIND;
    }
}

/*
 * ---- AT+MTMEAS cluster 0x0099 ---------------------------------------------
 *
 * The EVSE's whole attribute surface is delegate-served, so this is the only
 * way the host can set any of it (AT_MT_SPEC.md 3.25's 0x0099 rows).
 * Nineteen field ids, of which fourteen are always present, two are SOC-only
 * and three are never built: UserMaximumChargeCurrent (7),
 * RandomizationDelayWindow (8) and ApproximateEVEfficiency (13) are optional
 * attributes esp-matter never creates and this port therefore never declares
 * either, so they answer MT_ATTR_ERR_ATTRIBUTE (+MTERR:4) on every variant.
 *
 * WHY AT+MTMEAS AND NOT AT+MTATTR for those same three, which are also the
 * cluster's only WRITABLE attributes: AT_MT_SPEC.md 3.25 routes every
 * 0x0099 field through AT+MTMEAS, and on the C6 that routing avoided
 * DE270's open half (esp-matter's WriteAttribute detour collapses a
 * writable managed-internally attribute's failure to a bare ERROR). That
 * gap does not exist here: mt_matter_attr_write()'s carve-out arm answers
 * an Instance-served attribute with +MTERR:11 or, for the Chime pair (this
 * image's writable Instance-served attributes), its server's own setters.
 * And the three are never declared on this port, so AT+MTATTR answers
 * +MTERR:4 for them from attr_locate() before either arm is reached.
 *
 * Existence gate: this port asks the endpoint's OWN declared metadata
 * (emberAfLocateAttributeMetadata), not a feature-bit table, for the same
 * reason the C6 asks esp-matter's. Three separate conditions converge on one
 * check: StateOfCharge and BatteryCapacity exist only on variant 0; the three
 * optionals exist nowhere; and the four NextCharge* attributes exist only
 * because charging preferences is unconditional. Asking the metadata answers
 * all three without a second table to keep in sync, and it answers what a
 * controller would actually see.
 *
 * Called only from mt_matter_meas_set(), which already holds the StackLock
 * and resolved the endpoint and cluster lookups. Two passes, the family's
 * hard contract: every pair is validated before any pair is applied.
 */
static int mt_meas_evse_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                              uint8_t count)
{
    using namespace chip::app::Clusters::EnergyEvse;

    HearthEvseDelegate *d = evse_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run; defensive, the pool bridges'
         * standing answer. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Pass 1: field known, attribute present on this endpoint, value in the
     * XML's range for it. */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t attr_id;
        int64_t  lo;
        int64_t  hi;
        switch (fields[i]) {
        case MT_EVSE_F_STATE:
            attr_id = Attributes::State::Id;
            lo = 0; hi = (int64_t)StateEnum::kUnknownEnumValue - 1;
            break;
        case MT_EVSE_F_SUPPLY_STATE:
            attr_id = Attributes::SupplyState::Id;
            lo = 0; hi = (int64_t)SupplyStateEnum::kUnknownEnumValue - 1;
            break;
        case MT_EVSE_F_FAULT_STATE:
            attr_id = Attributes::FaultState::Id;
            lo = 0; hi = (int64_t)FaultStateEnum::kUnknownEnumValue - 1;
            break;
        case MT_EVSE_F_CHARGING_ENABLED_UNTIL:
            attr_id = Attributes::ChargingEnabledUntil::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_CIRCUIT_CAPACITY:
            attr_id = Attributes::CircuitCapacity::Id;
            lo = kMinimumChargeCurrentLimit; hi = kMeasValueAbsMax;
            break;
        case MT_EVSE_F_MIN_CHARGE_CURRENT:
            attr_id = Attributes::MinimumChargeCurrent::Id;
            lo = kMinimumChargeCurrentLimit; hi = kMeasValueAbsMax;
            break;
        case MT_EVSE_F_MAX_CHARGE_CURRENT:
            attr_id = Attributes::MaximumChargeCurrent::Id;
            lo = kMinimumChargeCurrentLimit; hi = kMeasValueAbsMax;
            break;
        case MT_EVSE_F_USER_MAX_CHARGE_CURRENT:
            attr_id = Attributes::UserMaximumChargeCurrent::Id;
            lo = kMinimumChargeCurrentLimit; hi = kMeasValueAbsMax;
            break;
        case MT_EVSE_F_RANDOMIZATION_WINDOW:
            attr_id = Attributes::RandomizationDelayWindow::Id;
            lo = 0; hi = kMaxRandomizationDelayWindow;
            break;
        case MT_EVSE_F_NEXT_CHARGE_START:
            attr_id = Attributes::NextChargeStartTime::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_NEXT_CHARGE_TARGET:
            attr_id = Attributes::NextChargeTargetTime::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_NEXT_CHARGE_ENERGY:
            attr_id = Attributes::NextChargeRequiredEnergy::Id;
            lo = 0; hi = kMeasValueAbsMax;
            break;
        case MT_EVSE_F_NEXT_CHARGE_SOC:
            attr_id = Attributes::NextChargeTargetSoC::Id;
            lo = 0; hi = 100;
            break;
        case MT_EVSE_F_APPROX_EFFICIENCY:
            attr_id = Attributes::ApproximateEVEfficiency::Id;
            lo = 0; hi = UINT16_MAX;
            break;
        case MT_EVSE_F_STATE_OF_CHARGE:
            attr_id = Attributes::StateOfCharge::Id;
            lo = 0; hi = 100;
            break;
        case MT_EVSE_F_BATTERY_CAPACITY:
            attr_id = Attributes::BatteryCapacity::Id;
            lo = 0; hi = kMeasValueAbsMax;
            break;
        case MT_EVSE_F_SESSION_ID:
            attr_id = Attributes::SessionID::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_SESSION_DURATION:
            attr_id = Attributes::SessionDuration::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_SESSION_ENERGY_CHARGED:
            attr_id = Attributes::SessionEnergyCharged::Id;
            lo = 0; hi = kMeasValueAbsMax;
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
        if (emberAfLocateAttributeMetadata(ep, Id, attr_id) == nullptr) {
            /* The attribute is not on THIS endpoint: a SOC field on a
             * variant-1 EVSE, or one of the three optionals nothing creates.
             * Distinct from a bad value, which is what makes the host able to
             * tell "wrong variant" from "wrong number". */
            return MT_ATTR_ERR_ATTRIBUTE;
        }
        if (values[i] < lo || values[i] > hi) {
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Pass 2: apply, one subscription report per applied field. */
    for (uint8_t i = 0; i < count; i++) {
        int64_t  v = values[i];
        uint32_t attr_id;
        switch (fields[i]) {
        case MT_EVSE_F_STATE:
            d->m_state = (StateEnum)v;
            attr_id    = Attributes::State::Id;
            break;
        case MT_EVSE_F_SUPPLY_STATE:
            d->m_supply_state = (SupplyStateEnum)v;
            attr_id           = Attributes::SupplyState::Id;
            break;
        case MT_EVSE_F_FAULT_STATE:
            d->m_fault_state = (FaultStateEnum)v;
            attr_id          = Attributes::FaultState::Id;
            break;
        case MT_EVSE_F_CHARGING_ENABLED_UNTIL:
            d->m_charging_enabled_until = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id                     = Attributes::ChargingEnabledUntil::Id;
            break;
        case MT_EVSE_F_CIRCUIT_CAPACITY:
            d->m_circuit_capacity = v;
            attr_id               = Attributes::CircuitCapacity::Id;
            break;
        case MT_EVSE_F_MIN_CHARGE_CURRENT:
            d->m_min_charge_current = v;
            attr_id                 = Attributes::MinimumChargeCurrent::Id;
            break;
        case MT_EVSE_F_MAX_CHARGE_CURRENT:
            d->m_max_charge_current = v;
            attr_id                 = Attributes::MaximumChargeCurrent::Id;
            break;
        case MT_EVSE_F_USER_MAX_CHARGE_CURRENT:
            d->m_user_max_charge_current = v;
            attr_id                      = Attributes::UserMaximumChargeCurrent::Id;
            break;
        case MT_EVSE_F_RANDOMIZATION_WINDOW:
            d->m_randomization_window = (uint32_t)v;
            attr_id                   = Attributes::RandomizationDelayWindow::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_START:
            d->m_next_charge_start = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id                = Attributes::NextChargeStartTime::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_TARGET:
            d->m_next_charge_target = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id                 = Attributes::NextChargeTargetTime::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_ENERGY:
            d->m_next_charge_energy = chip::app::DataModel::MakeNullable(v);
            attr_id                 = Attributes::NextChargeRequiredEnergy::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_SOC:
            d->m_next_charge_soc = chip::app::DataModel::MakeNullable((chip::Percent)v);
            attr_id              = Attributes::NextChargeTargetSoC::Id;
            break;
        case MT_EVSE_F_APPROX_EFFICIENCY:
            d->m_approx_efficiency = chip::app::DataModel::MakeNullable((uint16_t)v);
            attr_id                = Attributes::ApproximateEVEfficiency::Id;
            break;
        case MT_EVSE_F_STATE_OF_CHARGE:
            d->m_state_of_charge = chip::app::DataModel::MakeNullable((chip::Percent)v);
            attr_id              = Attributes::StateOfCharge::Id;
            break;
        case MT_EVSE_F_BATTERY_CAPACITY:
            d->m_battery_capacity = chip::app::DataModel::MakeNullable(v);
            attr_id               = Attributes::BatteryCapacity::Id;
            break;
        case MT_EVSE_F_SESSION_ID:
            d->m_session_id = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id         = Attributes::SessionID::Id;
            break;
        case MT_EVSE_F_SESSION_DURATION:
            d->m_session_duration = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id               = Attributes::SessionDuration::Id;
            break;
        default: /* MT_EVSE_F_SESSION_ENERGY_CHARGED, pass 1 admits nothing else */
            d->m_session_energy_charged = chip::app::DataModel::MakeNullable(v);
            attr_id                     = Attributes::SessionEnergyCharged::Id;
            break;
        }
        MatterReportingAttributeChangeCallback(ep, Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * The single-pair form of the same contract, mt_matter.h's
 * mt_matter_evse_set(). It runs the identical two-pass body, so the two
 * cannot drift; it exists because the header declares it and because a host
 * library with one value to push should not have to build a pair array.
 */
extern "C" int mt_matter_evse_set(uint16_t ep, uint8_t field, int64_t value)
{
    using namespace chip::app::Clusters::EnergyEvse;
    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    return mt_meas_evse_apply(ep, &field, &value, 1);
}

/*
 * DE397 live read for the sixteen EVSE values this port declares (the
 * k_instance_served rows; fields 7, 8 and 13 are never declared),
 * dispatched from
 * mt_matter_attr_read() under its StackLock: the delegate cache is the served
 * truth (the narrow shadows are seeds, and the six 64-bit declarations have
 * no slot at all). A null nullable answers MT_ATTR_ERR_TYPE, the
 * no-null-literal rule the EPM reader already applies, so a host sees
 * +MTERR:5 until the value is first pushed.
 */
static int mt_evse_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    using namespace chip::app::Clusters::EnergyEvse;

    HearthEvseDelegate *d = evse_for(ep);
    if (d == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }

    bool    u    = true;
    bool    null = false;
    int64_t v    = 0;
    switch (attr) {
    case Attributes::State::Id:       v = chip::to_underlying(d->m_state); break;
    case Attributes::SupplyState::Id: v = chip::to_underlying(d->m_supply_state); break;
    case Attributes::FaultState::Id:  v = chip::to_underlying(d->m_fault_state); break;
    case Attributes::ChargingEnabledUntil::Id:
        null = d->m_charging_enabled_until.IsNull();
        if (!null) {
            v = d->m_charging_enabled_until.Value();
        }
        break;
    case Attributes::CircuitCapacity::Id:
        v = d->m_circuit_capacity;
        u = false;
        break;
    case Attributes::MinimumChargeCurrent::Id:
        v = d->m_min_charge_current;
        u = false;
        break;
    case Attributes::MaximumChargeCurrent::Id:
        v = d->m_max_charge_current;
        u = false;
        break;
    case Attributes::NextChargeStartTime::Id:
        null = d->m_next_charge_start.IsNull();
        if (!null) {
            v = d->m_next_charge_start.Value();
        }
        break;
    case Attributes::NextChargeTargetTime::Id:
        null = d->m_next_charge_target.IsNull();
        if (!null) {
            v = d->m_next_charge_target.Value();
        }
        break;
    case Attributes::NextChargeRequiredEnergy::Id:
        u    = false;
        null = d->m_next_charge_energy.IsNull();
        if (!null) {
            v = d->m_next_charge_energy.Value();
        }
        break;
    case Attributes::NextChargeTargetSoC::Id:
        null = d->m_next_charge_soc.IsNull();
        if (!null) {
            v = d->m_next_charge_soc.Value();
        }
        break;
    case Attributes::StateOfCharge::Id:
        null = d->m_state_of_charge.IsNull();
        if (!null) {
            v = d->m_state_of_charge.Value();
        }
        break;
    case Attributes::BatteryCapacity::Id:
        u    = false;
        null = d->m_battery_capacity.IsNull();
        if (!null) {
            v = d->m_battery_capacity.Value();
        }
        break;
    case Attributes::SessionID::Id:
        null = d->m_session_id.IsNull();
        if (!null) {
            v = d->m_session_id.Value();
        }
        break;
    case Attributes::SessionDuration::Id:
        null = d->m_session_duration.IsNull();
        if (!null) {
            v = d->m_session_duration.Value();
        }
        break;
    case Attributes::SessionEnergyCharged::Id:
        u    = false;
        null = d->m_session_energy_charged.IsNull();
        if (!null) {
            v = d->m_session_energy_charged.Value();
        }
        break;
    default:
        /* Unreachable: only the k_instance_served rows route here. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }

    /* Signedness is reported before the null check, so a caller about to
     * WRITE can still fetch it (core/include/mt_matter.h:176-181, binding). */
    if (is_unsigned) {
        *is_unsigned = u;
    }
    if (null) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = v;
    return MT_ATTR_OK;
}
