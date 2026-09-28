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
 *    about 3.4 KB against about 67 KB of free heap at +MTREADY, while moving
 *    it into the endpoint block would take that block from 1,144 B to roughly
 *    2,900 B of the fixed 9,728 B endpoint arena for no gain at a cap of two.
 *    The endpoint-block technique pays when a store is charged per POSSIBLE
 *    endpoint (the mode and chime stores were 16-deep .bss); this one is
 *    already charged per DECLARED endpoint, because the pool allocates from
 *    the cluster-object arena when an EVSE is composed and never otherwise.
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
