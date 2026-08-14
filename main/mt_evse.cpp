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
 * handed out and the encoder reading it.
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
     * Disable / EnableCharging: both commands ARE in the accepted list,
     * unconditionally (esp_matter_cluster.cpp:3397-3398 creates them), so a
     * controller can invoke them today. This round gives the host no
     * adjudication surface for either (design spec 7.2 lists the MatterEvse
     * API and it has none), and the firmware cannot honour them on its own:
     * SupplyState is host-pushed state, so "enabled" is a claim only the host
     * can make. Failure, not Success, for the HearthDemDelegate reason
     * (main.cpp): an unimplemented handler that answers Success is a silent
     * lie on the wire, while a failure is a visible one. Forwarding them over
     * +MTCMD the way Round B forwards Boost is the natural way to implement
     * them and is a deliberate non-goal of this round.
     */
    chip::Protocols::InteractionModel::Status Disable() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }

    chip::Protocols::InteractionModel::Status EnableCharging(
        const chip::app::DataModel::Nullable<uint32_t> &enableChargeTime,
        const int64_t &minimumChargeCurrent, const int64_t &maximumChargeCurrent) override
    {
        (void)enableChargeTime;
        (void)minimumChargeCurrent;
        (void)maximumChargeCurrent;
        return chip::Protocols::InteractionModel::Status::Failure;
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
     * SetTargets is TASK 6's, and is deliberately left refusing until then.
     * The adjudicated design (design spec 3.2) stages the incoming rows into
     * the shared staging buffer, raises +MTCMD carrying only a row count,
     * lets the host pull the rows with AT+MTROWGET inside its own command
     * window and answer AT+MTCMDRESP, and only then merges through the same
     * path an AT-side apply uses. None of that machinery exists yet, and the
     * honest intermediate state is a refusal: accepting the schedule without
     * the host's verdict would apply a charging plan the sketch never saw,
     * which is precisely the decision this round hands to the host.
     */
    chip::Protocols::InteractionModel::Status SetTargets(
        const chip::app::DataModel::DecodableList<
            EE::Structs::ChargingTargetScheduleStruct::DecodableType> &chargingTargetSchedules)
        override
    {
        (void)chargingTargetSchedules;
        ESP_LOGW(TAG, "ep %u: SetTargets refused (adjudication lands in task 6)", mEndpointId);
        return chip::Protocols::InteractionModel::Status::Failure;
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
        m_loaded = true;

        char key[16];
        key_for(mEndpointId, key, sizeof(key));

        nvs_handle_t h;
        esp_err_t err = nvs_open(MT_EVSE_NVS_NS, NVS_READONLY, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return Status::Success; /* namespace never written */
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ep %u: nvs_open failed: %s", mEndpointId, esp_err_to_name(err));
            return Status::Failure;
        }
        size_t len = sizeof(s_blob);
        err = nvs_get_blob(h, key, s_blob, &len);
        nvs_close(h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return Status::Success; /* no schedule stored for this endpoint */
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ep %u: reading charging targets failed: %s", mEndpointId,
                     esp_err_to_name(err));
            return Status::Failure;
        }
        if (!decode(len)) {
            ESP_LOGE(TAG, "ep %u: stored charging targets are corrupt (%u bytes), ignored",
                     mEndpointId, (unsigned)len);
            return Status::Failure;
        }
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
     */
    chip::Protocols::InteractionModel::Status GetTargets(
        chip::app::DataModel::List<const EE::Structs::ChargingTargetScheduleStruct::Type>
            &chargingTargetSchedules) override
    {
        if (!m_loaded) {
            (void)LoadTargets();
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

extern "C" void *mt_matter_evse_delegate_alloc(uint16_t ep)
{
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
    (void)d->LoadTargets();
    return d;
}

/*
 * ---- AT+MTMEAS cluster 0x0099 ----------------------------------------------
 *
 * The EVSE's whole attribute surface is delegate-served, so this is the only
 * way the host can set any of it (design spec 5.5). Two-pass
 * validate-then-apply, the mt_matter_meas_set() hard contract: a bad third
 * pair leaves the first two unapplied.
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

int mt_evse_targets_apply_locked(uint16_t ep, const mt_row_stage_t *stage)
{
    HearthEvseDelegate *d = nullptr;
    int rc = locate(ep, &d);
    if (rc != MT_ROW_OK) {
        return rc;
    }

    /*
     * The stage belongs to this (ep, kind) only when all three agree; when it
     * does not, the call applies ZERO rows, which is the documented clear
     * (mt_matter.h's contract, and the count-0 exception in the file
     * comment). Do not read stage->row[] outside that match: the buffer is
     * mt_at.c's file-static staging area and may hold another endpoint's
     * half-built set.
     */
    bool match = (stage != nullptr && stage->active && stage->ep == ep &&
                  stage->kind == MT_ROW_KIND_EVSE_TARGET && stage->count > 0);

    if (!match) {
        d->clear_store();
        if (!d->save()) {
            return MT_ROW_ERR_PERSIST;
        }
        return MT_ROW_OK;
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

    for (uint16_t r = 0; r < stage->count; r++) {
        const mt_row_t *row = &stage->row[r];
        if (row->nfields != kRowFields || !row->present[kRowDay] ||
            !row->present[kRowTime]) {
            /* Defensive: mt_rows_stage() canonicalises nfields and refuses a
             * row missing a mandatory field, so this cannot fire from the AT
             * path. */
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
     * set never mentioned are untouched, which is the whole point.
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

    for (uint16_t r = 0; r < stage->count; r++) {
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
