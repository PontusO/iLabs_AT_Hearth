/*
 * mt_evse.cpp - the Energy EVSE (0x0099) delegate, its charging-target store,
 * and that store's persistence (energy round C2, task 4).
 *
 * Why this is its own translation unit rather than another block in main.cpp:
 * it is the largest delegate in the project by a wide margin (34 pure
 * virtuals against the DEM delegate's 17) and it owns a real data structure
 * with a persistence format, so it earns a file. The extern "C" bridge
 * functions the AT layer calls stay in main.cpp, because ChipStackLock lives
 * there; mt_evse.h is the seam.
 *
 * ---- THE PURE-VIRTUAL COUNT ----
 *
 * 34, counted from the pinned SDK's own header rather than transcribed from
 * any document: connectedhomeip/src/app/clusters/energy-evse-server/
 * energy-evse-server.h, class Delegate, is 36 lines ending in "= 0;" of which
 * two are not virtuals (`constexpr int64_t kMinimumChargeCurrentLimit = 0;`
 * at :38 and the `EndpointId mEndpointId = 0;` member initialiser at :159).
 * The round's design spec and plan both say 35; they are wrong by one, and
 * the build is the arbiter that settles it: a missing pure virtual makes this
 * class abstract and the pool array below fails to compile. The breakdown is
 * 8 command handlers, 23 attribute getters and 3 attribute setters.
 *
 * ---- THE TWO LIFETIME CONTRACTS, both documented by the SDK and enforced
 * by nothing ----
 *
 * 1. LoadTargets() MUST run before GetTargets(). Nothing in the server calls
 *    it: Instance::Init() (energy-evse-server.cpp:40-46) only registers the
 *    command and attribute interfaces, and HandleGetTargets()
 *    (:468-484) goes straight to the delegate. The obligation is stated twice
 *    in the reference implementation's own header
 *    (examples/energy-management-app/energy-management-common/energy-evse/
 *    include/EnergyEvseDelegateImpl.h:165-176, "MUST be called before
 *    GetTargets is called"). We satisfy it in mt_matter_evse_delegate_alloc()
 *    below, i.e. during the boot composition rebuild, and again lazily in
 *    GetTargets() itself so no future caller can reintroduce the gap.
 *
 * 2. GetTargets() hands back a NON-OWNING VIEW. Its out parameter is a
 *    DataModel::List over storage the delegate owns, and the encode happens
 *    LATER, at ctx.mCommandHandler.AddResponse() (:481). Both the schedule
 *    array AND every per-day target array must therefore still be alive and
 *    unmodified at that point. This delegate holds the entire store as
 *    members (m_schedules / m_targets below, about 1680 bytes) for exactly
 *    that reason. CHIP's own reference refuses a static [7][10] array because
 *    it "is 1680B" and heap-allocates each day separately (ChargingTargetsMemMgr);
 *    we do not copy that, because 1680 bytes against ~106 KB free heap is not
 *    the constraint it is for a general-purpose reference, and a fixed member
 *    array deletes the whole class of allocation-failure and lifetime bugs the
 *    reference has to manage by hand. Nothing here ever builds a schedule on
 *    the stack and returns a view over it: that is a use-after-free on the
 *    SUCCESS path, invisible to every test and visible on the bench only as
 *    random corruption.
 *
 * The mutating side of contract 2 is the ChipStackLock. Instance::HandleGetTargets
 * calls GetTargets() and AddResponse() back to back on the CHIP event loop
 * task, which holds the stack lock throughout; every mutation of this store
 * from the AT parser task goes through a bridge function in main.cpp that
 * takes the same lock, so no host apply can land between the view being
 * handed out and the encoder reading it. The OTHER mutator, task 6's
 * adjudicated SetTargets, needs no argument at all: it runs on the CHIP
 * event loop task itself, so it and the encode are the same thread of
 * execution and cannot interleave.
 *
 * ---- THE MERGE RULE ----
 *
 * An apply MERGES BY DAY. It does not replace wholesale. CHIP's reference
 * SetTargets() (EnergyEvseTargetsStore.cpp:244-386) walks each incoming
 * schedule against every stored one and splits the day bitmasks
 * (bitmaskA = current & new, bitmaskB = current & ~new), so setting Monday's
 * targets leaves Tuesday's untouched. Stated as a rule: days present in the
 * applied set replace those days ENTIRELY, days absent from it are
 * UNCHANGED. A wholesale replace passes every "apply then read back" test
 * anyone would naturally write and then silently destroys days a controller
 * never mentioned.
 *
 * The single exception, and this round has already produced two defects from
 * it (task 1's validate rejected the path outright, task 2's apply silently
 * committed staged rows instead of clearing): a count of 0 clears the ENTIRE
 * stored payload for that endpoint. That is how a host issues the
 * ClearTargets equivalent, and merging zero rows would otherwise be a no-op
 * with no way to empty a schedule at all.
 *
 * ---- PERSISTENCE, and its deliberate asymmetry with the composition ----
 *
 * The schedule lives in this firmware's OWN NVS namespace (MT_EVSE_NVS_NS),
 * not esp-matter's, and is USER DATA: it survives AT+MTRESET and a power
 * cycle, and AT+MTFRESET erases it (cmd_mtfreset in mt_at.c calls
 * mt_matter_evse_targets_erase_all()). That is the exact opposite of the
 * endpoint composition, which survives a factory reset because it is a
 * product definition rather than something an end user chose. Same NVS
 * partition, opposite lifetimes, and the reason is the difference between
 * "what this board IS" and "what its owner asked it to do".
 */

#include <stdio.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <nvs.h>
#include <nvs_flash.h>

/* The Delegate interface this file implements, plus (transitively) the
 * EnergyEvse struct/enum types, DataModel::List/Nullable and
 * MatterReportingAttributeChangeCallback. */
#include <app/clusters/energy-evse-server/energy-evse-server.h>

#include "mt_at.h"
#include "mt_evse.h"
#include "mt_matter.h"

static const char *TAG = "mt_evse";

namespace EE = chip::app::Clusters::EnergyEvse;

namespace {

/* Field order of MT_ROW_KIND_EVSE_TARGET, from mt_rows.c's
 * s_evse_target_fields[] table (day bitmap, minutes past midnight, SoC,
 * added energy). Named here rather than repeated as literals; mt_rows.h
 * deliberately publishes only the field COUNT, since the codec is the single
 * definition of the rules and the indices are read off its table. */
constexpr uint8_t kRowDay    = 0;
constexpr uint8_t kRowTime   = 1;
constexpr uint8_t kRowSoC    = 2;
constexpr uint8_t kRowEnergy = 3;
constexpr uint8_t kRowFields = 4;

/* Spec-defined store bounds, from the SDK header (energy-evse-server.h:40-41),
 * never transcribed. */
constexpr uint8_t kMaxDays   = EE::kEvseTargetsMaxNumberOfDays;  /* 7  */
constexpr uint8_t kMaxPerDay = EE::kEvseTargetsMaxTargetsPerDay; /* 10 */

/* Every day bit the cluster defines, bits 0..6 (TargetDayOfWeekBitmap
 * kSunday..kSaturday). Same mask mt_rows.c's field table enforces at stage
 * time. */
constexpr uint8_t kAllDaysMask = 0x7F;

/*
 * Energy ceiling for the mWh-valued pushes, the same +-2^62 clamp Round A
 * fixed for the measurement pipeline (kMeasValueAbsMax in main.cpp): the XML
 * types these energy-mWh and floors them at 0, and the ceiling only exists so
 * a 64-bit push cannot carry a value the TLV encoder would then have to
 * truncate.
 */
constexpr int64_t kEnergyMax = 4611686018427387904LL;

/* ---- the NVS blob ---------------------------------------------------------
 *
 * One blob per EVSE endpoint, keyed "t<ep>" in namespace MT_EVSE_NVS_NS:
 *
 *   u8  version (MT_EVSE_BLOB_V1)
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
 * format is small, it has to be readable by a future maintainer holding only
 * this comment, and a decoder that validates every field can treat a
 * corrupt blob as "no schedule" instead of trusting flash.
 *
 * Per-endpoint keys, not one blob for the whole device, so an apply rewrites
 * only the endpoint it touched. Known residue: changing the composition so a
 * former EVSE endpoint id becomes something else leaves that key behind. It
 * is inert (only an endpoint that HAS an EnergyEvse delegate ever reads one)
 * and AT+MTFRESET clears the namespace outright.
 */
#define MT_EVSE_NVS_NS  "mt_evse"
#define MT_EVSE_BLOB_V1 1

constexpr size_t kTargetBlobSize   = 1 + 2 + 1 + 8;                        /* 12 */
constexpr size_t kScheduleBlobSize = 2 + kMaxPerDay * kTargetBlobSize;     /* 122 */
constexpr size_t kBlobMax          = 2 + kMaxDays * kScheduleBlobSize;     /* 856 */

/*
 * The one scratch buffer every encode and decode uses. File-static rather
 * than a local because 856 bytes is a sixth of the AT parser task's
 * 6144-byte stack, and every caller is serialised: the mutating paths all
 * hold ChipStackLock, and the one caller that does not (LoadTargets() from
 * the boot composition rebuild) runs before esp_matter::start(), so there is
 * no CHIP event loop yet to race.
 */
uint8_t s_blob[kBlobMax];

void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void put_i64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
    }
}

int64_t get_i64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= ((uint64_t)p[i]) << (8 * i);
    }
    return (int64_t)u;
}

void key_for(uint16_t ep, char *out, size_t len)
{
    snprintf(out, len, "t%u", (unsigned)ep);
}

/*
 * ---- the delegate ----------------------------------------------------------
 *
 * Every attribute of EnergyEvse (0x0099) is MANAGED_INTERNALLY: the cluster's
 * Instance is its AttributeAccessInterface, so all 23 reads answer from the
 * cached members below and never from ember storage. The host fills that
 * cache through AT+MTMEAS cluster 0x0099 (MT_EVSE_F_* in mt_matter.h), the
 * same pull-model shape Round B gave the water heater and C1 gave DEM, which
 * also means these attributes never raise +MTATTR URCs and a host cache
 * updates on successful pushes only.
 *
 * Command handlers: NO ChipStackLock in any of them. They are invoked BY CHIP
 * from the Instance's command dispatch, which already runs on the CHIP event
 * loop task holding the stack (main.cpp's own rule, and the same reasoning
 * HearthWhmDelegate::HandleBoost and HearthDemDelegate::PowerAdjustRequest
 * carry).
 */
class HearthEvseDelegate : public EE::Delegate {
public:
    /* ---- host-pushed scalar cache -----------------------------------------
     * Defaults are the pre-first-push answers: the three enums at their zero
     * (NotPluggedIn / Disabled / NoError), the currents and the delay window
     * at 0, and every nullable NULL, which is the same "null until the host
     * first pushes it" contract the EPM measurement values ship with. */
    EE::StateEnum       m_state        = EE::StateEnum::kNotPluggedIn;
    EE::SupplyStateEnum m_supply_state = EE::SupplyStateEnum::kDisabled;
    EE::FaultStateEnum  m_fault_state  = EE::FaultStateEnum::kNoError;

    chip::app::DataModel::Nullable<uint32_t> m_charging_enabled_until;
    chip::app::DataModel::Nullable<uint32_t> m_discharging_enabled_until;

    int64_t  m_circuit_capacity       = 0;
    int64_t  m_min_charge_current     = 0;
    int64_t  m_max_charge_current     = 0;
    int64_t  m_max_discharge_current  = 0;
    int64_t  m_user_max_charge_current = 0;
    uint32_t m_randomization_window   = 0;

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

    /* ---- the charging-target store ----------------------------------------
     * Contract 2 in the file comment: this IS the memory GetTargets() hands
     * to the encoder, so it is held here and never rebuilt on a stack.
     * m_schedules[i].chargingTargets is a List view over m_targets[i], which
     * refresh_views() re-points after every mutation (a List is a pointer and
     * a length, so it goes stale the moment a count changes). */
    EE::Structs::ChargingTargetScheduleStruct::Type m_schedules[kMaxDays];
    EE::Structs::ChargingTargetStruct::Type         m_targets[kMaxDays][kMaxPerDay];
    uint8_t m_target_count[kMaxDays] = { 0 };
    uint8_t m_schedule_count         = 0;
    bool    m_loaded                 = false;

    void refresh_views()
    {
        for (uint8_t i = 0; i < m_schedule_count; i++) {
            m_schedules[i].chargingTargets =
                chip::app::DataModel::List<const EE::Structs::ChargingTargetStruct::Type>(
                    m_targets[i], m_target_count[i]);
        }
    }

    void clear_store()
    {
        for (uint8_t i = 0; i < kMaxDays; i++) {
            m_schedules[i].dayOfWeekForSequence =
                chip::BitMask<EE::TargetDayOfWeekBitmap>(0);
            m_schedules[i].chargingTargets =
                chip::app::DataModel::List<const EE::Structs::ChargingTargetStruct::Type>();
            m_target_count[i] = 0;
            for (uint8_t j = 0; j < kMaxPerDay; j++) {
                m_targets[i][j] = EE::Structs::ChargingTargetStruct::Type();
            }
        }
        m_schedule_count = 0;
    }

    uint8_t day_bits(uint8_t i) const
    {
        return m_schedules[i].dayOfWeekForSequence.GetField(
            static_cast<EE::TargetDayOfWeekBitmap>(kAllDaysMask));
    }

    void set_day_bits(uint8_t i, uint8_t bits)
    {
        m_schedules[i].dayOfWeekForSequence = chip::BitMask<EE::TargetDayOfWeekBitmap>(bits);
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
     * schedule left with no days at all. This is the "days present in the
     * applied set replace those days entirely" half of the merge; the caller
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

    /* ---- persistence ------------------------------------------------------ */

    /* Encode the store into s_blob. Returns the byte count. */
    size_t encode(void)
    {
        size_t n = 0;
        s_blob[n++] = MT_EVSE_BLOB_V1;
        s_blob[n++] = m_schedule_count;
        for (uint8_t i = 0; i < m_schedule_count; i++) {
            s_blob[n++] = day_bits(i);
            s_blob[n++] = m_target_count[i];
            for (uint8_t j = 0; j < m_target_count[i]; j++) {
                const EE::Structs::ChargingTargetStruct::Type &t = m_targets[i][j];
                uint8_t flags = 0;
                if (t.targetSoC.HasValue()) {
                    flags |= 0x01;
                }
                if (t.addedEnergy.HasValue()) {
                    flags |= 0x02;
                }
                s_blob[n++] = flags;
                put_u16(&s_blob[n], t.targetTimeMinutesPastMidnight);
                n += 2;
                s_blob[n++] = t.targetSoC.HasValue() ? t.targetSoC.Value() : 0;
                put_i64(&s_blob[n], t.addedEnergy.HasValue() ? t.addedEnergy.Value() : 0);
                n += 8;
            }
        }
        return n;
    }

    /*
     * Decode s_blob[0..len) into the store. Returns false and leaves the
     * store EMPTY on anything unexpected: a truncated or corrupt blob means
     * "no schedule", never a half-loaded one, because a half-loaded schedule
     * is a charging plan nobody authored.
     */
    bool decode(size_t len)
    {
        clear_store();
        if (len < 2 || s_blob[0] != MT_EVSE_BLOB_V1) {
            return false;
        }
        uint8_t sched = s_blob[1];
        if (sched > kMaxDays) {
            return false;
        }
        size_t n = 2;
        uint8_t seen_days = 0;
        for (uint8_t i = 0; i < sched; i++) {
            if (len - n < 2) {
                clear_store();
                return false;
            }
            uint8_t bits  = s_blob[n++];
            uint8_t count = s_blob[n++];
            if (bits == 0 || (bits & (uint8_t)~kAllDaysMask) != 0 || (bits & seen_days) != 0 ||
                count > kMaxPerDay) {
                clear_store();
                return false;
            }
            seen_days = (uint8_t)(seen_days | bits);
            if (len - n < (size_t)count * kTargetBlobSize) {
                clear_store();
                return false;
            }
            set_day_bits(i, bits);
            m_target_count[i] = count;
            for (uint8_t j = 0; j < count; j++) {
                uint8_t  flags  = s_blob[n++];
                uint16_t time   = get_u16(&s_blob[n]);
                n += 2;
                uint8_t  soc    = s_blob[n++];
                int64_t  energy = get_i64(&s_blob[n]);
                n += 8;
                if (time > 1439 || soc > 100 || energy < 0) {
                    clear_store();
                    return false;
                }
                EE::Structs::ChargingTargetStruct::Type t;
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
     * ("t<ep>") and endpoint ids are assigned in composition order, so
     * re-declaring an EVSE at the same index with the other variant hands the
     * new endpoint the old one's schedule. decode() cannot see it: every
     * target in that schedule is well formed, in range, and was legal for the
     * variant that wrote it. Served unchanged, GetTargets() answers a
     * controller with targets carrying a targetSoC the cluster's SOC
     * conformance says may only be absent or 100, or with targets lacking one
     * where ValidateTargets() makes it mandatory. Either way GetTargets hands
     * back a schedule its own SetTargets would have been refused, which is
     * the one thing mt_evse_targets_apply_locked()'s SOC check exists to
     * prevent on the other path.
     *
     * WHY IT LIVES IN LoadTargets() AND NOT AT THE BOOT CALL SITE. There are
     * THREE loads per endpoint, not one: the boot load from
     * mt_matter_evse_delegate_alloc(), the apply path's !m_loaded retry, and
     * GetTargets()'s own lazy retry. A boot load that hits a transient NVS
     * failure leaves the store empty and m_loaded false, so a check placed at
     * the boot site passes over nothing; the later retry then succeeds, loads
     * the cross-variant blob and serves it unchecked, which is the exact
     * sentence this check is written against, reached by a different door.
     * Hanging it off the one function all three go through is what makes
     * "every load is validated" a property rather than a habit.
     *
     * The variant is read from this endpoint's OWN metadata, the same source
     * mt_evse_targets_apply_locked() uses, so the two cannot disagree.
     * soc_reporting::add() must therefore have run before the first load;
     * mk_energy_evse() (mt_devtypes.cpp) carries that ordering and a guard
     * that fails the create loudly if it is ever broken.
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
        if (m_schedule_count == 0) {
            return;
        }
        const bool soc =
            (esp_matter::attribute::get(mEndpointId, EE::Id,
                                        EE::Attributes::StateOfCharge::Id) != nullptr);
        for (uint8_t i = 0; i < m_schedule_count; i++) {
            for (uint8_t j = 0; j < m_target_count[i]; j++) {
                const EE::Structs::ChargingTargetStruct::Type &t = m_targets[i][j];
                if (soc ? !t.targetSoC.HasValue()
                        : (t.targetSoC.HasValue() && t.targetSoC.Value() != 100)) {
                    ESP_LOGE(TAG,
                             "ep %u: the stored charging schedule was written for the other "
                             "SOC variant and cannot be served here; not serving it (the "
                             "stored blob is left alone). A controller would otherwise be "
                             "handed a schedule its own SetTargets would be refused",
                             mEndpointId);
                    clear_store();
                    return;
                }
            }
        }
    }

    /* Write the store to NVS. Returns false on any NVS failure. */
    bool save()
    {
        size_t len = encode();
        char key[16];
        key_for(mEndpointId, key, sizeof(key));

        nvs_handle_t h;
        esp_err_t err = nvs_open(MT_EVSE_NVS_NS, NVS_READWRITE, &h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ep %u: nvs_open failed: %s", mEndpointId, esp_err_to_name(err));
            return false;
        }
        err = nvs_set_blob(h, key, s_blob, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ep %u: saving charging targets failed: %s", mEndpointId,
                     esp_err_to_name(err));
            return false;
        }
        return true;
    }

    /* ---- the 8 command handlers ------------------------------------------- */

    /*
     * ---- Disable / EnableCharging: the EVSE's entire control surface ----
     *
     * Both are MANDATORY commands on this cluster and esp-matter creates
     * both unconditionally (esp_matter_cluster.cpp:3397-3398), so a
     * controller can invoke them the moment the endpoint exists. They used
     * to answer Failure, on the reasoning that the round gave the host no
     * adjudication surface for them; that reasoning was right about the
     * firmware being unable to honour them alone and wrong about the
     * conclusion. An EVSE that appears in a controller and refuses both to
     * start and to stop charging is not a partially implemented EVSE, it is
     * one with no control surface at all: everything else on the cluster is
     * a reading. So they forward, as ORDINARY SCALAR +MTCMD commands on the
     * standard 1000 ms path, the shape Round B gave the water heater's
     * Boost.
     *
     * Neither one writes any attribute on allow, and that is deliberate.
     * SupplyState, State and ChargingEnabledUntil are host-pushed
     * (AT+MTMEAS cluster 0x0099); the host is the single authority on what
     * the hardware actually did, and a firmware that also wrote them would
     * make two authorities disagree the first time a relay failed to close.
     * The host reports the resulting state by pushing it, the same division
     * HearthWhmDelegate::HandleBoost keeps with BoostState.
     */
    chip::Protocols::InteractionModel::Status Disable() override
    {
        /* No fields: NULL reproduces mt_cmd_forward()'s exact four-field
         * +MTCMD line. */
        bool allow = mt_cmd_forward_fields(mEndpointId, EE::Id, EE::Commands::Disable::Id, NULL);
        return allow ? chip::Protocols::InteractionModel::Status::Success
                     : chip::Protocols::InteractionModel::Status::Failure;
    }

    /*
     * EnableCharging forwards three fields:
     * "<chargingEnabledUntil>,<minimumChargeCurrent>,<maximumChargeCurrent>".
     *
     * chargingEnabledUntil is NULLABLE (null means "charge indefinitely"),
     * and null is rendered as an EMPTY TOKEN between its commas, which is
     * mt_cmd_forward_fields()'s documented convention for a field position
     * that carries no value (mt_at.h, and the microwave's
     * SetCookingParameters is the shipped precedent). Round B's Boost uses
     * a presence MASK instead, but that exists to carry the VALUES of
     * optional BOOLEANS, which an empty token cannot express, and it costs
     * a field of its own; with one nullable number and no booleans here,
     * the empty token says the same thing in the position the value would
     * have occupied.
     *
     * No validation of the two currents: Instance::HandleEnableCharging
     * (energy-evse-server.cpp:296-322) has already rejected either below
     * kMinimumChargeCurrentLimit and min > max with ConstraintError before
     * this is reached.
     */
    chip::Protocols::InteractionModel::Status EnableCharging(
        const chip::app::DataModel::Nullable<uint32_t> &enableChargeTime,
        const int64_t &minimumChargeCurrent, const int64_t &maximumChargeCurrent) override
    {
        /*
         * 64, not 48, and the arithmetic matters because snprintf truncates
         * in SILENCE: a truncated tail hands the host a wrong, smaller
         * maximum current, which is a safety-relevant number.
         * HandleEnableCharging bounds these only from below (negative, and
         * min > max, energy-evse-server.cpp:296-322), never from above, so
         * the real worst case is a non-null chargingEnabledUntil at
         * UINT32_MAX with both currents at INT64_MAX: 10 + 1 + 19 + 1 + 19
         * = 50, plus a NUL = 51. 48 truncated it. A negative current cannot
         * reach here, but would only add two sign characters (53), so 64
         * has headroom either way.
         */
        char fields[64];
        if (enableChargeTime.IsNull()) {
            snprintf(fields, sizeof(fields), ",%lld,%lld", (long long)minimumChargeCurrent,
                     (long long)maximumChargeCurrent);
        } else {
            snprintf(fields, sizeof(fields), "%lu,%lld,%lld",
                     (unsigned long)enableChargeTime.Value(), (long long)minimumChargeCurrent,
                     (long long)maximumChargeCurrent);
        }

        bool allow = mt_cmd_forward_fields(mEndpointId, EE::Id, EE::Commands::EnableCharging::Id,
                                           fields);
        return allow ? chip::Protocols::InteractionModel::Status::Success
                     : chip::Protocols::InteractionModel::Status::Failure;
    }

    /*
     * EnableDischarging: V2X only, and V2X is parked for this round (design
     * spec 5.3), so the feature bit is never set and the server answers
     * UnsupportedCommand before ever reaching here. The override exists
     * because the pure virtual does.
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
     * StartDiagnostics exists ONLY because the pure virtual does. Its command
     * can never dispatch: esp-matter declares
     * command::create_start_diagnostics but has zero callers for it anywhere,
     * so the command never enters this cluster's AcceptedCommandList,
     * get_energy_evse_enabled_optional_commands() (esp_matter_delegate_callbacks.cpp:177-186)
     * reads that same metadata and hands the Instance an empty optional-command
     * mask, and an invoke answers UnsupportedCommand at the dispatch. That
     * incoherence is left alone deliberately (design spec 5.3): hand-adding
     * the command entry would advertise a diagnostic self-test this firmware
     * cannot run. Failure is what an unreachable handler should return.
     */
    chip::Protocols::InteractionModel::Status StartDiagnostics() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }

    /*
     * ---- SetTargets: the adjudicated path ----------------------------------
     *
     * A controller's charging schedule does not become this device's
     * schedule until the host has seen it and said yes. The sequence, all
     * of it on the CHIP event loop task:
     *
     *   1. claim the inbound row stage and decode the schedule into it
     *   2. raise "+MTCMD:<seq>,<ep>,153,5,<rowcount>,<daymask>" (cluster
     *      0x0099, command 0x05) and block 3000 ms
     *   3. the host pulls the rows with AT+MTROWGET=<ep>,1,,<seq> and
     *      answers AT+MTCMDRESP=<seq>,<0|1>
     *   4. on allow, merge through mt_evse_targets_apply_locked(), which is
     *      the SAME function an AT+MTROWAPPLY commits through, so the two
     *      paths cannot end up storing different things from the same rows
     *   5. release the claim, on every path out
     *
     * <daymask> is the set of days the proposal touches, which is NOT
     * derivable from the rows: a day being emptied carries none. See the
     * `affected` comment in the body.
     *
     * The rows never travel in the +MTCMD line. 70 rows is about 2450
     * bytes, MT_CMD_LINE_MAX is 112 and snprintf truncates in silence, and
     * pushing that as unsolicited URCs into a sketch sitting in loop() is
     * B166 at scale (mt_at.h's inbound-stage comment carries the full
     * argument). The host pulls, inside a command where it is doing nothing
     * but draining the response.
     *
     * ---- WHAT IS NOT RE-VALIDATED HERE ----
     *
     * Instance::ValidateTargets() (energy-evse-server.cpp:370-468) has
     * already run and returned Success, so day-bit reuse across entries,
     * a time past 1439, both SoC rules of the SOC feature, a target with
     * neither optional, a negative added energy, and more than ten targets
     * in one day are all impossible by the time this is called. None of
     * them is checked again.
     *
     * mt_rows_stage() below is nonetheless the WRITER for this buffer, and
     * it validates as it writes. That is not a second opinion on CHIP's
     * work, it is the codec's own field table doing what it does on the AT
     * path too, and it catches the three things CHIP's validation does NOT
     * reject and this firmware cannot represent: a schedule entry with a
     * dayOfWeekForSequence of ZERO (ValidateTargets accepts it, since
     * `bitmap & 0` never collides and `|= 0` never marks a day), the
     * unbounded NUMBER of such entries that follows from it, and any total
     * row count past the 70 a stage holds. Those answer ConstraintError and
     * ResourceExhausted respectively, before the host is ever woken.
     */
    chip::Protocols::InteractionModel::Status SetTargets(
        const chip::app::DataModel::DecodableList<
            EE::Structs::ChargingTargetScheduleStruct::DecodableType> &chargingTargetSchedules)
        override
    {
        using chip::Protocols::InteractionModel::Status;

        mt_row_stage_t *stage = mt_rows_inbound_claim(mEndpointId, MT_ROW_KIND_EVSE_TARGET);
        if (stage == nullptr) {
            /* Another forward owns the stage, or the host is still
             * streaming the previous one out. Busy is retryable and true;
             * Failure would tell the controller its schedule was rejected. */
            ESP_LOGW(TAG, "ep %u: SetTargets busy, inbound stage in use", mEndpointId);
            return Status::Busy;
        }

        /*
         * Flatten (schedule, target) into rows, the same flattening
         * mt_evse_targets_get_locked() uses in the other direction: the day
         * bitmap is repeated on every row of its group, which is what lets
         * one row shape carry a nested payload.
         *
         * Re-iterating the DecodableList is sound: ValidateTargets() has
         * already walked this very list, begin() takes a copy of the TLV
         * reader, and the message buffer is alive for the whole invoke.
         */
        Status decode_err = Status::Success;
        uint16_t n = 0;
        /*
         * The affected-day mask, taken from the schedule ENTRIES and not
         * from the flattened rows. That distinction is the whole of defect
         * 1 of this task's review: an entry may carry an EMPTY
         * chargingTargets list, which ValidateTargets() accepts (innerIdx
         * starts at 0, only innerIdx > 10 is rejected) and which means
         * "this day now has no targets". It produces no row, so a mask
         * derived from rows would never mention that day, subtract_days()
         * would leave it alone, and the device would answer SUCCESS to a
         * deletion it did not perform. CHIP's reference store clears the
         * day in both of its arms (EnergyEvseTargetsStore.cpp:305-315 and
         * :347-370).
         */
        uint8_t affected = 0;
        auto sched_iter = chargingTargetSchedules.begin();
        while (decode_err == Status::Success && sched_iter.Next()) {
            const auto &entry = sched_iter.GetValue();
            uint8_t bits = entry.dayOfWeekForSequence.GetField(
                static_cast<EE::TargetDayOfWeekBitmap>(kAllDaysMask));
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
                row.nfields = kRowFields;
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
            ESP_LOGW(TAG, "ep %u: SetTargets not representable, status 0x%02x", mEndpointId,
                     (unsigned)chip::to_underlying(decode_err));
            mt_rows_inbound_release();
            return decode_err;
        }

        /*
         * Blocks up to 3000 ms. The host fetches the rows and answers in
         * that window; see mt_at.h for what a slow loop() costs.
         *
         * The affected-day mask rides along as the +MTCMD line's second
         * tail field, after the row count, because the host cannot
         * otherwise SEE a day it is being asked to empty: there is no row
         * to pull for it. Without it a sketch adjudicating
         * [{Mon, []}, {Tue, [t]}] would be shown one Tuesday row and would
         * have no idea Monday was being deleted.
         */
        bool allow = mt_rows_inbound_forward(mEndpointId, EE::Id, EE::Commands::SetTargets::Id,
                                             affected);

        Status out;
        if (!allow) {
            /* Deny or timeout: nothing was touched, the stored schedule is
             * byte-identical, and the controller is told so. */
            out = Status::Failure;
        } else if (n == 0 && affected == 0) {
            /*
             * AN EMPTY TOP-LEVEL LIST IS A NO-OP HERE AND A CLEAR ON THE AT
             * PATH, and that asymmetry is deliberate rather than an
             * inconsistency to be tidied away.
             *
             * The fabric has a separate ClearTargets command, so an empty
             * SetTargets means "I am setting no days", which merges to
             * nothing; CHIP's own reference store behaves identically (its
             * merge loop simply never iterates). The AT surface has NO
             * clear verb, so AT+MTROWAPPLY=<ep>,1,0 had to become one, and
             * mt_evse_targets_apply_locked() treats a rowless, dayless call
             * as "empty the store" for exactly that reason.
             *
             * Which means this branch must NOT call the merge: handing it
             * no rows and no days would wipe the user's whole schedule
             * because a controller sent an empty list.
             *
             * The gate is "no rows AND no days", not "no rows". Those
             * differ precisely for the case above: a non-empty list whose
             * entries carry no targets has n == 0 but names real days, and
             * it must CLEAR those days rather than fall in here and do
             * nothing. "Empty request" and "request to empty" are different
             * commands and conflating them was the defect.
             *
             * Counting entries instead would have been the obvious gate and
             * is wrong twice over: the count can exceed 255 through the same
             * zero-bitmap hole (so a uint8_t wraps), and entries that name
             * no day and carry no targets genuinely ARE a no-op, since there
             * is nothing to clear. n and affected are both derived from real
             * content and cannot overflow: affected is seven bits and n is
             * capped at 70.
             *
             * n > 0 with affected == 0 is unrepresentable, which is what
             * makes this gate safe: a row only exists for an entry, and
             * mt_rows_stage() refuses a row whose day bitmap is 0, so any
             * row implies a day bit.
             */
            ESP_LOGI(TAG, "ep %u: SetTargets with no schedules, nothing to merge", mEndpointId);
            out = Status::Success;
        } else {
            int rc = mt_evse_targets_apply_locked(mEndpointId, stage, affected);
            /* Already on the CHIP task inside a command dispatch, so the
             * stack lock is held and the _locked variant is the right call;
             * going through mt_matter_evse_targets_apply() would take
             * ChipStackLock a second time and deadlock on it. */
            if (rc == MT_ROW_OK) {
                out = Status::Success;
            } else {
                /* Includes MT_ROW_ERR_PERSIST, "merged but not saved".
                 * Failure, matching ClearTargets() below, which answers
                 * Failure for its own save() failure: within one cluster
                 * the two write commands report a durability failure the
                 * same way. The AT path reports it as +MTERR:7 instead,
                 * because it has a code for exactly that and the fabric
                 * does not. */
                ESP_LOGE(TAG, "ep %u: SetTargets merge failed, rc %d", mEndpointId, rc);
                out = Status::Failure;
            }
        }

        mt_rows_inbound_release();
        return out;
    }

    /*
     * LoadTargets: contract 1. Reads this endpoint's blob out of our own NVS
     * namespace into the member store. A missing key is success with an empty
     * schedule (a device that has never been given one), which is why the
     * not-found arm does not log.
     */
    chip::Protocols::InteractionModel::Status LoadTargets() override
    {
        using chip::Protocols::InteractionModel::Status;

        clear_store();

        char key[16];
        key_for(mEndpointId, key, sizeof(key));

        nvs_handle_t h;
        esp_err_t err = nvs_open(MT_EVSE_NVS_NS, NVS_READONLY, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            m_loaded = true; /* namespace never written: empty IS the truth */
            return Status::Success;
        }
        if (err != ESP_OK) {
            /* m_loaded stays FALSE, and the distinction is the whole point:
             * "there is nothing stored" and "I could not find out what is
             * stored" are different facts, and only the first one makes an
             * empty store authoritative. A transient read failure that
             * marked the store loaded would let the next apply merge into
             * nothing and then save that over a schedule which was intact
             * all along, which is silent data loss. */
            ESP_LOGE(TAG, "ep %u: nvs_open failed: %s", mEndpointId, esp_err_to_name(err));
            return Status::Failure;
        }
        size_t len = sizeof(s_blob);
        err = nvs_get_blob(h, key, s_blob, &len);
        nvs_close(h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            m_loaded = true; /* no schedule for this endpoint: also the truth */
            return Status::Success;
        }
        if (err != ESP_OK) {
            /* Transient again, same reasoning as the open failure above. */
            ESP_LOGE(TAG, "ep %u: reading charging targets failed: %s", mEndpointId,
                     esp_err_to_name(err));
            return Status::Failure;
        }
        if (!decode(len)) {
            /* CORRUPT is not transient: the bytes were read successfully and
             * they are not a schedule. Empty is then the only honest state,
             * re-reading would produce the same bytes forever, and an apply
             * overwriting them loses nothing that was ever a schedule. So
             * this arm DOES mark the store loaded, unlike the two above. */
            m_loaded = true;
            ESP_LOGE(TAG, "ep %u: stored charging targets are corrupt (%u bytes), ignored",
                     mEndpointId, (unsigned)len);
            return Status::Failure;
        }
        m_loaded = true;
        /* Every load goes through here, which is the point (the helper's own
         * comment): boot, the apply path's retry and GetTargets()'s lazy
         * retry all land on this line. */
        drop_store_if_variant_mismatch();
        ESP_LOGI(TAG, "ep %u: loaded %u charging target schedule(s)", mEndpointId,
                 m_schedule_count);
        return Status::Success;
    }

    /*
     * GetTargets: contract 2. Hands back a view over the member store, which
     * stays alive and unmodified until the encoder has run (see the file
     * comment). The lazy LoadTargets() is defence in depth, not the primary
     * satisfaction of contract 1: mt_matter_evse_delegate_alloc() has already
     * loaded at boot, and this guard is here so a future path that reaches a
     * delegate before the rebuild has loaded it cannot encode a view over an
     * uninitialised store.
     *
     * A RETRY THAT STILL CANNOT READ THE STORE NOW FAILS THE READ, and this
     * changed when the fabric write path landed. m_loaded false means a
     * previous LoadTargets() hit a TRANSIENT NVS failure, so this store is
     * empty only because nobody could find out what is in it (a corrupt
     * blob marks itself loaded, precisely so it does not land here). The
     * retry's outcome used to be discarded, which made an unreadable store
     * block a merge but not a read: a controller was told "no schedule",
     * confidently, about a schedule that may be sitting intact in flash.
     * That was survivable while the fabric could only read. It is not now:
     * the natural response to an empty schedule is to send a new one, and
     * the user would be re-authoring a plan that was never lost. An honest
     * failure is retryable; a confident empty list is not recoverable
     * because nothing about it looks wrong. Instance::HandleGetTargets
     * (energy-evse-server.cpp:472-485) passes a non-Success status straight
     * through as the command status and sends no response, which is exactly
     * the shape wanted.
     */
    chip::Protocols::InteractionModel::Status GetTargets(
        chip::app::DataModel::List<const EE::Structs::ChargingTargetScheduleStruct::Type>
            &chargingTargetSchedules) override
    {
        if (!m_loaded) {
            (void)LoadTargets();
            if (!m_loaded) {
                chargingTargetSchedules =
                    chip::app::DataModel::List<
                        const EE::Structs::ChargingTargetScheduleStruct::Type>();
                ESP_LOGE(TAG, "ep %u: charging targets unreadable, failing GetTargets",
                         mEndpointId);
                return chip::Protocols::InteractionModel::Status::Failure;
            }
        }
        refresh_views();
        chargingTargetSchedules =
            chip::app::DataModel::List<const EE::Structs::ChargingTargetScheduleStruct::Type>(
                m_schedules, m_schedule_count);
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /*
     * ClearTargets: empties the whole store, the same end state an
     * AT+MTROWAPPLY with count 0 reaches from the host side. Not adjudicated:
     * the round adjudicates SetTargets because it installs a plan, while a
     * clear only removes one, and the host learns about it the same way it
     * learns about any other fabric-side change, by reading back.
     */
    chip::Protocols::InteractionModel::Status ClearTargets() override
    {
        clear_store();
        if (!save()) {
            return chip::Protocols::InteractionModel::Status::Failure;
        }
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /* ---- the 23 attribute getters ----------------------------------------- */

    EE::StateEnum GetState() override { return m_state; }
    EE::SupplyStateEnum GetSupplyState() override { return m_supply_state; }
    EE::FaultStateEnum GetFaultState() override { return m_fault_state; }
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
     * VehicleID is PNC's, and PNC is off this round (design spec 5.3: no host
     * story for the string yet), so the attribute is never created and this
     * getter is unreachable. Null rather than a dangling CharSpan: a
     * Nullable<CharSpan> over storage this delegate does not own would be the
     * same non-owning-view mistake contract 2 is about, one attribute down.
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

    /* ---- the 3 attribute setters ------------------------------------------
     *
     * These are the fabric's write path: Instance::Write dispatches a
     * controller write straight into them (energy-evse-server.cpp), which is
     * why they validate and report rather than just assigning. They are also
     * the only writable attributes the cluster has, and DE270's carve-out
     * (a write to a delegate-served attribute over AT+MTATTR collapses to a
     * bare ERROR because the SDK discards the provider's status) is why the
     * HOST pushes the same three fields through AT+MTMEAS instead.
     *
     * All three attributes are optional and esp-matter creates none of them
     * (esp_matter_cluster.cpp:3378-3391 does not call
     * attribute::create_user_maximum_charge_current,
     * create_randomization_delay_window or create_approximate_ev_efficiency,
     * and no feature helper does either), so on this firmware's endpoints
     * they are absent from the metadata and these setters are unreachable
     * from the fabric until a thunk creates them. See the report's finding on
     * that; the cache is served either way, so the moment the attributes are
     * created these become live with no further change here.
     */
    CHIP_ERROR SetUserMaximumChargeCurrent(int64_t aNewValue) override
    {
        if (aNewValue < EE::kMinimumChargeCurrentLimit) {
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        if (m_user_max_charge_current != aNewValue) {
            m_user_max_charge_current = aNewValue;
            MatterReportingAttributeChangeCallback(mEndpointId, EE::Id,
                                                   EE::Attributes::UserMaximumChargeCurrent::Id);
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetRandomizationDelayWindow(uint32_t aNewValue) override
    {
        if (aNewValue > EE::kMaxRandomizationDelayWindow) {
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        if (m_randomization_window != aNewValue) {
            m_randomization_window = aNewValue;
            MatterReportingAttributeChangeCallback(mEndpointId, EE::Id,
                                                   EE::Attributes::RandomizationDelayWindow::Id);
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetApproximateEVEfficiency(chip::app::DataModel::Nullable<uint16_t> aNewValue) override
    {
        if (m_approx_efficiency != aNewValue) {
            m_approx_efficiency = aNewValue;
            MatterReportingAttributeChangeCallback(mEndpointId, EE::Id,
                                                   EE::Attributes::ApproximateEVEfficiency::Id);
        }
        return CHIP_NO_ERROR;
    }
};

/*
 * The pool, MT_EVSE_MAX slots (mt_matter.h documents the sizing), the
 * HearthWhmDelegate array-plus-next-counter shape with the endpoint id taken
 * at handout. Each slot carries the ~1680-byte target store, which is the
 * whole reason MT_EVSE_MAX is 2 and not 8.
 */
HearthEvseDelegate s_evse_delegates[MT_EVSE_MAX];
size_t             s_evse_next = 0;

/*
 * Reservations claimed by mt_matter_evse_reserve(), the count-only gate
 * mk_energy_evse() (mt_devtypes.cpp) calls BEFORE energy_evse::create().
 *
 * The final whole-branch review of this round found the EVSE thunk with
 * exactly the ordering task 8 had just fixed for the meter: energy_evse::
 * create() allocates the endpoint, marks it enabled and appends it to the
 * node's own endpoint list, and only afterwards did the thunk reach
 * mt_matter_evse_delegate_alloc(). A third declared EVSE therefore left a
 * LIVE, delegate-less EnergyEvse endpoint behind: visible to a controller
 * through provider::Endpoints() (which walks the node's own list), invisible
 * to AT+MTEP? (mt_matter_record_endpoint() is never reached for a thunk that
 * returned nullptr), and answering Failure on every attribute read because
 * no Instance was ever newed for it.
 *
 * The meter's fix does not transfer verbatim, which is why this is a second
 * counter rather than a reuse of that shape: mt_meter_reserve() is the WHOLE
 * gate there, because the meter's Instance is built much later by
 * mt_meter_register_all()'s scan. Here the delegate handout itself already
 * exists and needs the endpoint id create() has not produced yet, so the
 * gate is SPLIT: reserve() takes the capacity decision before anything is
 * built, and mt_matter_evse_delegate_alloc() CONSUMES that reservation
 * afterwards, once the id exists. The consumption check below
 * (s_evse_next >= s_evse_reserved) is what makes the split safe: alloc can
 * never hand out a slot that was not reserved first, so the two counters
 * cannot drift into a state where the reservation said no and the alloc
 * said yes.
 *
 * Monotonic with no release, boot-scoped, for the identical reason
 * s_meter_reserved is (mt_meter.cpp): ANY thunk returning nullptr aborts the
 * WHOLE composition rebuild for the rest of this boot (main.cpp's
 * comp.count = 0; break;), so no later attempt in the same boot can be
 * blocked by a reservation an aborted endpoint never consumed. A reboot
 * reinitialises both counters along with every other static here.
 */
size_t             s_evse_reserved = 0;

/* The by-endpoint lookup every bridge below starts from, the whm_for() shape. */
HearthEvseDelegate *evse_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_evse_next; i++) {
        if (s_evse_delegates[i].endpoint() == ep) {
            return &s_evse_delegates[i];
        }
    }
    return nullptr;
}

}  /* anonymous namespace: everything above is internal to this file, which
    * is also what lets the class's inline members use the file-static blob
    * buffer without an ODR argument. */

extern "C" bool mt_matter_evse_reserve(void)
{
    if (s_evse_reserved >= MT_EVSE_MAX) {
        return false;
    }
    /*
     * Commit the inbound row staging buffer here too (ruling DE419,
     * mt_at.h's mt_rows_inbound_commit()). This endpoint's SetTargets is
     * the only fabric-originated command in this firmware that carries
     * rows, so this reserve is the exact moment the buffer becomes
     * reachable; a composition with no EVSE never gets here and never pays
     * for it.
     *
     * It belongs in the RESERVE rather than in the delegate alloc for the
     * same reason the delegate count does: this runs before create(), so a
     * failure builds nothing at all, where a failure after create() would
     * leave a live, delegate-less EnergyEvse endpoint behind. That is the
     * whole point of s_evse_reserved above.
     *
     * Committing at composition time rather than allocating per forward is
     * what keeps SetTargets' answers on the Matter wire exactly what they
     * have always been: nothing on the fabric path allocates, so nothing on
     * the fabric path can be refused for want of memory. mt_at.h carries
     * the full reasoning.
     *
     * Idempotent, so a second and third EVSE cost nothing more, and the
     * reservation counter is not consumed when it fails.
     */
    if (!mt_rows_inbound_commit()) {
        ESP_LOGE(TAG, "inbound row stage could not be committed (%u bytes); an EVSE "
                 "cannot adjudicate SetTargets without it",
                 (unsigned)sizeof(mt_row_stage_t));
        return false;
    }
    s_evse_reserved++;
    return true;
}

extern "C" void *mt_matter_evse_delegate_alloc(uint16_t ep)
{
    /*
     * Consume a reservation mt_matter_evse_reserve() made before create()
     * ran (see s_evse_reserved's comment). This, not the MT_EVSE_MAX bound
     * below, is now the live capacity check: reserved can never exceed
     * MT_EVSE_MAX, so a handout with no outstanding reservation is either a
     * caller that skipped reserve() or a pool that is genuinely full, and
     * both must fail here.
     */
    if (s_evse_next >= s_evse_reserved) {
        return nullptr;
    }
    /* Defensive backstop only, unreachable given the check above, kept for
     * the same reason mt_meter_register_all()'s slot bound is: cheap, and
     * worth labelling as defensive rather than leaving a reader to wonder
     * whether it is still load-bearing. */
    if (s_evse_next >= MT_EVSE_MAX) {
        return nullptr;
    }
    HearthEvseDelegate *d = &s_evse_delegates[s_evse_next++];
    d->SetEndpointId(ep);

    /*
     * Contract 1, satisfied here: LoadTargets() before anything can call
     * GetTargets(). This runs from the boot composition rebuild, i.e. before
     * esp_matter::start(), so NVS is up (app_main calls nvs_flash_init()
     * first) and there is no CHIP event loop yet to lock against. A load
     * failure is logged by LoadTargets() itself and leaves an empty
     * schedule; it must not abort the endpoint, because a corrupt schedule
     * is not a reason to refuse to be an EVSE.
     */
    /* LoadTargets() validates the loaded schedule against this endpoint's SOC
     * variant itself (drop_store_if_variant_mismatch(), beside the store), so
     * this call site needs no check of its own and neither do the other two
     * load sites. */
    (void)d->LoadTargets();
    return d;
}

/*
 * ---- AT+MTMEAS cluster 0x0099 ----------------------------------------------
 *
 * The EVSE's whole attribute surface is delegate-served, so this is the only
 * way the host can set any of it (design spec 5.5). Two-pass
 * validate-then-apply, the mt_matter_meas_set() hard contract: a bad third
 * pair leaves the first two unapplied. Task 7 (energy round C2) wires this
 * into the AT+MTMEAS dispatcher (mt_at.c, mt_matter_meas_set() in main.cpp);
 * this function and its field table were already built here in task 4,
 * ahead of that wiring, because mt_matter_evse_set()'s single-pair form
 * needed the same validate/apply pass to exist.
 *
 * Why AT+MTMEAS and not AT+MTATTR, for the three attributes that are also
 * WRITABLE (UserMaximumChargeCurrent, RandomizationDelayWindow,
 * ApproximateEVEfficiency, the delegate's three setters below): those would
 * be the first writable delegate-served (managed-internally + WRITABLE)
 * attributes this project ships. DE270 (firmware 0.10.1) resolved the
 * read-only half of that write path (a non-writable managed-internally
 * attribute now answers +MTERR:11 instead of a bare ERROR) but its own
 * resolution note says the writable half is still deferred: none shipped as
 * of that fix, set_val_via_write_attribute() discards the provider's real
 * status regardless of value, and the documented remedy if one ever ships
 * is to call provider::WriteAttribute() from the bridge directly rather than
 * fork the SDK. Routing every EVSE scalar through AT+MTMEAS instead sidesteps
 * that gap rather than closing it: this is an AVOIDANCE, not a fix. The
 * irony worth keeping in mind while reading the table below: the three
 * writable attributes are exactly the three esp-matter never creates (see
 * the existence-gate paragraph), so today the avoidance is doubly
 * theoretical, on both the write path it sidesteps and the attributes it
 * would have applied to.
 *
 * Existence gate: every field is checked against the endpoint's OWN metadata
 * (esp_matter::attribute::get) rather than against a feature-bit table.
 * Three separate reasons converge on the same check. StateOfCharge and
 * BatteryCapacity exist only on variant 0 (the SOC feature axis).
 * UserMaximumChargeCurrent, RandomizationDelayWindow and
 * ApproximateEVEfficiency are optional and esp-matter creates none of them.
 * And the four NextCharge* attributes only exist because charging
 * preferences is unconditional. Asking the metadata answers all three
 * without a second table to keep in sync, and it answers what a controller
 * would actually see.
 */
int mt_evse_meas_apply_locked(uint16_t ep, const uint8_t *fields, const int64_t *values,
                              uint8_t count)
{
    HearthEvseDelegate *d = evse_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run, the same defensive answer
         * every other pool bridge gives. */
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
            attr_id = EE::Attributes::State::Id;
            lo = 0; hi = (int64_t)EE::StateEnum::kUnknownEnumValue - 1;
            break;
        case MT_EVSE_F_SUPPLY_STATE:
            attr_id = EE::Attributes::SupplyState::Id;
            lo = 0; hi = (int64_t)EE::SupplyStateEnum::kUnknownEnumValue - 1;
            break;
        case MT_EVSE_F_FAULT_STATE:
            attr_id = EE::Attributes::FaultState::Id;
            lo = 0; hi = (int64_t)EE::FaultStateEnum::kUnknownEnumValue - 1;
            break;
        case MT_EVSE_F_CHARGING_ENABLED_UNTIL:
            attr_id = EE::Attributes::ChargingEnabledUntil::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_CIRCUIT_CAPACITY:
            attr_id = EE::Attributes::CircuitCapacity::Id;
            lo = EE::kMinimumChargeCurrentLimit; hi = kEnergyMax;
            break;
        case MT_EVSE_F_MIN_CHARGE_CURRENT:
            attr_id = EE::Attributes::MinimumChargeCurrent::Id;
            lo = EE::kMinimumChargeCurrentLimit; hi = kEnergyMax;
            break;
        case MT_EVSE_F_MAX_CHARGE_CURRENT:
            attr_id = EE::Attributes::MaximumChargeCurrent::Id;
            lo = EE::kMinimumChargeCurrentLimit; hi = kEnergyMax;
            break;
        case MT_EVSE_F_USER_MAX_CHARGE_CURRENT:
            attr_id = EE::Attributes::UserMaximumChargeCurrent::Id;
            lo = EE::kMinimumChargeCurrentLimit; hi = kEnergyMax;
            break;
        case MT_EVSE_F_RANDOMIZATION_WINDOW:
            attr_id = EE::Attributes::RandomizationDelayWindow::Id;
            lo = 0; hi = EE::kMaxRandomizationDelayWindow;
            break;
        case MT_EVSE_F_NEXT_CHARGE_START:
            attr_id = EE::Attributes::NextChargeStartTime::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_NEXT_CHARGE_TARGET:
            attr_id = EE::Attributes::NextChargeTargetTime::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_NEXT_CHARGE_ENERGY:
            attr_id = EE::Attributes::NextChargeRequiredEnergy::Id;
            lo = 0; hi = kEnergyMax;
            break;
        case MT_EVSE_F_NEXT_CHARGE_SOC:
            attr_id = EE::Attributes::NextChargeTargetSoC::Id;
            lo = 0; hi = 100;
            break;
        case MT_EVSE_F_APPROX_EFFICIENCY:
            attr_id = EE::Attributes::ApproximateEVEfficiency::Id;
            lo = 0; hi = UINT16_MAX;
            break;
        case MT_EVSE_F_STATE_OF_CHARGE:
            attr_id = EE::Attributes::StateOfCharge::Id;
            lo = 0; hi = 100;
            break;
        case MT_EVSE_F_BATTERY_CAPACITY:
            attr_id = EE::Attributes::BatteryCapacity::Id;
            lo = 0; hi = kEnergyMax;
            break;
        case MT_EVSE_F_SESSION_ID:
            attr_id = EE::Attributes::SessionID::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_SESSION_DURATION:
            attr_id = EE::Attributes::SessionDuration::Id;
            lo = 0; hi = UINT32_MAX;
            break;
        case MT_EVSE_F_SESSION_ENERGY_CHARGED:
            attr_id = EE::Attributes::SessionEnergyCharged::Id;
            lo = 0; hi = kEnergyMax;
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
        if (esp_matter::attribute::get(ep, EE::Id, attr_id) == nullptr) {
            /* The attribute is not on THIS endpoint: a SOC field on a
             * variant-1 EVSE, or one of the three optionals esp-matter never
             * creates. Distinct from a bad value, which is what makes the
             * host able to tell "wrong variant" from "wrong number". */
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
            d->m_state = (EE::StateEnum)v;
            attr_id = EE::Attributes::State::Id;
            break;
        case MT_EVSE_F_SUPPLY_STATE:
            d->m_supply_state = (EE::SupplyStateEnum)v;
            attr_id = EE::Attributes::SupplyState::Id;
            break;
        case MT_EVSE_F_FAULT_STATE:
            d->m_fault_state = (EE::FaultStateEnum)v;
            attr_id = EE::Attributes::FaultState::Id;
            break;
        case MT_EVSE_F_CHARGING_ENABLED_UNTIL:
            d->m_charging_enabled_until =
                chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id = EE::Attributes::ChargingEnabledUntil::Id;
            break;
        case MT_EVSE_F_CIRCUIT_CAPACITY:
            d->m_circuit_capacity = v;
            attr_id = EE::Attributes::CircuitCapacity::Id;
            break;
        case MT_EVSE_F_MIN_CHARGE_CURRENT:
            d->m_min_charge_current = v;
            attr_id = EE::Attributes::MinimumChargeCurrent::Id;
            break;
        case MT_EVSE_F_MAX_CHARGE_CURRENT:
            d->m_max_charge_current = v;
            attr_id = EE::Attributes::MaximumChargeCurrent::Id;
            break;
        case MT_EVSE_F_USER_MAX_CHARGE_CURRENT:
            d->m_user_max_charge_current = v;
            attr_id = EE::Attributes::UserMaximumChargeCurrent::Id;
            break;
        case MT_EVSE_F_RANDOMIZATION_WINDOW:
            d->m_randomization_window = (uint32_t)v;
            attr_id = EE::Attributes::RandomizationDelayWindow::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_START:
            d->m_next_charge_start = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id = EE::Attributes::NextChargeStartTime::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_TARGET:
            d->m_next_charge_target = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id = EE::Attributes::NextChargeTargetTime::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_ENERGY:
            d->m_next_charge_energy = chip::app::DataModel::MakeNullable(v);
            attr_id = EE::Attributes::NextChargeRequiredEnergy::Id;
            break;
        case MT_EVSE_F_NEXT_CHARGE_SOC:
            d->m_next_charge_soc =
                chip::app::DataModel::MakeNullable((chip::Percent)v);
            attr_id = EE::Attributes::NextChargeTargetSoC::Id;
            break;
        case MT_EVSE_F_APPROX_EFFICIENCY:
            d->m_approx_efficiency = chip::app::DataModel::MakeNullable((uint16_t)v);
            attr_id = EE::Attributes::ApproximateEVEfficiency::Id;
            break;
        case MT_EVSE_F_STATE_OF_CHARGE:
            d->m_state_of_charge = chip::app::DataModel::MakeNullable((chip::Percent)v);
            attr_id = EE::Attributes::StateOfCharge::Id;
            break;
        case MT_EVSE_F_BATTERY_CAPACITY:
            d->m_battery_capacity = chip::app::DataModel::MakeNullable(v);
            attr_id = EE::Attributes::BatteryCapacity::Id;
            break;
        case MT_EVSE_F_SESSION_ID:
            d->m_session_id = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id = EE::Attributes::SessionID::Id;
            break;
        case MT_EVSE_F_SESSION_DURATION:
            d->m_session_duration = chip::app::DataModel::MakeNullable((uint32_t)v);
            attr_id = EE::Attributes::SessionDuration::Id;
            break;
        default: /* MT_EVSE_F_SESSION_ENERGY_CHARGED, pass 1 admits nothing else */
            d->m_session_energy_charged = chip::app::DataModel::MakeNullable(v);
            attr_id = EE::Attributes::SessionEnergyCharged::Id;
            break;
        }
        MatterReportingAttributeChangeCallback(ep, EE::Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * ---- the targets store, host side -----------------------------------------
 *
 * Locating an endpoint's store answers the two data-model error codes the AT
 * layer needs: an endpoint that is not in the composition at all
 * (MT_ROW_ERR_ENDPOINT, +MTERR:2) against one that exists but carries no
 * EnergyEvse cluster (MT_ROW_ERR_NO_PAYLOAD, +MTERR:4).
 */
static int locate(uint16_t ep, HearthEvseDelegate **out)
{
    if (esp_matter::endpoint::get(ep) == nullptr) {
        return MT_ROW_ERR_ENDPOINT;
    }
    if (esp_matter::cluster::get(ep, EE::Id) == nullptr) {
        return MT_ROW_ERR_NO_PAYLOAD;
    }
    HearthEvseDelegate *d = evse_for(ep);
    if (d == nullptr) {
        /* Cluster without a pool slot: unreachable after a successful boot
         * rebuild, since the thunk allocates one per EnergyEvse endpoint and
         * aborts the whole composition when the pool is exhausted. */
        return MT_ROW_ERR_NO_PAYLOAD;
    }
    *out = d;
    return MT_ROW_OK;
}

int mt_evse_targets_apply_locked(uint16_t ep, const mt_row_stage_t *stage, uint8_t clear_days)
{
    HearthEvseDelegate *d = nullptr;
    int rc = locate(ep, &d);
    if (rc != MT_ROW_OK) {
        return rc;
    }

    /*
     * The stage belongs to this (ep, kind) only when all three agree; when it
     * does not, the call carries ZERO rows. Do not read stage->row[] outside
     * that match: the pointer is one of the TWO staging buffers mt_at.c owns
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
     * (mt_matter.h's contract, and the count-0 exception in the file
     * comment). This is the AT path's clear verb, and only the AT path
     * reaches it: the fabric passes clear_days from the schedule entries it
     * was actually sent, and its own "empty list" case never calls here at
     * all (SetTargets above documents why an empty SetTargets is a no-op
     * where an AT+MTROWAPPLY count of 0 is a clear).
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
     * Rows to merge, or none when only clear_days brought us here. Every
     * loop below is bounded by this rather than by stage->count, so the
     * "day named with no targets" case walks the stage zero times and is
     * still a merge, not a wholesale clear.
     */
    uint16_t nrows = match ? stage->count : 0;

    /*
     * A MERGE needs the stored schedule to be known, and m_loaded false
     * means a previous load hit a transient NVS failure and this store is
     * empty only because nobody could read it (see LoadTargets()). Retry
     * once, and refuse if it still cannot be read: merging into an
     * assumed-empty store and saving the result would overwrite a schedule
     * that may be perfectly intact. The clear path above is deliberately
     * exempt, because there the host is asking for the stored payload to go
     * away regardless of what it says.
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
     * most kMaxDays distinct bitmaps, and at most kMaxPerDay targets sharing
     * one. mt_rows_validate() has already proven the set dense, in range, and
     * free of a day bit appearing in two different bitmaps.
     *
     * No target storage is built here. 7 x 10 ChargingTargetStructs is about
     * 1.7 KB and the AT parser task has a 6144-byte stack, which is the same
     * arithmetic that made mt_at.c pass the stage by pointer in the first
     * place; the merge below writes straight into the member store instead.
     */
    uint8_t bitmaps[kMaxDays];
    uint8_t counts[kMaxDays];
    uint8_t groups   = 0;
    uint8_t all_bits = 0;

    /*
     * The SOC-variant rule, which is the one rule in this pass that is about
     * a single target rather than the shape of the set.
     * Instance::ValidateTargets() (energy-evse-server.cpp:411-432) enforces
     * it on the FABRIC path: with the SOC feature present every target MUST
     * carry targetSoC (InvalidCommand otherwise), and without it targetSoC
     * must be absent or exactly 100 (ConstraintError otherwise). Nothing
     * enforced it on the AT path, so a host could install exactly the
     * schedule a controller's SetTargets would have been refused, and
     * GetTargets would then hand that schedule back to the controller.
     *
     * Read from this endpoint's OWN metadata, the same existence gate
     * mt_evse_meas_apply_locked() uses: StateOfCharge exists precisely when
     * soc_reporting::add() ran on this endpoint, which is the variant axis.
     * Asking the metadata rather than a variant argument means the two can
     * never disagree, and it is the same fact ValidateTargets reads from the
     * Instance's FeatureMap snapshot.
     */
    bool soc_feature = (esp_matter::attribute::get(ep, EE::Id,
                                                   EE::Attributes::StateOfCharge::Id) != nullptr);

    for (uint16_t r = 0; r < nrows; r++) {
        const mt_row_t *row = &stage->row[r];
        if (row->nfields != kRowFields || !row->present[kRowDay] ||
            !row->present[kRowTime]) {
            /* Defensive: mt_rows_stage() canonicalises nfields and refuses a
             * row missing a mandatory field, so this cannot fire from the AT
             * path. */
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
        if (bits == 0 || (bits & (uint8_t)~kAllDaysMask) != 0) {
            return MT_ROW_ERR_VALUE;
        }
        uint8_t g = 0;
        for (; g < groups; g++) {
            if (bitmaps[g] == bits) {
                break;
            }
        }
        if (g == groups) {
            if (groups >= kMaxDays) {
                return MT_ROW_ERR_VALUE; /* more than 7 distinct day groups */
            }
            if ((bits & all_bits) != 0) {
                /* This day already belongs to a DIFFERENT bitmap in the same
                 * set (an identical bitmap would have matched a group
                 * above), which CHIP answers ConstraintError for. Checked
                 * again here rather than trusting mt_rows_validate(): this
                 * function is the last gate before the store, and the same
                 * merge serves the fabric path in task 6. */
                return MT_ROW_ERR_VALUE;
            }
            bitmaps[groups] = bits;
            counts[groups]  = 0;
            groups++;
        }
        if (counts[g] >= kMaxPerDay) {
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
     * dayOfWeekForSequence and an empty chargingTargets list.
     * Instance::ValidateTargets() places no minimum on that list (innerIdx
     * starts at 0 and only innerIdx > 10 is rejected), so it arrives as a
     * perfectly valid command, and CHIP's own reference store clears the day
     * in both of its arms (EnergyEvseTargetsStore.cpp:305-315 copies the new
     * empty list over a matching bitmask, :347-370 appends the new entry with
     * its empty list). Deriving the mask from the flattened ROWS instead
     * would drop such a day silently: the user deletes Monday's targets, the
     * device answers SUCCESS, GetTargets keeps returning them, and the car
     * charges on a schedule that was deleted.
     *
     * Masked to the seven real days so a caller cannot subtract a bit that
     * is not a day.
     */
    all_bits = (uint8_t)(all_bits | (clear_days & kAllDaysMask));

    /*
     * Last read-only check: how many stored schedules will survive the
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
        if ((uint16_t)survivors + groups > kMaxDays) {
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
     *
     * The result cannot overflow kMaxDays: after the subtraction every
     * surviving schedule and every appended group owns at least one day, they
     * are pairwise day-disjoint, and there are only 7 days.
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
        const mt_row_t *row = &stage->row[r];
        uint8_t bits = (uint8_t)row->value[kRowDay];
        uint8_t slot = first_new;
        for (uint8_t g = 0; g < groups; g++) {
            if (bitmaps[g] == bits) {
                slot = (uint8_t)(first_new + g);
                break;
            }
        }
        EE::Structs::ChargingTargetStruct::Type t;
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

int mt_evse_targets_total_locked(uint16_t ep, uint16_t *total)
{
    if (total != nullptr) {
        *total = 0;
    }
    HearthEvseDelegate *d = nullptr;
    int rc = locate(ep, &d);
    if (rc != MT_ROW_OK) {
        return rc;
    }
    if (total != nullptr) {
        *total = d->row_total();
    }
    return MT_ROW_OK;
}

int mt_evse_targets_get_locked(uint16_t ep, uint16_t idx, mt_row_t *out, uint16_t *total)
{
    if (total != nullptr) {
        *total = 0;
    }
    HearthEvseDelegate *d = nullptr;
    int rc = locate(ep, &d);
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

    /* Walk the flattened (schedule, target) sequence to idx. */
    uint16_t seen = 0;
    for (uint8_t i = 0; i < d->m_schedule_count; i++) {
        if (idx - seen >= d->m_target_count[i]) {
            seen = (uint16_t)(seen + d->m_target_count[i]);
            continue;
        }
        const EE::Structs::ChargingTargetStruct::Type &t = d->m_targets[i][idx - seen];
        memset(out, 0, sizeof(*out));
        out->nfields = kRowFields;
        out->present[kRowDay]  = true;
        out->value[kRowDay]    = d->day_bits(i);
        out->present[kRowTime] = true;
        out->value[kRowTime]   = t.targetTimeMinutesPastMidnight;
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

int mt_evse_targets_erase_all_locked(void)
{
    /* RAM first, so a failing NVS erase still leaves the running device with
     * no schedule rather than one it cannot forget. AT+MTFRESET reboots
     * immediately afterwards either way. */
    for (size_t i = 0; i < s_evse_next; i++) {
        s_evse_delegates[i].clear_store();
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_EVSE_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 0; /* namespace never written: nothing to erase */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = nvs_erase_all(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erasing charging targets failed: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "charging targets erased");
    return 0;
}
