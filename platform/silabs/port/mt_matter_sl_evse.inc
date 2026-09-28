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
